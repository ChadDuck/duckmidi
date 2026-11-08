# Sample set files

Each sample set you save in **Edit > Samples** is a folder in the sample library. The folder is named after the set and holds each sample as a WAV, an SFZ file for each, and a `manifest.json` with the set's settings:

```text
samples/
  qwer/
    manifest.json
    asdf.wav
    asdf.sfz
```

This page describes those files, for anyone who wants to read or generate them.

## Saving and loading

- A set keeps one saved state. Saving replaces it; there are no older versions, lock files or pointer files.
- Each set has a stable ID. Projects find sets and presets by ID, so renaming a set (which moves its folder) doesn't break them.
- Saving happens in a temporary folder first. If it fails or you cancel, the previous set stays as it was. A folder holding unrelated files is never replaced.
- duckmidi finds sets by reading each folder's manifest. If your library and the program's own library both have a set with the same ID, yours wins.
- Unsaved edits and undo history live in memory only, never in the manifest.

## The manifest (version 3)

A manifest has `version` (3), `id`, `name`, `assets` and `presets`. Unknown fields, duplicate keys and other versions are rejected, including the unreleased version 2 layout that kept each sample in its own folder.

Naming rules:

- **IDs:** 1–48 lowercase ASCII letters, digits, `-` or `_`, starting with a letter or digit. Asset and preset IDs are unique within their lists.
- **Names:** UTF-8, up to 127 bytes.
- **Set folders and sample files:** must be valid Windows file names (no slashes, control characters, reserved device names, leading periods, or trailing spaces or periods). Sample names are unique within a set, ignoring case.

### Assets

An asset is one audio file: `id`, `file`, `frames`, `rate` and `channels`, plus an optional `sourceName` (the original file name shown in the playlist). Its file is `name.wav` in the set folder.

- WAVs are saved as 24-bit PCM at the source's own rate and channel count: one or two channels, 8–192 kHz. The manifest's frames, rate and channels must match the file.
- An asset used by several presets is saved only once.
- The WAV keeps the source's pitch and loop metadata, so **Reset to import values** still works after loading. Trimming and loop edits never change the audio itself.

### Presets

A preset has `id`, `name`, `drums` and at least one zone in `zones`. Melodic presets can suggest a General MIDI program with `suggestedProgram` (1–128).

Each zone has these fields:

| Field | Meaning |
| --- | --- |
| `asset` | The asset ID it plays. |
| `root`, `cents` | The root MIDI key, and how far the source is from it in cents (positive means sharp). |
| `keyLo`, `keyHi`, `velocityLo`, `velocityHi` | Inclusive key and velocity ranges. A drum zone uses one trigger key, which can differ from its root. |
| `gainDb`, `pan` | Gain in dB, and pan from −1 to 1. |
| `cut`, `cutBy` | A new note with a nonzero `cut` stops older notes with a matching `cutBy`, in the same preset and MIDI channel. 0 turns either side off. |
| `envelope` | `delay`, `attack`, `hold`, `decay` and `release` in seconds, and `sustain` as a level from 0 to 1. |
| `loop` | A `mode` of `none`, `continuous` or `sustain`. Looping modes add `start` and `end` frames at the source rate; `end` isn't included. |
| `playback` | Optional `start` and `end` frames (`end` not included). Leave it out to play the whole file; include it to keep the playback markers visible. An active loop must fit inside it. |
| `tone` | Optional tuning and effects, below. Missing fields count as zero or false. |

| Tone field | Range |
| --- | --- |
| `coarse`, `fine` | Semitones from −48 to 48, and cents from −100 to 100. The pitch correction played is `fine - cents`. |
| `customTracking`, `keyTrack` | On or off, and 0–200 cents per key. When off, melodic presets track 100 and drums 0. |
| `cutoff`, `resonance` | 0 for the default, or 20–20,000 Hz. Resonance is 0–24 dB. |
| `filterDepth` | Filter envelope depth, −4,800 to 4,800 cents. It uses the volume envelope's times and sustain level. |
| `vibratoRate`, `vibratoDepth` | 0 for the 5 Hz default, or 0.1–20 Hz. Depth is 0–200 cents. |
| `chorus`, `reverb` | Send amounts from 0 to 1. |

### Limits

- Manifest: 512 KiB, 256 assets, 256 presets and 512 zones.
- Audio: 16 million PCM values per asset and 32 million per set (frames × channels).

## SFZ files

Each preset also gets an SFZ file so you can use it in other samplers. A preset with one sample is named after that sample (`asdf.sfz`); a layered preset is named after the preset. If two would share a name, the later one adds its preset ID, as in `asdf-drums.sfz`.

How the settings carry over:

| duckmidi | SFZ |
| --- | --- |
| Loop end | Converted to SFZ's inclusive end |
| Unlooped drums | One-shot |
| `cut` / `cutBy` | `group` / `off_by`, with `off_mode=fast` |
| Coarse, fine, tracking | `transpose`, `tune`, `pitch_keytrack` |
| Filter | `cutoff`, `resonance` and `fileg_*` |
| Vibrato | `pitchlfo_freq`, `pitchlfo_depth` |
| Reverb, chorus | `effect1`, `effect2` (their sound depends on the player) |

Whole-number SFZ values are rounded. Envelopes, filters, vibrato, effects and choke groups can sound different from player to player, and each SFZ file says so. The manifest always keeps the exact values.
