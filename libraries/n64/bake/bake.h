#ifndef DUCK_N64_BAKE_H
#define DUCK_N64_BAKE_H

/* Generic sample baking: a SoundFont zone's static filter and chorus rendered into its audio,
   and band-limited resampling of 16-bit PCM. Nothing here knows about a game. Files in this
   folder include only the public plugin header and the shared utilities (libraries/common), and
   the game-specific code in libraries/n64 calls into them, never the other way, so the folder
   can be lifted into another exporter. */
#include "../../../include/duckmidi_export_plugin.h"
#include <stdbool.h>
#include <stdint.h>

/* Bakes the zone's static low-pass filter and chorus into mono PCM at `rate`, in place, as
   duckmidi plays them at the root key. Looped audio stays seamless: the loop body is the
   settled effect output running around the loop. `loop_end` 0 means unlooped. Sets
   *chorus_short when the loop is too short to hold the chorus, which is then left out. False
   reports allocation failure. */
bool es_native_bake(float *, uint32_t frames, uint32_t rate, uint32_t loop_start, uint32_t loop_end,
                    const DmeSampleToneParams *, bool *chorus_short);

enum { ES_PCM_OK, ES_PCM_LIMIT, ES_PCM_MEMORY, ES_PCM_CANCELLED };

enum { ES_PCM_TAPS = 32768 }; /* Longest filter es_pcm_resample builds. */

/* Filter length es_pcm_resample uses for a rate ratio (output rate / input rate, below 1). */
unsigned es_pcm_taps(double ratio);

/* Resamples 16-bit mono PCM (big- or little-endian, as `big` says) to `ratio` times its rate
   with a windowed polyphase low-pass. A loop [loop_start, loop_end) in the input (loop_end 0:
   none) keeps playing around itself, so output frames inside [out_start, out_end) read through
   it seamlessly. Writes out_frames samples to `out`; when the filter overshoots 16 bits the whole
   output is lowered and *lowered is set. ES_PCM_LIMIT: the filter would exceed ES_PCM_TAPS. */
int es_pcm_resample(const uint8_t *in, uint32_t frames, uint32_t loop_start, uint32_t loop_end,
                    bool big, double ratio, uint8_t *out, uint32_t out_frames, uint32_t out_start,
                    uint32_t out_end, const DmeHost *host, bool *lowered);

#endif
