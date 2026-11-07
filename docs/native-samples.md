# Native game samples

The M64 SM64/HackerSM64 and OoT exporters lower saved presets from frozen PCM. SM64 repository installation uses the US audio configuration. OoT resolves `VERSION` from `.make_options.mk` and the Makefile and uses that version's extracted sources, with authored `assets/audio/` inputs taking precedence. Ambiguous authored/extracted definitions or duplicate indices are errors.

| Setting | Native result |
| --- | --- |
| Playback range | Exported PCM is trimmed to the half-open frame range; the library is unchanged. |
| Channels and gain | Channels are averaged into mono, gain is baked once, and PCM rounds to 16-bit; peaks above full scale lower the whole region's gain. |
| Rate, root, cents, coarse/fine | C4 tuning combines the saved values with source rate; rates above 192 kHz and notes above the engine limit near 4x lower the exported sample rate while preserving pitch. A row that cannot fit uses its ordinary game mapping. |
| Replace rate | An adaptive Blackman-windowed low-pass avoids raising initialized consumers' playback rates; An incompatible Replace becomes Add; rates below 1,000 Hz or collapsed loops fall back to the ordinary mapping. |
| Continuous loop | Loop positions move relative to the trim. SM64 AIFF markers and OoT's converter-specific half-open WAV endpoint are emitted separately. Resampling rounds endpoints within one output frame. |
| Envelope | Delay/attack/hold/decay clamp to 0–100 seconds and round on the exporter's 240-step scale; sustain clamps to 0–1. Both engines square the envelope value, so levels are written as square roots. Decay follows duckmidi's SoundFont curve, a steady 96 dB fall per decay time down to sustain, as up to seven straight segments of at most 12 dB. Release is one native straight-line fade. The exporter picks the table entry closest to 0.458 (SM64) or 0.382 (OoT) times the release time, the fade that best follows duckmidi's level down to −60 dB; the start of the tail stays slightly louder and the very end slightly shorter. Engine update rates vary with the audio configuration. |
| Keys and velocities | Every matching region layers deterministically; unmatched notes are silent. Invalid or unavailable presets and selections beyond the 256-region preparation limit or native bank capacity use their ordinary game mapping; SM64 has 127 melodic/128 drum slots and OoT has 126 melodic/64 playable drum slots, shared with stock mappings. |
| Sustain-only loop | Exported as a continuous loop, including during release. |
| Cut / Cut by | Shortens earlier matching notes on the same source channel and saved preset, including pedal-held notes; the native release tail still plays. |
| Pan | Melodic zones use layer pan; drums use bank entries in Add and Replace. Authored MIDI CC10 anywhere on a channel, or non-default mixer pan, overrides preset pan. Generated centre defaults do not override it. |
| Reverb, vibrato | Channel commands at note starts: reverb send 0–1 becomes byte 0–127; vibrato rate and depth convert with each engine's update rate and pitch law (SM64/HackerSM64 240 updates/s, OoT 180 at 32 kHz NTSC). Ordinary notes return reverb and depth to zero. Authored CC91 owns a channel's reverb and CC1 its vibrato depth; a set preset vibrato rate still applies with CC1. When overlapping notes on one channel want different values, the latest note's value applies and the export warns. |
| Static filter, chorus | Baked into the exported PCM as duckmidi plays them at the root key: FluidSynth's low-pass with SoundFont resonance, and a mono fold of its default three-voice chorus. Loop bodies are the settled effect output running around the loop, and the chorus sweep rounds to whole cycles per loop or holds still in loops shorter than one cycle. Chorus is left out of loops shorter than its 10 ms delay. Baked effects move with playback pitch. The filter envelope is omitted. |
| Key tracking | Drum entries follow the zone's tracking exactly: 0% for percussion presets unless customized, as in playback. Melodic instruments always track 100%; other values are reported. |

The support matrix applies equally to saved packages and repository installation. “Omitted” means export succeeds and names the setting in its warning.

| Preset setting | SM64 Add | SM64 Replace | HackerSM64 Add | HackerSM64 Replace | OoT Add | OoT Replace |
| --- | --- | --- | --- | --- | --- | --- |
| Range, pitch, gain, envelope | Converted | Converted | Converted | Converted | Converted | Converted |
| Continuous loop | Native | Native | Native | Native | Native | Native |
| Sustain-only loop | Continuous | Continuous | Continuous | Continuous | Continuous | Continuous |
| Cut / Cut by | Shortened | Shortened | Shortened | Shortened | Shortened | Shortened |
| Melodic pan | Layer | Layer | Layer | Layer | Layer | Layer |
| Drum pan | Drum entry | Drum entry | Drum entry | Drum entry | Drum entry | Drum entry |
| Reverb, vibrato | Channel | Channel | Channel | Channel | Channel | Channel |
| Static filter, chorus | Baked | Baked | Baked | Baked | Baked | Baked |
| Filter envelope, melodic tracking other than 100% | Omitted | Omitted | Omitted | Omitted | Omitted | Omitted |
| Drum key tracking | Drum entry | Drum entry | Drum entry | Drum entry | Drum entry | Drum entry |

The warning window lists each changed setting under **Custom samples**, one line per preset, such as `epiano: chorus baked into the sample`. Ordinary rounding is not reported. Missing host services, failed frozen PCM reads, allocation failures, cancellation and repository safety/write errors still stop export.

Layer pan never consumes another channel. Overflow voices copy the source channel's pan weight, and shared layer subroutines establish their own pan state. Channels containing only stock mappings keep their existing encoded output. A panned Replace sets the pan of every native drum entry sharing that physical sample; a centred zone leaves each stock drum pan unchanged. Baked filter and chorus reach every consumer of a replaced sample, including sound effects, at whatever pitch they play it. Sequence-level melodic pan, reverb, vibrato and choke groups apply only to the exported song. Revert restores shared bank pan, sample and sequence changes together.

Add appends managed inputs beside native samples and preserves initialized ordering. Added samples are named after their preset, using letters, digits and underscores, with the region number for layered presets and a six-character identity suffix that keeps same-named presets from different sets apart. M64 allocates its managed banks after initialized banks. Identical shared Add assets remain while any active song needs them; conflicting definitions require distinct resources or explicit global Replace.

Replace resolves a physical AIFF/WAV and updates every current native bank/soundfont consumer, including default and explicit tuning, low/mid/high regions, drums and sound effects. Independent envelopes and transposition remain, while shared drum pan follows the replacement preset when its zone is panned. It keeps the exact filename and its first original backup. A generated song can use another existing samplebank when every custom region is assigned to compatible Replace targets in that bank and every used row is custom. Otherwise those other-bank replacements become Adds in the default bank. Missing physical targets or no matching inspected checkout also use Add; failure to open a configured checkout remains an error. Without a region choice, layered Replace uses the region covering the most played notes, with the lowest region index breaking ties.

Ordinary Add packages contain native audio and instructions. Replace packages describe the selected checkout's complete source changes, expected SHA-256 values, required backups and removals; they do not commit history. The game toolchain creates compressed samples, codebooks, loop state, objects and tables during its normal incremental build.

Native encoders may repeat a short loop to fill their minimum encoded loop length. The loop start and period remain equivalent; the encoded loop end can therefore exceed the source endpoint by whole loop periods. OoT font-cache estimates reserve a complete order-2 codebook with all 16 native predictors and loop state for every emitted sample, without assuming deduplication.
