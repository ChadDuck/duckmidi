#ifndef DM_SAMPLE_ANALYSIS_H
#define DM_SAMPLE_ANALYSIS_H

#include "import.h"

typedef enum {
    DM_SAMPLE_PITCH_DEFAULT,
    DM_SAMPLE_PITCH_SIGNAL,
    DM_SAMPLE_PITCH_FILENAME,
    DM_SAMPLE_PITCH_EMBEDDED,
} DmSamplePitchSource;

typedef struct {
    double delay, attack, hold, decay, release; /* Seconds. */
    double sustain;                             /* Linear gain, 0–1. */
} DmSampleEnvelope;

typedef struct {
    DmSamplePitchSource source;
    int root;
    double cents, frequency, confidence;
    bool conflicting_pitch;
    int program, drum_key; /* GM numbers or -1 when no strong match is available. */
    DmSampleEnvelope envelope;
} DmSampleAnalysis;

typedef struct {
    float minimum[2], maximum[2];
} DmSamplePeak;

typedef struct {
    size_t start, end; /* Half-open frame range at the source sample rate. */
} DmSampleLoop;

bool dm_sample_analyze(const DmSampleAudio *audio, const char *filename,
                       const DmSampleImportCallbacks *callbacks, DmSampleAnalysis *out);
bool dm_sample_peaks(const DmSampleAudio *audio, DmSamplePeak *bins, size_t count, float *absolute,
                     const DmSampleImportCallbacks *callbacks);
bool dm_sample_loop_valid(DmSampleLoop loop, size_t frames);
/* Leaves the requested range in place if no suitable joint crossing was found. */
bool dm_sample_loop_snap(const DmSampleAudio *audio, DmSampleLoop requested, DmSampleLoop *out);

#endif
