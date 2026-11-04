/* duckmidi plugin adapter for OoT export. Conversion and repository patches stay in ../src;
   settings in ui.c; substitutes in review.c. */
#define DME_BUILD_PLUGIN
#include "oot_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef OotPluginJob ExportJob;

#define MAX_FONTS 48     /* fontLoadStatus[0x30] in include/audio.h */
#define MAX_LEFTOVERS 16 /* Songs removed per install; each may remove two files. */

static bool name_allowed(const char *name, char *error, uint64_t cap) {
    if (!oot_name_valid(name)) {
        snprintf(error, (size_t)cap,
                 "The sequence name must start with a lowercase letter and use only lowercase "
                 "letters, digits and _ (at most 48).");
        return false;
    }

    /* seq_N names belong to the extracted vanilla sequences. */
    if (!strncmp(name, "seq_", 4)) {
        snprintf(error, (size_t)cap, "Choose a sequence name that does not start with seq_.");
        return false;
    }

    return true;
}

static void *DME_CALL create_job(const void *instance, char *error, uint64_t cap) {
    const OotPluginUi *s = instance;

    if (!name_allowed(s->name, error, cap)) {
        return NULL;
    }

    ExportJob *j = calloc(1, sizeof(*j));

    if (!j) {
        snprintf(error, (size_t)cap, "Cannot allocate OoT export settings.");
        return NULL;
    }

    snprintf(j->name, sizeof(j->name), "%s", s->name);
    j->song = s->song > 0 && s->song <= (int)oot_song_count ? oot_songs[s->song - 1].id : NULL;
    j->loop_points = s->loop_points && s->has_loop_points;
    j->enlarge = s->memory == 0;
    OotRequest *q = &j->request;
    memcpy(q->instruments, s->melodic, sizeof(q->instruments));
    memcpy(q->drums, s->drums, sizeof(q->drums));
    q->font = oot_ui_font(s);
    q->font_index = 38;
    q->volume = (uint32_t)s->volume;
    q->enlarge_memory = j->enlarge;
    q->loop = s->loop ? OOT_LOOP_SONG : OOT_LOOP_NONE;
    q->name = j->name;
    return j;
}

/* Copies the arrangement with colored ports moved to their channels and applies loop points. */
static int32_t prepare(ExportJob *j, const DmeContext *c, char *error, uint64_t cap) {
    if (!c || c->event_count > 2000000 || (!c->events && c->event_count) || !c->ppq) {
        snprintf(error, (size_t)cap, "Invalid OoT source timeline.");
        return DME_INVALID;
    }

    free(j->events);
    j->events = calloc((size_t)c->event_count + 1, sizeof(*j->events));
    OotPortLayout *layout = calloc(1, sizeof(*layout));

    if (!j->events || !layout ||
        !oot_port_layout(c->events, c->event_count, c->percussion_channels, layout)) {
        snprintf(error, (size_t)cap, "Cannot allocate the OoT timeline.");
        free(layout);
        return DME_NOMEM;
    }

    for (uint64_t i = 0; i < c->event_count; i++) {
        DmeEvent e = c->events[i];

        if (e.status >= MIDI_STATUS && e.status < MIDI_SYSTEM && e.port < OOT_PORTS) {
            e.status =
                (e.status & MIDI_STATUS_MASK) | layout->out[e.status & MIDI_CHANNEL_MASK][e.port];
        }

        j->events[i] = e;
    }

    snprintf(j->port_notice, sizeof(j->port_notice), "%s",
             layout->interference ? layout->notice : "");
    free(layout);
    OotRequest *q = &j->request;
    q->events = j->events;
    q->event_count = c->event_count;
    q->duration = c->duration;
    q->ppq = c->ppq;

    if (j->loop_points && c->loop_points) {
        uint64_t start = c->loop_points & DME_LOOP_START ? c->loop_start : 0;
        uint64_t end = c->loop_points & DME_LOOP_END ? c->loop_end : 0;

        if (end >= c->duration) {
            end = 0;
        }

        if (start >= (end ? end : c->duration)) {
            snprintf(error, (size_t)cap,
                     "M64 - Loop Start must come before M64 - Loop End and the end of the song.");
            return DME_INVALID;
        }

        q->loop = OOT_LOOP_REGION;
        q->loop_start = start;
        q->loop_end = end;
    }

    return DME_OK;
}

static void report(const DmeHost *host, const OotResult *result) {
    for (size_t i = 0; i < result->diagnostic_count; i++) {
        const OotDiagnostic *d = &result->diagnostics[i];
        uint32_t flags = DME_DISPLAY | (d->severity == DME_WARNING ? DME_POPUP : 0);
        host->diagnostic(host->user, d->severity, flags, d->text);
    }
}

static int32_t convert(ExportJob *j, const DmeContext *c, const DmeHost *host, char *error,
                       uint64_t cap, OotResult *result) {
    *result = (OotResult){0};
    int32_t status = prepare(j, c, error, cap);

    if (status != DME_OK) {
        return status;
    }

    if (*j->port_notice) {
        host->diagnostic(host->user, DME_WARNING, DME_DISPLAY | DME_POPUP, j->port_notice);
    }

    status = oot_convert(&j->request, host, false, result, error, (size_t)cap);
    report(host, result);

    if (host->cancelled(host->user)) {
        return DME_CANCELLED;
    }

    if (status != DME_OK && !*error) {
        snprintf(error, (size_t)cap, "OoT conversion failed.");
    }

    return status;
}

static int32_t DME_CALL run(void *job, const DmeContext *c, const DmeHost *host, char *error,
                            uint64_t cap) {
    ExportJob *j = job;
    OotResult result;
    int32_t status = convert(j, c, host, error, cap, &result);
    bool font = j->request.font == OOT_CUSTOM_FONT;
    char name[128];
    bool ok = status == DME_OK;

    if (ok) {
        snprintf(name, sizeof(name), "%s.seq", j->name);
        ok = host->artifact(host->user, name, (const uint8_t *)result.sequence.data,
                            result.sequence.size);
    }

    if (ok && font) {
        snprintf(name, sizeof(name), "Soundfont_%s.xml", j->name);
        ok = host->artifact(host->user, name, (const uint8_t *)result.soundfont.data,
                            result.soundfont.size);
    }

    if (ok) {
        char *guide = oot_install_guide(j->name, j->song, font, j->request.font_index,
                                        result.sequence_bytes, result.soundfont_bytes);
        ok = guide &&
             host->artifact(host->user, "INSTALL.txt", (const uint8_t *)guide, strlen(guide));
        free(guide);
    }

    oot_result_free(&result);

    if (status == DME_OK && !ok) {
        status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    host->progress(host->user, 100);
    return status;
}

static bool queue_text(const DmeHost *host, const char *path, OotText *text, uint32_t flags,
                       char *error, uint64_t cap) {
    bool ok = !text->failed &&
              host->install_file(host->user, path, (const uint8_t *)text->data, text->size, flags);
    oot_text_free(text);

    if (!ok) {
        snprintf(error, (size_t)cap, "Cannot queue %s for writing.", path);
    }

    return ok;
}

static bool generated(const uint8_t *data, uint64_t size) {
    const char *marker = "generated by duckmidi";
    size_t n = strlen(marker);
    uint64_t limit = size < 400 ? size : 400;

    for (uint64_t i = 0; i + n <= limit; i++) {
        if (!memcmp(data + i, marker, n)) {
            return true;
        }
    }

    return false;
}

/* A file at a duckmidi path must be one duckmidi wrote, so a user's own work is never replaced. */
static bool ours_or_missing(const DmeHost *host, const char *path, const uint8_t **data,
                            uint64_t *size, char *error, uint64_t cap) {
    *data = NULL;
    *size = 0;

    if (!host->read_file(host->user, path, data, size) || generated(*data, *size)) {
        return true;
    }

    snprintf(error, (size_t)cap,
             "%s already exists and was not made by duckmidi; choose another "
             "sequence name.",
             path);
    return false;
}

static int existing_font_index(const uint8_t *data, uint64_t size) {
    const char *key = "Index=\"";

    for (uint64_t i = 0; data && i + strlen(key) < size; i++) {
        if (!memcmp(data + i, key, strlen(key))) {
            return atoi((const char *)data + i + strlen(key));
        }
    }

    return -1;
}

static void song_paths(const char *name, char *seq, char *font, size_t capacity) {
    snprintf(seq, capacity, "assets/audio/sequences/%s.seq", name);
    snprintf(font, capacity, "assets/audio/soundfonts/Soundfont_%s.xml", name);
}

/* Songs earlier exports installed that the game no longer plays once this one is installed:
   their sequence is not in the sequence table, and every file of theirs that exists was made by
   duckmidi. Their files are removed so their soundfont slots are free. */
static bool remove_leftovers(ExportJob *j, const DmeHost *host, const uint8_t *table,
                             uint64_t table_size, const uint8_t *spec, uint64_t spec_size,
                             const char *id, char leftovers[][OOT_SONG_NAME_CAPACITY],
                             size_t *count, char *error, uint64_t cap) {
    char songs[MAX_LEFTOVERS * 2][OOT_SONG_NAME_CAPACITY];
    size_t found = oot_custom_songs(spec, spec_size, songs, MAX_LEFTOVERS * 2);
    *count = 0;

    for (size_t i = 0; i < found && *count < MAX_LEFTOVERS; i++) {
        if (oot_song_kept(table, table_size, songs[i], j->name, id)) {
            continue;
        }

        char paths[2][160];
        song_paths(songs[i], paths[0], paths[1], sizeof(paths[0]));
        bool exists[2], ours = true;

        for (int k = 0; k < 2; k++) {
            const uint8_t *data;
            uint64_t size;
            exists[k] = host->read_file(host->user, paths[k], &data, &size);
            ours &= !exists[k] || generated(data, size);
        }

        if (!ours || (!exists[0] && !exists[1])) {
            continue;
        }

        for (int k = 0; k < 2; k++) {
            if (exists[k] &&
                !host->install_file(host->user, paths[k], NULL, 0, DME_INSTALL_REMOVE)) {
                snprintf(error, (size_t)cap, "Cannot queue %s for removal.", paths[k]);
                return false;
            }
        }

        snprintf(leftovers[(*count)++], OOT_SONG_NAME_CAPACITY, "%s", songs[i]);
    }

    return true;
}

/* A copy of a duckmidi soundfont with another Index. */
static bool set_font_index(const uint8_t *data, uint64_t size, int index, OotText *out) {
    const char *key = "Index=\"";
    size_t n = strlen(key);
    *out = (OotText){0};

    for (uint64_t i = 0; i + n <= size; i++) {
        if (memcmp(data + i, key, n)) {
            continue;
        }

        uint64_t end = i + n;

        while (end < size && data[end] >= '0' && data[end] <= '9') {
            end++;
        }

        oot_text_append(out, (const char *)data, (size_t)(i + n));
        oot_text_add(out, "%d", index);
        oot_text_append(out, (const char *)data + end, (size_t)(size - end));
        return !out->failed;
    }

    return false;
}

/* OoT numbers soundfonts without gaps. The vanilla ones come first; duckmidi's follow in spec
   order, renumbered when earlier ones were removed. When a soundfont duckmidi did not make is
   present, nothing is renumbered and the new one takes the next place in the spec. Returns the
   new soundfont's index, or -1. */
static int number_fonts(ExportJob *j, const DmeHost *host, const uint8_t *spec, uint64_t spec_size,
                        int existing, char *error, uint64_t cap) {
    char fonts[MAX_FONTS][OOT_SONG_NAME_CAPACITY];
    unsigned vanilla;
    size_t count = oot_custom_fonts(spec, spec_size, fonts, MAX_FONTS, &vanilla);
    const uint8_t *data[MAX_FONTS] = {0};
    uint64_t sizes[MAX_FONTS] = {0};

    for (size_t i = 0; i < count; i++) {
        if (!strcmp(fonts[i], j->name)) {
            continue;
        }

        char seq[160], path[160];
        song_paths(fonts[i], seq, path, sizeof(path));

        if (!host->read_file(host->user, path, &data[i], &sizes[i]) ||
            !generated(data[i], sizes[i])) {
            return existing >= 0 ? existing : oot_next_font_index(spec, spec_size, j->name);
        }
    }

    int index = existing >= 0 ? existing : (int)(vanilla + count);

    for (size_t i = 0; i < count; i++) {
        int want = (int)(vanilla + i);

        if (!strcmp(fonts[i], j->name)) {
            index = want;
            continue;
        }

        if (existing_font_index(data[i], sizes[i]) == want) {
            continue;
        }

        char seq[160], path[160];
        OotText text;
        song_paths(fonts[i], seq, path, sizeof(path));

        if (!set_font_index(data[i], sizes[i], want, &text)) {
            snprintf(error, (size_t)cap, "%s has no soundfont Index.", path);
            return -1;
        }

        if (!queue_text(host, path, &text, 0, error, cap)) {
            return -1;
        }
    }

    return index;
}

/* Raises the audio caches and heap when the song needs more than the smallest scene has. */
static bool install_memory(const DmeHost *host, uint32_t sequence, uint32_t font, char *error,
                           uint64_t cap) {
    const uint8_t *data;
    uint64_t size;

    if (!host->read_file(host->user, OOT_SESSION_CONFIG, &data, &size)) {
        snprintf(error, (size_t)cap, "Cannot read %s in the chosen folder.", OOT_SESSION_CONFIG);
        return false;
    }

    OotText text;
    uint32_t growth;

    if (!oot_patch_session_config(data, size, sequence, font, &text, &growth, error, (size_t)cap)) {
        return false;
    }

    if (!growth) {
        oot_text_free(&text);
        return true;
    }

    if (!queue_text(host, OOT_SESSION_CONFIG, &text, DME_INSTALL_BACKUP, error, cap)) {
        return false;
    }

    const char *heap_files[] = {OOT_AUDIO_HEAP_C, OOT_BUFFERS_H};

    for (int i = 0; i < 2; i++) {
        if (!host->read_file(host->user, heap_files[i], &data, &size)) {
            snprintf(error, (size_t)cap, "Cannot read %s in the chosen folder.", heap_files[i]);
            return false;
        }

        if (!oot_patch_audio_heap(data, size, growth, &text, error, (size_t)cap) ||
            !queue_text(host, heap_files[i], &text, DME_INSTALL_BACKUP, error, cap)) {
            return false;
        }
    }

    char message[256];
    snprintf(message, sizeof(message),
             "The audio caches were raised to fit the song; the audio heap grows by %u bytes.",
             growth);
    host->diagnostic(host->user, DME_INFO, DME_DISPLAY, message);
    return true;
}

/* Installs into a zeldaret/oot checkout: the sequence and its soundfont are added, the song is
   registered in the sequence table and spec, and the audio memory grows when needed. */
static int32_t DME_CALL install(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                uint64_t cap) {
    ExportJob *j = job;
    const uint8_t *table, *spec, *old_seq, *old_font;
    uint64_t table_size, spec_size, old_seq_size, old_font_size;
    bool font = j->request.font == OOT_CUSTOM_FONT;

    if (!host->read_file(host->user, OOT_SEQUENCE_TABLE, &table, &table_size) ||
        !host->read_file(host->user, OOT_SPEC, &spec, &spec_size)) {
        snprintf(error, (size_t)cap,
                 "Choose the root folder of a zeldaret/oot checkout; it contains %s and %s.",
                 OOT_SEQUENCE_TABLE, OOT_SPEC);
        return DME_INVALID;
    }

    char seq_path[160], font_path[160];
    snprintf(seq_path, sizeof(seq_path), "assets/audio/sequences/%s.seq", j->name);
    snprintf(font_path, sizeof(font_path), "assets/audio/soundfonts/Soundfont_%s.xml", j->name);

    if (!ours_or_missing(host, seq_path, &old_seq, &old_seq_size, error, cap) ||
        (font && !ours_or_missing(host, font_path, &old_font, &old_font_size, error, cap))) {
        return DME_INVALID;
    }

    /* Songs this install leaves out of the game are removed with their spec lines. */
    char id[96], leftovers[MAX_LEFTOVERS][OOT_SONG_NAME_CAPACITY];
    size_t leftover_count;
    oot_song_id(j->name, j->song, id, sizeof(id));

    if (!remove_leftovers(j, host, table, table_size, spec, spec_size, id, leftovers,
                          &leftover_count, error, cap)) {
        return DME_FAILED;
    }

    oot_text_free(&j->spec_cleaned);

    if (!oot_spec_without(spec, spec_size, leftovers, leftover_count, &j->spec_cleaned)) {
        snprintf(error, (size_t)cap, "Cannot prepare %s.", OOT_SPEC);
        return DME_FAILED;
    }

    spec = (const uint8_t *)j->spec_cleaned.data;
    spec_size = j->spec_cleaned.size;
    int index = number_fonts(j, host, spec, spec_size,
                             font ? existing_font_index(old_font, old_font_size) : -1, error, cap);

    if (index < 0) {
        if (!*error) {
            snprintf(error, (size_t)cap, "%s has no Audiobank segment.", OOT_SPEC);
        }

        return DME_INVALID;
    }

    if (leftover_count) {
        char message[1024];
        size_t used =
            (size_t)snprintf(message, sizeof(message),
                             "Removed songs the game no longer plays, freeing their soundfonts:");

        for (size_t i = 0; i < leftover_count && used < sizeof(message); i++) {
            used += (size_t)snprintf(message + used, sizeof(message) - used, "%s %s", i ? "," : "",
                                     leftovers[i]);
        }

        host->diagnostic(host->user, DME_INFO, DME_DISPLAY, message);
    }

    if (font) {
        j->request.font_index = index;

        if (j->request.font_index >= MAX_FONTS) {
            snprintf(error, (size_t)cap,
                     "OoT has room for %d soundfonts; choose a vanilla soundfont instead.",
                     MAX_FONTS);
            return DME_INVALID;
        }
    }

    OotResult result;
    int32_t status = convert(j, c, host, error, cap, &result);

    if (status != DME_OK) {
        oot_result_free(&result);
        return status;
    }

    OotText table_text = {0}, spec_text = {0};
    char notice[256];
    bool ok = oot_patch_sequence_table(table, table_size, j->name, j->song, result.sequence_bytes,
                                       &table_text, notice, sizeof(notice), error, (size_t)cap) &&
              oot_patch_spec(spec, spec_size, j->name, font, &spec_text, error, (size_t)cap);

    if (ok && *notice) {
        host->diagnostic(host->user, DME_INFO, DME_DISPLAY, notice);
    }

    uint32_t need_sequence = result.sequence_bytes > OOT_SEQ_CACHE_MIN
                                 ? (uint32_t)((result.sequence_bytes + 15) & ~15ull)
                                 : 0;
    uint32_t need_font = font && result.soundfont_bytes > OOT_FONT_CACHE
                             ? (uint32_t)((result.soundfont_bytes + 15) & ~15ull)
                             : 0;

    /* With Stock, the conversion has already warned where the song does not fit. */
    if (ok && j->enlarge && (need_sequence || need_font)) {
        ok = install_memory(host, need_sequence, need_font, error, cap);
    }

    ok = ok && host->install_file(host->user, seq_path, (const uint8_t *)result.sequence.data,
                                  result.sequence.size, 0);
    ok = ok &&
         (!font || host->install_file(host->user, font_path, (const uint8_t *)result.soundfont.data,
                                      result.soundfont.size, 0));
    ok = ok && queue_text(host, OOT_SEQUENCE_TABLE, &table_text, 0, error, cap);
    ok = ok && queue_text(host, OOT_SPEC, &spec_text, 0, error, cap);
    oot_text_free(&table_text);
    oot_text_free(&spec_text);
    oot_result_free(&result);

    if (!ok) {
        if (!*error) {
            snprintf(error, (size_t)cap, "Cannot prepare the repository files.");
        }

        return host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    host->progress(host->user, 100);
    return DME_OK;
}

static int32_t DME_CALL preflight(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                  uint64_t cap) {
    ExportJob *j = job;
    int32_t status = prepare(j, c, error, cap);

    if (status != DME_OK) {
        return status;
    }

    OotResult result;
    status = oot_convert(&j->request, host, true, &result, error, (size_t)cap);
    bool ok = status == DME_OK && oot_review_build(j, c, &result);
    oot_result_free(&result);

    if (host->cancelled(host->user)) {
        return DME_CANCELLED;
    }

    if (status != DME_OK) {
        return status;
    }

    if (!ok) {
        snprintf(error, (size_t)cap, "Cannot analyze the mapped sample ranges.");
        return DME_FAILED;
    }

    return DME_OK;
}

static void DME_CALL destroy_job(void *job) {
    ExportJob *j = job;
    oot_review_free(j);
    oot_text_free(&j->spec_cleaned);
    free(j->events);
    free(j);
}

static const DmeExporter exporter = {"org.duckmidi.oot",
                                     "OoT",
                                     "zip",
                                     "OoT sequence package",
                                     "Save OoT Package",
                                     "Ocarina of Time sequence and soundfont export for the "
                                     "zeldaret/oot decomp.",
                                     DME_ZIP,
                                     oot_ui_create,
                                     oot_ui_destroy,
                                     oot_ui_inspect,
                                     oot_ui_describe,
                                     oot_ui_set,
                                     oot_ui_reset,
                                     create_job,
                                     run,
                                     destroy_job,
                                     preflight,
                                     oot_review_describe,
                                     oot_review_set,
                                     oot_review_apply,
                                     "Export to Repository",
                                     install};

static const DmeExporter *DME_CALL get_exporter(uint32_t i) {
    return i ? NULL : &exporter;
}

static const DmePlugin plugin = {
    DME_ABI, sizeof(DmePlugin), "org.duckmidi.ootexport", "OoT exporter", 1, get_exporter};

DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &plugin : NULL;
}
