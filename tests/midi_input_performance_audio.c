#include "../src/audio/audio.c"

static atomic_ullong callback_ns, callback_max, callback_count, callback_late;

static void SDLCALL measured_callback(void *context, SDL_AudioStream *stream, int needed,
                                      int total) {
    uint64_t start = SDL_GetTicksNS();
    callback(context, stream, needed, total);
    uint64_t elapsed = SDL_GetTicksNS() - start;
    atomic_fetch_add(&callback_ns, elapsed);
    atomic_fetch_add(&callback_count, 1);
    uint64_t maximum = atomic_load(&callback_max);

    if (elapsed > maximum) {
        atomic_store(&callback_max, elapsed);
    }

    if (needed > 0 && elapsed > (uint64_t)((needed + 7) / 8) * 1000000000 / RATE) {
        atomic_fetch_add(&callback_late, 1);
    }
}

void midi_performance_callbacks(DmAudio *a, bool reset) {
    SDL_LockAudioStream(a->stream);

    if (reset) {
        atomic_store(&callback_ns, 0);
        atomic_store(&callback_max, 0);
        atomic_store(&callback_count, 0);
        atomic_store(&callback_late, 0);
    }

    SDL_SetAudioStreamGetCallback(a->stream, measured_callback, a);
    SDL_UnlockAudioStream(a->stream);
}

void midi_performance_report(DmAudio *a) {
    unsigned long long count = atomic_load(&callback_count);
    printf("driver=%s callbacks=%llu mean_callback_ms=%.4f max_callback_ms=%.4f "
           "callback_budget_exceeded=%llu\n",
           dm_audio_driver(a), count, count ? atomic_load(&callback_ns) / (count * 1e6) : 0,
           atomic_load(&callback_max) / 1e6, (unsigned long long)atomic_load(&callback_late));
    SDL_LockAudioStream(a->stream);
    double begin = SDL_GetPerformanceCounter();

    for (unsigned i = 0; i < 1000; ++i) {
        float output[BLOCK * 2];
        run_block(a, output, BLOCK);
    }

    double elapsed = (SDL_GetPerformanceCounter() - begin) * 1000.0 / SDL_GetPerformanceFrequency();
    SDL_UnlockAudioStream(a->stream);
    printf("mean_block_ms=%.5f block_budget_ms=%.5f\n", elapsed / 1000, BLOCK * 1000.0 / RATE);
}
