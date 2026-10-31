# M64 export

The bundled M64 plugin exports music for the SM64 US audio engine, in stock [n64decomp/sm64](https://github.com/n64decomp/sm64) and [HackerSM64](https://github.com/HackerN64/HackerSM64) checkouts. It creates a sequence and sound-bank definitions using SM64's existing samples.

## Export a song

1. Press **Ctrl + E** and select **M64**.
2. Choose a **Sequence name** from the dropdown, or choose **Custom** and enter a filename stem.
3. Set a **Bank name** that does not conflict with another bank in your SM64 checkout.
4. Enable **Loop the complete song** if you want it to repeat, or **Use M64 loop points** to repeat part of it (see [Loop points](#loop-points)).
5. Choose **Sequence memory**: **Console-Compatible** makes the smallest memory change and still runs on console; **Emulator Only** uses all free RAM so later songs fit without new changes.
6. Review the instrument mappings, then click **Save M64 Package**, or **Export to Repository** to install into your SM64 checkout.
7. Choose a filename and extract the saved ZIP.

The export uses the active arrangement, including mute and solo. The last save folder is remembered. Keep `m64.dll` in the application's `plugins/` directory.

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

| File | Use |
| --- | --- |
| `<sequence>.m64` | Compiled music sequence |
| `<bank>.json` | Instrument and drum bank definitions |
| `sequences-entry.json` | Entry to merge into `sound/sequences.json` |
| `manifest.json` | Used samples, mappings, memory requirements, and warnings |
| `sequence-memory.txt` | Included when the song needs more sequence memory: the source edits for the chosen **Sequence memory** option, with what each file is and why it changes |

Sequence and bank names control the files inside the ZIP; the ZIP itself can have any name. Game sample audio is not included. Your SM64 checkout must supply the samples referenced by the bank.

## Export to Repository

**Export to Repository** installs the song straight into an SM64 decomp checkout:

1. Choose an existing song as the **Sequence name**; the export replaces it.
2. Click **Export to Repository** and choose the checkout's root folder (it contains `sound/sequences.json`).
3. Rebuild the checkout as described below.

The export writes `sound/sequences/us/<sequence>.m64` and `sound/sound_banks/<bank>.json`, points the song's entry in `sound/sequences.json` at the new bank, and applies the chosen **Sequence memory** changes when the song needs them. Exporting again to the same checkout updates those changes.

HackerSM64 checkouts are recognized automatically. There, the export raises `TEMPORARY_SEQ_MEM` in `src/audio/data.h` when the song needs more than the current setting.

**Keep original files** (on by default) saves each replaced file beside it as `<file>.bkp`. With it off, files are replaced directly, except stock audio memory files, which are always kept. **Revert repository export** restores every changed file and removes the backups the exports created, for all exports to that folder since the last revert during this session.

## Install into an SM64 checkout manually

1. Replace the chosen song's sequence input with the exported `.m64` under `sound/sequences/` or `sound/sequences/us/`. Remove the old input for that stem, including any assembly version, to avoid duplicate sequence names.
2. Place the bank JSON in `sound/sound_banks/`.
3. Merge `sequences-entry.json` into `sound/sequences.json`, replacing the chosen song's bank list. The exported file is a snippet, not a replacement for the entire table.
4. Compile the checkout for US with hash comparison disabled:

   ```sh
   make VERSION=us COMPARE=0
   ```

Keep the sequence and its generated bank together. Adding a new song instead of replacing one also requires choosing its position in the sequence table and updating game references. Refer to your [SM64 checkout's build instructions](https://github.com/n64decomp/sm64) for setup.

## Understand export warnings

An **Export warnings** popup lists engine limitations after a successful save. The ZIP has already been written; the same diagnostics are retained in `manifest.json`.

| Warning | What to check |
| --- | --- |
| Sample pitch range | Try a supported mapping from the review, or provide a suitable sample in your ROM. Stock playback caps near 4x sample speed. |
| More than 16 simultaneous notes | Listen for voice stealing. Notes are retained at this advisory threshold; release tails and sound effects also consume game voices. |
| Sequence memory | Apply `sequence-memory.txt` from a saved package, or use **Export to Repository**, which applies the chosen **Sequence memory** option. Insufficient memory can cause missing instruments. |
| Unsupported controls | Check the affected effect in-game; the warning identifies what was omitted or approximated. |
| Timing or note changes | Review the affected passage. Native tick rounding and hard engine limits can require changes that are reported explicitly. |

A memory warning alone does not cause musical reduction. The exporter tries lossless layouts first; only unrepresentable sequence branches trigger its last-resort size reduction. Extreme tempos, exhausted note layers, and other engine limits can also require audible changes.

Whole-song looping restores the initial tempo, instruments, and controller state. Non-looping exports include a one-beat release tail.

## Loop points

Add **M64 - Loop Start** and, optionally, **M64 - Loop End** from the Song event dialog (**Shift + E**), or from a playlist click on the event row. Each song has one of each; adding another moves it. With **Use M64 loop points** enabled in the export window, the song plays once from the start and then repeats from Loop Start to Loop End, or to the song end without a Loop End. This overrides **Loop the complete song**.

Notes sounding across Loop Start are restarted there on every repeat, and notes crossing Loop End are cut at it. Tempo and controller values at Loop Start are restored on each repeat. Loop points are saved as `loopStart` and `loopEnd` marker events, so they survive MIDI export.

For plugin development, see the [API reference](plugin-api.md).
