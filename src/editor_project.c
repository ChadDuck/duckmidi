#include "editor_internal.h"
#include "text/en.h"

uint32_t ed_dialog_event;
static void SDLCALL dialog_callback(void *userdata, const char *const *files, int filter);
static void reset_regions(Editor *e);

void ed_add_recent(Editor *e, const char *path) {
    int at = -1;

    for (int i = 0; i < e->recent_count; ++i) {
        if (!strcmp(path, e->recent[i])) {
            at = i;
        }
    }

    if (at < 0) {
        at = e->recent_count < 10 ? e->recent_count++ : 9;
    }

    for (int i = at; i > 0; --i) {
        memcpy(e->recent[i], e->recent[i - 1], DM_PATH);
    }

    snprintf(e->recent[0], DM_PATH, "%s", path);
}

uint32_t ed_shortcut_schema(void) {
    Editor defaults_editor = {0};
    ed_defaults(&defaults_editor);
    uint32_t hash = 2166136261u;

    for (int i = 0; i < ACT_COUNT; ++i) {
        hash = (hash ^ defaults_editor.bindings[i].key) * 16777619u;
        hash = (hash ^ defaults_editor.bindings[i].mods) * 16777619u;

        for (const char *s = ed_action_names[i]; *s; ++s) {
            hash = (hash ^ (unsigned char)*s) * 16777619u;
        }
    }

    return hash;
}

void ed_preference_save(Editor *e) {
    if (e->smoke) {
        return;
    }

    if (!dm_preferences_save(e, ed_shortcut_schema(), ed_action_names)) {
        ed_message(e, DM_TEXT_COULD_SAVE_CONFIGURATION);
    }
}

void ed_preference_load(Editor *e) {
    SDL_PathInfo info;

    if (!SDL_GetPathInfo(e->settings_path, &info)) {
        ed_preference_save(e);
        return;
    }

    if (!dm_preferences_load(e, ed_shortcut_schema())) {
        return;
    }

    for (int i = 0; i < ACT_COUNT; ++i) {
        if (e->bindings[i].mods != ed_normalized(e->bindings[i].mods) ||
            ed_reserved(e->bindings[i].key, e->bindings[i].mods)) {
            ed_defaults(e);
            ed_message(e, DM_TEXT_INVALID_SHORTCUT_SETTINGS);
            break;
        }
    }
}

static void SDLCALL dialog_callback(void *userdata, const char *const *files, int filter) {
    (void)filter;
    SDL_Event event;
    SDL_zero(event);
    event.type = ed_dialog_event;
    event.user.code = (int)(intptr_t)userdata;

    if (files && files[0]) {
        event.user.data1 = SDL_strdup(files[0]);
    } else if (!files) {
        event.user.data2 = SDL_strdup(SDL_GetError());
    }

    SDL_PushEvent(&event);
}

void ed_dialog(Editor *e, int kind) {
    if (e->dialog_busy) {
        return;
    }

    e->dialog_busy = true;
    static const SDL_DialogFileFilter open_filters[] = {
        {DM_TEXT_MIDI_DUCKMIDI_PROJECT, "mid;midi;rmi;mus;duck"}, {DM_TEXT_FILES, "*"}};
    static const SDL_DialogFileFilter save_filters[] = {{DM_TEXT_DUCKMIDI_PROJECT, "duck"}},
                                      mid_filters[] = {{DM_TEXT_STANDARD_MIDI_FILE, "mid"}},
                                      font_filters[] = {{DM_TEXT_SOUNDFONT, "sf2;sf3"}};
    static const SDL_DialogFileFilter audio_filters[] = {
        {DM_TEXT_WAV_PCM, "wav"},    {"MP3", "mp3"},   {DM_TEXT_AAC_M4A, "m4a"},
        {DM_TEXT_OGG_VORBIS, "ogg"}, {"Opus", "opus"}, {"FLAC", "flac"}};

    if (kind == DIALOG_OPEN) {
        SDL_ShowOpenFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window, open_filters, 2,
                               *e->midi_folder ? e->midi_folder : NULL, false);
    } else if (kind == DIALOG_FONT) {
        SDL_ShowOpenFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window, font_filters, 1,
                               *e->font_folder ? e->font_folder : NULL, false);
    } else if (kind == DIALOG_REPOSITORY) {
        /* The system folder picker lists every location Explorer shows, including WSL. */
        const char *start;

        if (!ed_export_repository_picker(e, &start)) {
            e->dialog_busy = false;
            return;
        }

        SDL_ShowOpenFolderDialog(dialog_callback, (void *)(intptr_t)kind, e->window, start, false);
    } else if (kind == DIALOG_EXPORT_PLUGIN) {
        const SDL_DialogFileFilter *filter;
        const char *suggested;

        if (!ed_export_picker(e, &filter, &suggested)) {
            e->dialog_busy = false;
            return;
        }

        SDL_ShowSaveFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window, filter, 1,
                               suggested);
    } else {
        const char *suggested = kind == DIALOG_SAVE && *e->path ? e->path : NULL;

        if (kind != DIALOG_SAVE) {
            const char *extension =
                kind == DIALOG_MIDI ? "mid" : dm_audio_codec_extension(e->audio_options.codec);

            if (!ed_export_suggest(e, extension, e->dialog_suggested,
                                   sizeof(e->dialog_suggested))) {
                e->dialog_busy = false;
                ed_message(e, DM_TEXT_SUGGESTED_EXPORT_PATH_TOO);
                return;
            }

            suggested = e->dialog_suggested;
        }

        SDL_ShowSaveFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window,
                               kind == DIALOG_SAVE   ? save_filters
                               : kind == DIALOG_MIDI ? mid_filters
                                                     : &audio_filters[e->audio_options.codec],
                               1, suggested);
    }
}

void ed_save(Editor *e, bool as) {
    if (ed_save_busy(e)) {
        ed_message(e, DM_TEXT_PROJECT_SAVE_ALREADY_RUNNING);
        return;
    }

    if (!e->active) {
        return;
    }

    if (as || !*e->path) {
        ed_dialog(e, DIALOG_SAVE);
    } else {
        if (!ed_save_to(e, e->path) && e->pending_action) {
            e->confirm = true;
        }
    }
}

void ed_open_audio_location(Editor *e) {
    if (!*e->audio_exported) {
        return;
    }

    char path[DM_PATH], url[DM_PATH * 3 + 16];
    const char *source = e->audio_exported;
    bool absolute = source[0] == '/' || source[0] == '\\' || (source[0] && source[1] == ':');
    char *cwd = absolute ? NULL : SDL_GetCurrentDirectory();

    if (!absolute && !cwd) {
        ed_message(e, SDL_GetError());
        return;
    }

    int size = snprintf(path, sizeof(path), "%s%s", cwd ? cwd : "", source);
    SDL_free(cwd);

    if (size < 0 || size >= (int)sizeof(path)) {
        ed_message(e, DM_TEXT_EXPORT_FOLDER_PATH_TOO);
        return;
    }

    for (char *p = path; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }

    char *slash = strrchr(path, '/');

    if (!slash) {
        ed_message(e, DM_TEXT_CANNOT_LOCATE_EXPORT_FOLDER);
        return;
    }

    slash[1] = 0;
    const char *prefix = path[0] == '/' ? (path[1] == '/' ? "file:" : "file://") : "file:///";
    size_t at = (size_t)snprintf(url, sizeof(url), "%s", prefix);
    static const char hex[] = "0123456789ABCDEF";

    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            strchr("/-_.~:", *p)) {
            url[at++] = (char)*p;
        } else {
            url[at++] = '%';
            url[at++] = hex[*p >> 4];
            url[at++] = hex[*p & 15];
        }
    }

    url[at] = 0;

    if (!SDL_OpenURL(url)) {
        ed_message(e, SDL_GetError());
    }
}

static void reset_regions(Editor *e) {
    ++e->export_revision;
    ++e->project_session;
    ed_export_reset_results(e);
    e->midi_events = false;
    e->event_ruler_drag = false;
    e->midi_error[0] = 0;
    e->midi_event_index = -1;
    e->preview_override = false;
    e->preview_source_channel = -1;
    e->channel_selected = true;
    e->curve_drag = e->event_marquee = false;
    e->wait_armed = e->take_started = false;
    e->event_selection.size = 0;
    e->selected_clip = -1;
    e->playlist_context_clip = -1;
    e->lfo_center_drag = false;
    memset(e->loop_enabled, 0, sizeof(e->loop_enabled));
    memset(e->channel_expanded, 0, sizeof(e->channel_expanded));
    e->loop_drag = e->lane_settings = e->automation_browser = e->lfo_editing = false;
    dm_audio_loop(e->audio, false, 0, 0);
}

void ed_new_project(Editor *e) {
    reset_regions(e);
    e->pattern_mode = false;
    e->automation_cc = -2;
    e->edit_focus = FOCUS_NONE;
    e->roll_auto_hidden = e->automation_all = false;
    e->pattern_highlight = e->erase_active = false;
    e->pan_context = 0;
    e->last_automation_valid = false;
    e->automation_node = -1;
    e->context_note = -1;
    ed_release_piano(e);
    e->record = false;
    e->recording_take = e->take_snapshot = false;
    dm_audio_recording(e->audio, false);
    dm_audio_transport(e->audio, false);
    char font[DM_PATH];
    snprintf(font, sizeof(font), "%s", e->project.soundfont);
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    snprintf(e->project.soundfont, sizeof(e->project.soundfont), "%s", font);
    memcpy(e->reset_channels, e->project.channels, sizeof(e->reset_channels));
    memset(e->start_beat, 0, sizeof(e->start_beat));
    memset(e->cursor_beat, 0, sizeof(e->cursor_beat));
    e->transient_pattern = -1;
    e->active = true;
    e->dirty = true;
    e->path[0] = 0;
    e->config = false;
    e->new_mode = 0;
    e->export_source[0] = 0;
    e->pattern_channel = e->selected_channel = 0;
    e->selected_pattern = dm_pattern_add(&e->project, NULL);
    e->pattern_channel = e->selected_channel;
    e->roll = true;
    ed_bind_pattern_channel(e);
    e->playlist_view = (View){1, 0, 0, 0};
    e->roll_view = (View){1, 0, 55 * 20, 0};
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    dm_audio_publish(e->audio, &e->project, 0, false, e->status, sizeof(e->status));
}

void ed_load(Editor *e, const char *path) {
    reset_regions(e);
    e->pattern_mode = false;
    e->automation_cc = -2;
    e->automation_node = -1;
    e->context_note = -1;
    char font[DM_PATH];
    snprintf(font, sizeof(font), "%s", e->project.soundfont);
    ed_release_piano(e);
    DmBytes b = {0};

    if (!dm_read(path, &b, e->status, sizeof(e->status))) {
        return;
    }

    char import_message[512] = {0};
    bool project = b.size >= 8 && !memcmp(b.data, "DUCKMIDI", 8);
    bool ok = project ? dm_unpack(&e->project, b.data, b.size, e->status, sizeof(e->status))
                      : dm_import(&e->project, b.data, b.size, -1, 0, e->status, sizeof(e->status));
    dm_bytes_free(&b);

    if (!ok) {
        return;
    }

    snprintf(e->export_source, sizeof(e->export_source), "%s", project ? "" : path);

    if (!project) {
        snprintf(import_message, sizeof(import_message), "%s", e->status);
    }

    if (!*e->project.soundfont) {
        snprintf(e->project.soundfont, sizeof(e->project.soundfont), "%s", font);
    }

    ed_release_piano(e);
    e->record = false;
    e->recording_take = e->take_snapshot = false;
    dm_audio_recording(e->audio, false);
    memcpy(e->reset_channels, e->project.channels, sizeof(e->reset_channels));
    memset(e->start_beat, 0, sizeof(e->start_beat));
    memset(e->cursor_beat, 0, sizeof(e->cursor_beat));
    e->transient_pattern = -1;
    e->active = true;
    e->dirty = !project;
    e->roll = true;
    e->selected_pattern = e->project.pattern_count ? 0 : dm_pattern_add(&e->project, NULL);
    e->pattern_channel = e->selected_channel = 0;

    for (int c = 0; c < 16; ++c) {
        if (e->project.channels[c].enabled) {
            e->selected_channel = c;
            e->channel_selected = true;
            break;
        }
    }

    ed_bind_pattern_channel(e);
    e->playlist_view = (View){1, 0, 0, 0};
    e->roll_view = (View){1, 0, 0, 0};
    e->path[0] = 0;

    if (project) {
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    dm_audio_publish(e->audio, &e->project, 0, false, e->status, sizeof(e->status));

    if (*e->project.soundfont) {
        ed_request_font(e, e->project.soundfont);
    }

    ed_add_recent(e, path);
    ed_preference_save(e);
    ed_message(e, project           ? DM_TEXT_PROJECT_OPENED
                  : *import_message ? import_message
                                    : DM_TEXT_MIDI_IMPORTED_REVIEW);
}

/* Original source ports are note ports 1..port_count-1 of a multi-port import. "All" also
   includes ports the user colored. Port 0 is the destination, so it never merges. */
static bool merged_port(const Editor *e, unsigned port, bool all) {
    if (!port || port >= DM_NOTE_PORTS) {
        return false;
    }

    return all || (e->project.port == DM_PORT_COMBINED && (int)port < e->project.port_count);
}

/* Lists channels that have content on more than one of the ports being combined, including the
   default port. Returns the number of such channels. */
int ed_port_collisions(Editor *e, bool all, char *text, size_t capacity) {
    const DmProject *p = &e->project;
    bool used[16][DM_NOTE_PORTS] = {{false}};

    for (size_t i = 0; i < p->clip_count; ++i) {
        const DmPattern *pattern = &p->patterns[p->clips[i].pattern];
        int c = p->clips[i].channel;

        for (size_t n = 0; n < pattern->count; ++n) {
            used[c][pattern->notes[n].port] = true;
        }

        for (size_t a = 0; a < p->automation_count; ++a) {
            if (p->automation[a].pattern == p->clips[i].pattern) {
                used[c][p->automation[a].port] = true;
            }
        }
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmBytes *m = &p->events[i].message;

        if (m->size && m->data[0] < MIDI_SYSTEM && p->events[i].port < DM_NOTE_PORTS) {
            used[m->data[0] & MIDI_CHANNEL_MASK][p->events[i].port] = true;
        }
    }

    int collisions = 0;
    size_t length = 0;
    text[0] = 0;

    for (int c = 0; c < 16; ++c) {
        int ports = used[c][0];

        for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
            ports += used[c][port] && merged_port(e, port, all);
        }

        if (ports > 1) {
            int written =
                snprintf(text + length, capacity - length, "%s%d", collisions ? ", " : "", c + 1);
            length += written > 0 && (size_t)written < capacity - length ? (size_t)written : 0;
            ++collisions;
        }
    }

    return collisions;
}

/* Moves the chosen ports' notes, controls and events to the default port. Undoable. */
void ed_combine_source_ports(Editor *e, bool all) {
    DmProject *p = &e->project;
    ed_checkpoint(e);
    ed_release_piano(e);

    for (size_t i = 0; i < p->pattern_count; ++i) {
        DmPattern *pattern = &p->patterns[i];

        for (size_t n = 0; n < pattern->count; ++n) {
            if (merged_port(e, pattern->notes[n].port, all)) {
                pattern->notes[n].port = 0;
            }
        }

        if (merged_port(e, (unsigned)pattern->port, all)) {
            pattern->port = 0;
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (merged_port(e, p->automation[i].port, all)) {
            p->automation[i].port = 0;
        }
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        if (merged_port(e, (unsigned)p->events[i].port, all)) {
            p->events[i].port = 0;
        }
    }

    size_t kept = 0;

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        if (!merged_port(e, (unsigned)p->port_mix[i].port, all)) {
            p->port_mix[kept++] = p->port_mix[i];
        }
    }

    p->port_mix_count = kept;
    memset(e->note_ports, 0, sizeof(e->note_ports));

    /* The original ports now share the default output. */
    if (p->port == DM_PORT_COMBINED) {
        p->port_count = 1;
    }

    ed_commit(e);
    ed_message(e, DM_TEXT_SOURCE_PORTS_COMBINED);
}

/* Combines at once, or asks first when channels on different ports would share an output. */
void ed_request_combine(Editor *e, bool all) {
    if (ed_port_collisions(e, all, e->combine_channels, sizeof(e->combine_channels))) {
        e->combine_request = all ? 2 : 1;
    } else {
        ed_combine_source_ports(e, all);
    }
}

void ed_combine_dialog(Editor *e) {
    if (!e->combine_request) {
        return;
    }

    bool open = true;

    if (ui_fixed_dialog(DM_TEXT_COMBINE_PORTS_TITLE, &open, 460, 180)) {
        char text[256];
        snprintf(text, sizeof(text), DM_TEXT_COMBINE_PORTS_WARNING, e->combine_channels);
        ui_confirmation_message(text);
        ui_center_row(212);

        if (ui_button(DM_TEXT_COMBINE, 100, 0)) {
            ed_combine_source_ports(e, e->combine_request == 2);
            open = false;
        }

        ui_same();

        if (ui_button(DM_TEXT_CANCEL, 100, 0)) {
            open = false;
        }
    }

    ui_end();

    if (!open) {
        e->combine_request = 0;
    }
}

void ed_pending_execute(Editor *e) {
    int ed_action = e->pending_action;
    e->pending_action = 0;
    e->confirm = false;

    if (ed_action == PENDING_NEW) {
        snprintf(e->new_name, sizeof(e->new_name), DM_TEXT_UNTITLED);
        e->new_bpm = 120;
        e->new_numerator = e->new_denominator = 4;
        e->new_mode = 1;
        e->config = true;
    } else if (ed_action == PENDING_OPEN) {
        ed_dialog(e, DIALOG_OPEN);
    } else if (ed_action == PENDING_PATH) {
        ed_load(e, e->pending_path);
    } else if (ed_action == PENDING_QUIT) {
        e->running = false;
    }
}

void ed_request(Editor *e, int ed_action, const char *path) {
    if (ed_save_busy(e)) {
        ed_message(e, DM_TEXT_WAIT_PROJECT_SAVE_FINISH);
        return;
    }

    if (path && strlen(path) >= DM_PATH) {
        ed_message(e, DM_TEXT_PATH_EXCEEDS_SUPPORTED);
        return;
    }

    if (e->dialog_busy) {
        return;
    }

    e->pending_action = ed_action;

    if (path) {
        snprintf(e->pending_path, sizeof(e->pending_path), "%s", path);
    }

    if (e->dirty) {
        e->confirm = true;
    } else {
        ed_pending_execute(e);
    }
}
