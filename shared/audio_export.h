#ifndef DM_AUDIO_EXPORT_H
#define DM_AUDIO_EXPORT_H
#include "../src/duck.h"
typedef enum { DM_AUDIO_WAV, DM_AUDIO_MP3, DM_AUDIO_AAC, DM_AUDIO_VORBIS, DM_AUDIO_OPUS, DM_AUDIO_FLAC, DM_AUDIO_CODECS } DmAudioCodec;
typedef struct {
    DmAudioCodec codec;
    int bitrate, quality, bits, compression;
    bool variable;
} DmAudioOptions;
typedef struct DmAudioEncoder DmAudioEncoder;
DmAudioOptions dm_audio_options_default(void);
const char *dm_audio_codec_name(DmAudioCodec codec);
const char *dm_audio_codec_extension(DmAudioCodec codec);
bool dm_audio_options_valid(const DmAudioOptions *options);
DmAudioEncoder *dm_audio_encoder_begin(DmOutput *output, const DmAudioOptions *options, char *error, size_t cap);
bool dm_audio_encoder_write(DmAudioEncoder *encoder, const float *stereo, int frames);
bool dm_audio_encoder_finish(DmAudioEncoder *encoder, bool commit);
#endif
