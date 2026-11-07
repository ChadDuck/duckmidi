# Sample set manifest

Each set has a stable ID and a single saved state. The set's name is its folder name. Its samples, their SFZ companions and the manifest sit together in that folder, and each SFZ shares its sample's name:

```text
samples/
  qwer/
    manifest.json
    asdf.wav
    asdf.sfz
```

Saving replaces the set's contents. Renaming a set moves it to the new name while retaining its ID. Projects identify sets and presets by stable IDs and use their latest saved state. There are no revision folders, lock files or current-pointer files. Temporary staging folders are used during saving and removed after the replacement succeeds; cancellation or a failed write keeps the previous saved set. A folder containing unrelated files is not replaced.

The catalog discovers sets by reading each named folder's manifest. A valid set in the writable library takes precedence over a program-library set with the same ID. Unsaved edits and undo/redo states live in memory for the session; they are not part of the manifest.

## Version 3

Each manifest has `version`, `id`, `name`, `assets`, and `presets`. The version is 3. IDs have 1–48 lowercase ASCII letters, digits, hyphens or underscores and start with a letter or digit. Names are UTF-8, up to 127 bytes. Set folder and sample file names must be valid Windows filenames: no path separators, control characters, reserved device names, trailing spaces or periods, or leading periods. Sample names must be unique within the set, ignoring case. Asset and preset IDs are unique within their respective lists. Earlier unreleased sample manifests, including version 2 sets that kept each sample in its own folder, are unsupported.

An asset has `id`, `file`, `frames`, `rate`, and `channels`, with optional `sourceName` for the original filename shown in the playlist. Its file is `name.wav` in the set folder. Shared assets are written once even when used by several regions or presets. Loaded presets with multiple distinct assets preserve those assets' file names. WAVs hold one or two channels of PCM at 8–192 kHz. Channel count, rate, and frames must match the manifest. Published WAVs use 24-bit PCM and preserve the source rate and channel count. Original embedded pitch and forward loop metadata are retained in those WAVs so resetting a loaded draft can reconstruct its import settings. Playback and loop edits do not trim or rewrite the original imported file.

A preset has `id`, `name`, `drums`, and a nonempty `zones` array. Melodic presets can include `suggestedProgram`, a one-based GM program number from 1 to 128. This is an authoring suggestion. Each zone has the fields below.

| Fields | Units and meaning |
| --- | --- |
| `asset` | An asset ID in the manifest. |
| `root`, `cents` | Root MIDI key and the sample's measured pitch offset from that key. Positive cents describe a sharp source. |
| `keyLo`, `keyHi`, `velocityLo`, `velocityHi` | Inclusive ranges. Drum zones use a single trigger key, which can differ from the root. |
| `gainDb`, `pan` | Gain in dB and pan from −1 to 1. |
| `cut`, `cutBy` | A new note with nonzero `cut` stops older voices whose `cutBy` matches, within the same preset and MIDI channel; 0 disables each side independently. |
| `envelope` | `delay`, `attack`, `hold`, `decay`, and `release` in seconds, and `sustain` as a linear 0–1 level. |
| `loop` | An object with mode `none`, `continuous`, or `sustain`. Looping modes also have `start` and `end` frame indices at the source rate. The interval is half-open. |
| `playback` | Optional half-open `start`/`end` frame bounds; omission plays the full asset. An explicit object keeps the playback bars visible. Active loops must lie within these bounds. |
| `tone` | An optional object containing the tuning and effects below. Omitted fields use zero or false. |

| Tone field | Units and range |
| --- | --- |
| `coarse`, `fine` | Integer semitones from −48 to 48, and cents from −100 to 100. Playback fine correction is `fine - cents`. |
| `customTracking`, `keyTrack` | Boolean and cents per MIDI key from 0 to 200. With custom tracking disabled, Melody uses 100 and Drums uses 0. |
| `cutoff`, `resonance` | Zero for the default cutoff, or 20–20,000 Hz. Resonance is 0–24 dB. |
| `filterDepth` | Envelope depth in cents from −4,800 to 4,800. The filter envelope shares the volume envelope's times and linear sustain level. |
| `vibratoRate`, `vibratoDepth` | Zero for the 5 Hz default, or 0.1–20 Hz. Pitch depth is 0–200 cents. |
| `chorus`, `reverb` | Send amounts from 0 to 1. |

The manifest limit is 512 KiB, with at most 256 assets, 256 presets, and 512 zones. The library bounds PCM to 16 million values per asset and 32 million per set. Duplicate keys, unsupported versions, and unexpected fields are invalid.

## SFZ companions

Publication writes one companion per preset. A preset that plays one sample is named after it (`asdf.wav`, `asdf.sfz`); a layered preset is named after the preset. When two presets would share a name, the later one adds its preset ID, as in `asdf-drums.sfz`. It converts the manifest's exclusive loop end to an inclusive SFZ end, and writes one-shot playback for unlooped drum regions. `cut` maps to `group` and `cutBy` maps to `off_by`, with `off_mode=fast`. Envelope and choke response can vary by SFZ player. The manifest carries the original values.

Coarse tuning maps to `transpose`, fine correction to `tune`, and tracking to `pitch_keytrack`. The filter uses `cutoff`, `resonance`, and `fileg_*` opcodes. Vibrato uses `pitchlfo_freq` and `pitchlfo_depth`. Reverb and chorus sends map to SFZ v1's `effect1` and `effect2`, whose effects depend on the player. Integer SFZ modulation depths and tracking are rounded to the nearest unit. The generated file records that envelope, filter, vibrato, effects and choke response vary by player.
