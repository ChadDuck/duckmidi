# M64 export

The bundled M64 plugin exports music for the stock SM64 US audio engine. It creates a sequence and sound-bank definitions using SM64's existing samples.

## Export a song

1. Press **Ctrl + E** and select **M64**.
2. Choose a **Sequence name** from the dropdown, or choose **Custom** and enter a filename stem.
3. Set a **Bank name** that does not conflict with another bank in your SM64 checkout.
4. Enable **Loop the complete song** if you want it to repeat.
5. Review the instrument mappings, then click **Save M64 package (.zip)...**.
6. Choose a filename and extract the saved ZIP.

The export uses the active arrangement, including mute and solo. The last save folder is remembered. Keep `m64.dll` in the application's `plugins/` directory.

## Choose instrument mappings

Select a channel/program on the left, then choose its SM64 instrument on the right. The same GM instrument can have different replacements on different channels. Enable **Edit percussion mappings** to change the drum notes used in the song; **Reset SM64 mappings** restores defaults.

SM64 has fewer instrument timbres than General MIDI, so mappings are approximations. Where a stock instrument has higher-register samples, the exporter selects them as needed to preserve pitch. Listen to the result in-game when choosing substitutes.

If a mapping exceeds the stock sample's pitch range, a review appears before output is saved:

- **Cancel Export** stops without replacing the destination.
- **Export Anyways** keeps the original mappings, including their range limitations.
- **Export with Changes** uses the selected substitutes. This changes the instrument timbre while retaining the source pitches.

Choose from the recommendations or the manual dropdown. Unsupported manual selections are marked in the review. Mapping changes remain in the export panel for the session; they do not edit the project.

Independent note-color ports are not supported by M64. Remove explicit note colors before exporting to this format.

## Package contents

| File | Use |
| --- | --- |
| `<sequence>.m64` | Compiled music sequence |
| `<bank>.json` | Instrument and drum bank definitions |
| `sequences-entry.json` | Entry to merge into `sound/sequences.json` |
| `manifest.json` | Used samples, mappings, memory requirements, and warnings |

Sequence and bank names control the files inside the ZIP; the ZIP itself can have any name. Game sample audio is not included. Your SM64 checkout must supply the samples referenced by the bank.

## Install into an SM64 checkout

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
| Sequence memory | Increase the target ROM's sequence pools to meet `minimum_temporary_sequence_pool_bytes` in the manifest. Insufficient memory can cause missing instruments. |
| Unsupported controls | Check the affected effect in-game; the warning identifies what was omitted or approximated. |
| Timing or note changes | Review the affected passage. Native tick rounding and hard engine limits can require changes that are reported explicitly. |

A memory warning alone does not cause musical reduction. The exporter tries lossless layouts first; only unrepresentable sequence branches trigger its last-resort size reduction. Extreme tempos, exhausted note layers, and other engine limits can also require audible changes.

Whole-song looping restores the initial tempo, instruments, and controller state. Intro/loop regions are not currently available. Non-looping exports include a one-beat release tail.

For plugin development, see the [API reference](plugin-api.md).
