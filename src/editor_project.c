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
        ed_message(e, DM_TEXT_COULD_NOT_SAVE_CONFIGURATION);
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
            ed_message(e, DM_TEXT_INVALID_SHORTCUT_SETTINGS_ALL_SHORTCUTS_RESET_TO_DEFAULTS);
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
        {DM_TEXT_MIDI_OR_DUCKMIDI_PROJECT, "mid;midi;rmi;mus;duck"},
        {DM_TEXT_ALL_FILES, "*"}};
    static const SDL_DialogFileFilter save_filters[] = {{DM_TEXT_DUCKMIDI_PROJECT, "duck"}},
                                      mid_filters[] = {{DM_TEXT_STANDARD_MIDI_FILE, "mid"}},
                                      font_filters[] = {{DM_TEXT_SOUNDFONT, "sf2;sf3"}};
    static const SDL_DialogFileFilter audio_filters[]={{"WAV / PCM","wav"},{"MP3","mp3"},{"AAC (M4A)","m4a"},{"Ogg Vorbis","ogg"},{"Opus","opus"},{"FLAC","flac"}};
    if (kind == DIALOG_OPEN) {
        SDL_ShowOpenFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window, open_filters, 2,
                               *e->midi_folder ? e->midi_folder : NULL, false);
    } else if (kind == DIALOG_FONT) {
        SDL_ShowOpenFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window, font_filters, 1,
                               *e->font_folder ? e->font_folder : NULL, false);
    } else if (kind == DIALOG_EXPORT_PLUGIN) {
        const SDL_DialogFileFilter *filter;const char *suggested;
        if(!ed_export_picker(e,&filter,&suggested)){e->dialog_busy=false;return;}
        SDL_ShowSaveFileDialog(dialog_callback,(void *)(intptr_t)kind,e->window,filter,1,suggested);
    } else {
        const char *suggested=kind==DIALOG_SAVE&&*e->path?e->path:NULL;
        if(kind!=DIALOG_SAVE) {
            const char *extension=kind==DIALOG_MIDI?"mid":dm_audio_codec_extension(e->audio_options.codec);
            if(!ed_export_suggest(e,extension,e->dialog_suggested,sizeof(e->dialog_suggested))) {
                e->dialog_busy=false;ed_message(e,"The suggested export path is too long.");return;
            }
            suggested=e->dialog_suggested;
        }
        SDL_ShowSaveFileDialog(dialog_callback, (void *)(intptr_t)kind, e->window,
                               kind == DIALOG_SAVE   ? save_filters
                               : kind == DIALOG_MIDI ? mid_filters
                                                     : &audio_filters[e->audio_options.codec],
                               1, suggested);
    }
}

void ed_save(Editor *e, bool as) {
    if(ed_save_busy(e)){ed_message(e,"A project save is already running.");return;}
    if (!e->active) {
        return;
    }
    if (as || !*e->path) {
        ed_dialog(e, DIALOG_SAVE);
    } else {
        if(!ed_save_to(e, e->path)&&e->pending_action)e->confirm=true;
    }
}

void ed_open_audio_location(Editor *e) {
    if(!*e->audio_exported)return;
    char path[DM_PATH],url[DM_PATH*3+16];
    const char *source=e->audio_exported;
    bool absolute=source[0]=='/'||source[0]=='\\'||(source[0]&&source[1]==':');
    char *cwd=absolute?NULL:SDL_GetCurrentDirectory();
    if(!absolute&&!cwd){ed_message(e,SDL_GetError());return;}
    int size=snprintf(path,sizeof(path),"%s%s",cwd?cwd:"",source);SDL_free(cwd);
    if(size<0||size>=(int)sizeof(path)){ed_message(e,"Export folder path is too long.");return;}
    for(char *p=path;*p;p++)if(*p=='\\')*p='/';
    char *slash=strrchr(path,'/');if(!slash){ed_message(e,"Cannot locate the export folder.");return;}slash[1]=0;
    const char *prefix=path[0]=='/'?(path[1]=='/'?"file:":"file://"):"file:///";
    size_t at=(size_t)snprintf(url,sizeof(url),"%s",prefix);
    static const char hex[]="0123456789ABCDEF";
    for(const unsigned char *p=(const unsigned char *)path;*p;p++) {
        if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("/-_.~:",*p))url[at++]=(char)*p;
        else {url[at++]='%';url[at++]=hex[*p>>4];url[at++]=hex[*p&15];}
    }
    url[at]=0;
    if(!SDL_OpenURL(url))ed_message(e,SDL_GetError());
}

static void reset_regions(Editor *e) {
    ++e->export_revision;
    ++e->project_session;
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
    e->export_source[0]=0;
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
    snprintf(e->export_source,sizeof(e->export_source),"%s",project?"":path);
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
                                    : DM_TEXT_MIDI_IMPORTED_REVIEW_THE_PROJECT_SETTINGS);
}

void ed_combine_source_ports(Editor *e) {
    DmProject next;
    dm_project_init(&next);
    if (!dm_import(&next, e->project.original.data, e->project.original.size, DM_PORT_COMBINED,
                   e->project.sequence, e->status, sizeof(e->status))) {
        dm_project_free(&next);
        return;
    }
    snprintf(next.name, sizeof(next.name), "%s", e->project.name);
    snprintf(next.soundfont, sizeof(next.soundfont), "%s", e->project.soundfont);
    next.percussion_override=e->project.percussion_override;
    memcpy(next.percussion_keys,e->project.percussion_keys,128);
    ed_release_piano(e);
    dm_audio_transport(e->audio, false);
    dm_audio_recording(e->audio, false);
    e->record = e->recording_take = e->take_snapshot = false;
    dm_project_free(&e->project);
    e->project = next;
    e->selected_pattern = e->project.pattern_count ? 0 : -1;
    e->selected_channel = e->project.clip_count ? e->project.clips[0].channel : 0;
    ed_bind_pattern_channel(e);
    memcpy(e->reset_channels, e->project.channels, sizeof(e->reset_channels));
    e->pattern_mode = false;
    e->path[0] = 0;
    e->drag = 0;
    e->marquee = false;
    e->transient_pattern = -1;
    e->playlist_view = (View){1, 0, 0, 0};
    e->roll_view = (View){1, 0, 60 * 20, 0};
    memset(e->start_beat, 0, sizeof(e->start_beat));
    memset(e->cursor_beat, 0, sizeof(e->cursor_beat));
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    e->dirty = true;
    ed_publish(e);
    ed_message(e, DM_TEXT_SOURCE_PORTS_COMBINED_INTO_ONE_ARRANGEMENT_ORIGINAL_MIDI_RETAINED_UNCHAN);
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
    } else if (ed_action == PENDING_COMBINE) {
        ed_combine_source_ports(e);
    } else if (ed_action == PENDING_QUIT) {
        e->running = false;
    }
}

void ed_request(Editor *e, int ed_action, const char *path) {
    if(ed_save_busy(e)){ed_message(e,"Wait for the project save to finish, or cancel it from File.");return;}
    if (path && strlen(path) >= DM_PATH) {
        ed_message(e, DM_TEXT_PATH_EXCEEDS_THE_SUPPORTED_UTF_8_PATH_LENGTH);
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
