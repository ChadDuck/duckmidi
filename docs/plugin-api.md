# Export plugin API

Plugins add export formats and tools to duckmidi. A plugin is a native library in the `plugins/` folder beside duckmidi, built against [`duckmidi_export_plugin.h`](../include/duckmidi_export_plugin.h). duckmidi draws your settings and review windows, so a plugin needs no UI code.

Plugins run inside duckmidi, so only install ones you trust. Restart duckmidi after adding or replacing one.

## Basics

Mark callbacks with `DME_CALL` and the entry point with `DME_PUBLIC`, and define `DME_BUILD_PLUGIN` when building a Windows DLL. Strings are UTF-8 and zero-terminated, and buffer capacities count that terminator. When you return a failure, write the reason into the error buffer. Don't let exceptions cross the API.

Ownership is simple:

- Descriptors and function tables live until the module unloads.
- Your plugin allocates and frees its own instances and jobs. Instances and jobs must not share mutable state.
- Contexts and host callbacks are only borrowed for the call, so copy anything you keep.
- Settings and review views you hand to duckmidi must stay valid until the next call that replaces them.

| Constant | Meaning |
| --- | --- |
| `DME_ABI` | The ABI a plugin must register with, currently 11. |
| `DME_OK` | Success. |
| `DME_INVALID` | Invalid input or setting. |
| `DME_CANCELLED` | The user cancelled. |
| `DME_FAILED` | Failure; explain why in the error buffer. |
| `DME_NOMEM` | Out of memory. |
| `DME_UNAVAILABLE` | The tool or operation isn't available for this input. |
| `DME_INFO`, `DME_WARNING`, `DME_ERROR` | Diagnostic severity. |
| `DME_DISPLAY` | Show the diagnostic in the export panel. |
| `DME_POPUP` | Show the warning in the popup after a successful export. |
| `DME_LIST` | With `DME_POPUP`: the message is a `"Heading\nItem"` list entry. Repeated items are shown once. |
| `DME_ALTERED` | With `DME_POPUP`: notes had to change to fit the target, so the popup warns of audible changes. |
| `DME_SINGLE_FILE`, `DME_ZIP` | Save one file, or package every file into a ZIP. |
| `DME_LOOP_START`, `DME_LOOP_END` | Bits of `DmeContext.loop_points` for the song's loop markers. |

## Data types

### `DmeEvent`

One playback event: absolute `tick`, stable `source_id`, MIDI `status`, data bytes `a` and `b`, and a logical `port`. Events arrive routed, with muted channels removed, and keep their order at equal ticks. Status `0xff` is a tempo change (microseconds per quarter note in `a`), and `0xf0` marks a message duckmidi can't pass through. Ports are separate controller spaces, not extra MIDI channels.

`flags` is zero for events the user wrote or imported. `DME_EVENT_DEFAULT_CONTROL` marks a mixer default duckmidi filled in, which shouldn't override a preset's own setting. Non-default mixer pan and explicit centre pan are left unflagged.

```c
DmeEvent note = { .tick=0, .source_id=1, .status=0x90,
                  .a=60, .b=100, .port=2 };
```

### `DmeContext`

The song being exported:

- `events` and `event_count`, plus `duration` in ticks and `ppq`.
- `project_session`, which changes when another project opens. Use it to reset song-specific settings.
- The project name (empty if unsaved), the imported MIDI file's path (or empty), and the 128 program and drum names.
- `percussion_channels`: bit `c` is set when MIDI channel `c + 1` plays drums. That's usually channel 10, but GS, XG and GM2 files can change it.
- `loop_points`, `loop_start` and `loop_end`: where the song's **M64 - Loop Start** and **M64 - Loop End** events sit, in event ticks.

A job sees the same frozen song from preflight through conversion, even if the user keeps editing.

```c
double beats = (double)context->duration / context->ppq;
const char *name = context->program_names[40];
```

#### Custom samples in the context

During an export, `project_revision` identifies the frozen song. If channels play custom samples, `effective_patches` runs parallel to the events. Each entry holds the original 14-bit bank and MIDI program, and a nonzero `sample_preset` picks a preset (counting from one). Note-offs carry the patch, source ID and velocity of their note-on. A missing sample has a zero index and sets `DME_SAMPLE_MISSING`, keeping the original patch.

Presets list their saved IDs and a `generation` value that changes whenever the sample changes. Zones point at assets (counting from zero), and each asset gives its channel count, rate and length. `DmeHost.read_sample` copies interleaved float PCM into your buffer, with capacity counted in floats. Loops are half-open ranges at the asset's own rate.

### `DmeValue`, `DmeChoice`, `DmeMappingRow`

`DmeValue` holds a setting's `type` and either an `integer` or borrowed `text`. `DmeChoice` pairs a value with its label.

A `DmeMappingRow` is one line of a mapping list: a stable `id`, a label, the selected `value` and a `default_value`. Its choices come from the parent field. Optionally, `similar` lists good alternatives to the default, best first, which duckmidi shows as **Similar instruments** above **Manual selection**. Every similar value must also be one of the field's choices, and the list shouldn't change when the user picks from it.

IDs identify settings, so labels can change freely.

```c
DmeValue enabled = { DME_BOOL, 1, NULL };
DmeChoice modes[] = { {0,"Normal"}, {1,"Alternate"} };
DmeMappingRow row = { "source.0", "Channel 1", 0, 0 };
```

### `DmeField`, `DmeUi`

A `DmeUi` is a list of fields. Each field has an `id`, a `label`, optional `help`, a `type`, and the value, limits, choices or rows that type needs. Types are BOOL, INT, TEXT, CHOICE, MAPPING, NOTICE and ACTION. ACTION fields are buttons beside Save, and a MAPPING field becomes a source list with a choice panel. Text capacities include the terminator.

A view stays valid until the next describe, inspect or reset.

```c
DmeField field = { .id="loop", .label="Loop", .type=DME_BOOL,
                   .value={DME_BOOL,1,NULL} };
DmeUi view = { &field, 1 }; /* Keep the field in your plugin's state. */
```

### `DmeReviewRow`, `DmeReview`

A review asks the user about problems before an export. It has a description and one row per affected source. Each row has a stable ID, label, issue text, the original `current_mapping`, the draft `value`, a `supported` flag, recommendations and the full list of choices. The draft belongs to the job and shouldn't touch the frozen settings until `review_apply`. Return zero rows to skip the review.

```c
*review = (DmeReview){0}; /* Nothing to ask. */
/* Or: *review = (DmeReview){ state->description, state->rows, state->count }; */
```

### `DmeHost`, `DmeExporter`, `DmePlugin`

`DmeHost` holds the worker callbacks and a `user` pointer to pass back unchanged. `DmeExporter` describes one format and its callbacks. `DmePlugin` registers the module: ABI, struct size, ID, name, and the number of exporters and tools with a lookup callback for each. Leave a lookup NULL when its count is zero.

Every exporter callback is required except these optional ones:

- `install_label` and `history`, which are set together or left NULL together.
- `set_custom`.

```c
static const DmePlugin module = {
    .abi = DME_ABI, .size = sizeof(DmePlugin), .id = "example.module",
    .name = "Example", .exporter_count = 1, .exporter = exporter_at
};
```

### Tools and the main thread

An optional `bind_main` callback receives the main-thread host after registration, and NULL before unload. Its `request_tool` asks for a tool by ID on the main thread.

`DmeTool.interface_at` returns a function table for a given interface version. duckmidi's Samples tool, `org.duckmidi.samples`, is documented in [`duckmidi_samples_tool.h`](../include/duckmidi_samples_tool.h). Its jobs are created on the main thread, run on a worker, then finished or discarded back on the main thread.

## Registration

### `duckmidi_export_plugin(uint32_t abi)`

The library's one exported function. Return your plugin descriptor when `abi` is `DME_ABI`, or NULL otherwise. duckmidi also checks the descriptor's `abi` and `size`. Plugins built for an older ABI must be rebuilt.

```c
DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &module : NULL;
}
```

### `DmePlugin.exporter(uint32_t index)`

Return the exporter at `index`, or NULL past `exporter_count`. Module and exporter IDs must be unique; a duplicate exporter ID stops the module from loading.

```c
static const DmeExporter *DME_CALL exporter_at(uint32_t index) {
    return index == 0 ? &format : NULL;
}
```

## Settings (main thread)

Settings last until duckmidi closes.

### `create(void)`

Allocate settings with their defaults. Return NULL if allocation fails.

```c
static void *DME_CALL create(void) {
    Settings *s = calloc(1,sizeof(*s));
    if (s) s->loop=1;
    return s;
}
```

### `destroy(void *instance)`

Free everything the settings own. duckmidi calls this before unloading, even when registration fails. Jobs clean up separately in `destroy_job`.

```c
static void DME_CALL destroy(void *instance) { free(instance); }
```

### `inspect(instance, context, error, capacity)`

Update settings that depend on the song, such as the list of instruments in use. duckmidi calls this again after edits. Copy anything you need afterwards, and don't keep the context pointer.

```c
static int32_t DME_CALL inspect(void *s,const DmeContext *c,char *e,uint64_t n) {
    (void)e; (void)n;
    ((Settings *)s)->session=c->project_session;
    return DME_OK;
}
```

### `describe(instance, DmeUi *ui)`

Fill in the fields from the current settings and return a view of them, stored in your instance.

```c
static int32_t DME_CALL describe(void *user,DmeUi *ui) {
    Settings *s=user;
    s->fields[0]=(DmeField){ .id="loop", .label="Loop", .type=DME_BOOL,
                            .value={DME_BOOL,s->loop,NULL} };
    *ui=(DmeUi){s->fields,1}; return DME_OK;
}
```

### `set(instance, id, value, error, capacity)`

Check the ID, type and value before changing anything, and copy accepted text into your own storage. An ACTION value is a button click. Reject unknown IDs and bad values with a message instead of quietly fixing them.

```c
if (!strcmp(id,"loop") && value->type==DME_BOOL &&
   value->integer>=0 && value->integer<=1) {
    s->loop=value->integer; return DME_OK;
}
snprintf(error,(size_t)capacity,"Invalid loop setting."); return DME_INVALID;
```

### `reset(instance)`

Restore the defaults. An action handled in `set` may call it too.

```c
static void DME_CALL reset(void *user) { ((Settings *)user)->loop=1; }
```

### `create_job(instance, error, capacity)`

Copy the settings into a new job before the export starts. Deep-copy strings, arrays and buffers. Later changes to the settings must not affect the job.

```c
Job *job=calloc(1,sizeof(*job));
if (!job) { snprintf(error,(size_t)capacity,"Cannot allocate export job."); return NULL; }
job->loop=((const Settings *)instance)->loop;
return job;
```

## Preflight and review

### `preflight(job, context, host, error, capacity)` (worker)

Check the song after the user picks a destination, and prepare any review. Don't write files or artifacts here, and check for cancellation during long work. Return `DME_OK` even when a review is needed; fail only for unsupported input or errors.

```c
if (host->cancelled(host->user)) return DME_CANCELLED;
job->issue_count=analyze_ranges(context,job);
return DME_OK;
```

### `review(job, DmeReview *view)` (main thread)

Return the job's review, or an empty one. duckmidi shows the affected sources, your recommendations, the full choices and three buttons: **Export with Changes**, **Export Anyways** and **Cancel**. Keep the view valid until the next review call, and update `supported` when the draft changes.

```c
*view=(DmeReview){job->description,job->rows,job->issue_count};
return DME_OK;
```

### `review_set(job, id, value, error, capacity)` (main thread)

Check the row ID and choice, then update only the draft. You can accept an unsupported choice with `supported=0` so the user can decide for themselves.

```c
if (!strcmp(id,"source.0") && value>=0 && value<128) {
    job->draft_preset=value; return DME_OK;
}
snprintf(error,(size_t)capacity,"Unknown review choice."); return DME_INVALID;
```

### `review_apply(instance, job, error, capacity)` (main thread)

Called when the user picks **Export with Changes**. Apply the draft to the job, and to the settings if that makes sense. **Export Anyways** runs the job unchanged, and **Cancel** destroys it. duckmidi rejects a review if the song changed in the meantime.

```c
job->preset=job->draft_preset;
((Settings *)instance)->preset=job->preset;
return DME_OK;
```

## Conversion and cleanup

### `run(job, context, host, error, capacity)` (worker)

Convert the song and hand each output file to `host->artifact`. Check for cancellation, report progress and useful diagnostics, and stop if a callback returns zero. Don't use host callbacks after returning. duckmidi writes the final files itself, safely, once the export succeeds.

```c
if (host->cancelled(host->user)) return DME_CANCELLED;
const uint8_t bytes[]={'o','k','\n'};
if (!host->artifact(host->user,"song.txt",bytes,sizeof(bytes))) return DME_FAILED;
host->progress(host->user,100);
return DME_OK;
```

### Export to Repository (optional)

Lets the user install straight into a folder, such as a game's source checkout. Set `install_label` to name the button and `history` to return a `DmeHistoryAdapter` for `DME_REPOSITORY_API`. duckmidi asks for the folder, runs preflight and review as usual, then installs through its history runtime, which can revert any export later, even after a restart. Plugins never write repository files themselves. See [Repository history](#repository-history).

### `destroy_job(job)` (main thread)

Free everything the job owns. This runs after the worker finishes, whether the export succeeded, failed or was cancelled.

```c
static void DME_CALL destroy_job(void *user) {
    Job *job=user; free(job->buffer); free(job->rows); free(job);
}
```

## Host callbacks (worker thread)

Always pass `host->user` back unchanged.

### `cancelled(host->user)`

Nonzero means stop soon and return `DME_CANCELLED`. Check it every so often during long loops.

```c
for(size_t i=0;i<count;i++) {
    if (!(i%1024) && host->cancelled(host->user)) return DME_CANCELLED;
    convert_one(i);
}
```

### `progress(host->user, percent)`

Report progress from 0 to 100. Every stage or so is plenty.

```c
host->progress(host->user,50);
```

### `diagnostic(host->user, severity, flags, message)`

Send a readable message. `DME_DISPLAY` shows it in the export panel. `DME_POPUP` warnings are gathered into one popup after a successful save; failed or cancelled exports show none. To stop an export, send an error and return a failure.

Short list items work best for routine changes. Save full sentences for warnings that need advice.

```c
host->diagnostic(host->user,DME_WARNING,DME_POPUP|DME_LIST,"Omitted\nController 74");
host->diagnostic(host->user,DME_WARNING,DME_DISPLAY|DME_POPUP,
                 "Some notes exceed this target's playback range.");
```

### `artifact(host->user, name, data, size)`

Hand over one output file with a unique name. In a ZIP, `/` makes folders, as in `"mod/sound/song.m64"`. duckmidi copies the data before returning, so you can reuse the buffer right away. Zero means the file was rejected or the export was cancelled. A rejection still stops the export even if you ignore it. Don't call this during preflight.

```c
if (!host->artifact(host->user,"bank.json",buffer,length)) return DME_FAILED;
free(buffer); /* Safe: duckmidi already copied it. */
```

## Putting it together

Gather the callbacks above into one descriptor and return it from `exporter_at`:

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

### Limits

| What | Limit |
| --- | --- |
| IDs | ASCII letters, digits, `.`, `_`, `/` or `-`; up to 96 bytes; unique in their scope |
| Extensions | Lowercase letters and digits, no dot; up to 16 bytes |
| Modules and formats | 64 total, 32 exporters per module |
| Settings | 64 fields, 512 choices per field, 4,096 mapping rows, text up to 4,096 bytes |
| Reviews | 2,048 rows; a selection must be one of the row's choices, and recommendations must come from them too |
| Output | 20,000 files and 512 MiB total; `DME_SINGLE_FILE` needs exactly one |
| ZIP paths | Use `/`; under 8,192 bytes, each part under 256 bytes |

## Custom samples

`DmeContext.library` lists saved sample sets, presets and zones, without audio. The most recently changed set is the default, and saved IDs identify a choice. `DmeMappingRow.source` keeps the original channel, route, bank, and program or drum key. Its optional `custom` part supplies the Custom and Show all defaults, the catalog, Add/Replace state and the repository files a sample can replace. `set_custom` receives the full selection. Copy any strings you keep, since these views can change after a setter or inspect.

Jobs copy mapping choices on the main thread. On the worker, `freeze_sample` checks a selection's `generation` and returns a handle to fixed preset, zone and asset views. `read_frozen_sample` reads that handle's float PCM and returns `DME_OK`. Frozen audio stays available for the whole export, even if the user edits the set meanwhile, up to 64 million float values.

## Repository history

The history adapter, in [`duckmidi_repository_plugin.h`](../include/duckmidi_repository_plugin.h), only covers what's specific to your format. duckmidi's runtime handles the rest for every exporter: the history database, safe writes, crash recovery, spotting outside edits and the history panel.

| Callback | Thread | Purpose |
| --- | --- | --- |
| `detect` | worker | Recognize the checkout and fill in `DmeHistoryTarget`: an ID, a version, the section name for the history panel, and `DME_REPOSITORY_CUSTOM` if custom samples work there. Fail with a reason otherwise. |
| `inventory` | worker, optional | List the files that make up the starting state, with `list_directory` and `list_add`. Without it, each file's original is captured the first time an export writes it. |
| `allow` | any, optional | Limit which paths the format may write. |
| `render` | worker | Turn every current intent into the full desired files with `plan_put` and `plan_affect`, plus `plan_backup` and `plan_depend` when needed. Paths no longer rendered go back to their original. |
| `prepare` | worker, optional | Check the checkout before installing. |
| `install` | worker | Turn the export job into intents and call `apply`. |
| `targets` | worker, optional | List the files a custom sample may replace. |
| `finish` | main, optional | Receive the detected checkout, target and replace targets after any action. |
| `begin_install` | main, optional | Copy request settings, such as **Keep original files**, into the export job. |

An intent is one logical change, such as a song slot. It has a stable key, a short label for the history list and a recipe in your own format. Applying an intent with an existing key replaces it, so each song keeps one history row. Because every intent is rendered together, reverting one export leaves later ones intact.

```c
static int32_t DME_CALL render(const DmeHistoryApi *api, DmeHistory *history,
                               const DmeHistoryIntent *intents, uint32_t count,
                               DmeHistoryPlan *plan, char *error, uint64_t cap) {
    for (uint32_t i = 0; i < count; i++) {
        if (!api->plan_put(plan, "music/song.txt", intents[i].recipe, intents[i].recipe_size, 1,
                           error, cap) ||
            !api->plan_affect(plan, intents[i].key.data, "music/song.txt", "song")) {
            return 0;
        }
    }

    return 1;
}
```

Each callback gets the runtime's `DmeHistoryApi`, whose handles only last for that call. Read original files with `baseline` or `baseline_exists`, and current ones with `read`. Paths are relative to the checkout and use `/`; traversal, links and aliased paths are refused. For storage and recovery details, see the [repository history reference](repository-history.md).

`DmeHost.plugins_directory` and `DmeMainHost.plugins_directory` give the plugin folder. Copy the path if you keep it.

## Examples

- [`export_plugin_fixture.c`](../tests/export_plugin_fixture.c): a complete standalone exporter.
- [`repository_history_test.c`](../tests/repository_history_test.c): a complete history adapter.
- [`libraries/m64export/plugin/`](../libraries/m64export/plugin/): a real plugin with settings, review, conversion and a history adapter.
