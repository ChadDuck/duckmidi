#ifndef M64EXPORT_H
#define M64EXPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#define M64_CALL __cdecl
#if defined(M64_BUILD_DLL)
#define M64_PUBLIC __declspec(dllexport)
#else
#define M64_PUBLIC
#endif
#else
#define M64_CALL
#define M64_PUBLIC __attribute__((visibility("default")))
#endif

/* One current ABI, delivered with its host. No editor, MIDI parser, or file I/O.
   Events use absolute PPQ ticks and stable array order at a shared tick.
   Status 0xff represents tempo, a is microseconds/quarter; 0xf0 is an
   unsupported source message (reported, not silently interpreted).
   Other statuses are ordinary MIDI channel messages, with a/b data values. */
typedef struct {
    uint64_t tick, source_id;
    uint32_t status, a, b;
} M64Event;

/* Tuning is the multiplier at MIDI C4, as used by the SM64 bank assembler.
   Overrides refer to samples in the existing "instruments" sample bank.
   Melodic notes retain standard MIDI pitch; drums use tuning directly. Known
   stock melodic families may select higher registers with proportional tuning
   to avoid US playback-rate clipping. Uncovered ranges emit diagnostic 59;
   register choices appear in the bank and manifest. */
typedef struct {
    const char *sample;
    double tuning;
    uint32_t release_rate, attack, decay, sustain;
} M64Instrument;

typedef struct {
    const M64Event *events;
    uint64_t event_count, duration;
    uint32_t ppq, target; /* target must be M64_TARGET_SM64_US */
    uint32_t loop;       /* 0 one shot, 1 restart the complete song */
    uint32_t voice_limit; /* expected game voice budget, 1..64; excess gates warn */
    const char *sequence_name, *bank_name; /* ASCII letters/digits/_/- only */
    const M64Instrument *programs; /* NULL = built-in GM map; otherwise 128 entries */
    const M64Instrument *drums;    /* NULL = built-in drum map; otherwise 128 entries */
    /* Optional 16*128 channel/program definitions. Overrides programs, allowing
       independent mappings for identical GM programs on different channels. */
    const M64Instrument *channel_programs;
} M64Request;

typedef struct {
    void *user;
    int32_t (M64_CALL *should_cancel)(void *user);
    void (M64_CALL *progress)(void *user, uint32_t stage, uint32_t percent);
} M64Hooks;

enum { M64_TARGET_SM64_US = 1 };
enum { M64_OK, M64_INVALID, M64_UNSUPPORTED, M64_LIMIT, M64_CANCELLED, M64_NOMEM };
enum { M64_INFO, M64_WARNING, M64_ERROR };
enum { M64_SEQUENCE, M64_BANK_JSON, M64_SEQUENCE_JSON, M64_MANIFEST, M64_ARTIFACT_COUNT };
typedef struct M64Result M64Result;
typedef struct {
    uint32_t channel,program,percussion,note_low,note_high,note_count;
    double demand,maximum_rate; /* C4-relative demand (drums: control multiplier). */
    char sample[97];
} M64RangeIssue;
typedef struct {
    uint32_t severity, code;
    uint64_t source_id, tick;
    int32_t channel;
    const char *message;
} M64DiagnosticView;
typedef struct {
    uint32_t kind;
    const uint8_t *data;
    uint64_t size;
} M64ArtifactView;

/* Input memory must remain valid until compile returns. Hooks run on that
   calling thread. Result and all returned views belong to this library.
   On failure a diagnostic result may be returned, but artifact_count is zero.
   No result survives result_free or unloading the library. Independent calls
   share no mutable state. No callbacks may re-enter the same compilation. */
typedef struct {
    int32_t (M64_CALL *compile)(const M64Request *, const M64Hooks *, M64Result **);
    uint64_t (M64_CALL *diagnostic_count)(const M64Result *);
    int32_t (M64_CALL *diagnostic_at)(const M64Result *, uint64_t, M64DiagnosticView *);
    uint64_t (M64_CALL *artifact_count)(const M64Result *);
    int32_t (M64_CALL *artifact_at)(const M64Result *, uint64_t, M64ArtifactView *);
    void (M64_CALL *result_free)(M64Result *);
    const M64Instrument *(M64_CALL *default_program)(uint32_t);
    const M64Instrument *(M64_CALL *default_drum)(uint32_t);
    /* Analysis produces diagnostics/range issues, with no output artifacts. */
    int32_t (M64_CALL *analyze)(const M64Request *,const M64Hooks *,M64Result **);
    uint64_t (M64_CALL *range_count)(const M64Result *);
    int32_t (M64_CALL *range_at)(const M64Result *,uint64_t,M64RangeIssue *);
    int32_t (M64_CALL *preset_supported)(const M64RangeIssue *,uint32_t preset);
} M64Api;

/* The single required entry point. Hosts never probe older interfaces. */
M64_PUBLIC const M64Api *M64_CALL m64_get_api_3(void);
#ifdef __cplusplus
}
#endif
#endif
