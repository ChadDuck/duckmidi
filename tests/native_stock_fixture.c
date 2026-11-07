/* Build against either registration header to compare emitted stock bytes across an ABI change. */
#include "duckmidi_export_plugin.h"
#include "duckmidi_midi.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *destination;

static int32_t DME_CALL cancelled(void *user) {
    (void)user;
    return 0;
}

static void DME_CALL progress(void *user, uint32_t value) {
    (void)user;
    (void)value;
}

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t flags, const char *text) {
    (void)user;
    (void)severity;
    (void)flags;
    (void)text;
}

static int32_t DME_CALL artifact(void *user, const char *name, const uint8_t *bytes,
                                 uint64_t size) {
    (void)user;
    char path[4096], parent[4096];
    assert(!strstr(name, ".."));
    assert(SDL_snprintf(path, sizeof(path), "%s/%s", destination, name) < (int)sizeof(path));
    SDL_snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    assert(slash);
    *slash = 0;
    assert(SDL_CreateDirectory(parent) && SDL_SaveFile(path, bytes, (size_t)size));
    return 1;
}

int main(int argc, char **argv) {
    assert((argc == 3 || argc == 4) && SDL_Init(0));
    destination = argv[2];
    SDL_SharedObject *object = SDL_LoadObject(argv[1]);
    assert(object);
    DmePluginEntry entry = (DmePluginEntry)SDL_LoadFunction(object, "duckmidi_export_plugin");
    assert(entry);
    const DmePlugin *plugin = entry(DME_ABI);
    assert(plugin && plugin->exporter_count == 1);
    const DmeExporter *api = plugin->exporter(0);
    void *instance = api->create();
    assert(instance);
    DmeEvent events[] = {{0, 1, MIDI_CONTROL, MIDI_CC_PAN, 64, 0},
                         {0, 2, MIDI_NOTE_ON, 60, 100, 0},
                         {96, 3, MIDI_NOTE_OFF, 60, 0, 0},
                         {96, 4, MIDI_NOTE_ON, 64, 100, 0},
                         {192, 5, MIDI_NOTE_OFF, 64, 0, 0},
                         {192, 6, MIDI_NOTE_ON | MIDI_DRUM_CHANNEL, 42, 90, 0},
                         {288, 7, MIDI_NOTE_OFF | MIDI_DRUM_CHANNEL, 42, 0, 0}};
    DmeContext context = {.events = events,
                          .event_count = 7,
                          .duration = 384,
                          .ppq = 96,
                          .percussion_channels = 1u << MIDI_DRUM_CHANNEL};
    DmeHost host = {.cancelled = cancelled,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = artifact};
    char error[4096] = {0};
    assert(api->inspect(instance, &context, error, sizeof(error)) == DME_OK);
    if (argc == 4) {
        assert(api->set(instance, "target", &(DmeValue){DME_CHOICE, 1}, error, sizeof(error)) ==
               DME_OK);
    }
    void *job = api->create_job(instance, error, sizeof(error));
    assert(job && api->run(job, &context, &host, error, sizeof(error)) == DME_OK);
    api->destroy_job(job);
    api->destroy(instance);
    SDL_UnloadObject(object);
    SDL_Quit();
    return 0;
}
