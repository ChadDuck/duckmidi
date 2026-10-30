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
#define DME_ABI 3u
enum { DME_OK, DME_INVALID, DME_CANCELLED, DME_FAILED, DME_NOMEM };
enum { DME_INFO, DME_WARNING, DME_ERROR };
enum { DME_DISPLAY = 1u, DME_POPUP = 2u }; /* Summary and explicit completion warning. */
enum { DME_SINGLE_FILE, DME_ZIP };
enum { DME_BOOL, DME_INT, DME_TEXT, DME_CHOICE, DME_MAPPING, DME_NOTICE, DME_ACTION };

typedef struct {
    uint64_t tick, source_id;
    uint32_t status, a, b, port;
} DmeEvent;
/* Absolute PPQ ticks; stable order at a shared tick. 0xff = tempo in a
   (microseconds/quarter); 0xf0 = unsupported source message. Other messages
   use MIDI status/data bytes. Events already reflect routing and mute/solo. */
typedef struct {
    const DmeEvent *events;
    uint64_t event_count, duration, project_session;
    uint32_t ppq;
    const char *project_name, *source_name;
    const char *program_names[128], *drum_names[128];
} DmeContext;
typedef struct { uint32_t type; int32_t integer; const char *text; } DmeValue;
typedef struct { int32_t value; const char *label; } DmeChoice;
typedef struct {
    const char *id, *label;
    int32_t value, default_value;
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
typedef struct { const DmeField *fields; uint32_t count; } DmeUi;
typedef struct {
    const char *id,*label,*description,*current_mapping;
    int32_t value,supported;
    const DmeChoice *recommended;
    uint32_t recommended_count;
    const DmeChoice *choices;
    uint32_t choice_count;
} DmeReviewRow;
typedef struct {const char *description;const DmeReviewRow *rows;uint32_t count;} DmeReview;
typedef struct {
    void *user;
    int32_t (DME_CALL *cancelled)(void *user);
    void (DME_CALL *progress)(void *user, uint32_t percent);
    void (DME_CALL *diagnostic)(void *user,uint32_t severity,uint32_t flags,const char *message);
    /* Copies bytes before returning. Leaf names only; no paths or duplicates.
       Return zero on cancellation/failure. Never publish files yourself. */
    int32_t (DME_CALL *artifact)(void *user,const char *name,const uint8_t *data,uint64_t size);
} DmeHost;
typedef struct {
    const char *id, *name, *extension, *filter_name, *save_label, *description;
    uint32_t container;
    void *(DME_CALL *create)(void);
    void (DME_CALL *destroy)(void *instance);
    /* Main thread. Context is borrowed for this call; copy anything retained.
       inspect refreshes source-dependent options, without changing the project. */
    int32_t (DME_CALL *inspect)(void *instance,const DmeContext *,char *error,uint64_t capacity);
    int32_t (DME_CALL *describe)(void *instance,DmeUi *ui);
    /* Views remain valid through setters until the next describe/inspect/reset.
       IDs identify fields or mapping rows; ACTION is a button activation. */
    int32_t (DME_CALL *set)(void *instance,const char *id,const DmeValue *,char *error,uint64_t capacity);
    void (DME_CALL *reset)(void *instance);
    /* Main thread: freeze settings. Job owns its copies; later UI/project edits
       cannot affect it. NULL means failure, with a readable error. */
    void *(DME_CALL *create_job)(const void *instance,char *error,uint64_t capacity);
    /* Worker thread. Input/host borrowed until return. Artifact/diagnostic
       callbacks run synchronously on this worker. No callbacks after return. */
    int32_t (DME_CALL *run)(void *job,const DmeContext *,const DmeHost *,char *error,uint64_t capacity);
    /* Main thread, after the worker has joined, including on cancellation. */
    void (DME_CALL *destroy_job)(void *job);
    /* Worker preflight creates no artifacts. Main-thread review views and draft
       setters remain job-owned. Apply commits only after explicit user choice;
       exporting unchanged must retain the original frozen settings. */
    int32_t (DME_CALL *preflight)(void *job,const DmeContext *,const DmeHost *,char *error,uint64_t capacity);
    int32_t (DME_CALL *review)(void *job,DmeReview *);
    int32_t (DME_CALL *review_set)(void *job,const char *id,int32_t value,char *error,uint64_t capacity);
    int32_t (DME_CALL *review_apply)(void *instance,void *job,char *error,uint64_t capacity);
} DmeExporter;
typedef struct {
    uint32_t abi, size;
    const char *id, *name;
    uint32_t exporter_count;
    const DmeExporter *(DME_CALL *exporter)(uint32_t index);
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
