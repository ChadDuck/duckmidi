# MIDI input backend

The backend monitors and records standard MIDI channel messages into the open note pattern.
The computer-keyboard piano remains available independently. There is no input device picker or
mapping interface yet. Projects keep schema 14 and their existing note and automation formats.

## Devices and lifetime

`src/midi_input/midi_input.h` provides `dm_midi_input_create`, `destroy`, `refresh`, `ports`, `open`, `close`,
`status`, `stats` and `set_thru`. Pass NULL to `create` for RtMidi, or a
`DmMidiInputBackend` to isolate the device boundary. On successful creation, the input owns the
backend and calls its destructor on its worker. A failed creation leaves it with the caller.

Open and close use stable device names, not enumeration indices. RtMidi decorates names with
positions that change when devices are added or replugged (WinMM appends the port index, ALSA
the client and port numbers); the RtMidi backend removes them and numbers identical names
`#2`, `#3` in port order. Up to eight devices can be open
at once. A closed slot can be reused once both consumers have drained it. Status distinguishes
closed, opening, open, missing and failed; driver errors accompany failures. Refresh requests
enumeration and `ports` copies the last snapshot, up to 128 names of 255 UTF-8 bytes each.
Identical devices are told apart by that number, which follows their current port order.
If a driver changes a port's name, it requires a new request with that name.

The request and snapshot functions take only a short state lock; they never wait for a driver.
Creation does not enumerate. The worker keeps one enumeration client and polls requested
devices once a second, closes missing
ports and reopens them by name when they return. Missing saved devices remain requested.
Destruction joins the worker and waits for any driver operation and callbacks to finish.

`editor_init` creates the input, and `editor_free` detaches and destroys it before the audio.
`ed_midi_input_attach` transfers ownership of a replacement input to the editor. Detach audio
with `dm_audio_attach_midi_input(audio, NULL)` before destroying an independently owned input.
Detachment waits for the current audio block and output worker access.

## Threads and queues

- The MIDI input worker owns all RtMidi input enumeration, opening, closing and reconnection.
- Each device callback normalizes, timestamps and enqueues events without allocation, locking,
  logging or editor access.
- Each device has separate fixed SPSC monitor and recording rings of 16,384 events. Worker
  releases use separate 4,096-event rings, keeping each ring single-producer.
- The audio consumer drains at most 1,024 events per block. The editor drains at most 8,192 per
  frame, including background frames while minimized. Main and release rings are merged by
  receipt time, preserving order within each device.
- Full rings drop the newest delivery and count it. Consumers release their tracked notes and
  discard queued events through the recovery timestamp, preventing a dropped Note Off from
  leaving a voice held indefinitely.

`begin`, `pop`, `end` and `overflow` are consumer APIs: consumer 0 belongs to audio, 1 to the
editor. Never use two readers for one consumer. With no open input or pending release, `begin`
returns after one atomic readiness load. Worker-held key bitmaps are atomic; neither consumer
reads callback-private state.

Received and filtered statistics include system traffic seen by the core and malformed input.
Echoes have a separate count. Dropped totals count malformed messages and failed ring deliveries;
one message can lose both its monitor and record deliveries. Audio statistics also count failed
thru queue writes.

## Message handling

| Message | Monitoring | Recording |
| --- | --- | --- |
| Note On / Off | Keyboard route | Note, received velocity and timestamp |
| Note On with velocity zero | Normalized to Note Off, release velocity 64 | Note release |
| CC 0–119 except bank select 0 and 32 | Keyboard controllers, optional song policy | Discrete CC automation |
| Pitch bend | Full 14-bit value | Discrete bend automation |
| Channel / poly pressure | Keyboard controllers | Discrete pressure automation; poly retains its key |
| All Sound Off / All Notes Off, CC 120 / 123 | Synthesized releases of that device's held keys on the source channel | Releases, no channel-mode point |
| Reset All Controllers, CC 121 | Controller reset | Ignored |
| Other channel modes, program change and bank select | Ignored | Ignored |
| System common, realtime and SysEx | Ignored | Ignored |
| Stray data, invalid data bytes or truncated channel messages | Counted and dropped | Ignored |

RPN, NRPN and 14-bit CC pairs retain their received order; no manufacturer interpretation or
controller thinning is performed. Channel pressure uses the current project's one-data-byte
representation (`a` is pressure and `b` is zero), as MIDI import does.

RtMidi ignores SysEx and timing messages, but delivers active sensing to the core. Once a
device sends active sensing, 300 ms without any message triggers a release on the worker's
100 ms check. Device loss and close do the same. A device-release marker reconciles consumer
held state after callback/timeout races; it is never forwarded or recorded as automation.
Safety resets also release sustain and centre bend on routes used by the lost device. Slow
driver operations can delay worker sensing checks; they cannot block the GUI or audio thread.

## Monitoring, routing and thru

`dm_audio_input_route` supplies the keyboard route and an enable flag. The editor refreshes the
route when relevant project or editor state changes while an input is active. MIDI input does
not require computer-piano mode. Inactive projects and a playlist without a selected channel
disable the route. Source channels remain in received events; monitoring rewrites them and
recording uses the open pattern's channel and note-color port.

Held state is separate for device, source channel and key. Audio also retains the destination of
each held note, so its release reaches the original route after a route change. Shared pitches
stay sounding until both MIDI and computer-keyboard sources release them. MIDI notes follow
the main keyboard synth, including sample instruments, rather than auditioning notes in the
Instrument, Samples or Percussion windows.

`dm_audio_input_controllers` switches between `DM_INPUT_CONTROLLERS_KEYBOARD` (default) and
`DM_INPUT_CONTROLLERS_SONG`. `input_controller_targets` is the single destination policy.
Controllers reach the keyboard synth and an active Instrument audition voice. Song policy also
addresses `keyboard_parts[route]`, including its note-color port; volume and pan update the live
mixer view. Recording is identical under either policy.

Thru defaults on, following the computer piano, and can be disabled through
`dm_midi_input_set_thru`. Audio queues messages for the existing output worker. That worker
records outgoing short messages in a 256-entry atomic echo guard while inputs are open.
Matching input bytes on the output channel within 10 ms are dropped and counted. An identical
hand-played message in that window is also dropped. Thru and its interface default remain GUI
stage decisions.

Panic resets the keyboard synth's controllers and bend. Stop and a stopped-to-playing transport
start reset controllers without retriggering keys. Existing running seeks and song/region wraps
preserve keyboard sustain and held voices, as required by the keyboard loop regressions.

## Timing and recording

Audio publishes a seqlock clock containing monotonic nanoseconds, song seconds, playback state,
transport generation, loop counter and latency. Every accepted event retains its receipt timestamp. While
playing, its heard position is `position + elapsed - latency`. The default compensation is one
device buffer; `dm_audio_input_offset` replaces it in seconds. The clock uses atomic payloads
and bounded reader attempts so a suspended writer cannot stall a driver callback.
On contention the callback extrapolates from its last consistent clock snapshot and counts a
timing fallback; it never silently loses a Note Off while waiting for the clock writer.

Recording converts seconds through the project's tempo map and shares take origin, note
insertion and overlap replacement with the computer piano. MIDI supplies its received velocity;
computer keys retain velocity 100 and their existing timing. The first armed MIDI Note On can
start a wait-for-input take. Until audio's playing clock is available, subsequent events use the
first note's receipt time and the take's start position.

MIDI notes and points collect in a take buffer and are written to the project at most every
100 ms, at take end and at a detected loop wrap. Recorded material lies behind the
playhead and is already heard through monitoring, so playback is rebuilt with the ordinary
`ed_publish` only when it can change what plays next: when replace mode first clears a lane,
0.25 s before the transport wraps (so the next pass plays this one), at the wrap, and when
recording is switched off during playback. Stop and seek republish anyway. This keeps schedule
rebuilds of large projects off the per-batch path; the computer piano still publishes each note. Automation
uses a stable sorted merge, retaining the order of same-time CC pairs. One snapshot covers the
whole take, including simultaneous keyboard notes. Stop, seek and switching recording
off close recorded MIDI notes. Focus changes and other computer-key releases do not; monitoring
continues independently.

Replace mode clears an automation lane on that port when the take first touches it. Overlap
mode replaces existing points between the lane's first and last recorded positions, inclusive;
later batches remove only newly covered extensions, preserving points already recorded in this
pass. Each loop pass starts afresh, so a new pass replaces the previous pass in its lanes or
range instead of interleaving with it. Untouched lanes and other ports remain intact.
Note patterns retain their existing note-based playback length; controller-only input retains
all points but does not extend that playback span.

A removed take pattern or a replaced project (a new project session) cancels pending take
data. Generation changes accompany publications and transport discontinuities. Continuous schedule
refreshes keep held notes across generations; the separate loop counter closes
them at the loop end and drops their remainder. Explicit editor seeks finish held notes before
the jump and discard older queued receipts. Automation patterns refuse note recording.

The drawing path batches overlapping discrete markers and handles within each display pixel;
hit testing still visits the original points, and zooming separates them again.
Dense step-handle hints are drawn near the pointer instead of redrawing overlapping outlines.

## Preferences

The existing INI application section accepts one new key:

```ini
midi_inputs="Keyboard input","Controller, port 2","Device ""quoted"" name"
```

Names are comma-separated quoted strings. Double quotes are doubled and backslashes are
literal, matching other preferences. An empty value means no saved inputs. Empty names,
duplicates, control characters, overlong names and lists above eight are rejected by the
existing preference validation. The existing `midi_output` key is unchanged.

The editor restores saved requests after loading preferences and collects requested names
before shutdown. The desktop editor remains the application entry point. Developer tools and
tests do not read or write installed preferences or user media.

## Developer monitor

Build from an initialized MSYS2 shell, or Command Prompt with the repository build environment:

```text
cmake --build build/development/cmake --target midi_monitor
build\development\cmake\bin\midi_monitor.exe --list
build\development\cmake\bin\midi_monitor.exe --name Keyboard --all --seconds 30 --log agents/reports/input.csv
```

`--list` shows the stable names duckmidi saves. `--port N` selects an index and `--name TEXT`
a case-insensitive, unambiguous substring. Repeat
these options for multiple devices. With no selection, the first available input is used.
`--raw` hides decoding; `--all` prints filtered traffic instead of a count line each second.
`--virtual NAME` creates a virtual input on Linux or macOS; WinMM requires a system loopback.
Stop with Ctrl+C or `--seconds S`. `--self-test --all` runs generated traffic without a device.

Each line shows elapsed time and delta, device, bytes, channel, decoding and the verdict from
the application core. C4 is MIDI key 60. Bend shows signed and raw values. CSV includes filtered
messages and a summary. The callback captures at most 256 raw bytes per message; original
lengths and SysEx manufacturer IDs are retained. Capture overflow is counted. Summary SysEx
details cover up to 64 manufacturers, with their shortest and longest message.

The exit summary covers channel/type counts, held keys, velocity-zero releases, controller
ranges/distinct values/peak rates and mapping hints, bend return to centre, pressure by key,
clock BPM, sensing, SysEx and filtered/dropped totals. Relative, switch, 14-bit and RPN hints
describe observed traffic; they do not change input interpretation.

## Validation

`midi_input_core`, `midi_input_worker`, `midi_monitor_decode`, `midi_input_audio_test` and
`midi_recording_test` exercise the core, scripted drivers, audio and editor boundaries.
The existing recording, loop, feedback, live-edit and gesture tests remain enabled.

The manual `midi_input_performance_test` reuses the dense frame fixture. Run from the project
root, with generated SoundFonts available:

```text
set DM_MIDI_TEST_DRIVER=dummy
build\development\cmake\tests\bin\midi_input_performance_test.exe 60 record
set DM_MIDI_TEST_DRIVER=wasapi
build\development\cmake\tests\bin\midi_input_performance_test.exe 60
```

It is built but not registered with CTest. It sends 3,000 controllers and 20 note pairs per
second, measures GUI and callback budgets,
checks drops and exercises a five-second open. Callback budget overruns are measured; SDL does
not expose a hardware underrun counter here. Device, OS-loopback and listening checks require
available hardware. Results and limits are in
[`agents/reports/2026-11-08-midi-input/VALIDATION.md`](../agents/reports/2026-11-08-midi-input/VALIDATION.md).
