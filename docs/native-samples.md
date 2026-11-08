# Custom samples in SM64 and OoT

When you export a song that uses your own samples, the M64 (SM64 and HackerSM64) and OoT exporters convert each sample preset into the game's own format. The game can't do everything duckmidi's sampler does, so some settings are converted, baked into the audio or left out. This page lists what happens to each one.

To set up custom samples for an export, see [Custom game samples](user-guide.md#custom-game-samples) in the user guide.

## Which game sources are used

- **SM64 and HackerSM64:** Export to Repository uses the US audio setup.
- **OoT:** the game version comes from `.make_options.mk` and the Makefile, and duckmidi uses that version's extracted sources. Your own files in `assets/audio/` take priority. If a definition is ambiguous or an index is used twice, the export stops with an error.

## What happens to each setting

Saved packages and Export to Repository, Add and Replace, and every game all handle settings the same way.

| Setting | In the game |
| --- | --- |
| Playback start and end | The exported audio is trimmed to them. Your saved sample isn't changed. |
| Channels and gain | Stereo is mixed to mono, gain is applied and the audio is rounded to 16-bit. If that would clip, the whole sample is turned down. |
| Pitch (rate, root, cents, coarse, fine) | Combined into the game's tuning. Rates above 192 kHz, or notes beyond the engine's limit of about 4x speed, lower the exported sample rate without changing pitch. If a sample still can't fit, that row uses its ordinary game instrument. |
| Continuous loop | Kept, adjusted for the trim. Resampling can shift the loop points by up to one frame. |
| Sustain-only loop | Becomes a continuous loop, including during release. |
| Envelope | Converted to the game's envelope; see [Envelopes](#envelopes). |
| Keys and velocities | Every matching region plays, layered. Notes no region covers are silent. |
| Cut / Cut by | Earlier matching notes on the same channel and preset are shortened, including ones held by the pedal. The game's release tail still plays. |
| Pan | Melodic samples use the note layer's pan; drums use their drum entry. MIDI pan (CC10) anywhere on the channel, or a non-default mixer pan, overrides the preset's pan. duckmidi's automatic centre pan doesn't. |
| Reverb, vibrato | Become the channel's reverb and vibrato at each note start; see [Reverb and vibrato](#reverb-and-vibrato). |
| Filter, chorus | Baked into the exported audio; see [Baked effects](#baked-effects). |
| Filter envelope | Left out. |
| Key tracking | Drums keep theirs exactly (0% unless you changed it). Melodic samples always track 100%; any other value is left out. |

Anything left out is still exported; the warning window names it.

### Envelopes

- Delay, attack, hold and decay are limited to 0–100 seconds and rounded to the exporter's 240-step scale. Sustain is limited to 0–1.
- Both games square the envelope level, so duckmidi writes square roots to match.
- Decay follows duckmidi's own curve, a steady 96 dB drop per decay time down to the sustain level, as up to seven straight segments of at most 12 dB each.
- Release is a single straight fade. duckmidi picks the game's fade closest to 0.458 (SM64) or 0.382 (OoT) times the release time, which best matches duckmidi's fade down to −60 dB. The start of the tail ends up slightly louder and the very end slightly shorter.

How fast envelopes update depends on the game's audio settings.

### Reverb and vibrato

- The reverb send (0–1) becomes a channel reverb of 0–127.
- Vibrato rate and depth are converted for each engine's update rate and pitch scale: 240 updates per second for SM64 and HackerSM64, 180 for OoT (32 kHz NTSC).
- Ordinary notes set reverb and vibrato depth back to zero.
- MIDI reverb (CC91) on a channel overrides the preset's reverb, and MIDI modulation (CC1) overrides its vibrato depth. A preset's vibrato rate still applies alongside CC1.
- If overlapping notes on one channel want different values, the latest note wins and the export warns.

### Baked effects

Filter and chorus are rendered into the exported audio the way duckmidi plays them at the root key: FluidSynth's low-pass filter with SoundFont resonance, and a mono mix of its three-voice chorus.

- Because they're baked in, they shift with the pitch each note plays at.
- Loops contain the settled effect. The chorus sweep is rounded to whole cycles per loop, or holds still in loops shorter than one cycle.
- Chorus is left out of loops shorter than its 10 ms delay.

## Add and Replace

**Add** puts your sample into the game next to its existing samples, without changing their order.

- Added samples are named after their preset, using letters, digits and `_`. Layered presets add the region number, and a six-character suffix keeps same-named presets from different sets apart.
- M64 places its banks after the checkout's existing ones.
- Identical samples shared by several songs stay as long as any song uses them. Different samples with the same definition need separate names, or a Replace.

**Replace** swaps an existing game sample (AIFF or WAV) for yours, everywhere the game uses it: every bank and soundfont, all tunings and note ranges, drums and sound effects.

- Each instrument keeps its own envelope and transposition.
- If your sample would make existing instruments play at a higher rate, it's low-pass filtered and resampled to fit instead.
- The file keeps its exact name, and its first original is kept as a `.bkp`.
- If your preset is panned, every drum sharing the sample takes that pan. A centred preset leaves stock drum pan alone.
- Baked filter and chorus reach everything that uses the sample, including sound effects, at whatever pitch they play it.
- A song can use another existing sample bank only if every custom region is a compatible Replace in that bank and every row it uses is custom. Otherwise those replacements become Adds in the default bank.
- A Replace also becomes an Add when its target file is missing, when no inspected checkout matches, or when it isn't compatible. Failing to open a configured checkout is still an error.
- For a layered preset with no region chosen, Replace uses the region that plays the most notes, picking the lowest-numbered one on a tie.

Reverting a song in **Repository history** restores shared drum pan, samples and sequence changes together.

## Limits

- Up to 256 regions per export, within the game's bank space. SM64 has 127 melodic and 128 drum slots; OoT has 126 melodic and 64 playable drum slots. Stock instruments share these slots.
- Replace targets below 1,000 Hz, or loops that would collapse, fall back to the ordinary instrument.
- Presets that are missing, invalid or over these limits use the row's ordinary game instrument.

## Packages

- **Add** packages contain the converted audio and install steps.
- **Replace** packages describe every source change for the chosen checkout: expected SHA-256 values, the backups to make and the files to remove. They don't record history.

The game's own build turns the audio into compressed samples, codebooks and tables as usual.

## Warnings

The warning window lists each changed setting under **Custom samples**, one line per preset, such as `epiano: chorus baked into the sample`. Normal rounding isn't reported.

These still stop the export: missing host services, failing to read sample audio, running out of memory, cancelling, and repository safety or write errors.

## Fine print

- Layer pan never uses up another channel. Extra voices copy their channel's pan, and shared layer routines set their own.
- Channels that only use stock instruments export exactly as before.
- Sequence-level melodic pan, reverb, vibrato and choke groups only affect the exported song.
- To meet the minimum loop length, the game's encoder may repeat a short loop. The loop sounds the same, but its encoded end can sit whole loop lengths past yours.
- OoT's memory estimate reserves a full codebook (all 16 predictors) and loop state for every sample, without assuming any are shared.
