#include "../shared/spectrum.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int checks;
static void check(int ok, const char *label) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL spectrum: %s\n", label); exit(1); }
}
int main(void) {
    float stereo[DM_SPECTRUM_FRAMES * 2] = {0}, bands[DM_SPECTRUM_BANDS];
    dm_spectrum_analyze(stereo, bands);
    for (int i = 0; i < DM_SPECTRUM_BANDS; ++i) check(bands[i] == 0, "silence stays invisible");
    const float tones[] = {440, 2000, 8000};
    for (int tone = 0; tone < 3; ++tone) {
        for (int i = 0; i < DM_SPECTRUM_FRAMES; ++i) {
            float sample = .05f * sinf(6.28318530718f * tones[tone] * i / 48000);
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = -sample;
        }
        dm_spectrum_analyze(stereo, bands);
        int peak = 0;
        for (int i = 0; i < DM_SPECTRUM_BANDS; ++i) {
            check(isfinite(bands[i]) && bands[i] >= 0 && bands[i] <= 1, "spectrum is finite and bounded");
            if (bands[i] > bands[peak]) peak = i;
        }
        float frequency = sqrtf(dm_spectrum_band_edge(peak) * dm_spectrum_band_edge(peak + 1));
        check(fabsf(logf(frequency / tones[tone])) < .22f, "stereo tone appears at its actual frequency");
        check(bands[peak] > .6f, "opposite-phase stereo does not cancel in the analyzer");
    }
    int previous_end = -1;
    for (int band = 0; band < DM_SPECTRUM_BANDS; ++band) {
        int first = (int)ceilf(dm_spectrum_band_edge(band) * DM_SPECTRUM_FRAMES / 48000);
        int end = (int)ceilf(dm_spectrum_band_edge(band + 1) * DM_SPECTRUM_FRAMES / 48000);
        check(end > first, "each bar owns at least one FFT bin");
        check(band == 0 || first == previous_end, "adjacent bars have no shared FFT bins or gaps");
        previous_end = end;
    }
    float at60 = 0, at120 = 0;
    for (int i = 0; i < 60; ++i) at60 = dm_visual_smooth(at60, 1, 1.f / 60, .025f, .16f);
    for (int i = 0; i < 120; ++i) at120 = dm_visual_smooth(at120, 1, 1.f / 120, .025f, .16f);
    check(fabsf(at60 - at120) < .00001f, "meter response is independent of GUI frame rate");
    float first = dm_visual_smooth(0, 1, 1.f / 60, .025f, .16f);
    float second = dm_visual_smooth(first, 1, 1.f / 60, .025f, .16f);
    check(first > 0 && first < second && second < 1, "meter rises on intervening GUI frames");
    check(dm_visual_smooth(second, 0, 1.f / 60, .025f, .16f) > 0, "meter release decays smoothly");
    clock_t start = clock();
    for (int i = 0; i < 1000; ++i) dm_spectrum_analyze(stereo, bands);
    printf("Spectrum analysis: %.3f ms/frame (1000 stereo FFTs)\n", 1000.0 * (clock() - start) / CLOCKS_PER_SEC / 1000);
    printf("PASS spectrum: %d checks\n", checks);
}
