# M64 converter

A C17 converter for the stock SM64 US music sequence engine. It takes resolved MIDI events and produces an M64 sequence, bank JSON, sequence-table entry, and manifest.

## Layout

- [`include/m64export/m64export.h`](../../include/m64export/m64export.h): public converter API.
- `src/`: conversion and stock instrument mappings.
- `plugin/`: duckmidi settings, mapping review, and export adapter.
- `catalog.json`: sample references, tuning, and register families; contains no sample audio.

The converter can be built as a shared or static library. Its API is independent of the editor and file dialogs; hosts supply events, save the returned artifacts, and handle cancellation through callbacks.

## Use the API

Add the repository's `include/` directory to your compiler's header search path. Obtain the function table from `m64_get_api_3`:

```c
#include <m64export/m64export.h>

const M64Api *api = m64_get_api_3();
M64Event events[] = {
    {0, 1, 0xc0, 0, 0},
    {0, 2, 0x90, 60, 100},
    {960, 3, 0x80, 60, 0}
};
M64Request request = {0};
request.events = events;
request.event_count = 3;
request.duration = 960;
request.ppq = 960;
request.target = M64_TARGET_SM64_US;
request.voice_limit = 16;
request.sequence_name = "03_custom_song";
request.bank_name = "26_custom_song";

M64Result *result = NULL;
int32_t status = api->compile(&request, NULL, &result);
/* Inspect diagnostics on success or failure.
   When status == M64_OK, save the artifacts before freeing the result. */
api->result_free(result);
```

Events use absolute ticks and must be sorted, preserving their order at equal ticks. Status `0xff` carries tempo in microseconds per quarter in `a`; other channel messages use MIDI status/data bytes. Resolve patterns, channel routing, mute/solo, and automation before calling the API.

Input buffers belong to the host and remain valid until the call returns. Results belong to the library; free them with `result_free` before unloading it. Independent calls may run concurrently.

`analyze` checks ranges without producing artifacts. Use `range_count`, `range_at`, and `preset_supported` to build a mapping review. The public header describes the request fields, callbacks, diagnostics, and result accessors.

## Build and test

The application's build script builds `plugins/m64.dll`. To build just the library, run these commands from the repository root with CMake, Ninja, and GCC on PATH:

```sh
cmake -S libraries/m64export -B build/development/m64 -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build/development/m64
```

The targets are `m64export` and `m64export_static`. Windows setup is covered in [ABOUT.md](../../ABOUT.md#building).

Run `tools/test-m64export.ps1` for converter and independent decoder checks; its Python checks require Python 3. `tools/test-export-plugins.ps1 -HostTests` checks the desktop adapter. Validation output is stored under `agents/reports/`.

## Integration

The DLL also exposes duckmidi's export-plugin entry point. `plugin/ui.c` declares settings, `plugin/review.c` handles substitute recommendations, and `plugin/exporter.c` adapts host jobs to the converter. The static target contains only converter code.

See the [plugin API](../../docs/plugin-api.md) for the host contract and the [M64 guide](../../docs/sm64-export.md) for mappings, warnings, package contents, and ROM integration.

The sequence format and bank schema follow [n64decomp/sm64](https://github.com/n64decomp/sm64). The repository's [MIT license](../../LICENSE) applies to this implementation.
