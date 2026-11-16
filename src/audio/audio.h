#ifndef DM_AUDIO_H
#define DM_AUDIO_H
#include "core/duck.h"
#include "midi_input/types.h"
#include "../../shared/spectrum.h"
#include "../../shared/audio_export.h"
#include "../../include/duckmidi_export_plugin.h"
typedef struct DmAudio DmAudio;
typedef struct DmMidiInput DmMidiInput;

typedef enum { DM_INPUT_CONTROLLERS_KEYBOARD, DM_INPUT_CONTROLLERS_SONG } DmInputControllerPolicy;

typedef struct {
    uint64_t played, dropped;
} DmAudioInputStats;

void dm_audio_attach_midi_input(DmAudio *audio, DmMidiInput *input);
void dm_audio_input_route(DmAudio *audio, int channel, bool enabled);
/* Monitor a translated event through the existing input controller policy and MIDI thru. */
void dm_audio_input_event(DmAudio *audio, int route, int part, DmMidiInputEvent event);
void dm_audio_input_controllers(DmAudio *audio, DmInputControllerPolicy policy);
void dm_audio_input_thru(DmAudio *audio, DmMidiInput *input, bool enabled);
/* Incoming key to sounding key for monitored Note On and key pressure; NULL plays keys as they
   arrive. A held key's Note Off releases the key chosen at its Note On. */
void dm_audio_input_note_map(DmAudio *audio, const uint8_t map[128]);
/* Offset in seconds, or NAN for one device buffer. */
void dm_audio_input_offset(DmAudio *audio, double seconds);
DmAudio *dm_audio_create(char *error, size_t cap);
void dm_audio_destroy(DmAudio *audio);
bool dm_audio_publish(DmAudio *audio, const DmProject *project, double seconds, bool playing,
                      char *error, size_t cap);
/* Requests a fresh asset lookup on the next publication, including missing revisions. */
void dm_audio_reload_samples(DmAudio *audio);

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
/* Moves playback to seconds on the schedule already published, without rebuilding it: the same
   reset, chase and start as a publish there. project must be the one that schedule was built
   from. Returns false, changing nothing, when there is no schedule or the project's samples
   changed; the caller then publishes. */
bool dm_audio_seek(DmAudio *audio, const DmProject *project, double seconds, bool playing);
void dm_audio_loop(DmAudio *audio, bool enabled, double start, double end);
/* Replace future events at the current audio position, retaining sounding note releases.
   pattern NULL plays the song. */
bool dm_audio_update(DmAudio *audio, const DmProject *project, const DmPatternPreview *pattern,
                     char *error, size_t cap);
/* Copy project and build its schedule on a background thread; dm_audio_update_poll publishes it
   on the GUI thread. Returns false only on allocation or thread-creation failure. */
bool dm_audio_update_async(DmAudio *audio, const DmProject *project,
                           const DmPatternPreview *pattern, uint64_t session, uint64_t revision);

/* A background build dm_audio_update_poll published: its request's revision and the project
   snapshot it was built from, whose tempo map times what now plays. The caller frees project. */
typedef struct {
    uint64_t revision;
    DmProject project;
} DmAudioPublished;

/* Poll for a completed background schedule of session, built for revision or an earlier request
   (builds finish in request order, so it is newer than what plays). Returns 1 when published
   (published, when not NULL, receives the build), 0 when nothing is ready (dropping another
   session's results), or -1 on failure (error set when known). */
int dm_audio_update_poll(DmAudio *audio, uint64_t session, uint64_t revision,
                         DmAudioPublished *published, char *error, size_t cap);
/* Drop any pending background schedule request and unpublished result. dm_audio_publish,
   dm_audio_publish_pattern and dm_audio_update call it, so a synchronous publish always wins. */
void dm_audio_update_cancel(DmAudio *audio);
void dm_audio_transport(DmAudio *audio, bool playing);
void dm_audio_panic(DmAudio *audio);
double dm_audio_position(DmAudio *audio);
/* The song position in seconds that was audible at ticks_ns (an SDL_GetTicksNS time): the
   transport clock advanced to that moment, less the input latency, as MIDI device input is
   timed. While stopped it is the stopped position. loop, when given, receives the loop pass of
   that reading, and is left unchanged when the clock could not be read. */
double dm_audio_heard_seconds(DmAudio *audio, uint64_t ticks_ns, uint32_t *loop);
/* The transport's current loop pass, as input events are stamped with it; 0 when unreadable. */
uint32_t dm_audio_loop_pass(DmAudio *audio);
bool dm_audio_playing(DmAudio *audio);
/* Internal-only edit gesture voice, isolated from song and keyboard input. */
void dm_audio_edit_preview(DmAudio *audio, int program, float pitch, bool on);
void dm_audio_edit_preview_part(DmAudio *audio, int part, int program, float pitch, bool on);
void dm_audio_preview(DmAudio *audio, int channel, int key, int velocity, bool on);
void dm_audio_audition(DmAudio *audio, int channel, int key, int velocity, double duration);
bool dm_audio_sample_audition(DmAudio *audio, const DmeContext *context, const DmeHost *host,
                              int key, bool sustained, bool looping, char *error, size_t cap);
bool dm_audio_instrument_preview(DmAudio *audio, const DmPatchReplacement *replacement, int route,
                                 char *error, size_t cap);
void dm_audio_sample_audition_stop(DmAudio *audio);
void dm_audio_sample_edit(DmAudio *audio, const char *locator, const char *set_id,
                          const char *preset_id, uint32_t region, const char *asset,
                          const DmeSampleZoneParams *params);
bool dm_audio_sample_audition_edit(DmAudio *audio, const DmeSampleZoneParams *params);
bool dm_audio_sample_audition_loop(DmAudio *audio, uint64_t start, uint64_t end);
void dm_audio_program(DmAudio *audio, int channel, int program);
void dm_audio_keyboard_program(DmAudio *audio, int channel, int program, bool drums);
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
