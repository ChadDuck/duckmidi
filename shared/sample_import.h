#ifndef DM_SAMPLE_IMPORT_H
#define DM_SAMPLE_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float *pcm; /* Interleaved native-rate mono or stereo, normalized to [-1, 1]. */
    size_t frames;
    int rate, channels;
    int embedded_root; /* MIDI key, or -1 if no valid instrument metadata was found. */
    double embedded_cents;
    uint64_t embedded_loop_start, embedded_loop_end; /* Half-open; zero end means absent. */
    char format[32], codec[32];
} DmSampleAudio;

typedef struct {
    void *user;
    bool (*canceled)(void *user);
    void (*progress)(void *user, uint64_t bytes_read, uint64_t file_bytes);
} DmSampleImportCallbacks;

typedef struct SDL_IOStream SDL_IOStream;

/* Reads a local UTF-8 path. On success the caller owns the returned audio and frees it with
   dm_sample_audio_free. Callbacks run synchronously on the calling thread. */
bool dm_sample_decode(const char *path, const DmSampleImportCallbacks *callbacks,
                      DmSampleAudio *out, char *error, size_t cap);
/* The caller owns the seekable stream and keeps it open until this returns. */
bool dm_sample_decode_stream(SDL_IOStream *stream, const char *name,
                             const DmSampleImportCallbacks *callbacks, DmSampleAudio *out,
                             char *error, size_t cap);
void dm_sample_audio_free(DmSampleAudio *audio);

#endif
