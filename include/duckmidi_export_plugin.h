#ifndef DUCKMIDI_EXPORT_PLUGIN_H
#define DUCKMIDI_EXPORT_PLUGIN_H
/* Current C ABI. UTF-8 strings; fixed-width numbers; no SDL/editor/UI types.
   Install native modules in plugins/ beside the desktop executable. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#define DME_CALL __cdecl
#if defined(DME_BUILD_PLUGIN)
#define DME_PUBLIC __declspec(dllexport)
#else
#define DME_PUBLIC
#endif
#else
#define DME_CALL
#define DME_PUBLIC __attribute__((visibility("default")))
#endif
#define DME_ABI 11u
/* Limits the host checks on what a plugin returns and plugins check on what the host passes; both
   sides size their tables by them. */
#define DME_PATH_LIMIT 8192   /* bytes in a path, locator or other long string */
#define DME_ROW_LIMIT 2048    /* rows in one review or mapping table */
#define DME_LABEL_LIMIT 512   /* bytes in a label or name */
#define DME_CHOICE_LIMIT 512  /* choices offered for one field or row */
#define DME_PRESET_LIMIT 8192 /* sample presets in one context */
#define DME_SAMPLES_TOOL_ID "org.duckmidi.samples"

enum { DME_OK, DME_INVALID, DME_CANCELLED, DME_FAILED, DME_NOMEM, DME_UNAVAILABLE };

enum { DME_INFO, DME_WARNING, DME_ERROR };

/* DISPLAY: summary; POPUP: completion warning. A POPUP warning with LIST is one item, written
   "Heading\nItem"; the host lists items under their headings. ALTERED marks a warning whose
   export had to change notes as a last resort to fit the target. */
enum { DME_DISPLAY = 1u, DME_POPUP = 2u, DME_LIST = 4u, DME_ALTERED = 8u };

enum { DME_SINGLE_FILE, DME_ZIP };

enum { DME_BOOL, DME_INT, DME_TEXT, DME_CHOICE, DME_MAPPING, DME_NOTICE, DME_ACTION };

enum { DME_LOOP_START = 1u, DME_LOOP_END = 2u }; /* DmeContext.loop_points */

typedef struct {
    uint32_t custom_tracking;
    double coarse, fine, tracking;          /* Semitones, cents, cents per MIDI key. */
    double cutoff, resonance, filter_depth; /* Hz (zero uses the default), dB, cents. */
    double vibrato_rate, vibrato_depth;     /* Hz and cents. */
    double chorus, reverb;                  /* Linear send amounts, zero to one. */
} DmeSampleToneParams;

typedef struct {
    uint64_t tick, source_id;
    uint32_t status, a, b, port;
    uint32_t flags;
} DmeEvent;

/* A controller inserted for an unset mixer value, rather than authored MIDI input. */
enum { DME_EVENT_DEFAULT_CONTROL = 1u };

typedef struct {
    const char *data;
    uint64_t length; /* UTF-8 byte length excluding any terminator. */
} DmeString;

enum { DME_SAMPLE_NO_LOOP, DME_SAMPLE_LOOP_CONTINUOUS, DME_SAMPLE_LOOP_SUSTAIN };

enum { DME_SAMPLE_PERCUSSION = 1u, DME_SAMPLE_MISSING = 2u };

typedef struct {
    uint32_t size, root, key_lo, key_hi, velocity_lo, velocity_hi, cut_group, cut_by, loop_mode;
    uint64_t loop_start, loop_end; /* Half-open frames at the asset rate. */
    double cents, gain_db, pan; /* Pan is -1 to 1. Cents measure source pitch relative to root. */
    double delay, attack, hold, decay, sustain, release; /* Seconds and linear sustain level. */
    DmeSampleToneParams tone;
    uint64_t playback_start, playback_end; /* Half-open; zero end uses the asset end. */
    uint32_t playback_set; /* Keep manually adjusted boundary bars visible, even at full range. */
} DmeSampleZoneParams;

typedef struct {
    uint32_t size, rate, channels;
    uint64_t frames;
    DmeString id;
} DmeSampleAsset;

typedef struct {
    uint32_t size, asset; /* Zero-based context asset index. */
    DmeSampleZoneParams params;
} DmeSampleZone;

typedef struct {
    uint32_t size, generation, flags, first_zone, zone_count;
    DmeString locator, set_id, preset_id, name;
} DmeSamplePreset;

typedef struct {
    uint32_t bank, program, flags;
    uint32_t sample_preset; /* One-based context preset index. Zero uses the original patch. */
    uint32_t note_velocity;
    uint64_t note_source_id; /* Source ID of the note-on, including on its paired note-off. */
} DmeEffectivePatch;

typedef struct {
    uint32_t size, generation, available, first_preset, preset_count;
    uint64_t modified;
    DmeString locator, id, name, error;
} DmeSampleCatalogSet;

typedef struct {
    uint32_t size, set, flags, suggested_program, first_zone, zone_count;
    DmeString id, name;
} DmeSampleCatalogPreset;

/* Metadata only; immutable for an inspection/descriptor lifetime. PCM is frozen separately. */
typedef struct {
    uint32_t size, set_count, preset_count, zone_count;
    const DmeSampleCatalogSet *sets;
    const DmeSampleCatalogPreset *presets;
    const DmeSampleZoneParams *zones;
} DmeSampleCatalog;

typedef struct {
    uint32_t size, generation, flags;
    DmeString locator, set_id, preset_id;
} DmeSampleSelection;

typedef struct {
    uint32_t size, preset_count, zone_count, asset_count;
    uint64_t handle;
    const DmeSamplePreset *presets;
    const DmeSampleZone *zones;
    const DmeSampleAsset *assets;
} DmeFrozenSample;

typedef struct {
    uint32_t channel, port, bank, program, drum_key, flags;
} DmeMappingSource;

typedef struct {
    DmeString id, label, path;
} DmeSampleTarget;

enum { DME_SAMPLE_ADD, DME_SAMPLE_REPLACE };

typedef struct {
    uint32_t size, enabled, show_all, operation;
    uint32_t region_set, replace_zone; /* Replace: explicit preset-relative region when layered. */
    DmeSampleSelection sample;
    DmeString physical_target;
} DmeCustomSelection;

typedef struct {
    uint32_t size, available;
    DmeString unavailable_reason;
    DmeCustomSelection selection;
    const DmeSampleCatalog *catalog;
    const DmeSampleTarget *targets;
    uint32_t target_count;
} DmeCustomMapping;

/* Absolute PPQ ticks; stable order at a shared tick. 0xff = tempo in a
   (microseconds/quarter); 0xf0 = unsupported source message. Other messages
   use MIDI status/data bytes. Events already reflect routing and mute/solo. */
typedef struct {
    const DmeEvent *events;
    uint64_t event_count, duration, project_session;
    uint32_t ppq;
    const char *project_name, *source_name;
    const char *program_names[128], *drum_names[128];
    /* Loop points marked in the arrangement, in the same ticks as events. */
    uint64_t loop_start, loop_end;
    uint32_t loop_points; /* DME_LOOP_START and/or DME_LOOP_END */
    /* Bit c set: MIDI channel c + 1 is a percussion part. Channel 10 usually is; imported GS, XG
       or GM2 files can add others or make channel 10 melodic. */
    uint32_t percussion_channels;
    uint64_t project_revision;
    /* Optional array parallel to events. Identity is resolved on the original route at note-on
       and follows that note through remapping and release. A missing sample preserves bank and
       program and sets DME_SAMPLE_MISSING. */
    const DmeEffectivePatch *effective_patches;
    const DmeSamplePreset *sample_presets;
    const DmeSampleZone *sample_zones;
    const DmeSampleAsset *sample_assets;
    uint32_t sample_preset_count, sample_zone_count, sample_asset_count;
    const DmeSampleCatalog *library;
} DmeContext;

typedef struct {
    uint32_t type;
    int32_t integer;
    const char *text;
} DmeValue;

typedef struct {
    int32_t value;
    const char *label;
} DmeChoice;

typedef struct {
    const char *id, *label;
    int32_t value, default_value;
    /* Optional choices similar to default_value that cover the source's notes, best first;
       each value must appear in the parent field's choices. */
    const DmeChoice *similar;
    uint32_t similar_count;
    DmeMappingSource source;
    const DmeCustomMapping *custom;
} DmeMappingRow;

typedef struct {
    const char *id, *label, *help;
    uint32_t type;
    DmeValue value;
    int32_t minimum, maximum;
    uint32_t text_capacity;
    const DmeChoice *choices;
    uint32_t choice_count;
    const DmeMappingRow *rows;
    uint32_t row_count;
} DmeField;

typedef struct {
    const DmeField *fields;
    uint32_t count;
} DmeUi;

typedef struct {
    const char *id, *label, *description, *current_mapping;
    int32_t value, supported;
    const DmeChoice *recommended;
    uint32_t recommended_count;
    const DmeChoice *choices;
    uint32_t choice_count;
} DmeReviewRow;

typedef struct {
    const char *description;
    const DmeReviewRow *rows;
    uint32_t count;
} DmeReview;

typedef struct {
    void *user;
    int32_t(DME_CALL *cancelled)(void *user);
    void(DME_CALL *progress)(void *user, uint32_t percent);
    void(DME_CALL *diagnostic)(void *user, uint32_t severity, uint32_t flags, const char *message);
    /* Copies bytes before returning. Leaf names only; no paths or duplicates.
       Return zero on cancellation/failure. Never publish files yourself. */
    int32_t(DME_CALL *artifact)(void *user, const char *name, const uint8_t *data, uint64_t size);
    /* Worker only. Reads a bounded span of interleaved normalized native-rate PCM from the
       frozen asset view. Capacity is measured in float values. Returns a DME_* status. */
    int32_t(DME_CALL *read_sample)(void *user, uint32_t asset, uint64_t start_frame,
                                   uint64_t frames, float *pcm, uint64_t capacity);
    /* Worker preparation only. Copies/retains one selected saved preset without opening a UI.
       Successful views and their PCM stay immutable until the host destroys this export job,
       including across preflight/review/run. A changed selection generation fails explicitly.
       Tokens are job-local and cannot be used by another job or after destruction. */
    int32_t(DME_CALL *freeze_sample)(void *user, const DmeSampleSelection *, DmeFrozenSample *,
                                     char *error, uint64_t capacity);
    int32_t(DME_CALL *read_frozen_sample)(void *user, uint64_t handle, uint32_t asset,
                                          uint64_t start_frame, uint64_t frames, float *pcm,
                                          uint64_t capacity);
    DmeString plugins_directory;
} DmeHost;

struct DmeHistoryAdapter;

typedef struct {
    const char *id, *name, *extension, *filter_name, *save_label, *description;
    uint32_t container;
    void *(DME_CALL *create)(void);
    void(DME_CALL *destroy)(void *instance);
    /* Main thread. Context is borrowed for this call; copy anything retained.
       inspect refreshes source-dependent options, without changing the project. */
    int32_t(DME_CALL *inspect)(void *instance, const DmeContext *, char *error, uint64_t capacity);
    int32_t(DME_CALL *describe)(void *instance, DmeUi *ui);
    /* Views remain valid through setters until the next describe/inspect/reset.
       IDs identify fields or mapping rows; ACTION is a button activation. */
    int32_t(DME_CALL *set)(void *instance, const char *id, const DmeValue *, char *error,
                           uint64_t capacity);
    void(DME_CALL *reset)(void *instance);
    /* Main thread: freeze settings. Job owns its copies; later UI/project edits
       cannot affect it. NULL means failure, with a readable error. */
    void *(DME_CALL *create_job)(const void *instance, char *error, uint64_t capacity);
    /* Worker thread. Input/host borrowed until return. Artifact/diagnostic
       callbacks run synchronously on this worker. No callbacks after return. */
    int32_t(DME_CALL *run)(void *job, const DmeContext *, const DmeHost *, char *error,
                           uint64_t capacity);
    /* Main thread, after the worker has joined, including on cancellation. */
    void(DME_CALL *destroy_job)(void *job);
    /* Worker preflight creates no artifacts. Main-thread review views and draft
       setters remain job-owned. Apply commits only after explicit user choice;
       exporting unchanged must retain the original frozen settings. */
    int32_t(DME_CALL *preflight)(void *job, const DmeContext *, const DmeHost *, char *error,
                                 uint64_t capacity);
    int32_t(DME_CALL *review)(void *job, DmeReview *);
    int32_t(DME_CALL *review_set)(void *job, const char *id, int32_t value, char *error,
                                  uint64_t capacity);
    int32_t(DME_CALL *review_apply)(void *instance, void *job, char *error, uint64_t capacity);
    /* Optional folder install, such as a game source checkout. install_label names the button;
       history returns the repository adapter for DME_REPOSITORY_API (see
       duckmidi_repository_plugin.h). Both are NULL when unsupported. */
    const char *install_label;
    /* Optional typed custom mapping setter; IDs identify source rows, not list positions. */
    int32_t(DME_CALL *set_custom)(void *instance, const char *id, const DmeCustomSelection *,
                                  char *error, uint64_t capacity);
    const struct DmeHistoryAdapter *(DME_CALL *history)(uint32_t version);
} DmeExporter;

typedef struct {
    uint32_t size;
    void *user;
    /* Main thread only. The host opens or focuses the existing tool instance. */
    int32_t(DME_CALL *request_tool)(void *user, const char *id, uint64_t length);
    DmeString plugins_directory;
} DmeMainHost;

typedef struct {
    uint32_t size;
    const char *id, *name;
    void *(DME_CALL *create)(void);
    void(DME_CALL *destroy)(void *instance);
    int32_t(DME_CALL *open)(void *instance);
    /* Returns a tool-specific immutable function table, or NULL for an unsupported version. */
    const void *(DME_CALL *interface_at)(uint32_t version);
} DmeTool;

typedef struct {
    uint32_t abi, size;
    const char *id, *name;
    uint32_t exporter_count;
    const DmeExporter *(DME_CALL *exporter)(uint32_t index);
    uint32_t tool_count;
    const DmeTool *(DME_CALL *tool)(uint32_t index);
    /* Optional main-thread binding after registration. NULL detaches before unload. */
    void(DME_CALL *bind_main)(const DmeMainHost *host);
} DmePlugin;

/* Exactly one entry point, no legacy probes. Return NULL for an unsupported ABI.
   Descriptors/function tables live until module unload. Instance/job memory is
   freed only by its module. Different instances/jobs share no mutable state.
   Native plugins run in-process and must be trusted; this is not a sandbox. */
typedef const DmePlugin *(DME_CALL *DmePluginEntry)(uint32_t abi);
DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi);
#ifdef __cplusplus
}
#endif
#endif
