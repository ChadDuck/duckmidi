#include "import.h"
#include "../../src/core/duck.h"
#include "../../src/text/en.h"
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MAX_BYTES (256 * 1024 * 1024)
#define SAMPLE_MAX_VALUES (16 * 1024 * 1024)
#define SAMPLE_MAX_FRAME 65536
#define SAMPLE_IO_BUFFER 32768
#define SAMPLE_METADATA_CHUNKS 1024
#define SAMPLE_FORMATS "wav,mp3,mov,mp4,m4a,3gp,3g2,mj2,ogg,flac,aiff"

typedef struct {
    SDL_IOStream *file;
    int64_t size, reported;
    const DmSampleImportCallbacks *callbacks;
    DmSampleAudio audio;
    size_t capacity;
    AVChannelLayout layout;
    enum AVSampleFormat input_format;
    SwrContext *convert;
    AVFormatContext *format;
    AVIOContext *io;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
} SampleReader;

static int canceled(void *user) {
    const SampleReader *reader = user;
    const DmSampleImportCallbacks *cb = reader->callbacks;
    return cb && cb->canceled && cb->canceled(cb->user);
}

static int read_pcm_file(void *user, uint8_t *data, int size) {
    SampleReader *reader = user;

    if (canceled(reader)) {
        return AVERROR_EXIT;
    }

    int64_t offset = SDL_TellIO(reader->file);

    if (offset < 0 || offset > reader->size) {
        return AVERROR(EIO);
    }

    size_t bytes = SDL_ReadIO(reader->file, data, (size_t)SDL_min(size, reader->size - offset));

    if (!bytes) {
        SDL_IOStatus status = SDL_GetIOStatus(reader->file);
        return status == SDL_IO_STATUS_EOF || offset == reader->size ? AVERROR_EOF : AVERROR(EIO);
    }

    const DmSampleImportCallbacks *cb = reader->callbacks;
    int64_t end = offset + (int64_t)bytes;

    if (cb && cb->progress && end > reader->reported) {
        reader->reported = end;
        cb->progress(cb->user, (uint64_t)end, (uint64_t)reader->size);
    }

    return (int)bytes;
}

static int64_t seek_pcm_file(void *user, int64_t offset, int whence) {
    SampleReader *reader = user;
    whence &= ~AVSEEK_FORCE;

    if (canceled(reader)) {
        return AVERROR_EXIT;
    }

    if (whence == AVSEEK_SIZE) {
        return reader->size;
    }

    int64_t base = whence == SEEK_CUR   ? SDL_TellIO(reader->file)
                   : whence == SEEK_END ? reader->size
                                        : 0;

    if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || base < 0 ||
        offset < -base || offset > reader->size - base) {
        return AVERROR(EINVAL);
    }

    int64_t position = SDL_SeekIO(reader->file, base + offset, SDL_IO_SEEK_SET);
    return position >= 0 ? position : AVERROR(EIO);
}

static int deny_external_io(AVFormatContext *ctx, AVIOContext **pb, const char *url, int flags,
                            AVDictionary **options) {
    (void)ctx;
    (void)pb;
    (void)url;
    (void)flags;
    (void)options;
    return AVERROR(EACCES);
}

static void read_metadata(SampleReader *reader) {
    uint8_t header[12];

    if (SDL_ReadIO(reader->file, header, sizeof(header)) != sizeof(header)) {
        return;
    }

    bool wave = !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4);
    bool aiff = !memcmp(header, "FORM", 4) &&
                (!memcmp(header + 8, "AIFF", 4) || !memcmp(header + 8, "AIFC", 4));

    if (!wave && !aiff) {
        return;
    }

    int64_t end = 8 + (int64_t)(wave ? AV_RL32(header + 4) : AV_RB32(header + 4));
    end = SDL_min(end, reader->size);

    int64_t pos = 12;

    for (unsigned i = 0; i < SAMPLE_METADATA_CHUNKS && pos <= end - 8 && !canceled(reader); ++i) {
        uint8_t chunk[8], instrument[36];

        if (SDL_SeekIO(reader->file, pos, SDL_IO_SEEK_SET) != pos ||
            SDL_ReadIO(reader->file, chunk, sizeof(chunk)) != sizeof(chunk)) {
            break;
        }

        uint32_t bytes = wave ? AV_RL32(chunk + 4) : AV_RB32(chunk + 4);
        pos += 8;

        if (bytes > end - pos) {
            break;
        }

        if (wave && !memcmp(chunk, "smpl", 4) && bytes >= sizeof(instrument) &&
            SDL_ReadIO(reader->file, instrument, sizeof(instrument)) == sizeof(instrument)) {
            uint32_t key = AV_RL32(instrument + 12);

            if (key < 128) {
                reader->audio.embedded_root = (int)key;
                reader->audio.embedded_cents = AV_RL32(instrument + 16) * (100.0 / 4294967296.0);
            }

            uint32_t loops = AV_RL32(instrument + 28);

            /* Only complete forward, infinite, integral loop records are representable. */
            if (loops && loops <= (bytes - 36) / 24) {
                for (uint32_t loop = 0; loop < loops && !canceled(reader); ++loop) {
                    uint8_t record[24];

                    if (SDL_ReadIO(reader->file, record, sizeof(record)) != sizeof(record)) {
                        break;
                    }

                    uint64_t start = AV_RL32(record + 8);
                    uint64_t end = (uint64_t)AV_RL32(record + 12) + 1;

                    if (!AV_RL32(record + 4) && !AV_RL32(record + 16) && !AV_RL32(record + 20) &&
                        start < end && end - start >= 2) {
                        reader->audio.embedded_loop_start = start;
                        reader->audio.embedded_loop_end = end;
                        break;
                    }
                }
            }
        }

        if (wave && !memcmp(chunk, "inst", 4) && bytes >= 7 &&
            SDL_ReadIO(reader->file, instrument, 7) == 7) {
            int correction = (int8_t)instrument[1];

            if (instrument[0] < 128 && correction >= -50 && correction <= 50 &&
                (reader->audio.embedded_root < 0 ||
                 (reader->audio.embedded_root == instrument[0] && !reader->audio.embedded_cents))) {
                reader->audio.embedded_root = instrument[0];
                reader->audio.embedded_cents = -correction;
            }
        }

        if (aiff && !memcmp(chunk, "INST", 4) && bytes >= 20 &&
            SDL_ReadIO(reader->file, instrument, 2) == 2) {
            int cents = (int8_t)instrument[1];

            if (instrument[0] < 128 && cents >= -50 && cents <= 50) {
                reader->audio.embedded_root = instrument[0];
                reader->audio.embedded_cents = cents;
            }

            break;
        }

        pos += (int64_t)bytes + (bytes & 1u);
    }
}

static bool supported_codec(enum AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_PCM_S8:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F32BE:
    case AV_CODEC_ID_PCM_F64LE:
    case AV_CODEC_ID_PCM_F64BE:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_OPUS:
    case AV_CODEC_ID_FLAC:
        return true;
    default:
        return false;
    }
}

static int append_frame(SampleReader *reader, const AVFrame *frame) {
    DmSampleAudio *audio = &reader->audio;

    if (frame->sample_rate != audio->rate || frame->ch_layout.nb_channels != audio->channels ||
        frame->nb_samples <= 0 || frame->nb_samples > SAMPLE_MAX_FRAME) {
        return AVERROR_INVALIDDATA;
    }

    if (!reader->convert) {
        av_channel_layout_default(&reader->layout, audio->channels);
        reader->input_format = frame->format;
        int result =
            swr_alloc_set_opts2(&reader->convert, &reader->layout, AV_SAMPLE_FMT_FLT, audio->rate,
                                &reader->layout, frame->format, audio->rate, 0, NULL);

        if (result < 0 || (result = swr_init(reader->convert)) < 0) {
            return result;
        }
    }

    if (frame->format != (int)reader->input_format) {
        return AVERROR_INVALIDDATA;
    }

    size_t channels = (size_t)audio->channels;
    size_t frames = audio->frames + (size_t)frame->nb_samples;

    if (frames > SAMPLE_MAX_VALUES / channels) {
        return AVERROR(EFBIG);
    }

    if (!dm_reserve((void **)&audio->pcm, &reader->capacity, frames * channels, sizeof(float))) {
        return AVERROR(ENOMEM);
    }

    float *pcm = audio->pcm + audio->frames * channels;
    uint8_t *output = (uint8_t *)pcm;
    int count = swr_convert(reader->convert, &output, frame->nb_samples,
                            (const uint8_t **)frame->extended_data, frame->nb_samples);

    if (count != frame->nb_samples) {
        return count < 0 ? count : AVERROR_INVALIDDATA;
    }

    for (size_t i = 0; i < (size_t)count * channels; ++i) {
        pcm[i] = isfinite(pcm[i]) ? SDL_clamp(pcm[i], -1.0f, 1.0f) : 0.0f;
    }

    audio->frames += (size_t)count;
    return 0;
}

static int decode_packet(SampleReader *reader, const AVPacket *packet) {
    int result = avcodec_send_packet(reader->codec, packet);

    if (result < 0) {
        return result;
    }

    while ((result = avcodec_receive_frame(reader->codec, reader->frame)) >= 0) {
        if (canceled(reader)) {
            return AVERROR_EXIT;
        }

        result = append_frame(reader, reader->frame);
        av_frame_unref(reader->frame);

        if (result < 0) {
            return result;
        }
    }

    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

static const char *decode_stream(SampleReader *reader, int stream, int *result) {
    AVPacket *packet = reader->packet;

    while (!canceled(reader) && (*result = av_read_frame(reader->format, packet)) >= 0) {
        if (packet->stream_index == stream) {
            *result = packet->flags & AV_PKT_FLAG_CORRUPT ? AVERROR_INVALIDDATA
                                                          : decode_packet(reader, packet);
        }

        av_packet_unref(packet);

        if (*result < 0) {
            break;
        }
    }

    if (*result == AVERROR_EOF) {
        *result = decode_packet(reader, NULL);
    }

    if (*result < 0) {
        return *result == AVERROR(EFBIG) ? DM_TEXT_SAMPLE_IMPORT_LIMIT
                                         : DM_TEXT_SAMPLE_IMPORT_DECODE;
    }

    return reader->audio.frames ? NULL : DM_TEXT_SAMPLE_IMPORT_EMPTY;
}

static const char *read_file(SampleReader *reader, const char *path, int *result) {
    if (canceled(reader)) {
        return DM_TEXT_SAMPLE_IMPORT_CANCELED;
    }

    reader->size = SDL_GetIOSize(reader->file);

    if (reader->size <= 0 || reader->size > SAMPLE_MAX_BYTES) {
        return reader->size > 0 ? DM_TEXT_SAMPLE_IMPORT_LIMIT : DM_TEXT_SAMPLE_IMPORT_FILE;
    }

    read_metadata(reader);

    if (SDL_SeekIO(reader->file, 0, SDL_IO_SEEK_SET) != 0) {
        return DM_TEXT_SAMPLE_IMPORT_FILE;
    }

    uint8_t *buffer = av_malloc(SAMPLE_IO_BUFFER);

    if (buffer) {
        reader->io = avio_alloc_context(buffer, SAMPLE_IO_BUFFER, 0, reader, read_pcm_file, NULL,
                                        seek_pcm_file);
    }

    if (!reader->io) {
        av_free(buffer);
        return DM_TEXT_SAMPLE_IMPORT_MEMORY;
    }

    reader->format = avformat_alloc_context();

    if (!reader->format) {
        return DM_TEXT_SAMPLE_IMPORT_MEMORY;
    }

    AVFormatContext *ctx = reader->format;
    ctx->pb = reader->io;
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    ctx->io_open = deny_external_io;
    ctx->interrupt_callback = (AVIOInterruptCB){canceled, reader};
    ctx->max_streams = 16;
    ctx->probesize = 1024 * 1024;
    AVDictionary *options = NULL;
    *result = av_dict_set(&options, "format_whitelist", SAMPLE_FORMATS, 0);

    if (*result >= 0) {
        *result = avformat_open_input(&reader->format, path, NULL, &options);
    }

    av_dict_free(&options);

    ctx = reader->format;

    if (*result < 0 || (*result = avformat_find_stream_info(ctx, NULL)) < 0) {
        return DM_TEXT_SAMPLE_IMPORT_FORMAT;
    }

    int index = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if (index < 0) {
        *result = index;
        return DM_TEXT_SAMPLE_IMPORT_CODEC;
    }

    const AVStream *stream = ctx->streams[index];
    const AVCodecParameters *params = stream->codecpar;
    const AVCodec *decoder =
        supported_codec(params->codec_id) ? avcodec_find_decoder(params->codec_id) : NULL;

    if (!decoder) {
        return DM_TEXT_SAMPLE_IMPORT_CODEC;
    }

    if (params->sample_rate < 8000 || params->sample_rate > 192000 ||
        params->ch_layout.nb_channels < 1 || params->ch_layout.nb_channels > 2) {
        return DM_TEXT_SAMPLE_IMPORT_LAYOUT;
    }

    reader->codec = avcodec_alloc_context3(decoder);
    reader->packet = av_packet_alloc();
    reader->frame = av_frame_alloc();
    AVCodecContext *codec = reader->codec;

    if (!codec || !reader->packet || !reader->frame) {
        return DM_TEXT_SAMPLE_IMPORT_MEMORY;
    }

    *result = avcodec_parameters_to_context(codec, params);

    if (*result >= 0) {
        codec->max_samples = 2 * SAMPLE_MAX_FRAME;
        codec->thread_count = 1;
        codec->pkt_timebase = stream->time_base;
        *result = avcodec_open2(codec, decoder, NULL);
    }

    if (*result < 0) {
        return DM_TEXT_SAMPLE_IMPORT_CODEC;
    }

    DmSampleAudio *audio = &reader->audio;
    audio->rate = params->sample_rate;
    audio->channels = params->ch_layout.nb_channels;
    snprintf(audio->format, sizeof(audio->format), "%s", ctx->iformat->name);
    snprintf(audio->codec, sizeof(audio->codec), "%s", decoder->name);
    return decode_stream(reader, index, result);
}

bool dm_sample_decode_stream(SDL_IOStream *stream, const char *name,
                             const DmSampleImportCallbacks *callbacks, DmSampleAudio *out,
                             char *error, size_t cap) {
    SampleReader reader = {.file = stream, .callbacks = callbacks, .audio.embedded_root = -1};
    int result = 0;
    const char *failure = read_file(&reader, name, &result);

    if (canceled(&reader)) {
        failure = DM_TEXT_SAMPLE_IMPORT_CANCELED;
        result = 0;
    }

    av_frame_free(&reader.frame);
    av_packet_free(&reader.packet);
    avcodec_free_context(&reader.codec);
    avformat_close_input(&reader.format);

    if (reader.io) {
        av_freep(&reader.io->buffer);
        avio_context_free(&reader.io);
    }

    swr_free(&reader.convert);
    av_channel_layout_uninit(&reader.layout);

    if (failure) {
        dm_sample_audio_free(&reader.audio);
        char detail[AV_ERROR_MAX_STRING_SIZE];

        if (result < 0 && av_strerror(result, detail, sizeof(detail)) == 0) {
            snprintf(error, cap, "%s: %s", failure, detail);
        } else {
            snprintf(error, cap, "%s", failure);
        }

        return false;
    }

    if (reader.audio.embedded_loop_end > reader.audio.frames) {
        reader.audio.embedded_loop_start = reader.audio.embedded_loop_end = 0;
    }

    *out = reader.audio;
    return true;
}

bool dm_sample_decode(const char *path, const DmSampleImportCallbacks *callbacks,
                      DmSampleAudio *out, char *error, size_t cap) {
    SDL_PathInfo info;
    SDL_IOStream *stream;

    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE ||
        !(stream = SDL_IOFromFile(path, "rb"))) {
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_IMPORT_FILE);
        return false;
    }

    bool ok = dm_sample_decode_stream(stream, path, callbacks, out, error, cap);
    SDL_CloseIO(stream);
    return ok;
}

void dm_sample_audio_free(DmSampleAudio *audio) {
    free(audio->pcm);
    *audio = (DmSampleAudio){.embedded_root = -1};
}
