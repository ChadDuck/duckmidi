#ifndef DM_SPECTRUM_H
#define DM_SPECTRUM_H

#define DM_SPECTRUM_FRAMES 4096
#define DM_SPECTRUM_BANDS 48

/* 48 kHz interleaved stereo; no UI, devices, allocation or persistent state. */
void dm_spectrum_analyze(const float *stereo, float *bands);
float dm_spectrum_band_edge(int edge);
float dm_visual_smooth(float value, float target, float seconds, float rise, float fall);

#endif
