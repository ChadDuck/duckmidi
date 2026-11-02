#ifndef DM_AUDIO_H
#define DM_AUDIO_H
#include "duck.h"
#include "../shared/spectrum.h"
#include "../shared/audio_export.h"
typedef struct DmAudio DmAudio;
DmAudio *dm_audio_create(char *error, size_t cap);
void dm_audio_destroy(DmAudio *audio);
bool dm_audio_publish(DmAudio *audio, const DmProject *project, double seconds, bool playing,
                      char *error, size_t cap);

/* Pattern playback. The channel's song events come from the placement region starting at song
   beat origin. program -1 keeps the project's channels; 0..127 or 128 (percussion) plays only
   the channel with that instrument. With follow, the region's program changes still apply. */
typedef struct {
    int pattern, channel, program;
    bool follow;
    double origin, length;
} DmPatternPreview;

/* Builds the pattern playback schedule without publishing it. */
bool dm_pattern_schedule(const DmProject *project, const DmPatternPreview *pattern,
                         DmSchedule *schedule);
bool dm_audio_publish_pattern(DmAudio *audio, const DmProject *project,
                              const DmPatternPreview *pattern, double seconds, bool playing,
                              char *error, size_t cap);
void dm_audio_loop(DmAudio *audio, bool enabled, double start, double end);
/* Replace future events at the current audio position, retaining sounding note releases.
   pattern NULL plays the song. */
bool dm_audio_update(DmAudio *audio, const DmProject *project, const DmPatternPreview *pattern,
                     char *error, size_t cap);
void dm_audio_transport(DmAudio *audio, bool playing);
void dm_audio_panic(DmAudio *audio);
double dm_audio_position(DmAudio *audio);
bool dm_audio_playing(DmAudio *audio);
/* Internal-only edit gesture voice, isolated from song and keyboard input. */
void dm_audio_edit_preview(DmAudio *audio, int program, float pitch, bool on);
void dm_audio_preview(DmAudio *audio, int channel, int key, int velocity, bool on);
void dm_audio_audition(DmAudio *audio, int channel, int key, int velocity, double duration);
void dm_audio_program(DmAudio *audio, int channel, int program);
void dm_audio_keyboard_program(DmAudio *audio, int channel, int program);
void dm_audio_control(DmAudio *audio, int channel, int controller, int value);
float dm_audio_peak(DmAudio *audio, int channel);
/* Volume and pan each part is playing with (255 when not yet set); false when stopped. */
bool dm_audio_mix_state(DmAudio *audio, uint8_t volume[DM_PARTS], uint8_t pan[DM_PARTS]);
void dm_audio_part_control(DmAudio *audio, int port, int channel, int controller, int value);
void dm_audio_keyboard_mix(DmAudio *audio, int route, int part, int volume, int pan);
void dm_audio_analyzer_enable(DmAudio *audio, bool enabled);
bool dm_audio_analyzer_read(DmAudio *audio, float *stereo);
bool dm_audio_soundfont(DmAudio *audio, const char *path, char *error, size_t cap);
bool dm_audio_busy(DmAudio *audio);
bool dm_audio_poll(DmAudio *audio, char *message, size_t cap, bool *success, int *kind);
void dm_audio_cancel(DmAudio *audio);
bool dm_audio_retry(DmAudio *audio, char *error, size_t cap);
int dm_audio_buffer_frames(DmAudio *audio);
double dm_audio_buffer_ms(DmAudio *audio);
const char *dm_audio_driver(DmAudio *audio);
int dm_audio_device_count(DmAudio *audio);
void dm_audio_device_name(DmAudio *audio, int index, char *name, size_t cap);
bool dm_audio_device(DmAudio *audio, int index, char *error, size_t cap);
void dm_audio_metronome(DmAudio *audio, bool enabled, double bpm);
void dm_audio_monitor(DmAudio *audio, bool enabled);
void dm_audio_recording(DmAudio *audio, bool enabled);
bool dm_audio_export(DmAudio *audio, const DmProject *project, const char *path,
                     const DmAudioOptions *options, char *error, size_t cap);
bool dm_audio_exporting(DmAudio *audio);
int dm_audio_export_progress(DmAudio *audio);
#endif
