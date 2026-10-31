#include "../shared/audio_export.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A non-frame-aligned stereo signal exercises buffering, padding and flushing. */
int main(int argc, char **argv) {
    assert(argc == 2);
    char path[DM_PATH], error[512];

    for (int codec = 0; codec < DM_AUDIO_CODECS; ++codec) {
        for (int variant = 0; variant < 2; ++variant) {
            DmAudioOptions options = dm_audio_options_default();
            options.codec = (DmAudioCodec)codec;

            if (codec == DM_AUDIO_WAV || codec == DM_AUDIO_FLAC) {
                options.bits = variant ? 24 : 16;
            } else {
                options.variable = variant != 0;
            }

            snprintf(path, sizeof(path), "%s/codec-%d-%d.%s", argv[1], codec, variant,
                     dm_audio_codec_extension(options.codec));
            error[0] = 0;
            DmOutput output;
            assert(dm_output_begin(&output, path, error, sizeof(error)));
            DmAudioEncoder *encoder =
                dm_audio_encoder_begin(&output, &options, error, sizeof(error));

            if (!encoder) {
                fprintf(stderr, "%s: %s\n", path, error);
            }

            assert(encoder);

            for (int start = 0; start < 52817;) {
                float stereo[514];
                int frames = 52817 - start;

                if (frames > 257) {
                    frames = 257;
                }

                for (int i = 0; i < frames; ++i) {
                    double t = (start + i) / 48000.0;
                    stereo[2 * i] = (float)(0.25 * sin(6.283185307179586 * 440 * t));
                    stereo[2 * i + 1] = (float)(0.125 * sin(6.283185307179586 * 660 * t));
                }

                assert(dm_audio_encoder_write(encoder, stereo, frames));
                start += frames;
            }

            bool ok = dm_audio_encoder_finish(encoder, true);

            if (!ok) {
                fprintf(stderr, "%s: %s\n", path, error);
            }

            assert(ok && dm_output_finish(&output, true, error, sizeof(error)));
            DmBytes bytes = {0};
            assert(dm_read(path, &bytes, error, sizeof(error)) && bytes.size > 64);
            dm_bytes_free(&bytes);
            printf("PASS %s\n", path);
        }
    }

    /* Aborting an export must discard its temporary output. */
    snprintf(path, sizeof(path), "%s/cancelled.wav", argv[1]);
    DmOutput output;
    error[0] = 0;
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    DmAudioOptions options = dm_audio_options_default();
    DmAudioEncoder *encoder = dm_audio_encoder_begin(&output, &options, error, sizeof(error));
    assert(encoder);
    dm_audio_encoder_finish(encoder, false);
    dm_output_finish(&output, false, error, sizeof(error));
    FILE *file = fopen(output.temporary, "rb");
    assert(!file);
    file = fopen(path, "rb");
    assert(!file);
    puts("PASS cancellation removes the temporary output");
    return 0;
}
