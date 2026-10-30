#include "editor_internal.h"
#include "text/en.h"

static void footer(Editor *e, UiPoint origin, float body);
static void finish_frame(Editor *e);
static void editor_pointer_state(Editor *e);
static void right_workspace(Editor *e, float right, float body, UiPoint origin);

void editor_init(Editor *e, SDL_Window *window, SDL_Renderer *renderer, DmAudio *audio,
                 const char *base, bool smoke) {
    memset(e, 0, sizeof(*e));
    e->window = window;
    e->renderer = renderer;
    e->audio = audio;
    e->audio_options = dm_audio_options_default();
    e->running = true;
    e->sidebar = 465;
    e->mixer_height = 420;
    e->label_width = 154;
    e->automation_height = 180;
    e->automation_center = 60;
    e->accent = 0xff56eeffu;
    e->transient_pattern = e->pattern_drag = e->rename_pattern = -1;
    e->automation_node = e->event_edit = e->event_auto_edit = -1;
    e->percussion_note=36;
    e->focused = true;
    e->display = SDL_GetDisplayForWindow(window);
    e->roll_height = 320;
    e->selected_pattern = -1;
    e->instrument_channel = -1;
    e->selected_clip = -1;
    e->context_note = -1;
    e->capture = -1;
    e->drag_index = -1;
    e->automation_cc = -2;
    e->bend_range = 12;
    e->roll_view.zoom = e->playlist_view.zoom = 1;
    e->roll_view.scroll_y = 36 * 20;
    e->device = -1;
    e->preview_route = -1;
    e->preview_source_channel = -1;
    e->channel_selected = true;
    e->midi_event_index = -1;
    e->midi_split = 460;
    e->ui_size = 1;
    e->os_cursor = false;
    e->midi_values = false;
    e->event_ruler_drag = false;
    e->midi_hex = true;
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_BEGIN, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_POSITION, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_COMPLETE, true);
    e->export_all = true;
    e->export_metadata = DM_METADATA_DUCKMIDI_ONLY;
    e->export_revision=1;
    e->monitor = true;
    e->smoke = smoke;
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        e->held[i] = -1;
    }
    for (int note = 0; note < 128; ++note) {
        e->record_start[note] = -1;
    }
    ed_defaults(e);
    dm_project_init(&e->project);
    ed_dialog_event = SDL_RegisterEvents(1);
    snprintf(e->settings_path, sizeof(e->settings_path), DM_TEXT_SDUCKMIDI_SETTINGS_INI, base);
    ed_message(e, DM_TEXT_READY_OPEN_A_MIDI_FILE_OR_CREATE_A_PROJECT);
    if (!smoke) {
        SDL_PathInfo initial_info;bool first_launch=!SDL_GetPathInfo(e->settings_path,&initial_info);
        ed_preference_load(e);
        if(first_launch) {
            char font[DM_PATH],sample[DM_PATH];SDL_PathInfo font_info,sample_info;
            bool paths=snprintf(font,sizeof(font),"%sassets/FluidR3.sf2",base)<(int)sizeof(font)&&snprintf(sample,sizeof(sample),"%sprojects/funky-beat.duck",base)<(int)sizeof(sample);
            if(paths&&SDL_GetPathInfo(font,&font_info)&&font_info.type==SDL_PATHTYPE_FILE&&SDL_GetPathInfo(sample,&sample_info)&&sample_info.type==SDL_PATHTYPE_FILE) {
                ed_load(e,sample);
                if(e->active){snprintf(e->preferred_font,DM_PATH,"%s",font);ed_preference_save(e);}
            }
        }
        snprintf(e->project.soundfont, DM_PATH, "%s", e->preferred_font);
        if (*e->preferred_font) {
            ed_request_font(e, e->preferred_font);
        }
        if (*e->output_name) {
            bool found = false;
            for (int i = 0; i < dm_audio_device_count(audio); ++i) {
                char name[256];
                dm_audio_device_name(audio, i, name, sizeof(name));
                if (!strcmp(name, e->output_name) &&
                    dm_audio_device(audio, i, e->status, sizeof(e->status))) {
                    e->device = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ed_message(e, DM_TEXT_SAVED_MIDI_OUTPUT_UNAVAILABLE_USING_SOUNDFONT_MONITORING);
            }
        }
        dm_audio_monitor(audio, e->device < 0 || e->monitor);
    }
}

void editor_free(Editor *e) {
    ed_generate_free(e);
    ed_save_free(e);
    ed_export_free(e);
    dm_audio_analyzer_enable(e->audio, false);
    free(e->drag_notes);
    ed_preference_save(e);
    ed_release_piano(e);
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    dm_bytes_free(&e->clipboard);
    dm_bytes_free(&e->event_selection);
    dm_bytes_free(&e->record_original);
    dm_bytes_free(&e->midi_rows);
    dm_bytes_free(&e->midi_draft);
    dm_bytes_free(&e->midi_text);
    dm_bytes_free(&e->event_drag);
    dm_bytes_free(&e->clip_drag);
    dm_project_free(&e->project);
}

static void footer(Editor *e, UiPoint origin, float body) {
    ui_position(origin.x, origin.y + body + 7);
    ui_separator();
    char footer_text[768];
    snprintf(footer_text, sizeof(footer_text), "%s   %s%s   |   %s",
             dm_audio_playing(e->audio) ? DM_TEXT_PLAYING : DM_TEXT_READY,
               e->active ? e->project.name : DM_TEXT_APP_NAME, e->dirty ? " *" : "", *e->hover?e->hover:e->status);
    ui_dim(footer_text);
}

void editor_background(Editor *e) {
    ed_generate_poll(e);
    ed_save_poll(e);
    ed_export_poll(e);

    bool success = false;
    int job = 0;
    if (dm_audio_poll(e->audio, e->status, sizeof(e->status), &success, &job)) {
        if(job==2) {
            if(success&&*e->audio_export_pending)snprintf(e->audio_exported,DM_PATH,"%s",e->audio_export_pending);
            e->audio_export_pending[0]=0;
        }
        if (job == 1 && e->font_request) {
            if (success) {
                snprintf(e->preferred_font, DM_PATH, "%s", e->font_pending);
                ed_preference_save(e);
                if (strcmp(e->project.soundfont, e->font_pending)) {
                    ed_checkpoint(e);
                    snprintf(e->project.soundfont, sizeof(e->project.soundfont), "%s",
                             e->font_pending);
                    if (e->active) {
                        ed_changed(e);
                    }
                }
                ed_publish(e);
            }
            e->font_pending[0] = 0;
            e->font_request = false;
        }
    }
    if (e->font_queued && !dm_audio_busy(e->audio)) {
        char path[DM_PATH];
        snprintf(path, sizeof(path), "%s", e->font_next);
        e->font_queued = false;
        ed_request_font(e, path);
    }
}

void ed_welcome(Editor *e, float width, float body) {
    (void)width;
    ui_child(DM_TEXT_WELCOME, 0, body, false);
    UiPoint p=ui_cursor(), available=ui_available();
    float card_width=fminf(540,available.x),card_height=fminf(260,available.y);
    ui_position(p.x+(available.x-card_width)*.5f,p.y+(available.y-card_height)*.5f);
    ui_child(DM_TEXT_GET_STARTED,card_width,card_height,false);
    int action=ui_welcome_content();
    if(action)ed_request(e,action==1?PENDING_NEW:PENDING_OPEN,NULL);
    ui_child_end();
    ui_child_end();
}

static void finish_frame(Editor *e) {
    if (!ui_mouse().left) {
        e->pattern_drag = -1;
    }
    if (e->pending_save && !e->drag && !e->marquee) {
        bool as = e->pending_save_as;
        e->pending_save = false;
        ed_save(e, as);
    }
}

static void editor_pointer_state(Editor *e) {
    UiMouse pointer = ui_mouse();
    if (!pointer.middle) {
        e->pan_context = 0;
    }
    if (!pointer.right || pointer.shift) {
        e->erase_active = false;
    }
}

static void right_workspace(Editor *e, float right, float body, UiPoint origin) {
    if (e->roll && !e->roll_auto_hidden) {
        e->roll_height = ed_clampf(e->roll_height, 110, fmaxf(110, body - 100));
        float top = fmaxf(80, body - e->roll_height - 8);
        ed_playlist(e, right, top);
        ui_position(origin.x + e->sidebar + 10, origin.y + top + 1);
        ui_canvas(DM_TEXT_ROLL_DIVIDER, right, 8);
        if (ui_hover() || ui_active()) {
            ui_resize_cursor(true);
            ed_hover(e, DM_TEXT_DRAG_TO_RESIZE_THE_PIANO_ROLL);
        }
        if (ui_active()) {
            e->roll_height =
                ed_clampf(origin.y + body - ui_mouse().pos.y, 110, fmaxf(110, body - 100));
        }
        ui_position(origin.x + e->sidebar + 10, origin.y + top + 8);
        if (ed_pattern(e) && ed_pattern(e)->kind) {
            ed_event_editor(e, right, e->roll_height);
        } else {
            ed_roll(e, right, e->roll_height);
        }
    } else {
        ed_playlist(e, right, body);
    }
}

void editor_frame(Editor *e, float width, float height) {
    ui_custom_cursor(!e->os_cursor);
    ed_finish_edit_feedback(e);
    width /= ui_scale();
    height /= ui_scale();
    editor_pointer_state(e);
    if (!e->scrub && dm_audio_playing(e->audio)) {
        e->cursor_beat[e->pattern_mode] = dm_seconds_beat(&e->project, dm_audio_position(e->audio));
    }
    ui_theme(e->accent);
    ui_clamp_windows();
    editor_background(e);
    e->hover[0] = 0;
    ed_menu(e);
    ui_root(27, height - 27);
    ed_toolbar(e);
    ui_separator();
    UiPoint origin = ui_cursor();
    UiPoint available = ui_available();
    float footer_height = 30, body = available.y - footer_height;
    e->sidebar = ed_clampf(e->sidebar, 235, width * .45f);
    if (!e->active) {
        ed_welcome(e, width, body);
    } else {
        UiPoint start = origin;
        UiPoint left = {e->sidebar, body};
        float mix = ed_clampf(e->mixer_height, 110, fmaxf(110, body - 120));
        ed_pattern_panel(e, left.x, body - mix - 10);
        ui_position(start.x, start.y + body - mix - 9);
        ui_canvas(DM_TEXT_MIXER_DIVIDER, left.x, 10);
        if (ui_hover() || ui_active()) {
            ui_resize_cursor(true);
        }
        if (ui_active()) {
            e->mixer_height =
                ed_clampf(start.y + body - ui_mouse().pos.y, 110, fmaxf(110, body - 120));
        }
        ui_position(start.x, start.y + body - mix);
        ed_mixer(e, left.x, mix);
        ui_position(origin.x + e->sidebar, origin.y);
        ui_canvas(DM_TEXT_SIDEBAR_DIVIDER, 10, body);
        if (ui_hover() || ui_active()) {
            ui_resize_cursor(false);
            ed_hover(e, DM_TEXT_DRAG_TO_RESIZE_THE_PATTERN_BROWSER_AND_MIXER);
        }
        if (ui_active()) {
            e->sidebar = ed_clampf(ui_mouse().pos.x - origin.x, 235, width * .45f);
        }
        ui_position(origin.x + e->sidebar + 10, origin.y);
        float right = available.x - e->sidebar - 10;
        right_workspace(e, right, body, origin);
    }
    footer(e, origin, body);
    ui_end();
    ed_dialogs(e);
    if (e->drop_active && !e->drop_handled) {
        bool font = e->preferences && ed_inside(e->drop_position, e->preferences_rect);
        UiPoint display = ui_display();
        ui_drop_overlay(font ? e->preferences_rect : (UiRect){0, 0, display.x, display.y},
                        font ? DM_TEXT_DROP_SOUNDFONT_HERE : DM_TEXT_DROP_FILE_HERE);
    }
    finish_frame(e);
}

void editor_smoke(Editor *e) {
    ed_new_project(e);
    snprintf(e->project.name, sizeof(e->project.name), DM_TEXT_EVENING_SKETCH);
    dm_pattern_name(&e->project,0,DM_TEXT_SOFT_KEYS);
    int pitches[] = {60, 64, 67, 69, 64, 67, 65, 69};
    for (int i = 0; i < 8; ++i) {
        dm_note_add(&e->project.patterns[0],
                    (DmNote){i * .5, .4, (uint8_t)pitches[i], (uint8_t)(85 + i * 4), 0,0});
    }
    int bass = dm_pattern_add(&e->project, DM_TEXT_LOW_TIDE);
    for (int i = 0; i < 4; ++i) {
        dm_note_add(&e->project.patterns[bass],
                    (DmNote){i, 0.8, (uint8_t)(36 + (i % 2) * 7), 95, 0,0});
    }
    e->project.channels[1].program = 33;
    e->project.channels[0].program = 0;
    for (int i = 0; i < 4; ++i) {
        dm_clip_add(&e->project, (DmClip){i * 4, 0, 0, 0, 0, 0});
        dm_clip_add(&e->project, (DmClip){i * 4, bass, 1, 0, 0, 0});
    }
    e->selected_pattern = 0;
    e->roll_view.scroll_y = (127 - 70) * 20;
    e->focus_pattern = false;
    e->dirty = false;
    ed_publish(e);
}
