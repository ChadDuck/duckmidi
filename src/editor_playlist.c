#include "editor_internal.h"
#include "text/en.h"

static bool placement_valid(Editor *e, int pattern_id, int encoded);
static int placement_lane(Editor *e, int pattern_id, int encoded);
static void pattern_drop(Editor *e, UiRect r, UiRect grid, View *v, float scale, float row,
                         const int *list, int count, int dropped);
static bool split_automation(DmProject *p, int source, int left, int right, double cut);
static void playlist_tools(Editor *e);
static bool channel_expander(Editor *e, UiRect r, float label, float row, int channel, float y,
                             UiMouse mouse);
static void playlist_rows(Editor *e, UiRect r, View *v, float label, float row, const int *list,
                          int count, UiMouse mouse);
static void event_card(Editor *e, UiRect grid, float x, float y, float width, const char *label,
                       uint32_t color, int index, bool automation);
static void playlist_events(Editor *e, UiRect r, UiRect grid, View *v, float scale);
static void clip_edit_begin(Editor *e);
static int clip_row(DmClip clip, const int *list, int count);
static void draw_playlist_clip(Editor *e, DmClip *clip, UiRect box, UiRect grid, float scale);
static int playlist_clips(Editor *e, UiRect r, UiRect grid, View *v, float scale, float row,
                          const int *list, int count, UiMouse mouse);
static bool playlist_label_resize(Editor *e, UiRect r, float *label, UiMouse mouse);
static void playlist_context(Editor *e, int hit);

int ed_playlist_channels(Editor *e, int *list) {
    int base[16], count = ed_channels(e, base), n = 0;

    for (int i = 0; i < count; ++i) {
        int c = base[i];
        list[n++] = c;

        if (e->project.channels[c].enabled && e->channel_expanded[c]) {
            for (int lane = 1; lane <= e->project.lane_count[c]; ++lane) {
                list[n++] = c + lane * 16;
            }
        }
    }

    return n;
}

static bool placement_valid(Editor *e, int pattern_id, int encoded) {
    if (pattern_id < 0 || (size_t)pattern_id >= e->project.pattern_count || encoded < 0 ||
        encoded >= 16 * (DM_LANES + 1)) {
        return false;
    }

    int lane = encoded / 16;
    return (!lane || e->project.patterns[pattern_id].kind) &&
           lane <= e->project.lane_count[encoded % 16];
}

static int placement_lane(Editor *e, int pattern_id, int encoded) {
    int channel = encoded % 16, lane = encoded / 16;

    if (!e->project.patterns[pattern_id].kind) {
        return lane ? -1 : 0;
    }

    lane = lane ? lane : 1;

    if (e->project.lane_count[channel] < lane) {
        e->project.lane_count[channel] = lane;
    }

    e->channel_expanded[channel] = true;
    return lane;
}

int ed_place_pattern(Editor *e, int id, int encoded, double beat) {
    if (!placement_valid(e, id, encoded) || !isfinite(beat) || beat < 0) {
        return -1;
    }

    ed_checkpoint(e);
    int lane = placement_lane(e, id, encoded);

    if (!dm_clip_add(&e->project, (DmClip){.beat = beat,
                                           .pattern = id,
                                           .channel = encoded % 16,
                                           .selected = 1,
                                           .lane = lane})) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PLACEMENT);
        return -1;
    }

    ed_select_placement_route(e, (int)e->project.clip_count - 1);
    ed_commit(e);
    return (int)e->project.clip_count - 1;
}

static void pattern_drop(Editor *e, UiRect r, UiRect grid, View *v, float scale, float row,
                         const int *list, int count, int dropped) {
    UiMouse mouse = ui_mouse();
    int pattern_id = dropped >= 0 ? dropped : e->pattern_drag;

    if (pattern_id < 0 || (size_t)pattern_id >= e->project.pattern_count ||
        !ed_inside(mouse.pos, grid)) {
        return;
    }

    int line = (int)((mouse.pos.y - r.y - 28 + v->scroll_y) / row);

    if (line < 1 || line > count) {
        return;
    }

    if (!placement_valid(e, pattern_id, list[line - 1])) {
        return;
    }

    double beat = ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, 1, mouse.alt);
    UiRect ghost = {grid.x + (float)beat * scale - v->scroll_x, r.y + 28 + line * row - v->scroll_y,
                    (float)e->project.patterns[pattern_id].length * scale, row - 4};
    ui_rect(ghost, (e->accent & 0xffffffu) | 0x44000000u, 3);
    ui_border(ghost, e->accent, 3, 2);
    ui_label_fit((UiPoint){ghost.x + 6, ghost.y + 6}, ghost.w - 12, TEXT,
                 e->project.patterns[pattern_id].name);

    if (dropped >= 0) {
        int placed = ed_place_pattern(e, pattern_id, list[line - 1], beat);

        if (placed < 0) {
            return;
        }

        ed_release_piano(e);
        ed_open_pattern(e, pattern_id, false);
        e->selected_clip = placed;
        e->pattern_channel = e->selected_channel = list[line - 1] % 16;
        e->channel_selected = true;

        if (!e->preview_override) {
            ed_preview_channel(e, e->pattern_channel, true);
        }

        ed_publish(e);
    }
}

void ed_split_note(DmPattern *p, size_t index, double at) {
    DmNote *n = &p->notes[index];

    if (at <= n->beat || at >= n->beat + n->length) {
        return;
    }

    DmNote right = *n;
    right.beat = at;
    right.length = n->beat + n->length - at;
    n->length = at - n->beat;
    dm_note_add(p, right);
}

/* Slicing keeps both halves linked to the same pattern. Each half is a window into the pattern:
   the left half keeps its start and ends at the cut, and the right half starts at the cut with
   its offset advanced by the same amount. Earlier resizes and slices are kept because the window
   is derived from the placement's current offset and length. */
void ed_split_clip(Editor *e, size_t index, double at) {
    DmClip clip = e->project.clips[index];
    double cut = at - clip.beat, length = dm_clip_length(&e->project, clip);

    if (cut <= 0 || cut >= length) {
        return;
    }

    e->project.clips[index].length = cut;
    clip.beat = at;
    clip.offset += cut;
    clip.length = length - cut;
    clip.selected = 0;

    if (!dm_clip_add(&e->project, clip)) {
        e->project.clips[index].length = length;
    }
}

bool ed_intersects(UiRect a, UiRect b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

UiRect ed_marquee_box(Editor *e, UiMouse mouse) {
    return (UiRect){fminf(e->marquee_start.x, mouse.pos.x), fminf(e->marquee_start.y, mouse.pos.y),
                    fabsf(mouse.pos.x - e->marquee_start.x),
                    fabsf(mouse.pos.y - e->marquee_start.y)};
}

bool ed_black_key(int step) {
    switch (step) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

static void playlist_tools(Editor *e) {
    bool compact = ui_available().x < 700;

    if (!compact) {
        ui_align_frame();
        ui_text(DM_TEXT_ARRANGEMENT);
        ui_same();
    }

    static const char *names[] = {DM_TEXT_SELECT, DM_TEXT_MARQUEE, DM_TEXT_SLICE, DM_TEXT_DELETE,
                                  DM_TEXT_BRUSH};
    static const UiIcon icons[] = {UI_POINTER, UI_BOX, UI_SCISSORS, UI_DELETE, UI_BRUSH};

    for (int i = 0; i < 5; ++i) {
        if (i || !compact) {
            ui_same();
        }

        ui_accent(e->tool == i);
        ui_id(500 + i);

        if (ui_icon_button("##Tool", icons[i], 42, 32)) {
            e->tool = i;
        }

        ui_pop_id();
        ui_accent_end();
        ed_action_tip(e, names[i], ACT_SELECT + i);
    }
}

void ed_playlist_label_text(Editor *e, UiRect r, float label, float row, const int *list, int i,
                            float y, char *text, size_t capacity) {
    if (!i) {
        snprintf(text, capacity, "%s", label < 80 ? DM_TEXT_EV : DM_TEXT_EVENTS_TITLE);
    } else {
        int c = list[i - 1] % 16, lane = list[i - 1] / 16;

        if (lane) {
            snprintf(text, capacity, "%s", e->project.lane_names[c][lane - 1]);
            return;
        }

        if (label < 100) {
            snprintf(text, capacity, "%02d", c + 1);
        } else {
            snprintf(text, capacity, "%02d  %s", c + 1,
                     !e->project.channels[c].enabled || e->project.channels[c].program < 0
                         ? DM_TEXT_CHOOSE_INSTRUMENT
                     : c == MIDI_DRUM_CHANNEL ? DM_TEXT_PERCUSSION
                                              : dm_program_name(e->project.channels[c].program));
        }

        if (e->channel_selected && c == e->selected_channel) {
            ui_rect((UiRect){r.x, y, 3, row}, e->accent, 0);
        }
    }
}

static bool channel_expander(Editor *e, UiRect r, float label, float row, int channel, float y,
                             UiMouse mouse) {
    /* The expander fills the row height and meets the grid edge, so a click aimed at it can
       never fall into the grid and place a pattern. */
    UiRect button = {r.x + label - 18, y, 18, row};
    bool hovered = ed_inside(mouse.pos, button) && ed_inside(mouse.pos, r);

    if (hovered) {
        ui_rect(button, mouse.left ? DIM : GRID, 0);
        ed_hover(e, DM_TEXT_EXPAND_COLLAPSE_AUTOMATION);
    }

    float x = button.x + 9, cy = y + row * .5f;

    if (e->channel_expanded[channel]) {
        ui_triangle((UiPoint){x - 4, cy - 2}, (UiPoint){x + 4, cy - 2}, (UiPoint){x, cy + 3}, TEXT);
    } else {
        ui_triangle((UiPoint){x + 2, cy - 4}, (UiPoint){x + 2, cy + 4}, (UiPoint){x - 3, cy}, TEXT);
    }

    if (hovered && mouse.clicked) {
        if (!e->project.lane_count[channel]) {
            ed_checkpoint(e);
            e->project.lane_count[channel] = 1;
            ed_changed(e);
        }

        e->channel_expanded[channel] = !e->channel_expanded[channel];
        return true;
    }

    return hovered;
}

void ed_playlist_label_action(Editor *e, int channel, int lane, UiMouse mouse) {
    if (mouse.clicked) {
        ed_release_piano(e);
        e->selected_channel = channel;
        e->channel_selected = true;
        e->context = 1;

        if (!lane &&
            (e->project.channels[channel].program < 0 || !e->project.channels[channel].enabled)) {
            e->instrument_channel = channel;
        }
    }

    if (mouse.right_clicked) {
        ed_release_piano(e);
        e->selected_channel = channel;
        e->channel_selected = true;

        if (lane || mouse.shift) {
            e->lane_channel = channel;
            e->lane_index = lane;
            e->lane_settings = true;
            snprintf(e->rename_text, 64, "%s",
                     lane ? e->project.lane_names[channel][lane - 1] : "");
        } else {
            e->instrument_channel = channel;
        }
    }
}

void ed_playlist_label(Editor *e, UiRect r, float label, float row, const int *list, int i, float y,
                       UiMouse mouse) {
    char text[128];
    ed_playlist_label_text(e, r, label, row, list, i, y, text, sizeof(text));
    int encoded = i ? list[i - 1] : 0, channel = encoded % 16, lane = encoded / 16;
    ui_clip((UiRect){r.x, y, label - 20, row});
    ui_label_fit((UiPoint){r.x + (lane ? 22 : 10), y + 22}, label - (lane ? 36 : 34),
                 i ? TEXT : DIM, text);
    ui_unclip();

    if (!i) {
        return;
    }

    if (!lane && e->project.channels[channel].enabled &&
        channel_expander(e, r, label, row, channel, y, mouse)) {
        return;
    }

    if (!ed_inside(mouse.pos, (UiRect){r.x, y, label, row}) || !ed_inside(mouse.pos, r)) {
        return;
    }

    ed_playlist_label_action(e, channel, lane, mouse);

    if (lane) {
        ed_hover(e, DM_TEXT_RIGHT_CLICK_RENAME);
    }
}

static void playlist_rows(Editor *e, UiRect r, View *v, float label, float row, const int *list,
                          int count, UiMouse mouse) {
    for (int i = 0; i < count + 1; ++i) {
        float y = r.y + 28 + i * row - v->scroll_y;
        UiRect line = {r.x, y, r.w, row};

        if (y + row < r.y + 28 || y > r.y + r.h) {
            continue;
        }

        ui_rect(line, i % 2 ? COLOR(19, 19, 19) : COLOR(24, 24, 24), 0);
        ui_line((UiPoint){r.x, y + row}, (UiPoint){r.x + r.w, y + row}, GRID, 1);
        ed_playlist_label(e, r, label, row, list, i, y, mouse);
    }
}

const char *ed_event_label(const DmBytes *m) {
    if (!m->size) {
        return DM_TEXT_EVENT;
    }

    if (m->data[0] < MIDI_SYSTEM) {
        switch (m->data[0] & MIDI_STATUS_MASK) {
        case MIDI_PROGRAM:
            return DM_TEXT_PROGRAM;
        case MIDI_CONTROL:
            return DM_TEXT_CONTROLLER;
        case MIDI_PITCH_BEND:
            return DM_TEXT_BEND;
        default:
            return DM_TEXT_CHANNEL_EVENT;
        }
    }

    if (m->data[0] != MIDI_META || m->size < 2) {
        return DM_TEXT_SYSEX;
    }

    switch (m->data[1]) {
    case MIDI_META_TEMPO:
        return DM_TEXT_TEMPO;
    case MIDI_META_TIME:
        return DM_TEXT_METER;
    case MIDI_META_MARKER:
        return dm_loop_marker(m) == 1   ? DM_TEXT_M64_LOOP_START
               : dm_loop_marker(m) == 2 ? DM_TEXT_M64_LOOP_END
                                        : DM_TEXT_MARKER;
    case 3:
        return DM_TEXT_TRACK_NAME;
    case 1:
        return DM_TEXT_TEXT;
    case 0:
        return DM_TEXT_SEQUENCE_NUMBER;
    case 2:
        return DM_TEXT_COPYRIGHT;
    case 4:
        return DM_TEXT_INSTRUMENT_NAME;
    case 5:
        return DM_TEXT_LYRICS;
    case 7:
        return DM_TEXT_CUE_POINT;
    case 8:
        return DM_TEXT_PROGRAM_NAME;
    case 9:
        return DM_TEXT_DEVICE_NAME;
    case 0x20:
        return DM_TEXT_CHANNEL_PREFIX;
    case 0x21:
        return DM_TEXT_MIDI_PORT;
    case 0x54:
        return DM_TEXT_SMPTE_OFFSET;
    case 0x59:
        return DM_TEXT_KEY_SIGNATURE;
    case 0x7f:
        return DM_TEXT_SEQUENCER_DATA;
    default:
        return DM_TEXT_MIDI_METADATA;
    }
}

void ed_song_event_open(Editor *e, int index, bool automation) {
    e->midi_events = false;
    e->song_event = true;
    e->event_edit = automation ? -1 : index;
    e->event_auto_edit = automation ? index : -1;
    e->event_source_kind = automation ? 2 : 1;
    e->event_kind = 3;

    if (automation) {
        DmAutomation a = e->project.automation[index];
        e->event_beat = (float)a.beat;
        e->event_channel = a.channel;
        e->event_program = a.a;
        return;
    }

    ed_decode_source_event(e, &e->project.events[index]);
}

static void event_card(Editor *e, UiRect grid, float x, float y, float width, const char *label,
                       uint32_t color, int index, bool automation) {
    UiRect box = {x, y, width, 22};

    if (!ed_intersects(box, grid)) {
        return;
    }

    ui_rect(box, color, 3);
    ui_label_fit((UiPoint){x + 4, y + 3}, width - 8, TEXT, label);
    UiMouse mouse = ui_mouse();

    if (ed_inside(mouse.pos, box) && ed_inside(mouse.pos, grid)) {
        ed_hover(e, label);

        if (mouse.clicked) {
            ed_song_event_open(e, index, automation);
        }
    }
}

bool ed_initial_gui_event(const DmEvent *event) {
    if (event->beat != 0 || event->message.size < 2) {
        return false;
    }

    const uint8_t *m = event->message.data;

    if (m[0] == MIDI_META) {
        return m[1] == MIDI_META_TEMPO || m[1] == MIDI_META_TIME;
    }

    return (m[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM ||
           ((m[0] & MIDI_STATUS_MASK) == MIDI_CONTROL && event->message.size == 3 &&
            (m[1] == MIDI_CC_VOLUME || m[1] == MIDI_CC_PAN));
}

static void playlist_events(Editor *e, UiRect r, UiRect grid, View *v, float scale) {
    for (size_t i = 0; i < e->project.event_count; ++i) {
        DmEvent *event = &e->project.events[i];

        if (ed_initial_gui_event(event)) {
            continue;
        }

        float x = grid.x + (float)event->beat * scale - v->scroll_x,
              y = r.y + 34 + (i % 2) * 25 - v->scroll_y;
        bool tempo = event->message.size > 1 && event->message.data[0] == MIDI_META &&
                     event->message.data[1] == MIDI_META_TEMPO;
        event_card(e, grid, x, y, ed_clampf(scale * .9f, 14, 180), ed_event_label(&event->message),
                   tempo                             ? COLOR(123, 54, 43)
                   : dm_loop_marker(&event->message) ? COLOR(40, 92, 104)
                                                     : COLOR(69, 62, 43),
                   (int)i, false);
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern >= 0 || a.type != MIDI_PROGRAM || a.beat == 0) {
            continue;
        }

        event_card(e, grid, grid.x + (float)a.beat * scale - v->scroll_x, r.y + 40 - v->scroll_y,
                   ed_clampf(scale * .9f, 14, 180), DM_TEXT_PROGRAM, COLOR(69, 62, 43), (int)i,
                   true);
    }
}

static void clip_edit_begin(Editor *e) {
    if (!e->clip_moved) {
        ed_checkpoint(e);
    }

    e->clip_moved = true;
    ed_changed(e);
}

void ed_resize_clip(Editor *e, DmClip *clip, double length) {
    if (length == dm_clip_length(&e->project, *clip)) {
        return;
    }

    clip_edit_begin(e);
    clip->length = length;
}

void ed_move_clip(Editor *e, DmClip *clip, int encoded, double beat) {
    if (!placement_valid(e, clip->pattern, encoded) || !isfinite(beat) || beat < 0) {
        return;
    }

    int channel = encoded % 16, lane = encoded / 16;

    if (e->project.patterns[clip->pattern].kind && !lane) {
        lane = 1;
    }

    if (beat == clip->beat && channel == clip->channel && lane == clip->lane) {
        return;
    }

    clip_edit_begin(e);
    lane = placement_lane(e, clip->pattern, encoded);
    clip->beat = beat;
    clip->channel = channel;
    clip->lane = lane;
    e->project.channels[channel].enabled = true;

    if (e->selected_clip >= 0 && (size_t)e->selected_clip < e->project.clip_count &&
        clip == &e->project.clips[e->selected_clip]) {
        ed_select_placement_route(e, e->selected_clip);
    }
}

void ed_playlist_drag(Editor *e, UiRect r, View *v, float row, float scale, const int *list,
                      int count, UiMouse mouse) {
    if ((e->drag != 1 && e->drag != 9) || e->drag_index < 0 ||
        (size_t)e->drag_index >= e->project.clip_count) {
        return;
    }

    DmClip *c = &e->project.clips[e->drag_index];

    if (mouse.left) {
        double delta = (mouse.pos.x - e->marquee_start.x) / scale;

        if (e->drag == 9) {
            ed_resize_clip(e, c, fmax(.25, ed_snap(e->drag_length + delta, .25, mouse.alt)));
        } else {
            int line = (int)((mouse.pos.y - r.y - 28 + v->scroll_y) / row);
            size_t clips = e->project.clip_count;

            if (e->clip_drag.size != clips * sizeof(DmClip)) {
                e->drag = 0;
                return;
            }

            const DmClip *original = (const DmClip *)e->clip_drag.data;
            double offset = ed_snap(e->drag_beat + delta, 1, mouse.alt) - e->drag_beat;
            int anchor = clip_row(original[e->drag_index], list, count),
                shift = anchor > 0 && line > 0 && line <= count ? line - anchor : 0;

            for (size_t i = 0; i < clips; i++) {
                if (original[i].selected) {
                    offset = fmax(offset, -original[i].beat);
                    int at = clip_row(original[i], list, count);

                    if (at < 1) {
                        shift = 0;
                    } else {
                        shift = (int)fmax(1 - at, fmin(count - at, shift));
                    }
                }
            }

            for (size_t i = 0; i < clips && shift; i++) {
                if (original[i].selected) {
                    int at = clip_row(original[i], list, count);

                    if (!placement_valid(e, original[i].pattern, list[at - 1 + shift])) {
                        shift = 0;
                    }
                }
            }

            for (size_t i = 0; i < clips; i++) {
                if (original[i].selected) {
                    int at = clip_row(original[i], list, count);
                    int encoded =
                        shift ? list[at - 1 + shift] : original[i].channel + original[i].lane * 16;
                    ed_move_clip(e, &e->project.clips[i], encoded, original[i].beat + offset);
                }
            }
        }
    }

    if (mouse.released) {
        e->drag = 0;
        e->clip_drag.size = 0;

        if (e->clip_moved) {
            ed_publish(e);
        }
    }
}

void ed_marquee_outline(Editor *e, int context, UiMouse mouse) {
    if (e->marquee && e->context == context) {
        UiRect rect = {
            fminf(mouse.pos.x, e->marquee_start.x), fminf(mouse.pos.y, e->marquee_start.y),
            fabsf(mouse.pos.x - e->marquee_start.x), fabsf(mouse.pos.y - e->marquee_start.y)};
        ui_border(rect, GREEN, 0, 1);

        if (mouse.released) {
            e->marquee = false;
        }
    }
}

void ed_playlist_hit_click(Editor *e, UiMouse mouse, int hit, double beat) {
    DmClip *c = &e->project.clips[hit];

    if (e->tool == 3) {
        ed_checkpoint(e);
        ed_remove_placement(e, (size_t)hit);
        ed_commit(e);
    } else if (e->tool == 2) {
        ed_checkpoint(e);
        ed_split_clip(e, (size_t)hit, beat);
        ed_commit(e);
    } else {
        if (mouse.shift && c->selected) {
            c->selected = 0;
            bool any = false;

            for (size_t i = 0; i < e->project.clip_count; ++i) {
                any |= e->project.clips[i].selected;
            }

            if (!any) {
                ed_clear_pattern_selection(e);
            }

            return;
        }

        if (!mouse.shift && !c->selected) {
            ed_deselect(e);
        }

        ed_release_piano(e);
        c->selected = 1;
        ed_open_pattern(e, c->pattern, false);
        e->linked_highlight = false;
        e->selected_clip = hit;
        e->pattern_channel = e->selected_channel = c->channel;
        e->channel_selected = true;

        if (!e->preview_override) {
            ed_preview_channel(e, c->channel, true);
        }

        e->clip_moved = false;
        e->drag = e->drag_edge ? 9 : 1;
        e->drag_length = dm_clip_length(&e->project, *c);
        e->drag_index = hit;
        e->drag_beat = c->beat;
        e->marquee_start = mouse.pos;
        e->clip_drag.size = 0;

        if (e->drag == 1 &&
            !dm_bytes(&e->clip_drag, e->project.clips, e->project.clip_count * sizeof(DmClip))) {
            e->drag = 0;
            ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PLACEMENT_DRAG);
        }
    }
}

void ed_playlist_click(Editor *e, UiRect r, UiRect grid, View *v, float row, float scale,
                       const int *list, int count, UiMouse mouse, int hit) {
    if (mouse.clicked && !e->song_event && ed_inside(mouse.pos, grid)) {
        if (mouse.ctrl) {
            ed_deselect(e);
            e->marquee = true;
            e->marquee_start = mouse.pos;
            return;
        }

        double beat = ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, 1, mouse.alt);
        int line = (int)((mouse.pos.y - r.y - 28 + v->scroll_y) / row);

        if (hit >= 0) {
            ed_playlist_hit_click(e, mouse, hit, beat);
        } else if (e->tool == 1) {
            ed_deselect(e);
            e->marquee = true;
            e->marquee_start = mouse.pos;
        } else if (line > 0 && line <= count && ed_pattern(e)) {
            ed_deselect(e);
            ed_place_pattern(e, e->selected_pattern, list[line - 1], beat);
        } else if (line == 0) {
            e->event_edit = e->event_auto_edit = -1;
            e->event_source_kind = 0;
            e->song_event = true;
            e->event_beat = (float)beat;
            e->event_bpm = (float)e->project.bpm;
            e->event_channel = e->selected_channel;
            e->event_numerator = e->project.numerator;
            e->event_denominator = e->project.denominator;
        }
    }
}

static int clip_row(DmClip clip, const int *list, int count) {
    for (int k = 0; k < count; ++k) {
        if (list[k] == clip.channel + clip.lane * 16) {
            return k + 1;
        }
    }

    return -1;
}

static void draw_playlist_clip(Editor *e, DmClip *clip, UiRect box, UiRect grid, float scale) {
    DmPattern *p = &e->project.patterns[clip->pattern];
    uint32_t color = ed_pattern_color(e, clip->pattern);
    ui_rect(box, color, 5);
    bool linked =
        e->pattern_highlight && e->linked_highlight && clip->pattern == e->selected_pattern;

    /* Pattern colors may equal the application color, so link highlights use translucent white. */
    ui_border(box,
              clip->selected ? TEXT
              : linked       ? 0xa0ffffffu
                             : color,
              5, clip->selected || linked ? 2 : 1);
    ui_clip(box);
    ui_label((UiPoint){box.x + 9, box.y + 8}, BG, p->name);
    /* Only the part of the placement inside the grid is drawn. */
    float left = fmaxf(box.x, grid.x), right = fminf(box.x + box.w, grid.x + grid.w);
    ed_note_thumbnail(p, (NoteThumbnail){.visible = {left, box.y, fmaxf(0, right - left), box.h},
                                         .x = box.x,
                                         .x_scale = scale,
                                         .offset = clip->offset,
                                         .y = box.y + box.h - 10,
                                         .y_scale = .55f,
                                         .top = box.y + 25,
                                         .bottom = box.y + box.h - 4});

    if (p->kind) {
        ed_automation_preview(e, clip->pattern,
                              (UiRect){box.x + 6, box.y + 26, box.w - 12, box.h - 30}, BG,
                              clip->offset, dm_clip_length(&e->project, *clip));
    }

    ui_unclip();
}

static int playlist_clips(Editor *e, UiRect r, UiRect grid, View *v, float scale, float row,
                          const int *list, int count, UiMouse mouse) {
    int hit = -1;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        DmClip *clip = &e->project.clips[i];
        int row_index = clip_row(*clip, list, count);

        if (row_index < 0) {
            continue;
        }

        UiRect box = {grid.x + (float)clip->beat * scale - v->scroll_x,
                      r.y + 28 + row_index * row - v->scroll_y + 5,
                      (float)dm_clip_length(&e->project, *clip) * scale - 2, row - 10};

        if (!ed_intersects(box, grid)) {
            continue;
        }

        draw_playlist_clip(e, clip, box, grid, scale);

        if (ed_inside(mouse.pos, box) && ed_inside(mouse.pos, grid)) {
            bool edge = mouse.pos.x > box.x + box.w - 8;

            if (hit < 0 || edge || !e->drag_edge) {
                hit = (int)i;
                e->drag_edge = edge;
            }

            if (e->drag_edge) {
                ui_resize_cursor(false);
            }
        }

        if (e->marquee && e->context == 1) {
            UiRect selection = ed_marquee_box(e, mouse);
            clip->selected = ed_intersects(box, selection);
        }
    }

    return hit;
}

static bool playlist_label_resize(Editor *e, UiRect r, float *label, UiMouse mouse) {
    /* The handle sits on the grid side of the edge; the row expanders own the label side. */
    float edge = mouse.pos.x - (r.x + *label);

    if ((edge >= 0 && edge < 6 && mouse.pos.y >= r.y + 28 && ed_inside(mouse.pos, r)) ||
        e->drag == 6) {
        ui_resize_cursor(false);

        if (mouse.clicked) {
            e->drag = 6;
        }

        if (mouse.left && e->drag == 6) {
            e->label_width = ed_clampf(mouse.pos.x - r.x, 45, r.w * .6f);
            *label = e->label_width;
        }

        if (mouse.released && e->drag == 6) {
            e->drag = 0;
        }

        return true;
    }

    return false;
}

void ed_playlist_remove(Editor *e, int hit) {
    if (!e->erase_active) {
        ed_checkpoint(e);
    }

    e->erase_active = true;
    ed_remove_placement(e, (size_t)hit);
    ed_commit(e);
}

static void playlist_context(Editor *e, int hit) {
    UiMouse mouse = ui_mouse();

    if (mouse.shift && mouse.right_clicked) {
        e->playlist_context_clip = hit;
    }

    int index = e->playlist_context_clip;

    if (index < 0 || (size_t)index >= e->project.clip_count ||
        !ui_shift_context(DM_TEXT_PLACEMENT_ACTIONS)) {
        return;
    }

    DmClip clip = e->project.clips[index];

    if (ui_menu_item(DM_TEXT_OPEN_PATTERN, NULL, true)) {
        ed_open_pattern(e, clip.pattern, false);
        e->linked_highlight = false;
        e->selected_clip = index;
        e->pattern_channel = clip.channel;

        if (!e->preview_override) {
            ed_preview_channel(e, clip.channel, true);
        }
    }

    if (ui_menu_item(DM_TEXT_RENAME_PATTERN, NULL, true)) {
        e->rename_pattern = clip.pattern;
        snprintf(e->rename_text, 64, "%s", e->project.patterns[clip.pattern].name);
    }

    if (ui_menu_item(DM_TEXT_MAKE_UNIQUE, NULL, true)) {
        ed_checkpoint(e);
        int id = dm_pattern_copy(&e->project, clip.pattern);

        if (id >= 0) {
            e->project.clips[index].pattern = id;
            ed_open_pattern(e, id, false);
            e->linked_highlight = false;
            e->selected_clip = index;
            e->pattern_channel = clip.channel;

            if (!e->preview_override) {
                ed_preview_channel(e, clip.channel, true);
            }

            ed_commit(e);
        }
    }

    char binding[96];

    if (ui_menu_item(DM_TEXT_CHANGE_COLOR,
                     ed_binding_text(e, ACT_NOTE_COLOR, binding, sizeof(binding)), true)) {
        ed_pattern_color_request(e, clip.pattern);
    }

    if (ui_menu_item(DM_TEXT_DELETE_PLACEMENT, NULL, true)) {
        ed_playlist_remove(e, index);
        e->playlist_context_clip = -1;
    }

    ui_popup_end();
}

void ed_playlist(Editor *e, float width, float height) {
    ui_child(DM_TEXT_PLAYLIST, width, height, false);

    if (ui_panel_clicked()) {
        ed_focus_edit(e, FOCUS_PLAYLIST);
    }

    playlist_tools(e);
    UiPoint at = ui_cursor(), space = ui_available();
    UiRect r = {at.x, at.y, space.x - 14, fmaxf(60, space.y - 24)};
    ui_canvas(DM_TEXT_PLAYLIST_CANVAS, r.w, r.h);
    UiMouse mouse = ui_mouse();
    bool hovered = ui_hover();
    int dropped = ui_drop_pattern();

    if (hovered) {
        ed_hover(e, DM_TEXT_CLICK_PLACE_PATTERN);

        if (mouse.clicked || mouse.right_clicked) {
            e->context = 1;
        }
    }

    int list[16 * (DM_LANES + 1)], count = ed_playlist_channels(e, list);
    float label = ed_clampf(e->label_width, 45, fmaxf(45, r.w * .6f)), row = 64,
          scale = 40 * e->playlist_view.zoom;

    if (playlist_label_resize(e, r, &label, mouse)) {
        hovered = false;
    }

    View *v = &e->playlist_view;
    float total = fmaxf((float)dm_duration(&e->project) * scale, r.w - label);
    ed_grid_scroll(e, v, (UiRect){r.x + label, r.y, r.w - label, r.h}, total, (count + 1) * row,
                   row, false);
    scale = 40 * v->zoom;
    total = fmaxf((float)dm_duration(&e->project) * scale, r.w - label);
    v->scroll_x = fmaxf(0, v->scroll_x);
    ui_clip(r);
    ui_rect(r, BG, 0);
    playlist_rows(e, r, v, label, row, list, count, mouse);
    UiRect grid = {r.x + label, r.y + 28, r.w - label, r.h - 28};
    ui_clip(grid);
    int start = (int)floor(v->scroll_x / scale);

    for (int i = start; i < start + (int)(grid.w / scale) + 3; ++i) {
        float x = grid.x + i * scale - v->scroll_x;
        DmMeter meter = dm_meter(&e->project, i);
        bool bar = fabs(remainder(i - meter.origin, meter.bar)) < .001;
        ui_line((UiPoint){x, grid.y}, (UiPoint){x, r.y + r.h}, bar ? COLOR(58, 58, 58) : GRID, 1);
    }

    int hit = playlist_clips(e, r, grid, v, scale, row, list, count, mouse);
    pattern_drop(e, r, grid, v, scale, row, list, count, dropped);
    playlist_events(e, r, grid, v, scale);
    playlist_context(e, hit);
    ui_unclip();
    ed_ruler(e, r, v, scale, label);
    ui_unclip();
    ed_horizontal_scroll(v, (UiRect){grid.x, r.y, grid.w, r.h}, total);
    ui_scrollbar(DM_TEXT_PLAYLIST_VERTICAL, (UiRect){r.x + r.w + 2, r.y + 28, 12, r.h - 28}, true,
                 &v->scroll_y, r.h - 28, (count + 1) * row);
    ed_timeline_drag(e, r, v, scale, label, 1, hovered);

    if (hovered) {
        ed_playlist_click(e, r, grid, v, row, scale, list, count, mouse, hit);
    }

    if (hovered && mouse.right && !mouse.shift && hit >= 0 && !e->loop_drag &&
        ed_inside(mouse.pos, grid)) {
        ed_playlist_remove(e, hit);
    }

    ed_playlist_drag(e, r, v, row, scale, list, count, mouse);
    ed_marquee_outline(e, 1, mouse);
    ui_child_end();
}
