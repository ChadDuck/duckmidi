#include "pattern_generation.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DmNote note;
    int channel;
} Flat;

typedef struct {
    uint64_t key;
    size_t count, last;
    int id;
} Entry;

typedef struct {
    int source, event, output, next;
} AutoGroup;

static bool cancelled(const DmGenerationHooks *h) {
    return h && h->cancelled && h->cancelled(h->user);
}

static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static uint64_t real_hash(double x) {
    return mix((uint64_t)llround(x * 1e9));
}

static int flat_compare(const void *a, const void *b) {
    const Flat *x = a, *y = b;

    if (x->channel != y->channel) {
        return x->channel - y->channel;
    }

    if (x->note.beat != y->note.beat) {
        return x->note.beat < y->note.beat ? -1 : 1;
    }

    if (x->note.key != y->note.key) {
        return (int)x->note.key - y->note.key;
    }

    if (x->note.port != y->note.port) {
        return (int)x->note.port - y->note.port;
    }

    if (x->note.length != y->note.length) {
        return x->note.length < y->note.length ? -1 : 1;
    }

    return (int)x->note.velocity - y->note.velocity;
}

static size_t table_size(size_t n) {
    size_t size = 16;

    while (size < n * 2) {
        size *= 2;
    }

    return size;
}

static Entry *lookup(Entry *table, size_t size, uint64_t key) {
    size_t at = (size_t)mix(key) & (size - 1);

    while (table[at].count && table[at].key != key) {
        at = (at + 1) & (size - 1);
    }

    return &table[at];
}

static int control_group(DmAutomation a) {
    if (a.type == MIDI_PITCH_BEND) {
        return 128;
    }

    if (a.type == MIDI_PROGRAM) {
        return 129;
    }

    if (a.type == MIDI_CHANNEL_PRESSURE) {
        return 130;
    }

    if (a.type == MIDI_KEY_PRESSURE) {
        return 131;
    }

    if (a.a == MIDI_CC_DATA || a.a == MIDI_CC_DATA_LSB ||
        (a.a >= MIDI_CC_INCREMENT && a.a <= MIDI_CC_RPN_MSB)) {
        return 128;
    }

    return a.a == MIDI_CC_PORTAMENTO ? MIDI_CC_PORTAMENTO_TIME : a.a;
}

static bool separate_controls(const DmProject *source, DmProject *next, const bool *placed,
                              const DmGenerationHooks *hooks, DmGenerationStats *stats) {
    bool modular = false;

    for (size_t i = 0; i < source->clip_count; i++) {
        modular |= source->patterns[source->clips[i].pattern].kind || source->clips[i].length > 0;
    }

    for (size_t i = 0; i < source->automation_count; i++) {
        DmAutomation a = source->automation[i];
        modular |=
            a.pattern >= 0 && a.curve && a.type == MIDI_CONTROL &&
            (a.a == MIDI_CC_PORTAMENTO_TIME || a.a == MIDI_CC_SUSTAIN || a.a == MIDI_CC_SOSTENUTO);
    }

    size_t size = table_size(source->automation_count + 1), count = 0;
    Entry *table = calloc(size, sizeof(*table));
    AutoGroup *groups = calloc(source->automation_count + 1, sizeof(*groups));
    int *heads = malloc((source->pattern_count + 1) * sizeof(int)),
        *tails = malloc((source->pattern_count + 1) * sizeof(int));

    if (!table || !groups || !heads || !tails) {
        free(table);
        free(groups);
        free(heads);
        free(tails);
        return false;
    }

    for (size_t i = 0; i <= source->pattern_count; i++) {
        heads[i] = tails[i] = -1;
    }

    bool ok = true;
    int lanes[16][132] = {{0}};

    for (size_t i = 0; i < source->automation_count && ok; i++) {
        if (!(i % 1024) && cancelled(hooks)) {
            ok = false;
            break;
        }

        DmAutomation a = source->automation[i];

        if (a.pattern < 0 || !placed[a.pattern] || source->patterns[a.pattern].kind) {
            continue;
        }

        int event = control_group(a);
        uint64_t key =
            ((uint64_t)(unsigned)a.pattern << 15) | ((uint64_t)a.port << 8) | (unsigned)event;
        Entry *entry = lookup(table, size, key);

        if (!entry->count) {
            char name[64];
            snprintf(name, sizeof(name), "Controls %zu - %s", count + 1,
                     event == 128   ? "Pitch bend"
                     : event == 129 ? "Program"
                     : event == 130 ? "Pressure"
                     : event == 131 ? "Poly pressure"
                                    : "Controller");
            int id = dm_pattern_add(next, name);

            if (id < 0) {
                ok = false;
                break;
            }

            DmPattern *p = &next->patterns[id];
            p->kind = 1;
            p->control_scope = modular ? 1 : 2;
            p->event = event == 128   ? -2
                       : event == 129 ? -1
                       : event == 130 ? -4
                       : event == 131 ? -5
                                      : event;
            p->length = source->patterns[a.pattern].length;
            p->port = a.port;
            p->reference = source->patterns[a.pattern].count
                               ? source->patterns[a.pattern].notes[0].key
                               : source->patterns[a.pattern].reference;
            groups[count] = (AutoGroup){a.pattern, event, id, -1};

            if (tails[a.pattern] >= 0) {
                groups[tails[a.pattern]].next = (int)count;
            } else {
                heads[a.pattern] = (int)count;
            }

            tails[a.pattern] = (int)count;
            entry->key = key;
            entry->count = 1;
            entry->id = (int)count++;
            stats->automation_patterns++;
        }

        int id = groups[entry->id].output;
        a.pattern = id;
        next->patterns[id].length = fmax(next->patterns[id].length, a.beat);

        if (!dm_automation_add(next, a)) {
            ok = false;
        }
    }

    next->clip_count = 0;

    for (size_t i = 0; i < source->clip_count && ok; i++) {
        DmClip c = source->clips[i];

        if (source->patterns[c.pattern].kind) {
            ok = dm_clip_add(next, c);
            continue;
        }

        int original = c.pattern;

        for (int index = heads[original]; index >= 0 && ok; index = groups[index].next) {
            AutoGroup g = groups[index];

            if (!lanes[c.channel][g.event]) {
                int lane = next->lane_count[c.channel];
                bool created = lane < DM_LANES;

                if (created) {
                    lane++;
                }

                if (!lane) {
                    lane = 1;
                }

                next->lane_count[c.channel] = lane;
                lanes[c.channel][g.event] = lane;

                if (created) {
                    snprintf(next->lane_names[c.channel][lane - 1], 64, "Generated controls");
                }
            }

            c.pattern = g.output;
            c.lane = lanes[c.channel][g.event];
            c.row = 0; /* Automation subrows have one row. */
            c.length = dm_clip_length(source, source->clips[i]);
            c.selected = 0;
            ok = dm_clip_add(next, c);
        }
    }

    free(table);
    free(groups);
    free(heads);
    free(tails);
    return ok;
}

static bool matches(const DmPattern *p, const Flat *flat, size_t first, size_t end, double origin) {
    if (p->count != end - first) {
        return false;
    }

    for (size_t i = first; i < end; i++) {
        DmNote a = p->notes[i - first], b = flat[i].note;

        if (a.key != b.key || a.port != b.port || a.velocity != b.velocity ||
            fabs(a.length - b.length) > 1e-10 || fabs(a.beat - (b.beat - origin)) > 1e-10) {
            return false;
        }
    }

    return true;
}

/* Phrases are made of whole bars: a bar's notes are those starting inside it. */
typedef struct {
    double start, length;
    size_t first, count;
    uint64_t hash;
} Bar;

enum { PHRASE_BARS = 16, UNIQUE_BARS = 4, SONG_BARS = 200000 };

/* The song's bars up to end, following meter changes. */
static Bar *song_bars(const DmProject *p, double end, size_t *count) {
    Bar *bars = NULL;
    size_t n = 0, capacity = 0;

    for (double at = 0; at < end - 1e-9;) {
        DmMeter m = dm_meter(p, at);
        double index = floor((at - m.origin) / m.bar + 1e-9);
        double next = fmin(m.origin + (index + 1) * m.bar, m.next_change);

        if (next <= at + 1e-9) {
            next = at + m.bar;
        }

        if (n >= SONG_BARS || !dm_reserve((void **)&bars, &capacity, n + 1, sizeof(*bars))) {
            free(bars);
            return NULL;
        }

        bars[n++] = (Bar){at, next - at, 0, 0, 0};
        at = next;
    }

    *count = n;
    return bars;
}

/* Fills each bar with the channel's notes that start in it, and hashes its content relative to
   the bar start so equal bars anywhere in the song hash alike. */
static void fill_bars(Bar *bars, size_t n, const Flat *flat, size_t count) {
    size_t at = 0;

    for (size_t b = 0; b < n; b++) {
        Bar *bar = &bars[b];
        bar->first = at;
        uint64_t hash = mix(real_hash(bar->length));

        while (at < count && (b + 1 == n || flat[at].note.beat < bar->start + bar->length - 1e-9)) {
            DmNote note = flat[at].note;
            hash =
                mix(hash ^ note.key ^ ((uint64_t)note.velocity << 8) ^ ((uint64_t)note.port << 16) ^
                    real_hash(note.length) ^ mix(real_hash(note.beat - bar->start)));
            at++;
        }

        bar->count = at - bar->first;
        bar->hash = hash;
    }
}

static bool bars_equal(const Bar *bars, const Flat *flat, size_t a, size_t b, size_t length) {
    for (size_t k = 0; k < length; k++) {
        const Bar *x = &bars[a + k], *y = &bars[b + k];

        if (fabs(x->length - y->length) > 1e-10 || x->count != y->count) {
            return false;
        }

        for (size_t i = 0; i < x->count; i++) {
            DmNote p = flat[x->first + i].note, q = flat[y->first + i].note;

            if (p.key != q.key || p.port != q.port || p.velocity != q.velocity ||
                fabs(p.length - q.length) > 1e-10 ||
                fabs((p.beat - x->start) - (q.beat - y->start)) > 1e-10) {
                return false;
            }
        }
    }

    return true;
}

static uint64_t window_key(const Bar *bars, size_t start, size_t length) {
    uint64_t hash = mix(length);

    for (size_t k = 0; k < length; k++) {
        hash = mix(hash * UINT64_C(1000000007) + bars[start + k].hash);
    }

    return hash;
}

/* repeat[i]: the longest window of bars starting at bar i (up to PHRASE_BARS, starting and
   ending on a bar with notes) whose exact notes occur again elsewhere without overlapping it.
   For each length, keeping the earliest and latest start of every window content is enough:
   if any other occurrence is far enough away, one of the two is. */
static bool find_repeats(const Bar *bars, size_t n, const Flat *flat, size_t *repeat,
                         const DmGenerationHooks *hooks) {
    size_t slots = table_size(n + 1);
    Entry *table = calloc(slots, sizeof(*table));

    if (!table) {
        return false;
    }

    for (size_t length = 1; length <= PHRASE_BARS && length <= n; length++) {
        if (cancelled(hooks)) {
            free(table);
            return false;
        }

        memset(table, 0, slots * sizeof(*table));

        for (size_t i = 0; i + length <= n; i++) {
            if (!bars[i].count || !bars[i + length - 1].count) {
                continue;
            }

            uint64_t key = window_key(bars, i, length);
            Entry *e = lookup(table, slots, key);

            if (!e->count) {
                *e = (Entry){key, 1, i, (int)i};
            } else {
                e->last = i;
            }
        }

        for (size_t i = 0; i + length <= n; i++) {
            if (!bars[i].count || !bars[i + length - 1].count) {
                continue;
            }

            Entry *e = lookup(table, slots, window_key(bars, i, length));
            size_t first = (size_t)e->id, other = first + length <= i     ? first
                                                  : e->last >= i + length ? e->last
                                                                          : i;

            if (other != i && bars_equal(bars, flat, i, other, length)) {
                repeat[i] = length;
            }
        }
    }

    free(table);
    return true;
}

/* The note row for a placement over [start, end): the first row free by its start. A note held
   across the end of the previous phrase keeps that placement sounding, so the next one moves
   to another row instead of overlapping it. */
static int free_row(double *row_ends, int *rows, double start, double end) {
    int row = 0;

    while (row < *rows && row_ends[row] > start + 1e-9) {
        row++;
    }

    if (row == DM_NOTE_ROWS) {
        row = DM_NOTE_ROWS - 1;
    }

    if (row == *rows) {
        ++*rows;
    }

    row_ends[row] = fmax(row_ends[row], end);
    return row;
}

/* The phrase starting at bar i: the longest repeated window there, or else up to UNIQUE_BARS
   bars with notes that repeat nowhere. */
static size_t unit_length(const Bar *bars, size_t n, const size_t *repeat, size_t i) {
    size_t length = repeat[i];

    if (!length) {
        length = 1;

        while (length < UNIQUE_BARS && i + length < n && bars[i + length].count &&
               !repeat[i + length]) {
            length++;
        }
    }

    return length;
}

/* A way to cut a channel into cells: the song's bars, or a detected repeat period. */
typedef struct {
    Bar *cells;
    size_t count, *repeat, linked; /* linked: notes that land in repeated phrases */
} Grid;

static void grid_free(Grid *g) {
    free(g->cells);
    free(g->repeat);
    *g = (Grid){0};
}

/* Takes cells; fails only on allocation or cancellation. */
static bool grid_plan(Grid *g, Bar *cells, size_t count, const Flat *flat, size_t notes,
                      const DmGenerationHooks *hooks) {
    *g = (Grid){cells, count, calloc(count + 1, sizeof(size_t)), 0};

    if (!cells || !g->repeat) {
        grid_free(g);
        return false;
    }

    fill_bars(cells, count, flat, notes);

    if (!find_repeats(cells, count, flat, g->repeat, hooks)) {
        grid_free(g);
        return false;
    }

    for (size_t i = 0; i < count;) {
        if (!cells[i].count) {
            i++;
            continue;
        }

        size_t length = unit_length(cells, count, g->repeat, i);

        if (g->repeat[i]) {
            g->linked += cells[i + length - 1].first + cells[i + length - 1].count - cells[i].first;
        }

        i += length;
    }

    return true;
}

/* Cells of one period, lined up with anchor, from the song start to past the last note; the
   first cell is shorter when the period does not divide the anchor's beat. */
static Bar *period_cells(double period, double anchor, double last, size_t *count) {
    double origin = anchor - floor(anchor / period + 1e-9) * period;

    /* Rounding can leave the origin a hair below zero or below a whole period. */
    if (origin < 1e-9 || origin > period - 1e-9) {
        origin = 0;
    }
    size_t n = (origin > 1e-9) + (size_t)ceil((last - origin) / period + 1e-9) + 1;

    if (n > SONG_BARS) {
        return NULL;
    }

    Bar *cells = calloc(n, sizeof(*cells));

    if (!cells) {
        return NULL;
    }

    size_t k = 0;

    if (origin > 1e-9) {
        cells[k++] = (Bar){0, origin, 0, 0, 0};
    }

    for (size_t step = 0; k < n; step++) {
        cells[k++] = (Bar){origin + step * period, period, 0, 0, 0};
    }

    *count = n;
    return cells;
}

enum { PERIOD_CANDIDATES = 3, PERIOD_NEIGHBORS = 4 };

typedef struct {
    uint64_t hash;
    double beat;
} Onset;

static int onset_compare(const void *a, const void *b) {
    const Onset *x = a, *y = b;
    return x->hash != y->hash ? (x->hash < y->hash ? -1 : 1)
                              : (x->beat > y->beat) - (x->beat < y->beat);
}

/* Repeat periods, for songs whose phrases do not line up with the bars: every identical group
   of notes starting together votes for the distances to its next few occurrences, and the
   distances with most votes (at least two beats, so single repeated notes do not win) are
   returned, best first, each with the earliest beat that voted for it to line its cells up. */
static size_t repeat_periods(const Flat *flat, size_t count, double *periods, double *anchors) {
    Onset *onsets = malloc((count + 1) * sizeof(*onsets));
    size_t slots = table_size(count * PERIOD_NEIGHBORS + 1), groups = 0, found = 0;
    Entry *votes = calloc(slots, sizeof(*votes));
    /* Votes are keyed by the distance rounded to a millionth of a beat; the exact distance is
       kept so cells line up with the notes without drifting. */
    double *exact = calloc(slots, sizeof(double)), *earliest = calloc(slots, sizeof(double));

    if (!onsets || !votes || !exact || !earliest) {
        free(onsets);
        free(votes);
        free(exact);
        free(earliest);
        return 0;
    }

    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        uint64_t hash = mix(1);

        for (; end <= count; end++) {
            DmNote note = flat[end - 1].note;
            hash = mix(hash ^ note.key ^ ((uint64_t)note.velocity << 8) ^
                       ((uint64_t)note.port << 16) ^ real_hash(note.length));

            if (end == count || fabs(flat[end].note.beat - flat[i].note.beat) > 1e-10) {
                break;
            }
        }

        onsets[groups++] = (Onset){hash, flat[i].note.beat};
        i = end;
    }

    qsort(onsets, groups, sizeof(*onsets), onset_compare);

    for (size_t i = 0; i < groups; i++) {
        for (size_t j = i + 1; j < groups && j <= i + PERIOD_NEIGHBORS; j++) {
            double distance = onsets[j].beat - onsets[i].beat;

            if (onsets[j].hash != onsets[i].hash) {
                break;
            }

            if (distance >= 2 && distance <= 256) {
                uint64_t key = (uint64_t)llround(distance * 1e6);
                Entry *e = lookup(votes, slots, key);

                if (!e->count) {
                    exact[e - votes] = distance;
                    earliest[e - votes] = onsets[i].beat;
                }

                earliest[e - votes] = fmin(earliest[e - votes], onsets[i].beat);

                e->key = key;
                e->count++;
            }
        }
    }

    for (size_t pick = 0; pick < PERIOD_CANDIDATES; pick++) {
        Entry *best = NULL;

        for (size_t s = 0; s < slots; s++) {
            if (votes[s].count > 1 && (!best || votes[s].count > best->count)) {
                best = &votes[s];
            }
        }

        if (!best) {
            break;
        }

        anchors[found] = earliest[best - votes];
        periods[found++] = exact[best - votes];
        best->count = 0; /* Keeps the slot occupied for lookups but out of later picks. */
    }

    free(onsets);
    free(votes);
    free(exact);
    free(earliest);
    return found;
}

/* The pattern holding the notes flat[first, end) relative to origin: an earlier phrase's
   pattern when its notes are the same, or a new one. Returns -1 when no pattern can be made. */
static int phrase_pattern(DmProject *next, const Flat *flat, size_t first, size_t end,
                          double origin, uint64_t hash, Entry *patterns, size_t slots,
                          DmGenerationStats *stats) {
    size_t at = (size_t)mix(hash) & (slots - 1);

    for (; patterns[at].count; at = (at + 1) & (slots - 1)) {
        if (patterns[at].key == hash &&
            matches(&next->patterns[patterns[at].id], flat, first, end, origin)) {
            stats->linked_placements++;
            return patterns[at].id;
        }
    }

    char name[64];
    snprintf(name, sizeof(name), "Generated notes %zu", stats->note_patterns + 1);
    int id = dm_pattern_add(next, name);

    if (id < 0) {
        return -1;
    }

    DmPattern *p = &next->patterns[id];

    for (size_t j = first; j < end; j++) {
        DmNote note = flat[j].note;
        /* A note within rounding of the phrase start belongs to it. */
        note.beat = fmax(0, note.beat - origin);
        note.selected = 0;

        if (!dm_note_add(p, note)) {
            return -1;
        }
    }

    dm_pattern_fit(next, p);
    patterns[at] = (Entry){hash, 1, 0, id};
    stats->note_patterns++;
    return id;
}

/* One channel's notes become phrases. The channel is cut into cells, either the song's bars or
   a detected repeat period, whichever links more notes into repeated phrases (bars on a tie,
   so phrases line up with the music's bars when they can). Each phrase is the longest repeated
   window of cells at its position, or a short run of cells that repeat nowhere; equal phrases
   share one pattern and cells without notes get no placement. */
static bool channel_patterns(DmProject *next, const Flat *flat, size_t count, int channel,
                             const Bar *song, size_t bar_count, double channel_end, Entry *patterns,
                             size_t pattern_slots, const DmGenerationHooks *hooks,
                             DmGenerationStats *stats) {
    Grid best = {0};
    Bar *bars = malloc((bar_count + 1) * sizeof(*bars));

    if (bars) {
        memcpy(bars, song, bar_count * sizeof(*bars));
    }

    if (!grid_plan(&best, bars, bar_count, flat, count, hooks)) {
        return false;
    }

    double periods[PERIOD_CANDIDATES], anchors[PERIOD_CANDIDATES];
    size_t candidates = repeat_periods(flat, count, periods, anchors);

    /* A voted period may span several phrases or bars, so its halves and quarters are tried
       too; windows of several cells cover the longer phrases. */
    for (size_t c = 0; c < candidates * 3; c++) {
        double period = periods[c / 3] / (1 << (c % 3));

        if (period < 2) {
            continue;
        }

        size_t cells = 0;
        Bar *grid = period_cells(period, anchors[c / 3], flat[count - 1].note.beat, &cells);
        Grid option;

        if (!grid_plan(&option, grid, cells, flat, count, hooks)) {
            if (cancelled(hooks)) {
                grid_free(&best);
                return false;
            }

            continue;
        }

        if (option.linked > best.linked) {
            grid_free(&best);
            best = option;
        } else {
            grid_free(&option);
        }
    }

    Bar *cells = best.cells;
    double row_ends[DM_NOTE_ROWS] = {0};
    int rows = 0;
    bool ok = true;

    for (size_t i = 0; i < best.count && ok;) {
        if (!cells[i].count) {
            i++;
            continue;
        }

        size_t length = unit_length(cells, best.count, best.repeat, i);
        size_t first = cells[i].first,
               end = cells[i + length - 1].first + cells[i + length - 1].count;
        double origin = cells[i].start, span = 0, sounding = 0;

        for (size_t k = 0; k < length; k++) {
            span += cells[i + k].length;
        }

        for (size_t j = first; j < end; j++) {
            sounding = fmax(sounding, flat[j].note.beat + flat[j].note.length - origin);
        }

        int id = phrase_pattern(next, flat, first, end, origin, window_key(cells, i, length),
                                patterns, pattern_slots, stats);

        if (id < 0) {
            ok = false;
            break;
        }

        /* The placement covers its cells, within the channel's original end, and any note held
           past them. */
        double clip_length = fmax(sounding, fmin(span, channel_end - origin));
        int row = free_row(row_ends, &rows, origin, origin + clip_length);

        if (next->note_rows[channel] < rows) {
            next->note_rows[channel] = rows;
        }

        if (ok) {
            ok = dm_clip_add(next, (DmClip){.beat = origin,
                                            .pattern = id,
                                            .channel = channel,
                                            .length = clip_length,
                                            .row = row});
        }

        stats->notes += end - first;
        i += length;
    }

    grid_free(&best);
    return ok;
}

bool dm_generate_patterns(const DmProject *source, DmProject *output,
                          const DmGenerationHooks *hooks, DmGenerationStats *stats, char *error,
                          size_t cap) {
    DmProject next;
    dm_project_init(&next);
    *stats = (DmGenerationStats){0};
    Flat *flat = NULL;
    size_t count = 0, capacity = 0;
    bool *placed = calloc(source->pattern_count + 1, sizeof(bool));
    Entry *table = NULL;
    bool ok = placed && dm_project_copy(&next, source);
    double ends[16] = {0};

    for (size_t i = 0; i < source->clip_count && ok; i++) {
        if (cancelled(hooks)) {
            ok = false;
            break;
        }

        DmClip clip = source->clips[i];
        const DmPattern *p = &source->patterns[clip.pattern];

        if (p->kind) {
            continue;
        }

        placed[clip.pattern] = true;
        double length = dm_clip_length(source, clip), shown = clip.offset + length;
        ends[clip.channel] = fmax(ends[clip.channel], clip.beat + length);

        /* As playback reads a placement: its slice of the pattern, with notes crossing either
           edge shortened to the visible part. */
        for (size_t j = 0; j < p->count; j++) {
            DmNote note = p->notes[j];
            double start = fmax(note.beat, clip.offset), end = fmin(note.beat + note.length, shown);

            if (note.beat >= shown || end <= start) {
                continue;
            }

            note.beat = clip.beat + start - clip.offset;
            note.length = end - start;

            if (count >= 2000000 ||
                !dm_reserve((void **)&flat, &capacity, count + 1, sizeof(*flat))) {
                ok = false;
                break;
            }

            flat[count++] = (Flat){note, clip.channel};
        }
    }

    if (ok) {
        ok = separate_controls(source, &next, placed, hooks, stats);
    }

    if (ok && count) {
        qsort(flat, count, sizeof(*flat), flat_compare);
    }

    size_t slots = table_size(count + 1), bar_count = 0;
    double song_end = 0;

    for (int channel = 0; channel < 16; channel++) {
        song_end = fmax(song_end, ends[channel]);
    }

    Bar *bars = NULL;

    if (ok) {
        table = calloc(slots, sizeof(*table));
        bars = song_bars(source, song_end + 1e-6, &bar_count);
        ok = table && (bars || !bar_count);
    }

    size_t first = 0;

    for (int channel = 0; channel < 16 && ok; channel++) {
        if (hooks && hooks->progress) {
            hooks->progress(hooks->user, (unsigned)(channel * 100 / 16));
        }

        size_t end = first;

        while (end < count && flat[end].channel == channel) {
            end++;
        }

        size_t before = next.clip_count;

        if (end > first) {
            ok = channel_patterns(&next, flat + first, end - first, channel, bars, bar_count,
                                  ends[channel], table, slots, hooks, stats);
        }

        if (ends[channel] > 0 && next.clip_count == before && ok) {
            int id = dm_pattern_add(&next, "Generated silence");
            ok = id >= 0;

            if (ok) {
                next.patterns[id].length = ends[channel];
                ok = dm_clip_add(&next, (DmClip){.pattern = id, .channel = channel});
                stats->note_patterns++;
            }
        }

        if (next.clip_count > before) {
            DmClip *last = &next.clips[next.clip_count - 1];
            last->length = fmax(dm_clip_length(&next, *last), ends[channel] - last->beat);
        }

        first = end;
    }

    if (ok) {
        for (size_t i = source->pattern_count; i > 0; i--) {
            if (placed[i - 1] && !source->patterns[i - 1].kind) {
                dm_pattern_remove(&next, (int)i - 1);
            }
        }

        memcpy(next.channels, source->channels, sizeof(next.channels));
        stats->placements = next.clip_count;
        dm_project_free(output);
        *output = next;
        dm_project_init(&next);
    } else {
        snprintf(error, cap, "%s",
                 cancelled(hooks)
                     ? "Pattern generation cancelled."
                     : "Cannot generate patterns: memory, note or pattern limit exceeded.");
    }

    free(placed);
    free(flat);
    free(table);
    free(bars);
    dm_project_free(&next);
    return ok;
}
