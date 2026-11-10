#ifndef DM_AUDIO_SAMPLES_H
#define DM_AUDIO_SAMPLES_H

#include "../../shared/sample/export_snapshot.h"
#include <fluidsynth.h>

typedef struct DmSampleSynth DmSampleSynth;
typedef struct DmLiveSampleBank DmLiveSampleBank;

typedef enum { DM_SAMPLE_STOCK, DM_SAMPLE_RELEASE, DM_SAMPLE_ONE_SHOT } DmSampleStart;

typedef struct {
    struct {
        unsigned id;
        uint8_t channel;
        bool held, captured;
    } notes[512];

    bool down[16];
    unsigned next;
} DmSamplePedals;

void dm_sample_pedal_on(DmSamplePedals *pedals, unsigned id, unsigned channel);
void dm_sample_pedal_off(DmSamplePedals *pedals, fluid_synth_t *synth, unsigned id);
void dm_sample_pedal_cc(DmSamplePedals *pedals, fluid_synth_t *synth, unsigned channel,
                        unsigned controller, unsigned value);

/* The prepared bank can serve synths used sequentially on one synthesis thread.
   Destroy every synth using it before freeing its sample objects. */
DmSampleSynth *dm_sample_synth_create(const DmeContext *context,
                                      const DmSampleExportSnapshot *snapshot, const DmeHost *host);
void dm_sample_synth_free(DmSampleSynth *bank);
/* Isolated single-zone audition only. Serialize with rendering; retains voice/envelope phase. */
bool dm_sample_synth_loop(DmSampleSynth *bank, fluid_synth_t *synth, uint64_t start, uint64_t end);
bool dm_sample_synth_edit(DmSampleSynth *bank, fluid_synth_t *synth, uint32_t preset,
                          const char *asset, const DmeSampleZoneParams *params);
bool dm_sample_synth_edit_region(DmSampleSynth *bank, fluid_synth_t *synth, uint32_t preset,
                                 uint32_t region, const char *asset,
                                 const DmeSampleZoneParams *params);
/* Gain edits leave PCM to this: it rewrites at most budget frames and is true while frames
   remain. Serialize with rendering; between calls the sample plays partly at each gain. */
bool dm_sample_synth_rescale(DmSampleSynth *bank, uint64_t budget);
/* Voice IDs reserve bit 30 for an unlooped drum layer. The caller supplies it cleared. */
DmSampleStart dm_sample_synth_start(DmSampleSynth *bank, fluid_synth_t *synth, uint32_t preset,
                                    unsigned id, int channel, int key, int velocity);
/* True when the sample bank handled a note event. Stock events use the normal synth path. */
bool dm_sample_synth_note(DmSampleSynth *bank, fluid_synth_t *synth, DmSamplePedals *pedals,
                          const DmScheduled *event, DmeEffectivePatch patch);

DmLiveSampleBank *dm_live_sample_bank_create(const DmProject *project, const DmeHost *host);
void dm_live_sample_bank_free(DmLiveSampleBank *bank);
bool dm_live_sample_bank_matches(const DmLiveSampleBank *bank, const DmProject *project);
uint32_t dm_live_sample_bank_patch(const DmLiveSampleBank *bank, DmPatchKey key);
void dm_live_sample_bank_edit(DmLiveSampleBank *bank, fluid_synth_t *synth, const char *locator,
                              const char *set_id, const char *preset_id, uint32_t region,
                              const char *asset, const DmeSampleZoneParams *params);
bool dm_live_sample_bank_rescale(DmLiveSampleBank *bank, uint64_t budget);
DmSampleStart dm_live_sample_bank_start(const DmLiveSampleBank *bank, fluid_synth_t *synth,
                                        uint32_t preset, unsigned id, int channel, int key,
                                        int velocity);

#endif
