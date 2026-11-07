#include "sample_analysis.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

enum { PITCH_WINDOW = 2048, PITCH_LAG = 512, PITCH_WINDOWS = 6, SNAP_RADIUS = 48 };

typedef enum { NEUTRAL, SUSTAINED, PLUCKED, PERCUSSIVE } EnvelopePreset;

typedef struct {
    const char *alias;
    int program, drum_key;
    EnvelopePreset envelope;
} SampleAlias;

static const DmSampleEnvelope envelopes[] = {
    [NEUTRAL] = {.attack = 0.003, .release = 0.08, .sustain = 1},
    [SUSTAINED] = {.attack = 0.025, .decay = 0.18, .release = 0.2, .sustain = 0.9},
    [PLUCKED] = {.attack = 0.002, .decay = 0.25, .release = 0.12, .sustain = 0.65},
    [PERCUSSIVE] = {.attack = 0.001, .release = 0.06, .sustain = 1},
};

static const SampleAlias aliases[] = {
    {"piano", 0, -1, PLUCKED},      {"organ", 19, -1, SUSTAINED},   {"guitar", 24, -1, PLUCKED},
    {"bass", 33, -1, PLUCKED},      {"strings", 48, -1, SUSTAINED}, {"violin", 40, -1, SUSTAINED},
    {"trumpet", 56, -1, SUSTAINED}, {"sax", 65, -1, SUSTAINED},     {"flute", 73, -1, SUSTAINED},
    {"pad", 89, -1, SUSTAINED},     {"kick", -1, 36, PERCUSSIVE},   {"snare", -1, 38, PERCUSSIVE},
    {"clap", -1, 39, PERCUSSIVE},   {"hihat", -1, 42, PERCUSSIVE},  {"openhat", -1, 46, PERCUSSIVE},
    {"ride", -1, 51, PERCUSSIVE},   {"crash", -1, 49, PERCUSSIVE},  {"cowbell", -1, 56, PERCUSSIVE},
};

static bool is_canceled(const DmSampleImportCallbacks *cb) {
    return cb && cb->canceled && cb->canceled(cb->user);
}

static const char *basename_start(const char *path) {
    const char *name = path;

    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }

    return name;
}

static bool token_char(char c) {
    return isalnum((unsigned char)c) || (unsigned char)c >= 128;
}

static int note_token(const char *begin, const char *end) {
    static const int semitones[] = {9, 11, 0, 2, 4, 5, 7}; /* A–G. */
    int letter = toupper((unsigned char)*begin++) - 'A';

    if (letter < 0 || letter >= 7) {
        return -1;
    }

    int semitone = semitones[letter];

    if (begin < end && (*begin == '#' || *begin == 'b')) {
        semitone += *begin++ == '#' ? 1 : -1;
    }

    bool negative = begin < end && *begin == '-';
    begin += negative;

    if (end - begin != 1 || *begin < '0' || *begin > '9' || (negative && *begin != '1')) {
        return -1;
    }

    int octave = negative ? -1 : *begin - '0';
    int key = 12 * (octave + 1) + semitone;
    return key >= 0 && key < 128 ? key : -1;
}

static int filename_note(const char *name, const char *end, bool *conflict) {
    int root = -1;

    for (const char *begin = name; begin < end;) {
        const char *next = begin + 1;

        if (token_char(*begin)) {
            while (next < end && (token_char(*next) || *next == '#')) {
                ++next;
            }

            bool has_octave =
                next - begin > 2 || (next - begin == 2 && begin[1] != '#' && begin[1] != 'b');

            if (!has_octave && end - next >= 2 && next[0] == '-' && next[1] == '1' &&
                (end - next == 2 || !token_char(next[2]))) {
                next += 2;
            }

            int key = note_token(begin, next);

            if (key >= 0) {
                if (root >= 0 && root != key) {
                    *conflict = true;
                    return -1;
                }

                root = key;
            }
        }

        begin = next;
    }

    return root;
}

static bool word_equal(const char *begin, const char *end, const char *word) {
    while (begin < end && *word) {
        if (tolower((unsigned char)*begin++) != *word++) {
            return false;
        }
    }

    return begin == end && !*word;
}

static const SampleAlias *filename_alias(const char *name, const char *end) {
    const SampleAlias *match = NULL;

    for (const char *begin = name; begin < end;) {
        const char *next = begin + 1;

        if (token_char(*begin)) {
            while (next < end && token_char(*next)) {
                ++next;
            }

            for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); ++i) {
                const SampleAlias *alias = &aliases[i];

                if (word_equal(begin, next, alias->alias)) {
                    if (match && match != alias) {
                        return NULL;
                    }

                    match = alias;
                    break;
                }
            }
        }

        begin = next;
    }

    return match;
}

static double pitch_window(const double *signal, int samples, double rate, double *confidence) {
    double diff[PITCH_LAG + 1] = {0};
    int lag_min = (int)(rate / 2000);
    int lag_max = (int)(rate / 30);

    if (lag_max > samples / 3) {
        lag_max = samples / 3;
    }

    if (lag_max >= PITCH_LAG) {
        lag_max = PITCH_LAG - 1;
    }

    int points = samples - lag_max;
    double sum = 0;
    double energy = 0;

    for (int i = 0; i < points; ++i) {
        energy += signal[i] * signal[i];
    }

    if (points < 64 || energy < points * 0.000002) {
        return 0;
    }

    for (int lag = 1; lag <= lag_max; ++lag) {
        double value = 0;

        for (int i = 0; i < points; ++i) {
            double delta = signal[i] - signal[i + lag];
            value += delta * delta;
        }

        sum += value;
        diff[lag] = sum > 0 ? value * lag / sum : 1;

        if (lag > lag_min + 1 && diff[lag - 1] < 0.18 && diff[lag - 1] <= diff[lag - 2] &&
            diff[lag - 1] < diff[lag]) {
            int best = lag - 1;
            double curvature = diff[best - 1] + diff[best + 1] - 2 * diff[best];
            double offset = curvature > 0 ? (diff[best - 1] - diff[best + 1]) / (2 * curvature) : 0;
            *confidence = 1 - diff[best];
            return rate / (best + offset);
        }
    }

    return 0;
}

static double estimate(const DmSampleAudio *audio, const DmSampleImportCallbacks *callbacks,
                       double *confidence) {
    double signal[PITCH_WINDOW];
    int step = (audio->rate + 11999) / 12000;
    size_t size = audio->frames / (size_t)step;
    int count = size < PITCH_WINDOW ? (int)size : PITCH_WINDOW;
    double best = 0;

    for (int window = 0; window < PITCH_WINDOWS && !is_canceled(callbacks); ++window) {
        size_t start = (size - (size_t)count) * (size_t)window / (PITCH_WINDOWS - 1);

        for (int channel = 0; channel < audio->channels; ++channel) {
            for (int i = 0; i < count; ++i) {
                size_t frame = (start + (size_t)i) * (size_t)step;
                double sum = 0;

                for (int j = 0; j < step; ++j) {
                    sum += audio->pcm[(frame + (size_t)j) * (size_t)audio->channels + channel];
                }

                signal[i] = sum / step;
            }

            double score = 0;
            double hz = pitch_window(signal, count, (double)audio->rate / step, &score);

            if (score > *confidence) {
                *confidence = score;
                best = hz;
            }
        }

        if (size == (size_t)count || *confidence > 0.98) {
            break;
        }
    }

    return best;
}

static bool brief_transient(const DmSampleAudio *audio) {
    size_t window = (size_t)audio->rate / 50;

    if (audio->frames < 4 * window || audio->frames > (size_t)audio->rate / 2) {
        return false;
    }

    double head = 0, tail = 0;
    size_t channels = (size_t)audio->channels;

    for (size_t i = 0; i < window * channels; ++i) {
        head = fmax(head, fabs(audio->pcm[i]));
        tail = fmax(tail, fabs(audio->pcm[(audio->frames - window) * channels + i]));
    }

    return head >= 0.06 && tail < head * 0.05;
}

bool dm_sample_analyze(const DmSampleAudio *audio, const char *filename,
                       const DmSampleImportCallbacks *callbacks, DmSampleAnalysis *out) {
    DmSampleAnalysis analysis = {
        .root = 60, .program = -1, .drum_key = -1, .envelope = envelopes[NEUTRAL]};
    double confidence = 0;
    double hz = estimate(audio, callbacks, &confidence);

    if (is_canceled(callbacks)) {
        return false;
    }

    const char *name = basename_start(filename);
    const char *end = strrchr(name, '.');

    if (!end || end == name) {
        end = name + strlen(name);
    }

    int from_name = filename_note(name, end, &analysis.conflicting_pitch);
    double midi = hz > 0 ? 69 + 12 * log2(hz / 440) : -1;
    int from_audio = confidence >= 0.82 && midi >= -0.5 && midi < 127.5 ? (int)lround(midi) : -1;
    analysis.frequency = hz;

    if (from_name >= 0) {
        analysis.source = DM_SAMPLE_PITCH_FILENAME;
        analysis.root = from_name;
        analysis.confidence = 1;

        if (audio->embedded_root == from_name) {
            analysis.cents = audio->embedded_cents;
        }
    } else if (audio->embedded_root >= 0 && audio->embedded_root < 128) {
        analysis.source = DM_SAMPLE_PITCH_EMBEDDED;
        analysis.root = audio->embedded_root;
        analysis.cents = audio->embedded_cents;
        analysis.confidence = 1;
    } else if (from_audio >= 0) {
        analysis.source = DM_SAMPLE_PITCH_SIGNAL;
        analysis.root = from_audio;
        analysis.cents = 100 * (midi - from_audio);
        analysis.confidence = confidence;
    }

    analysis.conflicting_pitch |=
        (from_name >= 0 && from_name != analysis.root) ||
        (audio->embedded_root >= 0 && audio->embedded_root != analysis.root) ||
        (from_audio >= 0 && fabs(midi - analysis.root - analysis.cents / 100) > 0.6);
    const SampleAlias *alias = filename_alias(name, end);

    if (alias) {
        analysis.program = alias->program;
        analysis.drum_key = alias->drum_key;
        analysis.envelope = envelopes[alias->envelope];
    } else if (analysis.source == DM_SAMPLE_PITCH_DEFAULT && brief_transient(audio)) {
        analysis.envelope = envelopes[PERCUSSIVE];
    }

    *out = analysis;
    return true;
}

bool dm_sample_peaks(const DmSampleAudio *audio, DmSamplePeak *bins, size_t count, float *absolute,
                     const DmSampleImportCallbacks *callbacks) {
    if (!count || count > 4096 || !audio->frames) {
        return false;
    }

    *absolute = 0;

    for (size_t i = 0; i < count; ++i) {
        size_t whole = audio->frames / count;
        size_t rem = audio->frames % count;
        size_t first = whole * i + rem * i / count;
        size_t end = whole * (i + 1) + rem * (i + 1) / count;
        bins[i] = (DmSamplePeak){0};

        for (size_t frame = first; frame < (end > first ? end : first + 1); ++frame) {
            if (!(frame & 4095u) && is_canceled(callbacks)) {
                return false;
            }

            for (int ch = 0; ch < audio->channels; ++ch) {
                float value = audio->pcm[frame * (size_t)audio->channels + ch];

                if (frame == first || value < bins[i].minimum[ch]) {
                    bins[i].minimum[ch] = value;
                }

                if (frame == first || value > bins[i].maximum[ch]) {
                    bins[i].maximum[ch] = value;
                }

                *absolute = fmaxf(*absolute, fabsf(value));
            }
        }
    }

    return !is_canceled(callbacks);
}

bool dm_sample_loop_valid(DmSampleLoop loop, size_t frames) {
    return loop.start < loop.end && loop.end <= frames && loop.end - loop.start >= 2;
}

static int crossing(const DmSampleAudio *audio, size_t frame) {
    int directions = 0;

    for (int ch = 0; ch < audio->channels; ++ch) {
        size_t index = frame * (size_t)audio->channels + ch;
        float before = audio->pcm[index - (size_t)audio->channels];
        float after = audio->pcm[index];
        int sign = before < 0 && after >= 0 ? 1 : before > 0 && after <= 0 ? 2 : 0;

        if (!sign && fmaxf(fabsf(before), fabsf(after)) > 0.003f) {
            return -1;
        }

        directions |= sign << (2 * ch);
    }

    return directions;
}

static size_t distance(size_t a, size_t b) {
    return a > b ? a - b : b - a;
}

bool dm_sample_loop_snap(const DmSampleAudio *audio, DmSampleLoop requested, DmSampleLoop *out) {
    *out = requested;

    if (!dm_sample_loop_valid(requested, audio->frames)) {
        return false;
    }

    double best = HUGE_VAL;

    for (size_t a = requested.start > SNAP_RADIUS ? requested.start - SNAP_RADIUS : 1;
         a < audio->frames && a <= requested.start + SNAP_RADIUS; ++a) {
        int first = crossing(audio, a);

        if (first < 0) {
            continue;
        }

        for (size_t b = requested.end > SNAP_RADIUS ? requested.end - SNAP_RADIUS : 1;
             b < audio->frames && b <= requested.end + SNAP_RADIUS; ++b) {
            if (b < a + 2 || crossing(audio, b) != first) {
                continue;
            }

            double join = 0;

            for (int ch = 0; ch < audio->channels; ++ch) {
                size_t channels = (size_t)audio->channels;
                size_t left = a * channels + ch;
                size_t right = b * channels + ch;
                float step_a = audio->pcm[left] - audio->pcm[left - channels];
                float step_b = audio->pcm[right] - audio->pcm[right - channels];
                join += fabsf(audio->pcm[left] - audio->pcm[right]) + fabsf(step_a - step_b);
            }

            double score =
                join + 0.002 * (distance(a, requested.start) + distance(b, requested.end));

            if (join < 0.2 && score < best) {
                best = score;
                *out = (DmSampleLoop){a, b};
            }
        }
    }

    return isfinite(best);
}
