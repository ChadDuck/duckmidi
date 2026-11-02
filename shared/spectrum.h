#ifndef DM_SPECTRUM_H
#define DM_SPECTRUM_H

#define DM_SPECTRUM_FRAMES 4096
#define DM_SPECTRUM_BANDS 48
#define DM_SPECTRUM_TOP 18000.f    /* Highest band edge, in Hz. */
#define DM_SPECTRUM_MIN_TOP 4000.f /* Lowest top edge when the audio has no treble. */

/* 48 kHz interleaved stereo; no UI, devices, allocation or persistent state. Bands span up to
   `top` Hz. `content` receives the highest frequency the frame visibly contains, or 0 for
   silence, so callers can follow it with `top` (see dm_spectrum_follow_top). */
void dm_spectrum_analyze(const float *stereo, float top, float *bands, float *content);
float dm_spectrum_band_edge(int edge, float top);
/* Moves top toward the content's highest frequency: up quickly, down over a few seconds. */
float dm_spectrum_follow_top(float top, float content, float seconds);
float dm_visual_smooth(float value, float target, float seconds, float rise, float fall);

#endif
