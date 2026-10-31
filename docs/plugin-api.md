# Export plugin API reference

The public interface is [`duckmidi_export_plugin.h`](../include/duckmidi_export_plugin.h). The current registration ABI is **4**. A plugin is a native library placed in `plugins/` beside the application. Its formats appear in Export's dropdown, and duckmidi draws their declared settings and review controls. Plugins require no editor, SDL, or UI toolkit headers. [`duckmidi_midi.h`](../include/duckmidi_midi.h) names the MIDI status bytes, meta types and controller numbers used in events.

Plugins run inside the application; use trusted libraries that match its architecture and ABI. Settings last for the session. Restart the application after adding or replacing a plugin.

## Conventions and ownership

Use `DME_CALL` on callbacks and `DME_PUBLIC` on the exported entry point. Define `DME_BUILD_PLUGIN` when compiling a Windows DLL. Strings are UTF-8, terminated by a zero byte. Capacity arguments count bytes including that terminator. Fill an error buffer when returning failure. Do not throw exceptions across this C API.

Descriptors and function tables live until module unload. Instances and jobs are allocated and freed by their own plugin. Contexts and host callbacks are borrowed for a call; copy anything retained. Settings/review views are borrowed by the host, so their strings and arrays must stay valid for the documented lifecycle. Independent instances and jobs must not share mutable state.

| Constant | Meaning |
| --- | --- |
| `DME_ABI` | Required registration ABI, currently 4. |
| `DME_OK` | Operation succeeded. |
| `DME_INVALID` | Invalid input or setting. |
| `DME_CANCELLED` | Work was cancelled. |
| `DME_FAILED` | Operation failed; explain why. |
| `DME_NOMEM` | Allocation failed. |
| `DME_INFO`, `DME_WARNING`, `DME_ERROR` | Diagnostic severity. |
| `DME_DISPLAY` | Include a diagnostic in the export panel summary. |
| `DME_POPUP` | Include a warning in the completion popup after successful output. |
| `DME_SINGLE_FILE`, `DME_ZIP` | Save exactly one artifact, or package artifacts into ZIP. |
| `DME_LOOP_START`, `DME_LOOP_END` | Bits of `DmeContext.loop_points`: the arrangement marks a loop start, a loop end, or both. |
| `DME_INSTALL_BACKUP` | `install_file` flag: keep the original file even when the user turned backups off. |

## Public data types

### `DmeEvent`

A resolved playback event has absolute `tick`, stable `source_id`, MIDI `status`, data bytes `a` and `b`, and logical `port`. Events are already routed and filtered for mute/solo, and remain ordered at equal ticks. Status `0xff` denotes tempo with microseconds per quarter in `a`; `0xf0` denotes unsupported source messages. Ports are independent controller namespaces, not extra MIDI channel bits.

```c
DmeEvent note = { .tick=0, .source_id=1, .status=0x90,
                  .a=60, .b=100, .port=2 };
```

### `DmeContext`

The context borrows `events` and `event_count`, supplies `duration` in ticks, `ppq`, a `project_session` identifier, project/source names, and 128 program/drum name pointers. `loop_points` says which of `loop_start` and `loop_end` (in the same ticks as events) the arrangement marks with its **M64 - Loop Start** and **M64 - Loop End** events. Use the session identifier to reset source-specific settings when another project opens. A job receives the same frozen arrangement for preflight and conversion even if the live editor changes.

```c
double beats = (double)context->duration / context->ppq;
const char *name = context->program_names[40];
```

### `DmeValue`, `DmeChoice`, `DmeMappingRow`

`DmeValue` carries a setting's `type` and either `integer` or borrowed `text`. `DmeChoice` pairs an integer value with its label. A `DmeMappingRow` has a stable `id`, label, selected `value` and `default_value`; its choices come from the parent mapping field. IDs identify settings independently of visible labels.

```c
DmeValue enabled = { DME_BOOL, 1, NULL };
DmeChoice modes[] = { {0,"Normal"}, {1,"Alternate"} };
DmeMappingRow row = { "source.0", "Channel 1", 0, 0 };
```

### `DmeField`, `DmeUi`

A `DmeUi` views an array of fields. Each field has `id`, `label`, optional `help`, `type`, and the appropriate value/limits/choices/rows. Supported types are BOOL, INT, TEXT, CHOICE, MAPPING, NOTICE and ACTION. Text capacities include the zero terminator. Actions appear as buttons beside Save; a mapping field creates a source list and choice panel. Views remain valid through setters until the next describe, inspect, or reset callback.

```c
DmeField field = { .id="loop", .label="Loop", .type=DME_BOOL,
                   .value={DME_BOOL,1,NULL} };
DmeUi view = { &field, 1 }; /* Store field persistently in real plugin state. */
```

### `DmeReviewRow`, `DmeReview`

A review contains a description and affected-source rows. Each row supplies its stable ID, label, issue description, original `current_mapping`, selected draft `value`, `supported` flag, recommendations, and complete manual choices. This draft belongs to the job and must not modify frozen conversion settings until review_apply is called. Return zero rows to proceed without a review window.

```c
*review = (DmeReview){0}; /* No decision needed. */
/* Or: *review = (DmeReview){ state->description, state->rows, state->count }; */
```

### `DmeHost`, `DmeExporter`, `DmePlugin`

`DmeHost` carries callback functions and a `user` pointer passed back unchanged. `DmeExporter` describes one format and its lifecycle callbacks. `DmePlugin` registers a module with ABI, structure size, ID/name, exporter count, and lookup callback. A module can expose several exporters; each format receives its own settings instance. Every callback in the exporter table is required except the optional folder install, `install_label` and `install`, which are both set or both NULL.

```c
static const DmePlugin module = {
    DME_ABI, sizeof(DmePlugin), "example.module", "Example", 1, exporter_at
};
```

## Registration functions

### `duckmidi_export_plugin(uint32_t abi)`

This is the library's single exported entry point. Return the persistent plugin descriptor for `DME_ABI`, or NULL when the requested ABI is unsupported. The host also checks the descriptor's `abi` and exact `size`. `DmePluginEntry` is the function-pointer type used to load this symbol.

```c
DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &module : NULL;
}
```

### `DmePlugin.exporter(uint32_t index)`

Return the persistent exporter descriptor at an index below `exporter_count`. Return NULL for other indices. Give each module and exporter a unique stable ID; duplicate exporter IDs prevent that module from registering.

```c
static const DmeExporter *DME_CALL exporter_at(uint32_t index) {
    return index == 0 ? &format : NULL;
}
```

## Settings callbacks (main thread)

### `create(void)`

Allocate and initialize a format's settings, including defaults needed before an arrangement is inspected. Return the plugin-owned instance pointer, or NULL on allocation failure. The host immediately validates its settings declaration.

```c
static void *DME_CALL create(void) {
    Settings *s = calloc(1,sizeof(*s));
    if (s) s->loop=1;
    return s;
}
```

### `destroy(void *instance)`

Release every resource owned by a settings instance. The host calls this before unloading the module, including if registration fails after instance creation. Jobs own independent copies and use destroy_job for their cleanup.

```c
static void DME_CALL destroy(void *instance) { free(instance); }
```

### `inspect(instance, context, error, capacity)`

Refresh lightweight source-dependent settings, such as the list of instruments in use. The host resolves the arrangement on a worker before this callback and calls it again after edits. Copy names or other data needed after returning; do not retain the context pointer or alter the project.

```c
static int32_t DME_CALL inspect(void *s,const DmeContext *c,char *e,uint64_t n) {
    (void)e; (void)n;
    ((Settings *)s)->session=c->project_session;
    return DME_OK;
}
```

### `describe(instance, DmeUi *ui)`

Return a borrowed settings view backed by persistent instance storage. Rebuild field values from the current settings before assigning the view. The host renders and validates the declaration; no UI toolkit calls belong here.

```c
static int32_t DME_CALL describe(void *user,DmeUi *ui) {
    Settings *s=user;
    s->fields[0]=(DmeField){ .id="loop", .label="Loop", .type=DME_BOOL,
                            .value={DME_BOOL,s->loop,NULL} };
    *ui=(DmeUi){s->fields,1}; return DME_OK;
}
```

### `set(instance, id, value, error, capacity)`

Validate the field or mapping-row ID, value type and allowed value before changing settings. Copy accepted text into instance-owned storage. An ACTION value represents a button click. Reject unknown IDs and invalid values with an explanation rather than silently correcting them.

```c
if (!strcmp(id,"loop") && value->type==DME_BOOL &&
   value->integer>=0 && value->integer<=1) {
    s->loop=value->integer; return DME_OK;
}
snprintf(error,(size_t)capacity,"Invalid loop setting."); return DME_INVALID;
```

### `reset(instance)`

Restore the format's current defaults. Reset does not edit the arrangement or publish output. It can also be called by an action handled in set; keep its borrowed-view lifetime consistent with describe.

```c
static void DME_CALL reset(void *user) { ((Settings *)user)->loop=1; }
```

### `create_job(instance, error, capacity)`

Freeze settings into an independent job before worker processing begins. Deep copy any owned strings, arrays or buffers rather than copying their pointers. Return NULL with an explanation on failure. UI changes after this call must not change the job's original conversion settings.

```c
Job *job=calloc(1,sizeof(*job));
if (!job) { snprintf(error,(size_t)capacity,"Cannot allocate export job."); return NULL; }
job->loop=((const Settings *)instance)->loop;
return job;
```

## Preflight and review

### `preflight(job, context, host, error, capacity)` — worker

Analyze the frozen arrangement after the user chooses a destination, without returning artifacts or writing files. Prepare job-owned review data and poll cancellation during lengthy work. Return DME_OK even when a valid review is needed; use a failure status for unsupported input or analysis errors.

```c
if (host->cancelled(host->user)) return DME_CANCELLED;
job->issue_count=analyze_ranges(context,job);
return DME_OK;
```

### `review(job, DmeReview *view)` — main thread

Return the current job-owned review view, or an empty view when no decision is needed. The host shows affected sources, recommendations, manual choices and three export decisions. Keep borrowed arrays/strings valid until the next review callback and update the support flag when the draft selection changes.

```c
*view=(DmeReview){job->description,job->rows,job->issue_count};
return DME_OK;
```

### `review_set(job, id, value, error, capacity)` — main thread

Validate a review-row ID and manual/recommended choice, then update only the review draft. The original frozen settings remain available for Export Anyways. Unsupported manual choices may be accepted with `supported=0` so the user can make an informed decision.

```c
if (!strcmp(id,"source.0") && value>=0 && value<128) {
    job->draft_preset=value; return DME_OK;
}
snprintf(error,(size_t)capacity,"Unknown review choice."); return DME_INVALID;
```

### `review_apply(instance, job, error, capacity)` — main thread

Commit the reviewed draft only after Export with Changes is chosen. Update the job's conversion settings and, if appropriate, the settings instance. Cancel destroys the job without this call; Export Anyways runs the unchanged job. The host rejects a review if its source arrangement has changed.

```c
job->preset=job->draft_preset;
((Settings *)instance)->preset=job->preset;
return DME_OK;
```

## Conversion and cleanup

### `run(job, context, host, error, capacity)` — worker

Convert the frozen arrangement and return artifacts through the host. Poll cancellation, report progress and actionable diagnostics, and stop when an artifact callback returns zero. Callbacks are synchronous and must not be used after returning. The host handles packaging and atomic publication after a successful conversion; plugins never publish destination files themselves.

```c
if (host->cancelled(host->user)) return DME_CANCELLED;
const uint8_t bytes[]={'o','k','\n'};
if (!host->artifact(host->user,"song.txt",bytes,sizeof(bytes))) return DME_FAILED;
host->progress(host->user,100);
return DME_OK;
```

### `install(job, context, host, error, capacity)` — worker, optional

Install into a folder the user chooses, such as a game's source checkout. The host shows a button labeled `install_label` next to the save button, opens a folder picker, runs preflight and review as for a save, then calls `install` instead of `run`. Read existing files with `host->read_file` and queue whole replacement files with `host->install_file`; do not write files yourself.

The host writes queued files only after `install` returns `DME_OK`. Before replacing an existing file it keeps the original as `<file>.bkp` when the user ticked **Keep original files**, or when the file was queued with `DME_INSTALL_BACKUP`; an existing `.bkp` is never overwritten. The host records every change, so **Revert repository export** restores all originals and removes the backups it created.

```c
const uint8_t *table; uint64_t size;
if (!host->read_file(host->user,"sound/sequences.json",&table,&size)) return DME_INVALID;
if (!host->install_file(host->user,"sound/sequences.json",patched,patched_size,0)) return DME_FAILED;
```

### `destroy_job(job)` — main thread

Free all job-owned conversion buffers, frozen settings and review storage after the worker joins, whether it succeeded, failed, or was cancelled. The host also joins workers and destroys pending review jobs before unloading a module.

```c
static void DME_CALL destroy_job(void *user) {
    Job *job=user; free(job->buffer); free(job->rows); free(job);
}
```

## Host callbacks (worker thread)

### `cancelled(host->user)`

Return value zero means work may continue; nonzero means stop promptly and return DME_CANCELLED. Check at sensible intervals during analysis and conversion. The supplied user pointer must be passed unchanged.

```c
for(size_t i=0;i<count;i++) {
    if (!(i%1024) && host->cancelled(host->user)) return DME_CANCELLED;
    convert_one(i);
}
```

### `progress(host->user, percent)`

Report an integer percentage from 0 through 100. The host displays progress without letting the worker manipulate UI state. Report stages at useful intervals instead of calling once per byte or sample.

```c
host->progress(host->user,50);
```

### `diagnostic(host->user, severity, flags, message)`

Send a readable explanation at INFO, WARNING or ERROR severity. DISPLAY adds it to the inline summary. POPUP warning messages are consolidated into a scrollable warning after output is successfully saved; failed or cancelled exports do not produce a completion warning. Use ERROR to explain a failure, and return a failure status to prevent publication.

```c
host->diagnostic(host->user,DME_WARNING,DME_DISPLAY|DME_POPUP,
                 "Some notes exceed this target's playback range.");
```

### `artifact(host->user, name, data, size)`

Provide a unique leaf filename and bytes; the host copies the buffer before returning, so temporary conversion memory can then be reused. Return zero means failure or cancellation. Paths, duplicate names and excessive output are rejected; an ignored rejection still prevents publication. Preflight must not call this.

```c
if (!host->artifact(host->user,"bank.json",buffer,length)) return DME_FAILED;
free(buffer); /* Safe after the callback has copied it. */
```

### `read_file(host->user, path, &data, &size)` — install only

Read a file below the chosen folder. `path` is relative and uses `/`; absolute paths, `..` and drive letters are rejected. Returns zero when the file is missing or unreadable. The bytes stay valid until `install` returns. Up to 16 files can be read per install. NULL outside `install`.

### `install_file(host->user, path, data, size, flags)` — install only

Queue a whole file for writing below the chosen folder, using the same path rules. The host copies the bytes. `flags` may be `DME_INSTALL_BACKUP`. Up to 16 files of at most 64 MiB each. Returns zero for invalid paths, sizes or flags. NULL outside `install`.

## Registration example and limits

The examples above use plugin-owned `Settings` and `Job` types. Assemble the callbacks into one persistent descriptor using designated initializers, and register it with exporter_at and duckmidi_export_plugin shown earlier.

```c
static const DmeExporter format = {
    .id="example.text", .name="Text", .extension="txt",
    .filter_name="Text file", .save_label="Save text...",
    .description="Example exporter", .container=DME_SINGLE_FILE,
    .create=create, .destroy=destroy, .inspect=inspect, .describe=describe,
    .set=set, .reset=reset, .create_job=create_job,
    .run=run, .destroy_job=destroy_job, .preflight=preflight,
    .review=review, .review_set=review_set, .review_apply=review_apply
};
```

IDs contain ASCII letters, digits, `.`, `_`, `/` or `-` and are at most 96 bytes. Extensions contain lowercase ASCII letters/digits, without a dot, and are at most 16 bytes. The host accepts up to 64 modules/formats, 32 exporters per module, 64 setting fields, 512 choices per field, 4,096 mapping rows, 2,048 review rows, and text fields with capacities up to 4,096 bytes. A review selection must appear in its manual choices, and recommendations must be a subset of those choices. Setting IDs and row IDs must be unique in their scope.

Output is limited to 64 artifacts, each no larger than 512 MiB, with leaf names shorter than 256 bytes. SINGLE_FILE requires exactly one artifact. ZIP packages the returned artifacts together. The host supplies Save As, remembered folders, workers, cancellation, validation, temporary writes and atomic publication.

For a complete independent example, see [`export_plugin_fixture.c`](../tests/export_plugin_fixture.c). For a real plugin with separate settings, review policy and conversion code, see [`libraries/m64export/plugin/`](../libraries/m64export/plugin/).
