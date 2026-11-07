# M64 export

The bundled M64 plugin exports music for the SM64 US audio engine: stock [n64decomp/sm64](https://github.com/n64decomp/sm64), [HackerSM64](https://github.com/HackerN64/HackerSM64) and [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx). For SM64 and HackerSM64 it creates a sequence and sound-bank definitions using SM64's existing samples. For sm64coopdx it creates a mod that plays the sequence with the game's own instruments.

## Export a song

1. Press **Ctrl + E** and select **M64**.
2. Choose the **Target** game: **SM64** (the default), **HackerSM64** or **sm64coopdx**. See [Targets](#targets).
3. Choose a **Sequence name** from the dropdown, or choose **Custom** and enter a filename stem. Custom starts with the project's name, or the MIDI file's name for an unnamed project.
4. Set a **Bank name** that does not conflict with another bank in your checkout. Export to Repository uses it as the bank file name when it sorts after the checkout's existing banks; otherwise it puts the last bank's name in front, so existing bank numbers do not change. sm64coopdx has no bank name.
5. For SM64 and HackerSM64, choose **Sequence memory**: **Enlarge** makes the smallest memory change the song needs and still runs on console; **Stock** changes no game files besides the export itself.
6. Enable **Loop song** if you want it to repeat, or **Use M64 loop points** to repeat part of it (see [Loop points](#loop-points)).
7. Review the instrument mappings, then click **Save M64 Package**, or **Export to Repository** to install into your checkout.
8. Choose a filename and extract the saved ZIP.

The export uses the active arrangement, including mute and solo. The last save folder is remembered. Keep `m64.dll` in the application's `plugins/` directory.

## Targets

| | SM64 | HackerSM64 | sm64coopdx |
| --- | --- | --- | --- |
| Song files | `sound/sequences/us/`, built into the ROM | Same as SM64 | A mod folder, loaded by the game |
| Instruments | A new bank in `sound/sound_banks/` | Same as SM64 | The game's extended bank, 0x2A; mods cannot add banks |
| Sequence table | An entry in `sound/sequences.json` | Same as SM64 | `smlua_audio_utils_replace_sequence` in the mod's `main.lua` |
| Sequence memory | Audio presets, heap and buffer, set by **Sequence memory** | `TEMPORARY_SEQ_MEM` in `src/audio/data.h`, unless **Stock** | No limit: mod songs play from their own memory |

sm64coopdx builds its music from prebuilt audio data and your ROM, so it ignores `sound/sequences.json` and `sound/sound_banks/`. A mod replaces a song with `smlua_audio_utils_replace_sequence`, which plays the sequence with one existing bank. The export uses the extended bank (0x2A), which collects the instruments of SM64's music banks:

- Each mapping plays with the bank instrument that has the same sample and the closest envelope, so its sound and note range follow that stock instrument.
- Grand piano, the sawtooth and square synths, pan flute and harmonica exist only in bank instruments that a channel can hold but cannot switch to. A channel that changes instruments plays them with bright piano, organ, alto flute or accordion instead, with a warning.
- Drums use the bank's percussion where it has the sample. Snares, hi-hats, cymbals, triangles and cabasa exist only as instruments with fixed note ranges, so they can sound higher or lower than mapped, as in SM64's own songs. The same applies when one of these sounds is mapped to a melodic channel. The manifest lists each change.

A stock **Sequence name** replaces that song. Any other name adds a new song: `main.lua` gives it a new sequence ID, `SEQ_<NAME>`, and shows how to play it.

## Choose instrument mappings

Select a channel/program on the left, then choose its SM64 sample on the right. Each stock sample is listed once. Samples that SM64 uses with more than one setting appear once per variant, labeled by their envelope (melodic) or pitch (drums). The same GM instrument can have different replacements on different channels. Enable **Edit percussion mappings** to change the drum notes used in the song; **Reset SM64 mappings** restores defaults.

SM64 has fewer instrument timbres than General MIDI, so mappings are approximations. Where a stock instrument has higher-register samples, the exporter selects them as needed to preserve pitch. Listen to the result in-game when choosing substitutes.

If a mapping exceeds the stock sample's pitch range, a review appears before output is saved:

- **Cancel Export** stops without replacing the destination.
- **Export Anyways** keeps the original mappings, including their range limitations.
- **Export with Changes** uses the selected substitutes. This changes the instrument timbre while retaining the source pitches.

Choose from the recommendations or the manual dropdown. Unsupported manual selections are marked in the review. Mapping changes remain in the export panel for the session; they do not edit the project.

M64 has one set of controls per channel. Colored note ports move to unused channels, which keeps them exact. Drum ports, and ports beyond the free channels, merge into their channel. When merged ports change the same control, such as pitch bend, to different values, the export window shows a notice and the latest value wins.

## Package contents

The package follows **Target**. SM64 and HackerSM64 packages contain:

| File | Use |
| --- | --- |
| `<sequence>.m64` | Compiled music sequence |
| `<bank>.json` | Instrument and drum bank definitions |
| `sequences-entry.json` | Entry to merge into `sound/sequences.json` |
| `manifest.json` | Used samples, mappings, memory requirements, and warnings |
| `sequence-memory.txt` | Included when the song needs more sequence memory: for SM64, the source edits for the chosen **Sequence memory** option, with what each file is and why it changes; for HackerSM64, the `TEMPORARY_SEQ_MEM` value to set |

An sm64coopdx package contains a mod folder and a manifest:

| File | Use |
| --- | --- |
| `duckmidi_<sequence>/main.lua` | Mod script that plays the song with bank 0x2A |
| `duckmidi_<sequence>/sound/<sequence>.m64` | Compiled music sequence |
| `manifest.json` | The bank instrument chosen for each mapping, and warnings |

Sequence and bank names control the files inside the ZIP; the ZIP itself can have any name. Game sample audio is not included. Your checkout or ROM supplies the samples.

## Export to Repository

**Export to Repository** installs the song straight into a checkout:

1. Choose an existing song as the **Sequence name**; the export replaces it. sm64coopdx also accepts a new name.
2. The first time, click **Choose Folder** and choose the checkout's root folder (it contains `sound/sequences.json`). Then click **Export to Repository**.
3. Rebuild the checkout as described below.

The export recognizes the checkout's game and installs for it, whatever **Target** is set to; **Target** chooses the game of saved packages. When they differ, a warning names the game it installed for. sm64coopdx checkouts are recognized by `src/pc/lua/utils/smlua_audio_utils.c`, and HackerSM64 checkouts by `include/config/config_audio.h`.

For SM64, the export writes `sound/sequences/us/<sequence>.m64` and `sound/sound_banks/<bank>.json`, points the song's entry in `sound/sequences.json` at the new bank, and applies the chosen **Sequence memory** changes when the song needs them. Exporting again to the same checkout updates those changes. With **Stock**, the memory files are left alone and the export warns when the song is too large for the stock pools; with either **Enlarge** choice it does not warn, because the export makes room.

For HackerSM64, it writes the same files and raises `TEMPORARY_SEQ_MEM` in `src/audio/data.h` when the song needs more than the current setting, unless **Sequence memory** is **Stock**, which warns instead.

For sm64coopdx, it writes `mods/duckmidi_<sequence>/main.lua` and `mods/duckmidi_<sequence>/sound/<sequence>.m64`. The build copies `mods/` beside the game; enable the mod when you host a game.

Once the folder is chosen, later exports install there without asking. Change it in **Edit > Preferences** under **Export plugins**.

**Keep original files** (on by default, above the export buttons) also saves a copy of each changed checkout file as `<file>.bkp` beside it. A replaced sample always keeps one. duckmidi's own history keeps every original either way, so **Repository history** can revert any export, or all of them, even after a restart.

## Install into an SM64 checkout manually

1. Replace the chosen song's sequence input with the exported `.m64` under `sound/sequences/` or `sound/sequences/us/`. Remove the old input for that stem, including any assembly version, to avoid duplicate sequence names.
2. Place the bank JSON in `sound/sound_banks/`.
3. Merge `sequences-entry.json` into `sound/sequences.json`, replacing the chosen song's bank list. The exported file is a snippet, not a replacement for the entire table.
4. Compile the checkout for US with hash comparison disabled:

   ```sh
   make VERSION=us COMPARE=0
   ```

Keep the sequence and its generated bank together. Adding a new song instead of replacing one also requires choosing its position in the sequence table and updating game references. Refer to your [SM64 checkout's build instructions](https://github.com/n64decomp/sm64) for setup. HackerSM64 packages install the same way.

## Install an sm64coopdx mod manually

Extract the `duckmidi_<sequence>` folder into sm64coopdx's `mods` folder: the one in your sm64coopdx user folder, or `mods/` in a source checkout before building. Enable the mod when you host a game; players who join receive it.

## Understand export warnings

An **Export warnings** popup lists engine limitations after a successful save. The ZIP has already been written; the same diagnostics are retained in `manifest.json`.

| Warning | What to check |
| --- | --- |
| Sample pitch range | Try a supported mapping from the review, or provide a suitable sample in your ROM. Stock playback caps near 4x sample speed. |
| More than 16 simultaneous notes | Listen for voice stealing. Notes are retained at this advisory threshold; release tails and sound effects also consume game voices. |
| Sequence memory (SM64 and HackerSM64) | Apply `sequence-memory.txt` from a saved package, or use **Export to Repository**, which applies the chosen **Sequence memory** option. Insufficient memory can cause missing instruments. |
| sm64coopdx instruments | A channel that changes instruments plays a substitute, or a drum sound plays at another pitch. Keep the instrument alone on its channel, or choose another mapping. |
| Unsupported controls | Check the affected effect in-game; the warning identifies what was omitted or approximated. |
| Timing or note changes | Review the affected passage. Native tick rounding and hard engine limits can require changes that are reported explicitly. |

A memory warning alone does not cause musical reduction. When a song's script addresses pass 64 KiB, the exporter first tries lossless layouts. It then simplifies volume, expression, pan and pitch-bend curves within about 1 dB, 6 pan steps and 10 cents, then spaces those changes further apart; notes stay as written. Only if that is not enough does its last-resort reduction round note timing and velocity, which the warning window reports as audible changes. Extreme tempos, exhausted note layers, and other engine limits can also require audible changes.

Whole-song looping restores the initial tempo, instruments, and controller state. Non-looping exports include a one-beat release tail.

## Loop points

Add **M64 - Loop Start** and, optionally, **M64 - Loop End** from the Song event dialog (**Shift + E**), or from a playlist click on the event row. Each song has one of each; adding another moves it. With **Use M64 loop points** enabled in the export window, the song plays once from the start and then repeats from Loop Start to Loop End, or to the song end without a Loop End. This overrides **Loop song**.

Notes sounding across Loop Start are restarted there on every repeat, and notes crossing Loop End are cut at it. Tempo and controller values at Loop Start are restored on each repeat. Loop points are saved as `loopStart` and `loopEnd` marker events, so they survive MIDI export.

For plugin development, see the [API reference](plugin-api.md).
