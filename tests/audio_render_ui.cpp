/* Drives the Export window's audio render: codec controls, progress and cancellation. */
#include "test_ui.h"
#include <cassert>

/* Canceling a render leaves the file that was already at the destination untouched. */
extern "C" void test_audio_render_ui(Editor *e) {
    e->export_window = true;
    e->export_type = -1;
    e->audio_options.codec = DM_AUDIO_MP3;
    e->audio_options.variable = true;
    frame(e);
    frame(e);
    capture(e, "agents/reports/audio-render-codecs.bmp");
    auto *export_window = ImGui::FindWindowByName("Export");
    assert(export_window && export_window->Active);
    e->project.patterns[0].length = 600;
    char error[512];
    const char *path = "agents/reports/audio-render/cancel.wav";
    assert(dm_write(path, "sentinel", 8, error, sizeof(error)));
    e->audio_options = dm_audio_options_default();
    assert(dm_audio_export(e->audio, &e->project, path, &e->audio_options, error, sizeof(error)));
    assert(dm_audio_exporting(e->audio));
    frame(e);
    capture(e, "agents/reports/audio-render-progress.bmp");
    dm_audio_cancel(e->audio);
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(e->audio) && SDL_GetTicks() - start < 15000) {
        bool success;
        int kind;
        dm_audio_poll(e->audio, error, sizeof(error), &success, &kind);
        SDL_Delay(1);
    }

    assert(!dm_audio_exporting(e->audio));
    DmBytes file = {0};
    assert(dm_read(path, &file, error, sizeof(error)) && file.size == 8 &&
           !std::memcmp(file.data, "sentinel", 8));
    dm_bytes_free(&file);
    frame(e);
}
