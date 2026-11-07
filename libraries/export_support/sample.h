#ifndef DES_SAMPLE_H
#define DES_SAMPLE_H

#include "mapping.h"

enum { ES_NATIVE_SM64, ES_NATIVE_OOT, ES_NATIVE_REGIONS = 256 };

/* Categories of approximation, omitted settings and fallback needed for native export. */
enum {
    ES_FIX_PAN,
    ES_FIX_CHOKE,
    ES_FIX_SUSTAIN_LOOP,
    ES_FIX_FILTER,
    ES_FIX_FILTER_ENVELOPE,
    ES_FIX_CHORUS,
    ES_FIX_CHORUS_SHORT, /* Chorus left out of a loop shorter than its delay. */
    ES_FIX_TRACKING,
    ES_FIX_GAIN,         /* Gain lowered so the native PCM does not clip. */
    ES_FIX_TIMING,       /* Envelope times or levels clamped to native bounds. */
    ES_FIX_RELEASE,      /* The native release differs noticeably from the sample's. */
    ES_FIX_LENGTH,       /* Playback range truncated to the native frame bound. */
    ES_FIX_FORMAT,       /* Extra channels downmixed or a high rate resampled. */
    ES_FIX_REPLACE_ZONE, /* A layered Replace had no region chosen. */
    ES_FIX_REPLACE_ADD,  /* Replace could not apply, so the sample was added instead. */
    ES_FIX_ORDINARY,     /* The row uses its ordinary game instrument. */
    ES_FIX_IGNORED,      /* Custom choices do not apply to this target. */
    ES_FIX_DRUMS,        /* Custom drum layers past the native drum limit are omitted. */
    ES_FIX_COUNT
};

typedef struct {
    uint32_t count;
    char first[96]; /* First affected preset name. */
} EsNativeFix;

typedef struct {
    char name[128];
    char add_name[128];    /* Stable generated name if a physical Replace becomes an Add. */
    char preset[96];       /* Preset name for diagnostics. */
    uint16_t choice;       /* Shared saved-preset selection, used for whole-row fallbacks. */
    uint16_t preset_scope; /* Saved preset identity, independent of Add/Replace targets. */
    uint32_t fixes;        /* 1 << ES_FIX_* recorded after preparation, such as by resampling. */
    bool percussion;       /* From a percussion preset: 0% key tracking unless customized. */
    char *target;
    EsFile audio;
    DmeSampleZoneParams params;
    uint32_t rate, frames, loop_start, loop_end;
    uint32_t source_rate, source_channels;
    double tuning;           /* Playback rate at MIDI C4, before engine key-origin adjustment. */
    double reference_tuning; /* Initialized provider calibration for composed Replace banks. */
    double release_seconds;
    uint16_t release, point_count;
    int16_t points[8][2];
} EsNativeRegion;

typedef struct {
    uint32_t first;
    uint16_t count, choice;
    bool custom;
} EsNativeNote;

typedef struct {
    EsNativeRegion *regions;
    size_t count, capacity;
    EsNativeNote *notes; /* Parallel to the original source events. */
    uint16_t *layers;
    size_t layer_count, layer_capacity;
    uint64_t event_count;
    EsNativeFix fixes[ES_FIX_COUNT];
} EsNativeSamples;

typedef struct {
    uint64_t start, *end;
    uint16_t channel, region;
} EsNativeChokeNote;

/* Sorted by start; cuts earlier matching voices in the same source channel and saved preset.
   Native release tails remain audible. False reports cancellation or allocation failure. */
DES_API bool es_native_has_choke(const EsNativeSamples *);
DES_API bool es_native_choke(const EsNativeSamples *, const EsNativeChokeNote *, size_t,
                             const DmeHost *);

/* Channel effects from preset reverb and vibrato, as native command bytes. */
enum { ES_FX_REVERB, ES_FX_RATE, ES_FX_DEPTH, ES_FX_KINDS };

typedef struct {
    uint64_t start, end;
    uint32_t channel;
    int32_t region; /* -1 for an ordinary instrument note. */
} EsNativeFxNote;

typedef struct {
    uint64_t tick;
    uint32_t channel;
    uint8_t kind, value;
} EsNativeFxChange;

/* Walks each channel's notes in start order and returns the channel effect changes, in tick
   order, that give each note its preset's reverb and vibrato. Channels start at zero reverb and
   depth and at `initial_rate`, the rate byte the converter writes at channel start. Ordinary
   notes return reverb and depth to zero. midi[kind] has a bit for each channel whose MIDI controls
   that kind (CC91 for reverb, CC1 for depth), which then keeps its MIDI value; rate has no MIDI
   controller. Only channels with a custom note setting a value change at all. `shared` gets a bit
   for each channel where notes that overlap wanted different values; the latest note's value
   applies. The caller frees `changes`. False reports allocation failure. */
DES_API bool es_native_channel_fx(const EsNativeSamples *, const EsNativeFxNote *, size_t,
                                  uint32_t native, bool hacker, unsigned initial_rate,
                                  const uint16_t midi[ES_FX_KINDS], EsNativeFxChange **changes,
                                  size_t *change_count, uint16_t *shared);

/* Bakes the zone's static low-pass filter and chorus into mono PCM at `rate`, in place, as
   duckmidi plays them at the root key. Looped audio stays seamless: the loop body is the
   settled effect output running around the loop. `loop_end` 0 means unlooped. Adds ES_FIX_*
   bits to `fixes`. False reports allocation failure. */
DES_API bool es_native_bake(float *, uint32_t frames, uint32_t rate, uint32_t loop_start,
                            uint32_t loop_end, const DmeSampleToneParams *, uint32_t *fixes);

enum { ES_RESAMPLE_OK, ES_RESAMPLE_LIMIT, ES_RESAMPLE_FAILED };

/* Worker-only, immutable after success. Free on every exit, including preparation failure. */
DES_API bool es_native_samples(EsNativeSamples *, const EsMappingJob *, uint32_t target,
                               uint32_t native, const DmeContext *, const DmeHost *, char *,
                               size_t);
DES_API void es_native_samples_free(EsNativeSamples *);
/* Records one change for the export warning. Safe before preparation. */
DES_API void es_native_fix(EsNativeSamples *, unsigned kind, const char *preset);
/* Turns a Replace region into an Add region that keeps its derived name. */
DES_API void es_native_add_instead(EsNativeSamples *, EsNativeRegion *);
/* Removes every region of this selection and restores its notes' ordinary mappings. */
DES_API void es_native_fallback(EsNativeSamples *, size_t region, unsigned kind);

enum { ES_FIT_DONE, ES_FIT_RETRY, ES_FIT_FAILED };

/* Applies one trial conversion's per-region results. A region played above the engine's rate
   limit is resampled lower (a Replace is added instead first); a selection that cannot fit, by
   rate or by bank capacity, uses its ordinary mapping. `other_bank` says whether Replaces use a
   non-default sample bank; it is cleared when any change forces the default bank. Returns
   ES_FIT_RETRY after a change, so the caller converts again; every retry lowers a rate or
   removes a selection. ES_FIT_FAILED reports cancellation or allocation failure. */
DES_API int es_native_fit(EsNativeSamples *, const double *overspeed, const bool *capacity,
                          uint32_t native, bool *other_bank, const DmeHost *, char *, size_t);
/* Whether es_native_resample can fit the tuning without reaching ES_RESAMPLE_LIMIT. */
DES_API bool es_native_resample_fits(const EsNativeRegion *, double);
/* Lowers the rate until the C4 tuning fits. ES_RESAMPLE_LIMIT leaves the region unchanged. */
DES_API int es_native_resample(EsNativeRegion *, double, uint32_t, const DmeHost *, char *, size_t);
DES_API void es_native_samples_report(const EsNativeSamples *, uint32_t, const DmeHost *);
/* Native 0-127 pan for the zone; 64 is centre. */
DES_API unsigned es_native_pan(const EsNativeRegion *);
/* Key whose pitch a drum entry for `key` plays, following the zone's key tracking the way
   duckmidi playback does. Each drum entry has its own tuning, so any tracking is exact. */
DES_API double es_native_drum_key(const EsNativeRegion *, unsigned key);

#endif
