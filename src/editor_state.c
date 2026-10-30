#include "editor_internal.h"
#include "text/en.h"

const char *ed_action_names[ACT_COUNT] = {DM_TEXT_COMPUTER_KEYBOARD_PIANO,
                                          DM_TEXT_QUANTIZE,
                                          DM_TEXT_PLAY_PAUSE,
                                          DM_TEXT_FORWARD_ONE_SECOND,
                                          DM_TEXT_BACK_ONE_SECOND,
                                          DM_TEXT_FORWARD_QUARTER_BAR,
                                          DM_TEXT_BACK_QUARTER_BAR,
                                          DM_TEXT_GO_TO_BEGINNING,
                                          DM_TEXT_GO_TO_END,
                                          DM_TEXT_SCROLL_TO_TOP,
                                          DM_TEXT_SCROLL_TO_BOTTOM,
                                          DM_TEXT_NEXT_CREATE_PATTERN,
                                          DM_TEXT_PREVIOUS_PATTERN,
                                          DM_TEXT_SCROLL_UP,
                                          DM_TEXT_SCROLL_DOWN,
                                          DM_TEXT_MOVE_SELECTION_UP,
                                          DM_TEXT_MOVE_SELECTION_DOWN,
                                          DM_TEXT_TRANSPOSE_OCTAVE_UP,
                                          DM_TEXT_TRANSPOSE_OCTAVE_DOWN,
                                          DM_TEXT_ZOOM_IN,
                                          DM_TEXT_ZOOM_OUT,
                                          DM_TEXT_RESET_ZOOM,
                                          DM_TEXT_KEYBOARD_SHORTCUTS,
                                          DM_TEXT_PROJECT_SETTINGS,
                                          DM_TEXT_TOGGLE_PIANO_ROLL,
                                          DM_TEXT_SELECT_TOOL,
                                          DM_TEXT_MARQUEE_TOOL,
                                          DM_TEXT_SLICE_TOOL,
                                          DM_TEXT_DELETE_TOOL,
                                          DM_TEXT_BRUSH_TOOL,
                                          DM_TEXT_KEYBOARD_OCTAVE_DOWN,
                                          DM_TEXT_KEYBOARD_OCTAVE_UP,
                                          DM_TEXT_PLAY_FROM_BEGINNING,
                                          DM_TEXT_SONG_EVENTS,
                                          DM_TEXT_MUTE_SELECTED_CHANNEL,
                                          DM_TEXT_SOLO_SELECTED_CHANNEL,
                                          DM_TEXT_RECORD_KEYBOARD_PIANO_OFF,
                                          DM_TEXT_METRONOME,
                                          DM_TEXT_WAIT_FOR_INPUT,
                                          DM_TEXT_OVERLAPPING_NOTES,
                                          DM_TEXT_RECORD,
                                          DM_TEXT_DUPLICATE_SELECTION,
                                          DM_TEXT_SONG_PATTERN_PLAYBACK,"Color selected notes"};
static const SDL_Scancode default_keys[ACT_COUNT] = {
    SDL_SCANCODE_F1,        SDL_SCANCODE_Q,           SDL_SCANCODE_SPACE,
    SDL_SCANCODE_RIGHT,     SDL_SCANCODE_LEFT,        SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_LEFT,      SDL_SCANCODE_HOME,        SDL_SCANCODE_END,
    SDL_SCANCODE_PAGEUP,    SDL_SCANCODE_PAGEDOWN,    SDL_SCANCODE_KP_PLUS,
    SDL_SCANCODE_KP_MINUS,  SDL_SCANCODE_UP,          SDL_SCANCODE_DOWN,
    SDL_SCANCODE_UP,        SDL_SCANCODE_DOWN,        SDL_SCANCODE_UP,
    SDL_SCANCODE_DOWN,      SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
    SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_K,           SDL_SCANCODE_P,
    SDL_SCANCODE_E,         SDL_SCANCODE_1,           SDL_SCANCODE_2,
    SDL_SCANCODE_3,         SDL_SCANCODE_4,           SDL_SCANCODE_5,
    SDL_SCANCODE_KP_1,      SDL_SCANCODE_KP_2,        SDL_SCANCODE_SPACE,
    SDL_SCANCODE_E,         SDL_SCANCODE_M,           SDL_SCANCODE_S,
    SDL_SCANCODE_R,         SDL_SCANCODE_F2,          SDL_SCANCODE_F3,
    SDL_SCANCODE_F4,        SDL_SCANCODE_F5,          SDL_SCANCODE_D,
    SDL_SCANCODE_GRAVE,SDL_SCANCODE_C};
static void preview_open_pattern(Editor *e, int id);
static bool push(DmBytes *stack, int *count, const DmProject *p);
static void publish_loop(Editor *e);
static int clip_identity(const void *left, const void *right);
static void retain_note_selection(DmPattern *next, const DmPattern *live);
static void retain_selection(DmProject *next, const DmProject *live);

void ed_decode_source_event(Editor *e, DmEvent *event) {
    const uint8_t *m = event->message.data;
    e->event_beat = (float)event->beat;
    e->event_source_kind = 3;
    if (event->message.size < 3 || m[0] != 0xff) {
        if (event->message.size >= 2 && m[0] < 0xf0) {
            e->event_program = m[1];
            e->event_channel = m[0] & 15;
        }
        return;
    }
    if (m[1] == 0x51 && event->message.size == 6) {
        e->event_kind = 0;
        e->event_bpm = 60000000.0f / ((m[3] << 16) | (m[4] << 8) | m[5]);
    } else if (m[1] == 0x58 && event->message.size == 7) {
        e->event_kind = 1;
        e->event_numerator = m[3];
        e->event_denominator = 1 << m[4];
    } else if (m[1] == 6 && m[2] < 128 && event->message.size == (size_t)m[2] + 3) {
        e->event_kind = 2;
        snprintf(e->event_marker, sizeof(e->event_marker), "%.*s", m[2], m + 3);
    } else {
        return;
    }
    e->event_source_kind = 1;
}

void ed_focus_edit(Editor *e, EditFocus focus) {
    e->edit_focus = focus;
    if (focus == FOCUS_NOTES || focus == FOCUS_EVENTS) {
        e->context = 2;
    } else {
        e->context = 1;
    }
}

bool ed_velocity_editing(Editor *e) {
    return e->automation_cc == -3 && e->selected_pattern >= 0 &&
           (size_t)e->selected_pattern < e->project.pattern_count &&
           !e->project.patterns[e->selected_pattern].kind;
}

EditFocus ed_selection_focus(Editor *e) {
    return e->edit_focus ? e->edit_focus : e->context == 2 ? FOCUS_NOTES : FOCUS_PLAYLIST;
}

void ed_toggle_roll(Editor *e) {
    e->roll = !e->roll;
    if (e->roll) {
        e->roll_auto_hidden = false;
    }
}

int ed_channel_program(Editor *e, int channel) {
    return channel == 9 ? 128 : (int)fmax(0, e->project.channels[channel].program);
}

void ed_preview_channel(Editor *e, int channel, bool placed) {
    e->preview_source_channel = placed ? channel : -1;
    e->preview_override = false;
    e->preview_program = ed_channel_program(e, channel);
}

void ed_select_placement_route(Editor *e, int index) {
    DmClip clip = e->project.clips[index];
    ed_release_piano(e);
    e->selected_clip = index;
    e->pattern_channel = e->selected_channel = clip.channel;
    e->channel_selected = true;
    if (!e->preview_override) {
        ed_preview_channel(e, clip.channel, true);
    }
}

static void preview_open_pattern(Editor *e, int id) {
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == id) {
            ed_preview_channel(e, e->project.clips[i].channel, true);
            return;
        }
    }
    ed_preview_channel(e, e->selected_channel, false);
}

void ed_open_pattern(Editor *e, int id, bool created) {
    e->curve_drag = e->event_marquee = false;
    if (created || id != e->selected_pattern || !e->preview_override) {
        preview_open_pattern(e, id);
    }
    e->event_selection.size = 0;
    if (id != e->selected_pattern) {
        e->loop_enabled[1] = false;
        if (e->pattern_mode) {
            dm_audio_loop(e->audio, false, 0, 0);
        }
    }
    if (id >= 0 && (size_t)id < e->project.pattern_count) {
        e->automation_browser = e->project.patterns[id].kind != 0;
        if (e->automation_browser) {
            e->automation_cc = e->project.patterns[id].event;
        } else if (e->automation_cc == 7 || e->automation_cc == 10) {
            e->automation_cc = -3;
        }
    }
    e->selected_pattern = id;
    e->pattern_highlight = id >= 0;
    e->linked_highlight = id >= 0;
    e->roll_auto_hidden = false;
    if (created) {
        e->roll = true;
    }
    e->focus_pattern = true;
    e->automation_all = false;
}

SDL_Keymod ed_normalized(SDL_Keymod m) {
    return (SDL_Keymod)(((m & SDL_KMOD_CTRL) ? SDL_KMOD_CTRL : 0) |
                        ((m & SDL_KMOD_SHIFT) ? SDL_KMOD_SHIFT : 0) |
                        ((m & SDL_KMOD_ALT) ? SDL_KMOD_ALT : 0) |
                        ((m & SDL_KMOD_GUI) ? SDL_KMOD_GUI : 0));
}

void ed_defaults(Editor *e) {
    for (int i = 0; i < ACT_COUNT; ++i) {
        e->bindings[i] = (Binding){default_keys[i], (SDL_Keymod)0};
    }
    e->bindings[ACT_QUANTIZE].mods = SDL_KMOD_CTRL;
    e->bindings[ACT_DUPLICATE].mods = SDL_KMOD_CTRL;
    e->bindings[ACT_PLAY_START].mods = SDL_KMOD_SHIFT;
    e->bindings[ACT_NOTE_COLOR].mods=SDL_KMOD_ALT;
    e->bindings[ACT_SONG_EVENT].mods = SDL_KMOD_SHIFT;
    for (int i = ACT_FORWARD_BEAT; i <= ACT_BACK_BEAT; ++i) {
        e->bindings[i].mods = SDL_KMOD_SHIFT;
    }
    e->bindings[ACT_ITEM_UP].mods = e->bindings[ACT_ITEM_DOWN].mods = SDL_KMOD_SHIFT;
    e->bindings[ACT_OCTAVE_UP].mods = e->bindings[ACT_OCTAVE_DOWN].mods = SDL_KMOD_CTRL;
}

void ed_message(Editor *e, const char *text) {
    snprintf(e->status, sizeof(e->status), "%s", text);
}

DmPattern *ed_pattern(Editor *e) {
    return e->selected_pattern >= 0 && (size_t)e->selected_pattern < e->project.pattern_count
               ? &e->project.patterns[e->selected_pattern]
               : NULL;
}

void ed_bind_pattern_channel(Editor *e) {
    ed_release_piano(e);
    e->pattern_channel = e->selected_channel;
    e->automation_node = -1;
    e->last_automation_valid = false;
    e->focus_pattern = true;
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == e->selected_pattern) {
            e->pattern_channel = e->project.clips[i].channel;
            if (!e->preview_override) {
                ed_preview_channel(e, e->pattern_channel, true);
            }
            return;
        }
    }
    if (!e->preview_override) {
        ed_preview_channel(e, e->pattern_channel, false);
    }
}

void ed_clear_history(DmBytes *b, int *count) {
    for (int i = 0; i < *count; ++i) {
        dm_bytes_free(&b[i]);
    }
    *count = 0;
}

static bool push(DmBytes *stack, int *count, const DmProject *p) {
    DmBytes bytes = {0};
    if (!dm_pack(p, &bytes)) {
        return false;
    }
    size_t total = bytes.size;
    for (int i = 0; i < *count; ++i) {
        total += stack[i].size;
    }
    while (*count && (*count == 32 || total > 256u * 1024u * 1024u)) {
        total -= stack[0].size;
        dm_bytes_free(&stack[0]);
        memmove(stack, stack + 1, (size_t)(*count - 1) * sizeof(*stack));
        --*count;
    }
    stack[(*count)++] = bytes;
    return true;
}

void ed_checkpoint(Editor *e) {
    if (e->recording_take && e->take_snapshot) {
        return;
    }
    if (!push(e->undo, &e->undo_count, &e->project)) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_UNDO_SNAPSHOT);
    } else if (e->recording_take) {
        e->take_snapshot = true;
    }
    ed_clear_history(e->redo, &e->redo_count);
}

bool ed_add_note(Editor *e, DmPattern *p, DmNote note) {
    double previous = p->length;
    if (!dm_note_add(p, note)) {
        return false;
    }
    if (p->length > previous) {
        dm_pattern_fit(&e->project, p);
    }
    return true;
}

double ed_pattern_length(Editor *e) {
    if (!ed_pattern(e)) {
        return dm_bar(&e->project);
    }
    if (ed_pattern(e)->kind && e->selected_clip >= 0 &&
        (size_t)e->selected_clip < e->project.clip_count &&
        e->project.clips[e->selected_clip].pattern == e->selected_pattern) {
        return dm_clip_length(&e->project, e->project.clips[e->selected_clip]);
    }
    return ed_pattern(e)->length;
}

static void publish_loop(Editor *e) {
    int mode = e->pattern_mode;
    dm_audio_loop(e->audio, e->loop_enabled[mode],
                  dm_beat_seconds(&e->project, e->loop_start[mode]),
                  dm_beat_seconds(&e->project, e->loop_end[mode]));
}

bool ed_transport_publish(Editor *e, double seconds, bool playing) {
    bool ok;
    if (e->pattern_mode && ed_pattern(e)) {
        if (!e->preview_override && e->preview_source_channel >= 0) {
            e->preview_program = ed_channel_program(e, e->preview_source_channel);
        }
        ok = dm_audio_publish_pattern(e->audio, &e->project, e->selected_pattern,
                                      e->pattern_channel, e->preview_program, ed_pattern_length(e),
                                      seconds, playing, e->status, sizeof(e->status));
    } else {
        ok =
            dm_audio_publish(e->audio, &e->project, seconds, playing, e->status, sizeof(e->status));
    }
    if (ok) {
        publish_loop(e);
    }
    return ok;
}

double ed_transport_length(Editor *e) {
    return e->pattern_mode && ed_pattern(e) ? ed_pattern_length(e) : dm_duration(&e->project);
}

void ed_publish(Editor *e) {
    ++e->export_revision;
    if (e->active) {
        if (!e->preview_override && e->preview_source_channel >= 0) {
            e->preview_program = ed_channel_program(e, e->preview_source_channel);
        }
        if (dm_audio_update(e->audio, &e->project,
                            e->pattern_mode && ed_pattern(e) ? e->selected_pattern : -1,
                            e->pattern_channel, e->preview_program, ed_pattern_length(e), e->status,
                            sizeof(e->status))) {
            publish_loop(e);
        }
    }
}

void ed_changed(Editor *e) {
    ++e->export_revision;
    dm_patterns_fit(&e->project);
    if (ed_pattern(e) && ed_pattern(e)->count) {
        e->transient_pattern = -1;
    }
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == e->transient_pattern) {
            e->transient_pattern = -1;
        }
    }
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].pattern == e->transient_pattern) {
            e->transient_pattern = -1;
            break;
        }
    }
    e->dirty = true;
}

void ed_request_font(Editor *e, const char *path) {
    if (dm_audio_busy(e->audio)) {
        if (e->font_request) {
            dm_audio_cancel(e->audio);
        }
        e->font_request = false;
        e->font_queued = true;
        snprintf(e->font_next, sizeof(e->font_next), "%s", path);
        return;
    }
    if (dm_audio_soundfont(e->audio, path, e->status, sizeof(e->status))) {
        e->font_request = true;
        snprintf(e->font_pending, sizeof(e->font_pending), "%s", path);
        ed_message(e, DM_TEXT_LOADING_SOUNDFONT);
    }
}

void ed_commit(Editor *e) {
    ed_changed(e);
    ed_publish(e);
}

int ed_note_identity(const void *left, const void *right) {
    const DmNote *a = left, *b = right;
    if(a->port!=b->port)return (int)a->port-b->port;
    if (a->beat != b->beat) {
        return a->beat < b->beat ? -1 : 1;
    }
    if (a->key != b->key) {
        return a->key < b->key ? -1 : 1;
    }
    if (a->length != b->length) {
        return a->length < b->length ? -1 : 1;
    }
    return (int)a->velocity - b->velocity;
}

static int clip_identity(const void *left, const void *right) {
    const DmClip *a = left, *b = right;
    if (a->beat != b->beat) {
        return a->beat < b->beat ? -1 : 1;
    }
    if (a->pattern != b->pattern) {
        return a->pattern < b->pattern ? -1 : 1;
    }
    return (int)a->channel - b->channel;
}

static void retain_note_selection(DmPattern *next, const DmPattern *live) {
    DmNote *selected = malloc((live->count + 1) * sizeof(*selected));
    size_t count = 0;
    for (size_t i = 0; i < live->count && selected; ++i) {
        if (live->notes[i].selected) {
            selected[count++] = live->notes[i];
        }
    }
    if (selected) {
        qsort(selected, count, sizeof(*selected), ed_note_identity);
    }
    for (size_t i = 0; i < next->count; ++i) {
        next->notes[i].selected = selected && bsearch(next->notes + i, selected, count,
                                                      sizeof(*selected), ed_note_identity);
    }
    free(selected);
}

static void retain_selection(DmProject *next, const DmProject *live) {
    for (size_t i = 0; i < next->pattern_count; ++i) {
        DmPattern empty = {0};
        const DmPattern *before = i < live->pattern_count ? live->patterns + i : &empty;
        retain_note_selection(next->patterns + i, before);
    }
    DmClip *selected = malloc((live->clip_count + 1) * sizeof(*selected));
    size_t count = 0;
    for (size_t i = 0; i < live->clip_count && selected; ++i) {
        if (live->clips[i].selected) {
            selected[count++] = live->clips[i];
        }
    }
    if (selected) {
        qsort(selected, count, sizeof(*selected), clip_identity);
    }
    for (size_t i = 0; i < next->clip_count; ++i) {
        next->clips[i].selected =
            selected && bsearch(next->clips + i, selected, count, sizeof(*selected), clip_identity);
    }
    free(selected);
}

void ed_undo(Editor *e, bool redo) {
    e->midi_event_index = -1;
    e->song_event = false;
    e->curve_drag = e->event_marquee = false;
    e->wait_armed = e->take_started = false;
    e->event_selection.size = 0;
    ed_release_piano(e);
    e->recording_take = e->take_snapshot = false;
    char old_font[DM_PATH];
    snprintf(old_font, sizeof(old_font), "%s", e->project.soundfont);
    DmBytes *from = redo ? e->redo : e->undo, *to = redo ? e->undo : e->redo;
    int *n = redo ? &e->redo_count : &e->undo_count, *m = redo ? &e->undo_count : &e->redo_count;
    if (!*n) {
        return;
    }
    if (!push(to, m, &e->project)) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_UNDO_SNAPSHOT);
        return;
    }
    DmBytes b = from[--*n];
    DmProject next;
    dm_project_init(&next);
    if (dm_unpack(&next, b.data, b.size, e->status, sizeof(e->status))) {
        /* Identities cannot be reused when an undone creation is replaced by another edit. */
        next.next_pattern_uid = (uint32_t)fmax(next.next_pattern_uid, e->project.next_pattern_uid);
        retain_selection(&next, &e->project);
        dm_project_free(&e->project);
        e->project = next;
        dm_project_init(&next);
        if ((size_t)e->selected_pattern >= e->project.pattern_count) {
            e->selected_pattern = -1;
            ed_clear_pattern_selection(e);
        }
        e->automation_node = -1;
        ed_commit(e);
        if (strcmp(old_font, e->project.soundfont)) {
            ed_request_font(e, e->project.soundfont);
        }
    }
    dm_project_free(&next);
    dm_bytes_free(&b);
}
