# OoT export

The bundled OoT plugin exports music for the Ocarina of Time audio engine in a [zeldaret/oot](https://github.com/zeldaret/oot) decomp checkout. It writes a sequence in the decomp's sequence assembly format and, unless you choose a vanilla soundfont, a soundfont built from the game's own instruments.

## Export a song

1. Press **Ctrl + E** and select **OoT**.
2. Choose an **Area**: pick an existing song to replace it wherever the game plays it, or **New song** to add an `NA_BGM_<NAME>` ID that you play from a scene or from code.
3. Check the **Sequence name**: lowercase letters, digits and `_`, starting with a letter and not with `seq_`. It starts as the project's name, or the MIDI file's name for an unnamed project, and names the `.seq` file, the `Sequence_<name>` symbol and the custom sample set.
4. Choose a **Sample Set** (see [Choose a sample set](#choose-a-sample-set)).
5. Set the **Sequence volume**.
6. Choose **Sequence memory**: **Enlarge** lets Export to Repository make room for a large song; **Stock** changes no game files besides the export itself.
7. Set the loop options (see [Loop points](#loop-points)).
8. Review the instrument mappings, then click **Save OoT Package**, or **Export to Repository** to install into your checkout.

Muted channels are left out, and solo works as it does in playback. The exporter itself is `OoT.dll` in duckmidi's `plugins/` folder.

## Choose a sample set

| Sample Set | Use |
| --- | --- |
| **Custom** | Builds a new soundfont from instruments of every vanilla music soundfont that uses the main sample bank: orchestra, guitars, ocarina, choirs, koto, organ and more. Each General MIDI program gets the closest instrument. |
| An area, such as **Main Orchestra** | Uses that area's soundfont from the game as it is. No soundfont file is added, but only that set's instruments and drum kit are available. |

The driver's synthesized waves (square, sawtooth, triangle, sine and eighth-pulse) are available with either choice. Synth leads map to them by default.

Instrument names come from community instrument lists; the decomp leaves them unnamed. Variants of one instrument are labeled with the soundfont they come from, such as **Harp (Main Orchestra)**.

## Choose instrument mappings

Select a channel and program on the left, then choose its OoT instrument on the right: **Similar instruments** lists the instruments closest to the default mapping that can play the source's notes, as the review below suggests, and shows the one you picked, and **Manual selection** lists every instrument. The same General MIDI instrument can have different instruments on different channels. Enable **Edit percussion mappings** to choose the drum used for each percussion key; **Reset OoT mappings** restores the defaults. Changing **Sample Set** also restores them, because each sample set has its own instruments.

OoT drum kits are not laid out like General MIDI. Each kit holds a few sounds, each spread across a range of keys. The custom soundfont maps General MIDI drums to the orchestra kit: kick, snare, crash cymbal and timpani for toms. Keys between 21 and 84 can play; others are silent. With a vanilla soundfont, each key plays what that kit has there.

OoT plays a sample at most about 4x its recorded speed, so each instrument has a highest playable note. If a mapping cannot reach a source's notes, a review appears before output is saved:

- **Cancel Export** stops without saving.
- **Export Anyways** keeps the mappings; the highest notes play flat.
- **Export with Changes** uses the selected substitutes, preferring the same instrument from another soundfont, then the same family.

Mapping changes remain in the export panel for the session; they do not edit the project.

An OoT channel plays at most 4 notes at once. Channels with more overlapping notes continue on free channels with the same settings. Colored note ports also move to free channels, or merge with a notice when none are left.

## Package contents

| File | Use |
| --- | --- |
| `<name>.seq` | The sequence, as `aseq.h` assembly |
| `Soundfont_<name>.xml` | The custom soundfont, referencing the game's samples (not included with a vanilla soundfont) |
| `INSTALL.txt` | The manual install steps and the song's memory needs |

Game sample audio is not included; your checkout supplies it.

## Export to Repository

1. The first time, click **Choose Folder** and choose the checkout's root folder (it contains `include/tables/sequence_table.h` and `spec/spec`). Then click **Export to Repository**. Later exports install into the same folder without asking; change it in **Edit > Preferences** under **Export plugins**.
2. Run `make` in the checkout.

The export writes `assets/audio/sequences/<name>.seq` and `assets/audio/soundfonts/Soundfont_<name>.xml`, registers the song in `include/tables/sequence_table.h`, and adds both files to `spec/spec`. A new soundfont takes the next free index; exporting again keeps it. Files with these names that were in the checkout before duckmidi first used it are never replaced.

OoT has room for 48 soundfonts, 38 of them vanilla. Exporting again to the same song updates that export instead of adding another. Two installed songs cannot share a name; revert the earlier one in **Repository history**, or choose another name. Reverting a song in **Repository history** removes its files and `spec/spec` lines, and the remaining exports are composed again.

When a replaced song is a fanfare that loads into the small persistent cache and your song is larger, its table entry switches to the temporary cache.

duckmidi's **Repository history** remembers every original, so you can revert any export, or all of them, even after a restart. **Keep original files** (on by default, above the export buttons) also saves a `<file>.bkp` copy beside each changed file. A replaced sample always gets one while history is on.

If you turn history off (**Enable** in **Repository history**), exports write files directly and can't be reverted. **Keep original files** then decides whether the first original is kept as `<file>.bkp`; with both off, files are simply overwritten.

## Sequence memory

Songs load into a temporary sequence cache whose size depends on the scene: 0x3800 bytes (14 KiB) in most scenes and 0x2800 bytes (10 KiB) in the smallest. A song that does not fit does not play there. Soundfonts share a temporary font cache of at least 0x2600 bytes.

With **Enlarge**, Export to Repository raises the temporary sequence and font caches of every audio spec in `src/audio/game/session_config.c` that is too small, and grows `gAudioHeap` in `src/buffers/audio_heap.c` and `include/buffers.h` by the largest increase, so the other audio pools keep their size. The heap uses that much more RAM. With **Enlarge**, the export window does not warn about scene memory; with **Stock**, it warns where the song is too large to play.

The export window reports the song's size. Repeated bars and channels with the same automation are stored once, and short notes are used when they save space, so repetitive songs stay small. A song over OoT's 64 KiB sequence limit has its volume, pan and pitch-bend curves simplified, within about 1 dB, 6 pan steps and 10 cents first, until it fits; notes stay as written. Only if the simplest curves still do not fit are note timing and velocity rounded to coarser grids, which the warning window reports as audible changes.

## Install into an OoT checkout manually

`INSTALL.txt` in the package lists the steps for your song:

1. Copy `<name>.seq` to `assets/audio/sequences/`.
2. Copy `Soundfont_<name>.xml` to `assets/audio/soundfonts/`, and set its `Index` to the next free soundfont index (38 on a stock checkout).
3. In `include/tables/sequence_table.h`, change the first argument of the replaced song's line to `Sequence_<name>`, or add a `DEFINE_SEQUENCE` line for a new song.
4. Add `include "$(BUILD_DIR)/assets/audio/sequences/<name>.o"` to the `Audioseq` segment of `spec/spec`, and the soundfont's object after the last soundfont in the `Audiobank` segment.
5. Run `make`.

Hyrule Field's songs are parts driven by a controller sequence and cannot be replaced.

## Loop points

**Loop song** repeats the song from the start. Add **M64 - Loop Start** and, optionally, **M64 - Loop End** song events (**Shift + E**) and enable **Use loop points** to play once and then repeat from Loop Start to Loop End, or to the song end. Notes sounding across Loop Start restart there on every repeat, and the tempo and controller values at Loop Start are restored.

For plugin development, see the [API reference](plugin-api.md).
