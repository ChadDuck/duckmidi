#include "editor_internal.h"

/* Multi-port import and port combination, checked through the editor with a generated file. */
void test_editor_combine(Editor *e, const char *label);
static int checks;

static void check(bool ok, const char *label) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL port UI: %s\n", label);
        exit(1);
    }
}

static void draw(Editor *e) {
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

static void put(DmBytes *b, const uint8_t *data, size_t size) {
    check(dm_bytes(b, data, size), "fixture memory");
}

static void track(DmBytes *file, const uint8_t *events, size_t size) {
    uint8_t header[] = {'M', 'T', 'r', 'k', 0, 0, (uint8_t)(size >> 8), (uint8_t)size};
    put(file, header, sizeof(header));
    put(file, events, size);
}

/* Port 0 plays channel 1 with program 10. Port 1 plays channel 1 with program 20 and channel 2.
   Channel 1 therefore collides when the ports are combined. */
static void write_fixture(const char *path) {
    DmBytes file = {0};
    const uint8_t header[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 3, 0x03, 0xc0};
    const uint8_t conductor[] = {0, 0xff, 0x51, 3, 7, 0xa1, 0x20, 0, 0xff, 0x2f, 0};
    const uint8_t first[] = {0,   0xff, 0x21, 1,    0,  0, 0xc0, 10,   0,    0x90, 60,
                             100, 0x87, 0x40, 0x80, 60, 0, 0,    0xff, 0x2f, 0};
    const uint8_t second[] = {0,  0xff, 0x21, 1,    1,  0,  0xc0, 20,   0,    0x90,
                              64, 100,  0,    0x91, 48, 90, 0x87, 0x40, 0x80, 64,
                              0,  0,    0x81, 48,   0,  0,  0xff, 0x2f, 0};
    put(&file, header, sizeof(header));
    track(&file, conductor, sizeof(conductor));
    track(&file, first, sizeof(first));
    track(&file, second, sizeof(second));
    char error[256];
    check(dm_write(path, file.data, file.size, error, sizeof(error)), "write fixture");
    dm_bytes_free(&file);
}

static int notes_on_port(Editor *e, unsigned port) {
    int count = 0;

    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        for (size_t n = 0; n < e->project.patterns[i].count; ++n) {
            count += e->project.patterns[i].notes[n].port == port;
        }
    }

    return count;
}

static bool port_program(Editor *e, unsigned port, int program) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256];
    bool found = false;
    check(dm_export_timeline(&e->project, &events, &count, &duration, error, sizeof(error)),
          "timeline");

    for (size_t i = 0; i < count; ++i) {
        found |=
            events[i].status == 0xc0 && events[i].port == port && events[i].a == (uint32_t)program;
    }

    free(events);
    return found;
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Port import validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "actual UI");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", "", true);
    const char *path = "agents/reports/2026-10-30-run/two-ports.mid";
    write_fixture(path);
    ed_load(e, path);

    check(e->project.port == DM_PORT_COMBINED && e->project.port_count == 2,
          "a multi-port file opens as one arrangement");
    check(notes_on_port(e, 0) == 1 && notes_on_port(e, 1) == 2,
          "each source port's notes use their own note port");
    check(e->project.clip_count == 2, "channel 1 keeps one placement holding both ports' notes");
    check(port_program(e, 0, 10) && port_program(e, 1, 20),
          "each port keeps its own opening program");

    e->config = true;
    draw(e);
    draw(e);
    test_editor_combine(e, "Combine original source ports");
    check(e->combine_request == 1 && strstr(e->combine_channels, "1"),
          "combining warns about channel 1, used by both ports");
    e->combine_request = 0;
    ed_combine_source_ports(e, false);
    check(notes_on_port(e, 1) == 0 && notes_on_port(e, 0) == 3 && e->project.port_count == 1,
          "combining moves the original ports to the default port");
    ed_undo(e, false);
    check(notes_on_port(e, 1) == 2, "combining can be undone");

    /* A port the user colored is only merged by Combine ALL source ports. */
    e->project.patterns[0].notes[0].port = 5;
    ed_combine_source_ports(e, false);
    check(notes_on_port(e, 5) == 1, "original combine keeps user-colored ports");
    ed_combine_source_ports(e, true);
    check(notes_on_port(e, 5) == 0 && notes_on_port(e, 0) == 3, "combine all merges every port");

    printf("PASS port UI: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
