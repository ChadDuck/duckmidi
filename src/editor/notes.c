#include "editor/internal.h"
#include "text/en.h"

static void piano_glissando(Editor *e, int key);
static bool piano_mouse_active(Editor *e, UiMouse mouse);
static void piano_keys(Editor *e, UiRect r, View *v, float label, float row, UiMouse mouse);
static void note_context(Editor *e, DmPattern *p, int hit);
static void fit_dragged_notes(Editor *e, DmPattern *p);
static void note_drag(Editor *e, DmPattern *p, float scale, float row, UiMouse mouse);
static void note_bend(Editor *e, UiRect grid, View *v, float scale, float row, UiMouse mouse);
static void remove_overlapping_notes(Editor *e, DmPattern *p, int key, double beat);
static bool start_note_drag(Editor *e, DmPattern *p, int hit, UiMouse mouse, int edge);
static void piano_hit_click(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                            UiMouse mouse, int hit, UiRect hit_box, double beat);
static void brush_cell(Editor *e, DmPattern *p, int quarter, int key);
static void preview_instrument(Editor *e, float right);
static void piano_header(Editor *e, DmPattern *p);
static void pitch_underlay(Editor *e, UiRect grid, View *v, float scale);
static int piano_notes(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale,
                       float row, UiMouse mouse, UiRect *hit_result);
static int bend_hit(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse, bool segment);
static int bend_search(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse, bool segment);
static int insert_bend_node(Editor *e, int segment, UiRect grid, View *v, float scale,
                            UiMouse mouse);
static void move_bend_node(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse);
static void note_placement_tip(Editor *e, DmPattern *p, UiRect grid, View *v, float row,
                               UiMouse mouse, bool hovered);

static bool ed_sounding(Editor *e, int key) {
    if (ed_midi_input_lit(e, key)) {
        return true;
    }

    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        if (e->held[i] == key) {
            return true;
        }
    }

    return false;
}

static void piano_glissando(Editor *e, int key) {
    int previous = e->held[SDL_SCANCODE_UNKNOWN];

    if (previous != key) {
        if (previous >= 0) {
            dm_audio_preview(e->audio, e->held_channels[SDL_SCANCODE_UNKNOWN], previous, 0, false);
        }

        e->piano_channel = ed_keyboard_channel(e);
        e->held_channels[SDL_SCANCODE_UNKNOWN] = e->piano_channel;
        dm_audio_preview(e->audio, e->piano_channel, key, 100, true);
        e->held[SDL_SCANCODE_UNKNOWN] = key;
    }
}

static bool piano_mouse_active(Editor *e, UiMouse mouse) {
    return mouse.left && e->drag == DRAG_NONE &&
           (mouse.clicked || e->held[SDL_SCANCODE_UNKNOWN] >= 0);
}

static void piano_keys(Editor *e, UiRect r, View *v, float label, float row, UiMouse mouse) {
    for (int i = 0; i < 128; ++i) {
        float y = r.y + 28 + i * row - v->scroll_y;

        if (y + row < r.y + 28 || y > r.y + r.h) {
            continue;
        }

        int key = 127 - i, step = key % 12;
        bool black = ed_black_key(step);
        uint32_t key_color = ed_sounding(e, key) ? GREEN : ui_background_shade(black ? 31 : 62);
        ui_rect((UiRect){r.x, y, r.w, row},
                black ? ui_background_shade(14) : ui_background_shade(22), 0);
        ui_rect((UiRect){r.x, y, label - 2, row - 1}, key_color, 0);
        ui_line((UiPoint){r.x + label, y + row}, (UiPoint){r.x + r.w, y + row}, GRID, 1);
        static const char *note_names[] = {DM_TEXT_C,       DM_TEXT_C_SHARP, DM_TEXT_D,
                                           DM_TEXT_D_SHARP, DM_TEXT_E,       DM_TEXT_F,
                                           DM_TEXT_F_SHARP, DM_TEXT_G,       DM_TEXT_G_SHARP,
                                           DM_TEXT_A,       DM_TEXT_A_SHARP, DM_TEXT_B};

        if (step == 0 || row >= 18) {
            char text[32];
            snprintf(text, sizeof(text), "%s%d", note_names[step], key / 12 - 1);
            ui_label_fit((UiPoint){r.x + 9, y + 3}, label - 18, ui_text_on(key_color),
                         dm_channel_drums(&e->project, e->pattern_channel) ? dm_drum_name(key)
                                                                           : text);
        }

        if (piano_mouse_active(e, mouse) && ed_inside(mouse.pos, (UiRect){r.x, y, label, row}) &&
            ed_inside(mouse.pos, r)) {
            piano_glissando(e, key);
        }
    }
}

static void note_context(Editor *e, DmPattern *p, int hit) {
    UiMouse mouse = ui_mouse();

    if (mouse.right_clicked && ed_gesture_held(e, GESTURE_MENU, mouse)) {
        if (hit >= 0 && (size_t)hit < p->count) {
            e->automation_center = p->notes[hit].key;
            e->note_ports[e->pattern_channel] = p->notes[hit].port;
        }

        e->context_note = hit;
    }

    hit = e->context_note;

    if (ui_shift_context(DM_TEXT_NOTE_EVENTS)) {
        if (hit >= 0 && (size_t)hit < p->count) {
            int velocity = p->notes[hit].velocity;

            if (ui_slider(DM_TEXT_VELOCITY, &velocity, 1, 127, 180)) {
                if (ed_checkpoint(e)) {
                    p->notes[hit].velocity = (uint8_t)velocity;
                    ed_commit(e);
                }
            }
        }

        if (ui_menu_item(DM_TEXT_MODULATION_CC1, NULL, true)) {
            e->automation_cc = 1;
            e->automation_open = true;
        }

        if (ui_menu_item(DM_TEXT_EXPRESSION_CC11, NULL, true)) {
            e->automation_cc = 11;
            e->automation_open = true;
        }

        if (ui_menu_item(DM_TEXT_TREMOLO_DEPTH_CC92, NULL, true)) {
            e->automation_cc = 92;
            e->automation_open = true;
        }

        if (ui_menu_item(DM_TEXT_VIBRATO_RATE_CC76, NULL, true)) {
            e->automation_cc = 76;
            e->automation_open = true;
        }

        if (ui_menu_item(DM_TEXT_VIBRATO_DEPTH_CC77, NULL, true)) {
            e->automation_cc = 77;
            e->automation_open = true;
        }

        const int controls[] = {64, 66, 5, 91, 93, 74, 71};
        const char *names[] = {DM_TEXT_SUSTAIN_CC64,     DM_TEXT_SOSTENUTO_CC66,
                               DM_TEXT_PORTAMENTO_CC5,   DM_TEXT_REVERB_SEND_CC91,
                               DM_TEXT_CHORUS_SEND_CC93, DM_TEXT_BRIGHTNESS_CC74,
                               DM_TEXT_RESONANCE_CC71};

        for (int i = 0; i < 7; ++i) {
            if (ui_menu_item(names[i], NULL, true)) {
                e->automation_cc = controls[i];
                e->automation_open = true;
            }
        }

        if (ui_menu_item(DM_TEXT_PITCH_BEND, NULL, true)) {
            e->automation_cc = DM_EVENT_BEND;
            e->automation_open = true;
        }

        ui_popup_end();
    }
}

void ed_resize_note_group(Editor *e, DmPattern *p, double delta, bool free) {
    double offset;

    if (e->drag_edge < 0) {
        offset = ed_snap(e->drag_beat + delta, .25, free) - e->drag_beat;
        double lo = -HUGE_VAL, hi = HUGE_VAL;

        for (size_t i = 0; i < e->drag_note_count; ++i) {
            DmNote n = e->drag_notes[i];

            if (!n.selected) {
                continue;
            }

            lo = fmax(lo, -n.beat);
            hi = fmin(hi, n.length - 1.0 / DM_PPQ);
        }

        offset = fmax(lo, fmin(hi, offset));
    } else {
        double end = e->drag_beat + e->drag_length;
        offset = ed_snap(end + delta, .25, free) - end;

        for (size_t i = 0; i < e->drag_note_count; ++i) {
            DmNote n = e->drag_notes[i];

            if (n.selected) {
                offset = fmax(offset, 1.0 / DM_PPQ - n.length);
            }
        }
    }

    for (size_t i = 0; i < e->drag_note_count; ++i) {
        DmNote n = e->drag_notes[i];

        if (!n.selected) {
            continue;
        }

        p->notes[i].beat = n.beat + (e->drag_edge < 0 ? offset : 0);
        p->notes[i].length = n.length + (e->drag_edge < 0 ? -offset : offset);
    }
}

bool ed_gesture_moved(Editor *e, UiMouse mouse) {
    return fabsf(mouse.pos.x - e->marquee_start.x) > .5f ||
           fabsf(mouse.pos.y - e->marquee_start.y) > .5f;
}

bool ed_begin_edit_move(Editor *e) {
    if (!e->edit_moved) {
        if (!ed_checkpoint(e)) {
            e->drag = DRAG_NONE;
            e->drag_index = -1;
            e->drag_edge = 0;
            e->copy_button = -1;
            e->copy_pending = e->lone_click = false;
            e->event_drag.size = 0;
            free(e->drag_notes);
            e->drag_notes = NULL;
            e->drag_note_count = 0;
            return false;
        }
    }

    e->edit_moved = true;
    return true;
}

static void fit_dragged_notes(Editor *e, DmPattern *p) {
    double end = 0;

    for (size_t i = 0; i < p->count; ++i) {
        end = fmax(end, p->notes[i].beat + p->notes[i].length);
    }

    if (end > p->length) {
        p->length = end;
        dm_pattern_fit(&e->project, p);
    }
}

/* The copy gesture's first movement places copies of the selected notes over them, then drags
   the copies. Copies keep each note's length, velocity and color; pitch bends stay with the
   originals. One undo step covers both. */
static void make_note_copies(Editor *e, DmPattern *p) {
    if (!ed_begin_edit_move(e)) {
        return;
    }

    size_t originals = p->count;
    int dragged = e->drag_index;

    for (size_t i = 0; i < originals; ++i) {
        if (!p->notes[i].selected) {
            continue;
        }

        DmNote copy = p->notes[i];
        copy.bend_id = 0;
        p->notes[i].selected = 0;

        if (!dm_note_add(p, copy)) {
            ed_message(e, DM_TEXT_CANNOT_ALLOCATE_NOTE);
            break;
        }

        if ((int)i == dragged) {
            e->drag_index = (int)p->count - 1;
        }
    }

    e->copy_pending = false;
    free(e->drag_notes);
    e->drag_notes = malloc(p->count * sizeof(*p->notes));
    e->drag_note_count = e->drag_notes ? p->count : 0;

    if (!e->drag_notes) {
        e->drag = DRAG_NONE;
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_NOTE);
        return;
    }

    memcpy(e->drag_notes, p->notes, p->count * sizeof(*p->notes));
}

static void note_drag(Editor *e, DmPattern *p, float scale, float row, UiMouse mouse) {
    if (e->drag == DRAG_NOTE && e->drag_index >= 0 && (size_t)e->drag_index < p->count) {
        if (e->drag_edge) {
            ui_resize_cursor(false);
        }

        double delta = (mouse.pos.x - e->marquee_start.x) / scale;
        bool copying = e->copy_button >= 0;
        bool held = copying ? ed_mouse_down(mouse, e->copy_button) : mouse.left;

        if (held && ed_gesture_moved(e, mouse)) {
            if (e->copy_pending) {
                make_note_copies(e, p);

                if (e->drag != DRAG_NOTE) {
                    return;
                }
            }

            if (!ed_begin_edit_move(e)) {
                return;
            }

            if (e->drag_edge) {
                ed_resize_note_group(e, p, delta, ed_gesture_held(e, GESTURE_FREE, mouse));
            } else {
                DmNote dragged = p->notes[e->drag_index];
                double previous_offset = dragged.beat - e->drag_beat;
                int previous_pitch = dragged.key - e->drag_key;
                double offset =
                    ed_snap(e->drag_beat + delta, .25, ed_gesture_held(e, GESTURE_FREE, mouse)) -
                    e->drag_beat;
                int pitch_delta = -(int)roundf((mouse.pos.y - e->marquee_start.y) / row);

                for (size_t i = 0; i < e->drag_note_count; ++i) {
                    DmNote original = e->drag_notes[i];

                    if (original.selected) {
                        offset = fmax(offset, -original.beat);
                        pitch_delta =
                            (int)fmax(-original.key, fmin(127 - original.key, pitch_delta));
                    }
                }

                if (!copying) {
                    offset =
                        previous_offset + ed_note_bend_offset(e, e->drag_notes, e->drag_note_count,
                                                              offset - previous_offset);
                }

                for (size_t i = 0; i < e->drag_note_count; ++i) {
                    DmNote original = e->drag_notes[i];

                    if (original.selected) {
                        p->notes[i].beat = original.beat + offset;
                        p->notes[i].key = (uint8_t)(original.key + pitch_delta);
                    }
                }

                /* Copies leave pitch bends with the notes they were copied from. */
                if (!copying && (offset != previous_offset || pitch_delta != previous_pitch)) {
                    ed_follow_note_curves(e, e->selected_pattern, e->drag_notes, e->drag_note_count,
                                          previous_offset);
                }
            }

            if (!e->drag_edge) {
                ed_edit_feedback(e, p->notes[e->drag_index].key);
            }

            fit_dragged_notes(e, p);
            ed_changed(e);
        }

        if (copying ? !held : mouse.released) {
            /* A plain click on a selected note selects only that note. */
            if (!copying && e->lone_click && !e->edit_moved) {
                for (size_t i = 0; i < p->count; ++i) {
                    p->notes[i].selected = (int)i == e->drag_index;
                }
            }

            e->lone_click = false;
            e->drag = DRAG_NONE;
            e->copy_button = -1;
            e->copy_pending = false;

            if (e->edit_moved) {
                ed_publish(e);
            }
        }
    }
}

/* Shift + drag from a note draws a pitch bend. Released without moving, the press was a Shift +
   click, which only added the note to the selection. */
static void note_bend(Editor *e, UiRect grid, View *v, float scale, float row, UiMouse mouse) {
    if (e->drag == DRAG_NOTE_BEND) {
        bool moved = ed_pick_moved(e, e->bend_press, mouse);

        if (moved) {
            ui_clip(grid);
            ui_line(e->marquee_start, mouse.pos, 0x8897dbff, 3);
            ui_unclip();
        }

        if (mouse.released && !moved) {
            e->drag = DRAG_NONE;
        } else if (mouse.released) {
            double start = fmax(0, (e->marquee_start.x - grid.x + v->scroll_x) / scale),
                   end = fmax(0, (mouse.pos.x - grid.x + v->scroll_x) / scale);
            int semitones = (int)roundf((e->marquee_start.y - mouse.pos.y) / row);
            semitones = (int)ed_clampf(e->drag_key + semitones, 0, 127) - e->drag_key;
            start = ed_snap(start, .25, false);
            end = ed_snap(end, .25, false);
            e->bend_range = (int)fmax(12, abs(semitones));
            int value = (int)ed_clampf(
                MIDI_BEND_CENTER + semitones * MIDI_BEND_CENTER / e->bend_range, 0, MIDI_BEND_MAX);
            e->drag = DRAG_NONE;

            /* Its own undo step, separate from the note the line starts on. */
            if (!ed_automation_line_overlaps(e, start, end, -2)) {
                if (!ed_checkpoint(e)) {
                    return;
                }
            }

            DmPattern *p = ed_pattern(e);

            if (p && e->drag_index >= 0 && (size_t)e->drag_index < p->count) {
                ed_note_bend_line(e, start, end, value, &p->notes[e->drag_index]);
            }
            ed_commit(e);
        }
    }
}

static void remove_overlapping_notes(Editor *e, DmPattern *p, int key, double beat) {
    if (!e->overwrite) {
        for (size_t i = p->count; i > 0; --i) {
            DmNote n = p->notes[i - 1];

            if (n.port == ed_note_port(e) && n.key == key && n.beat < beat + .25 &&
                n.beat + n.length > beat) {
                memmove(&p->notes[i - 1], &p->notes[i], (p->count - i) * sizeof(n));
                --p->count;
            }
        }
    }
}

static bool start_note_drag(Editor *e, DmPattern *p, int hit, UiMouse mouse, int edge) {
    DmNote *n = &p->notes[hit];
    e->note_ports[e->pattern_channel] = n->port;
    e->edit_moved = false;
    e->drag = DRAG_NOTE;
    e->lone_click = false;
    e->drag_index = hit;
    e->drag_beat = n->beat;
    e->drag_length = n->length;
    e->drag_key = n->key;
    e->drag_edge = edge;
    free(e->drag_notes);
    e->drag_notes = malloc(p->count * sizeof(*p->notes));
    e->drag_note_count = e->drag_notes ? p->count : 0;

    if (!e->drag_notes) {
        e->drag = DRAG_NONE;
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_NOTE);
        return false;
    }

    memcpy(e->drag_notes, p->notes, p->count * sizeof(*p->notes));
    e->marquee_start = mouse.pos;
    return true;
}

/* Width of a note's resize edge inside the note, in pixels: it follows the zoom (scale is pixels
   per beat), so zoomed out most of a note moves it, and never takes more than a fifth of it. */
static float note_edge(float scale, float width) {
    return fminf(ed_clampf(scale * .06f, 2, 10), width * .2f);
}

/* -1 on the left resize edge, 1 on the right one, 0 inside. The hit area reaches outside the note
   by half an edge, so short notes stay easy to resize. */
static int note_edge_side(UiRect box, float scale, float x) {
    float edge = note_edge(scale, box.w);
    return x < box.x + edge ? -1 : x > box.x + box.w - edge ? 1 : 0;
}

static void piano_hit_click(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                            UiMouse mouse, int hit, UiRect hit_box, double beat) {
    DmNote *n = &p->notes[hit];

    if (e->tool == TOOL_DELETE) {
        if (!ed_checkpoint(e)) {
            return;
        }

        memmove(n, n + 1, (p->count - hit - 1) * sizeof(*n));
        --p->count;
        ed_commit(e);
    } else if (e->tool == TOOL_SLICE) {
        if (!ed_checkpoint(e)) {
            return;
        }

        ed_split_note(p, (size_t)hit, beat);
        ed_commit(e);
    } else {
        bool lone = !ed_gesture_held(e, GESTURE_EXTEND, mouse) && n->selected;

        if (!ed_gesture_held(e, GESTURE_EXTEND, mouse) && !n->selected) {
            ed_deselect(e);
        }

        n->selected = 1;
        int edge = note_edge_side(hit_box, scale, mouse.pos.x);

        if (!start_note_drag(e, p, hit, mouse, edge)) {
            return;
        }

        e->lone_click = lone;

        if (!e->drag_edge && !ed_gesture_held(e, GESTURE_NOTE_BEND, mouse)) {
            ed_edit_feedback(e, n->key);
        }

        /* Bend lines on notes belong to Point mode; Graph mode edits bends in the events. */
        if (ed_gesture_held(e, GESTURE_NOTE_BEND, mouse) && !e->drag_edge &&
            !dm_graph_resolution(p, DM_EVENT_BEND)) {
            e->bend_press = mouse.pos;
            e->pick_moved = false;
            e->marquee_start.x =
                grid.x +
                (float)ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, .25, false) * scale -
                v->scroll_x;
            e->marquee_start.y = grid.y + (127 - n->key) * row - v->scroll_y + row * .5f;
            e->drag = DRAG_NOTE_BEND;
            e->automation_cc = DM_EVENT_BEND;
            e->automation_center = n->key;
        }
    }
}

static void brush_cell(Editor *e, DmPattern *p, int quarter, int key) {
    if (quarter < 0 || key < 0 || key > 127) {
        return;
    }

    double beat = quarter * .25;

    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].key == key && fabs(p->notes[i].beat - beat) < 1e-8) {
            return;
        }
    }

    remove_overlapping_notes(e, p, key, beat);

    if (ed_add_note(e, p,
                    (DmNote){beat, .25, key, ED_DEFAULT_VELOCITY, 1, (uint8_t)ed_note_port(e)})) {
        ed_changed(e);
    }
}

void ed_brush_drag(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                   UiMouse mouse) {
    if (e->drag != DRAG_BRUSH) {
        return;
    }

    if (mouse.left) {
        int quarter = (int)floor(fmax(0, (mouse.pos.x - grid.x + v->scroll_x) / scale) * 4);
        int key =
            ed_gesture_held(e, GESTURE_BRUSH_KEY, mouse)
                ? e->brush_anchor
                : (int)ed_clampf(127 - floorf((mouse.pos.y - grid.y + v->scroll_y) / row), 0, 127);
        int steps = (int)fmax(abs(quarter - e->brush_quarter), abs(key - e->brush_pitch));

        for (int i = 0; i <= steps; ++i) {
            double t = steps ? (double)i / steps : 0;
            brush_cell(e, p, (int)round(e->brush_quarter + t * (quarter - e->brush_quarter)),
                       (int)round(e->brush_pitch + t * (key - e->brush_pitch)));
        }

        e->brush_quarter = quarter;
        e->brush_pitch = key;
        ed_edit_feedback(e, key);
    }

    if (mouse.released) {
        e->drag = DRAG_NONE;
        ed_publish(e);
    }
}

void ed_brush_begin(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse) {
    if (!ed_checkpoint(e)) {
        return;
    }

    ed_deselect(e);
    e->drag = DRAG_BRUSH;
    e->brush_quarter = (int)floor(fmax(0, (mouse.pos.x - grid.x + v->scroll_x) / scale) * 4);
    e->brush_pitch = e->brush_anchor =
        (int)ed_clampf(127 - floorf((mouse.pos.y - grid.y + v->scroll_y) / row), 0, 127);
    ed_brush_drag(e, p, grid, v, scale, row, mouse);
}

/* The copy gesture on a note selects it (keeping a selection it belongs to) and holds the
   group; the copies are made once the mouse moves. */
static bool start_note_copy(Editor *e, DmPattern *p, UiMouse mouse, int hit) {
    if (!p->notes[hit].selected) {
        ed_deselect(e);
        p->notes[hit].selected = 1;
    }

    if (!start_note_drag(e, p, hit, mouse, 0)) {
        return false;
    }

    e->pan_context = CONTEXT_NONE;
    e->copy_button = e->gestures[GESTURE_COPY_NOTES].button;
    e->copy_pending = true;
    ed_edit_feedback(e, p->notes[hit].key); /* Sounds at the press, like a left-drag. */
    return true;
}

void ed_piano_click(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, int hit, UiRect hit_box) {
    bool marquee =
        mouse.clicked && (ed_gesture_held(e, GESTURE_MARQUEE, mouse) || e->tool == TOOL_MARQUEE);

    if (hit >= 0 && !marquee && ed_inside(mouse.pos, grid) && e->tool != TOOL_SLICE &&
        e->tool != TOOL_DELETE && ed_gesture_pressed(e, GESTURE_COPY_NOTES, mouse)) {
        e->automation_node = -1;
        start_note_copy(e, p, mouse, hit);
        return;
    }

    if (mouse.clicked && ed_inside(mouse.pos, grid)) {
        if (e->tool == TOOL_BRUSH) {
            ed_brush_begin(e, p, grid, v, scale, row, mouse);
            return;
        }

        if (marquee) {
            e->automation_node = -1;
            ed_marquee_begin(e, FOCUS_NOTES, mouse, hit);
            return;
        }

        e->automation_node = -1;
        double beat = ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, .25,
                              ed_gesture_held(e, GESTURE_FREE, mouse));
        int key =
            (int)ed_clampf(127 - floorf((mouse.pos.y - r.y - 28 + v->scroll_y) / row), 0, 127);

        if (hit >= 0) {
            piano_hit_click(e, p, grid, v, scale, row, mouse, hit, hit_box, beat);
        } else {
            if (!ed_checkpoint(e)) {
                return;
            }

            ed_deselect(e);
            remove_overlapping_notes(e, p, key, beat);

            if (ed_add_note(e, p,
                            (DmNote){beat, .25, (uint8_t)key, ED_DEFAULT_VELOCITY, 1,
                                     (uint8_t)ed_note_port(e)})) {
                ed_changed(e);

                if (start_note_drag(e, p, (int)p->count - 1, mouse, 0)) {
                    /* Insertion already owns the undo snapshot; dragging stays in the same action.
                     */
                    e->edit_moved = true;
                    ed_edit_feedback(e, key);
                } else {
                    ed_publish(e);
                }
            }
        }
    }
}

void ed_select_preview_instrument(Editor *e, int program) {
    ed_release_piano(e);
    ed_focus_edit(e, FOCUS_NOTES);
    e->preview_program = program;
    e->preview_override = true;
    ed_keyboard_channel(e);

    if (e->pattern_mode) {
        ed_publish(e);
    }
}

enum { PREVIEW_RESET_WIDTH = 72 };

/* Note port buttons alone, for a placed pattern whose instrument dropdown is hidden. */
static void port_buttons_only(Editor *e, float right) {
    float ports = ed_port_buttons_width(e);
    /* Keeps the header as tall as the instrument buttons so the roll stays in place. */
    ui_dummy(0, 32);

    if (!ports) {
        return;
    }

    ui_same();
    UiPoint at = ui_cursor();

    if (ui_available().x < ports) {
        ui_newline();
        at = ui_cursor();
    }

    ui_position(fmaxf(at.x, right - ports), at.y);
    ed_port_buttons(e);
    ui_dummy(0, 0); /* Each port button keeps the line open; the roll starts below. */
}

static void preview_instrument(Editor *e, float right) {
    if (!ed_preview_instrument_shown(e)) {
        port_buttons_only(e, right);
        return;
    }

    ed_follow_preview(e);

    /* Shows the instrument playing at the current position; choosing one still sets the
       preview instrument, which overrides program changes in pattern playback. */
    int shown = !e->preview_override && e->preview_source_channel >= 0 ? ed_placed_program(e)
                                                                       : e->preview_program;

    float ports = ed_port_buttons_width(e);
    float buttons = 188 + PREVIEW_RESET_WIDTH + 8;
    float width = fminf(220, fmaxf(100, ui_available().x - buttons - 2 - ports));
    UiPoint at = ui_cursor();

    if (ui_available().x < width + buttons + ports) {
        ui_newline();
        at = ui_cursor();
    }

    if (ui_available().x < width + buttons + ports && ports) {
        ed_port_buttons(e);
        ui_newline();
        at = ui_cursor();
        ports = 0;
    }

    ui_position(fmaxf(at.x, right - width - buttons - ports), at.y);

    if (ports) {
        ed_port_buttons(e);
    }

    ui_width(width);
    const char *name = shown == 128 ? DM_TEXT_PERCUSSION : dm_program_name(shown);

    if (ui_bar_combo("##Preview instrument", name)) {
        for (int i = 0; i <= 128; ++i) {
            const char *label = i == 128 ? DM_TEXT_PERCUSSION : dm_program_name(i);

            if (ui_select(label, shown == i, 0, 0)) {
                ed_select_preview_instrument(e, i);
            }
        }

        ui_combo_end();
    }

    ui_same();
    ui_disabled(!e->channel_selected);

    if (ui_button(DM_TEXT_USE_SELECTED_CHANNEL, 180, 32)) {
        ed_select_preview_instrument(e, ed_channel_program(e, e->selected_channel));
    }

    ui_disabled_end();
    ui_same();
    ui_disabled(!e->preview_override);

    if (ui_button(DM_TEXT_RESET_PREVIEW_INSTRUMENT, PREVIEW_RESET_WIDTH, 32)) {
        ed_reset_preview_instrument(e);
    }

    ui_disabled_end();
}

static void piano_header(Editor *e, DmPattern *p) {
    UiPoint top = ui_cursor();
    float right = ui_cursor().x + ui_available().x;
    ui_position(top.x + 2, top.y + 2);

    if (ui_editor_menu()) {
        if (ui_menu_item(DM_TEXT_COLOR_SELECTED_NOTES, "Alt+C", true)) {
            e->note_color_popup = true;
        }

        /* Shown for percussion: the drum channel's pattern, or the Percussion preview. */
        if ((e->preview_program == 128 || dm_channel_drums(&e->project, e->pattern_channel) ||
             (e->channel_selected && dm_channel_drums(&e->project, e->selected_channel))) &&
            ui_menu_item(DM_TEXT_PERCUSSION_KEYBOARD_MAPPING, NULL, true)) {
            ed_percussion_open(e);
        }

        if (ui_menu_item(DM_TEXT_QUANTIZE, NULL, true)) {
            ed_quantize_notes(e);
        }

        ui_popup_end();
    }

    ui_position(top.x + 28, top.y);
    ui_align_frame();
    ui_text(DM_TEXT_PIANO_ROLL);
    ui_same();
    ui_toggle(DM_TEXT_EVENTS, &e->automation_open);
    ui_same();
    preview_instrument(e, right);
    ed_note_color_dialog(e);
}

static void pitch_underlay(Editor *e, UiRect grid, View *v, float scale) {
    UiPoint previous = {0};
    bool has = false;
    DmAutomation before = {0};
    ed_pitch_cache_begin(e);
    size_t first = ed_automation_visible_first(e, (v->scroll_x - 8) / scale, true);

    for (size_t i = first; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) ||
            a.type != MIDI_PITCH_BEND || a.curve < 0) {
            continue;
        }

        float pitch = ed_automation_pitch(e, a);
        UiPoint point = {grid.x + (float)a.beat * scale - v->scroll_x,
                         grid.y + (127 - pitch) * 20 - v->scroll_y + 10};

        if (has && previous.x > grid.x + grid.w + 8) {
            break;
        }

        if (point.x < grid.x - 8 && (!has || previous.x < grid.x - 8)) {
            previous = point;
            before = a;
            has = true;
            continue;
        }

        if (has) {
            ed_automation_connection(previous, point, before, a,
                                     ed_curve_units(e, before, grid, true),
                                     (ed_point_color(e) & 0xffffffu) | 0x88000000u);
        }

        ui_circle(point, e->automation_node == (int)i ? 6 : 3,
                  e->automation_node == (int)i ? TEXT
                                               : (ed_point_color(e) & 0xffffffu) | 0x99000000u);
        previous = point;
        has = true;
        before = a;
    }

    ed_pitch_cache_end(e);
}

/* The velocity line contrasts with the note: dark on light colors, light on dark ones. */
static uint32_t velocity_line_color(uint32_t color) {
    float r = (float)(color & 0xffu), g = (float)((color >> 8) & 0xffu),
          b = (float)((color >> 16) & 0xffu);
    float luminance = (.299f * r + .587f * g + .114f * b) / 255.0f;
    return luminance > .5f ? 0x8c000000u : 0xb4ffffffu;
}

/* A note's drawn box in the piano roll. */
static UiRect note_box(DmNote n, UiRect r, UiRect grid, View *v, float scale, float row) {
    return (UiRect){grid.x + (float)n.beat * scale - v->scroll_x,
                    r.y + 28 + (127 - n.key) * row - v->scroll_y + 1,
                    fmaxf(3, (float)n.length * scale), row - 2};
}

/* What a click on a note hits: its box, widened a little so short notes stay easy to reach. */
static UiRect note_target(UiRect box, float scale) {
    float pad = ed_clampf(scale * .03f, 1, 4);
    return (UiRect){box.x - pad, box.y, fmaxf(8, box.w + 2 * pad), box.h};
}

/* A right-drag erases every note its path crossed this frame (see ed_sweep). */
static void erase_swept_notes(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale,
                              float row, UiMouse mouse) {
    UiPoint from, to;
    ed_sweep(e, mouse, &from, &to);
    bool any = false;

    for (size_t i = 0; i < p->count && !any; ++i) {
        any = ed_sweep_hits(from, to,
                            note_target(note_box(p->notes[i], r, grid, v, scale, row), scale));
    }

    if (!any) {
        return;
    }

    if (!ed_erase_step(e)) {
        return;
    }

    size_t kept = 0;

    for (size_t i = 0; i < p->count; ++i) {
        if (!ed_sweep_hits(from, to,
                           note_target(note_box(p->notes[i], r, grid, v, scale, row), scale))) {
            p->notes[kept++] = p->notes[i];
        }
    }

    p->count = kept;
    ed_commit(e);
}

static int piano_notes(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale,
                       float row, UiMouse mouse, UiRect *hit_result) {
    int hit = -1;
    UiRect hit_box = {0};
    bool marquee = e->marquee && ed_selection_focus(e) == FOCUS_NOTES;
    UiRect selection = ed_marquee_box(e, mouse);

    for (size_t i = 0; i < p->count; ++i) {
        DmNote n = p->notes[i];
        UiRect box = note_box(n, r, grid, v, scale, row);

        bool visible = ed_intersects(box, grid);

        /* Notes have no order to select a range in, so a range click adds the note. */
        if (marquee) {
            bool clicked = (int)i == e->pick_hit;
            n.selected = ed_marquee_selects(e, i, visible && ed_intersects(box, selection),
                                            ed_picked_before(e, i) || clicked, mouse);
            p->notes[i].selected = n.selected;
        }

        if (!visible) {
            continue;
        }

        uint32_t color = ed_port_color(e, n.port);
        ui_rect(box, color, 3);
        /* Notes use the application or port color, so selection is outlined in white. */
        ui_border(box, n.selected ? TEXT : color, 3, n.selected ? 2 : 1);
        ui_line((UiPoint){box.x + 3, box.y + box.h - 3},
                (UiPoint){box.x + box.w * fminf(1, n.velocity / 127.0f), box.y + box.h - 3},
                velocity_line_color(color), 2);
        UiRect target = note_target(box, scale);

        if (ed_inside(mouse.pos, target) && ed_inside(mouse.pos, grid)) {
            hit = (int)i;
            hit_box = box;
        }
    }

    *hit_result = hit_box;
    return hit;
}

UiPoint ed_bend_point(Editor *e, DmAutomation a, UiRect grid, View *v, float scale) {
    return (UiPoint){grid.x + (float)a.beat * scale - v->scroll_x,
                     grid.y + (127 - ed_automation_pitch(e, a)) * 20 - v->scroll_y + 10};
}

static int bend_hit(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse, bool segment) {
    ed_pitch_cache_begin(e);
    int hit = bend_search(e, grid, v, scale, mouse, segment);
    ed_pitch_cache_end(e);
    return hit;
}

static int bend_search(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse, bool segment) {
    int previous = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) ||
            a.type != MIDI_PITCH_BEND || a.curve < 0) {
            continue;
        }

        UiPoint point = ed_bend_point(e, a, grid, v, scale);

        if (!segment && hypotf(point.x - mouse.pos.x, point.y - mouse.pos.y) < 9) {
            return (int)i;
        }

        if (segment && previous >= 0 &&
            ed_connection_pair(e, e->project.automation[previous], a, true)) {
            DmAutomation before = e->project.automation[previous];
            UiPoint first = ed_bend_point(e, before, grid, v, scale);
            double t = (mouse.pos.x - first.x) / (point.x - first.x);
            double linear =
                ed_event_value(before) + (ed_event_value(a) - ed_event_value(before)) * t;
            float y = first.y;

            if (dm_automation_curves(before)) {
                y += (point.y - first.y) * (float)t -
                     (float)(dm_curve_value(before, a, t) - linear) *
                         ed_curve_units(e, before, grid, true);
            }

            if (t > 0 && t < 1 && fabsf(mouse.pos.y - y) < 7) {
                return previous;
            }
        }

        previous = (int)i;
    }

    return -1;
}

static int insert_bend_node(Editor *e, int segment, UiRect grid, View *v, float scale,
                            UiMouse mouse) {
    DmAutomation a = e->project.automation[segment];
    a.beat = ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, .25, false);
    // Subdivide the existing connection before dragging the inserted point.
    for (size_t j = (size_t)segment + 1; j < e->project.automation_count; ++j) {
        DmAutomation b = e->project.automation[j];

        if (b.pattern != a.pattern || b.type != MIDI_PITCH_BEND || b.curve != a.curve) {
            continue;
        }

        DmAutomation first = e->project.automation[segment];
        a.beat = fmax(first.beat, fmin(b.beat, a.beat));

        if (a.beat == first.beat) {
            return segment;
        }

        if (a.beat == b.beat) {
            return (int)j;
        }

        a = ed_split_curve_node(e, segment, b, a.beat);
        break;
    }

    a.bend_note = 0;
    a.bend_root = DM_BEND_ROOT_NONE;
    dm_automation_add(&e->project, a);
    int hit = -1;

    for (size_t j = 0; j < e->project.automation_count; ++j) {
        DmAutomation b = e->project.automation[j];

        if (b.pattern == a.pattern && b.curve == a.curve && b.type == a.type && b.beat == a.beat &&
            b.a == a.a && b.b == a.b) {
            hit = (int)j;
        }
    }

    return hit;
}

static void move_bend_node(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse) {
    DmAutomation *a = &e->project.automation[e->automation_node];

    if (mouse.left && ed_gesture_moved(e, mouse)) {
        if (!ed_begin_edit_move(e)) {
            return;
        }

        float pitch = roundf(127 - (mouse.pos.y - grid.y + v->scroll_y - 10) / 20);
        DmAutomation edited = ed_expand_pitch_range(e, *a, pitch);
        e->automation_node = -1;
        ed_select_edited_node(e, edited);
        a = &e->project.automation[e->automation_node];
        a->beat = ed_snap(e->node_beat + (mouse.pos.x - e->marquee_start.x) / scale, .25, false);
        ed_set_event_value(a, (int)ed_clampf(MIDI_BEND_CENTER + (pitch - ed_pitch_anchor(e, *a)) *
                                                                    MIDI_BEND_CENTER /
                                                                    ed_automation_range(e, *a),
                                             0, MIDI_BEND_MAX));
        ed_edit_feedback(e, ed_automation_pitch(e, *a));
        ed_changed(e);
    }

    if (mouse.released && !e->edit_moved) {
        e->drag = DRAG_NONE;
        return;
    }

    if (mouse.released) {
        e->automation_cc = DM_EVENT_BEND;
        ed_curve_restore_position(e, a->curve);
        ed_sort_automation(e);
        e->automation_node = -1;
        e->drag = DRAG_NONE;
        ed_commit(e);
    }
}

bool ed_bend_mouse(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse) {
    if (e->drag == DRAG_BEND_POINT) {
        move_bend_node(e, grid, v, scale, mouse);
        return true;
    }

    if (ed_gesture_held(e, GESTURE_MARQUEE, mouse) || e->tool == TOOL_MARQUEE) {
        return false;
    }

    if (!ed_inside(mouse.pos, grid) || !mouse.clicked ||
        (!ed_gesture_held(e, GESTURE_BEND_POINT, mouse) &&
         !ed_gesture_held(e, GESTURE_BEND_EDIT, mouse))) {
        return false;
    }

    int hit = bend_hit(e, grid, v, scale, mouse, false);

    /* Shift + click selects a point; Shift + drag moves it, as Alt does. */
    if (hit >= 0 && ed_gesture_held(e, GESTURE_BEND_EDIT, mouse)) {
        e->automation_all = false;
    } else if (!ed_gesture_held(e, GESTURE_BEND_POINT, mouse)) {
        return false;
    }

    int segment = hit < 0 ? bend_hit(e, grid, v, scale, mouse, true) : -1;

    if (hit < 0 && segment < 0) {
        return false;
    }

    e->edit_moved = false;
    ed_focus_edit(e, FOCUS_EVENTS);

    if (hit < 0) {
        if (!ed_begin_edit_move(e)) {
            return true;
        }
        hit = insert_bend_node(e, segment, grid, v, scale, mouse);
    }

    if (hit < 0) {
        return true;
    }

    e->automation_node = hit;
    e->automation_cc = DM_EVENT_BEND;
    e->node_beat = e->project.automation[hit].beat;
    e->marquee_start = mouse.pos;
    e->drag = DRAG_BEND_POINT;
    return true;
}

void ed_piano_mouse(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, bool hovered, int hit, UiRect hit_box) {
    /* The resize cursor shows only where a click would resize: nothing else holds the mouse, and
       the tool and modifiers leave the edge to resizing. */
    bool idle = hovered && !e->drag && !e->marquee && !e->scrub && !e->pan_context &&
                e->pattern_drag < 0 && !mouse.left && !mouse.right && !mouse.middle;

    if (idle && hit >= 0 && !ed_gesture_held(e, GESTURE_MARQUEE, mouse) && e->tool != TOOL_SLICE &&
        e->tool != TOOL_DELETE && e->tool != TOOL_BRUSH &&
        note_edge_side(hit_box, scale, mouse.pos.x)) {
        ui_resize_cursor(false);
    }

    if (hovered && mouse.pos.x >= grid.x && mouse.pos.y >= grid.y) {
        int key = 127 - (int)((mouse.pos.y - r.y - 28 + v->scroll_y) / row);
        char text[128];
        snprintf(text, sizeof(text), DM_TEXT_PITCH_BEAT_ALT, key,
                 (mouse.pos.x - grid.x + v->scroll_x) / scale);
        ed_hover(e, text);
    }

    if (!e->event_ruler_drag) {
        ed_timeline_drag(e, r, v, scale, grid.x - r.x, 2, hovered);
    }

    bool handle = false, bend = false;

    /* Pitch bend points are shown and edited on the notes only in Point mode. */
    if (!dm_graph_resolution(p, DM_EVENT_BEND)) {
        ui_clip(grid);
        handle = ed_curve_handles(e, grid, v, scale, mouse, true, hovered);
        ui_unclip();
        /* A bend point under the cursor takes Shift + click before the note under it. */
        bool on_point = hit >= 0 && mouse.clicked && hovered &&
                        ed_gesture_held(e, GESTURE_BEND_EDIT, mouse) &&
                        bend_hit(e, grid, v, scale, mouse, false) >= 0;
        bool note_gesture = hit >= 0 && mouse.clicked && !on_point &&
                            (ed_gesture_held(e, GESTURE_FREE, mouse) ||
                             ed_gesture_held(e, GESTURE_NOTE_BEND, mouse)) &&
                            !ed_gesture_held(e, GESTURE_MARQUEE, mouse);
        bend = handle || (!note_gesture && (hovered || e->drag == DRAG_BEND_POINT) &&
                          ed_bend_mouse(e, grid, v, scale, mouse));
    }

    if (hovered && !bend) {
        ed_piano_click(e, p, r, grid, v, scale, row, mouse, hit, hit_box);
    }

    /* A right click clears the note selection; holding it erases notes. */
    if (hovered && mouse.right_clicked && !e->drag && ed_right_free(e, mouse) &&
        ed_inside(mouse.pos, grid)) {
        ed_deselect_notes(p);
    }

    if (hovered && mouse.right && ed_right_free(e, mouse) && ed_inside(mouse.pos, grid)) {
        erase_swept_notes(e, p, r, grid, v, scale, row, mouse);
    }
}

/* The pitch a pattern's view centers on: the mean key of the middle 80% of its notes, so a few
   outlying notes do not pull the view away from the rest. Counted per key, so it stays cheap. */
static float note_center(const DmPattern *p) {
    size_t counts[128] = {0};

    for (size_t i = 0; i < p->count; ++i) {
        ++counts[p->notes[i].key];
    }

    size_t skip = p->count / 10, keep = p->count - 2 * skip, taken = 0;
    double sum = 0;

    for (int key = 0; key < 128 && taken < keep; ++key) {
        size_t n = counts[key], dropped = n < skip ? n : skip;
        skip -= dropped;
        n -= dropped;
        n = n < keep - taken ? n : keep - taken;
        sum += (double)n * key;
        taken += n;
    }

    return (float)(sum / (double)taken);
}

void ed_focus_first_note(Editor *e, DmPattern *p, UiRect grid) {
    if (!e->focus_pattern) {
        return;
    }

    e->focus_pattern = false;

    if (!p->count) {
        e->roll_view.scroll_x = 0;
        return;
    }

    DmNote first = p->notes[0];
    int low = first.key, high = first.key;

    for (size_t i = 1; i < p->count; ++i) {
        if (p->notes[i].key < low) {
            low = p->notes[i].key;
        }

        if (p->notes[i].key > high) {
            high = p->notes[i].key;
        }

        if (p->notes[i].beat < first.beat) {
            first = p->notes[i];
        }
    }

    float scale = 80 * e->roll_view.zoom;
    e->roll_view.scroll_x = fmaxf(0, (float)first.beat * scale - grid.w * .5f);
    e->roll_view.scroll_y =
        ed_clampf((127 - note_center(p)) * 20 + 10 - grid.h * .5f, 0, fmaxf(0, 2560 - grid.h));
    e->automation_center = (low + high) * .5f;
}

static void note_placement_tip(Editor *e, DmPattern *p, UiRect grid, View *v, float row,
                               UiMouse mouse, bool hovered) {
    if (hovered && mouse.left && ed_inside(mouse.pos, grid) &&
        !ed_gesture_held(e, GESTURE_MARQUEE, mouse) && !ed_gesture_held(e, GESTURE_FREE, mouse) &&
        e->tool != TOOL_MARQUEE) {
        int key = 127 - (int)floorf((mouse.pos.y - grid.y + v->scroll_y) / row);

        if ((e->drag == DRAG_NOTE || e->drag == DRAG_NOTE_BEND) && e->drag_index >= 0 &&
            (size_t)e->drag_index < p->count) {
            key = p->notes[e->drag_index].key;
        }

        char value[32];
        ed_pitch_text(value, sizeof(value), (float)key);
        ui_placement_tip(value);
    }
}

void ed_roll(Editor *e, float width, float height) {
    ui_child(DM_TEXT_PIANO_ROLL_TITLE, width, height, false);

    if (ui_panel_clicked()) {
        ed_focus_edit(e, FOCUS_NOTES);
    }

    DmPattern *p = ed_pattern(e);

    if (!p) {
        ui_dim(DM_TEXT_SELECT_CREATE_PATTERN);
        ui_child_end();
        return;
    }

    e->last_note_pattern_uid = p->uid;
    piano_header(e, p);
    UiPoint at = ui_cursor(), space = ui_available();
    float automation_height =
        e->automation_open ? ed_clampf(e->automation_height, 100, fmaxf(100, height - 140)) : 0;
    UiRect r = {at.x, at.y, space.x - 14, fmaxf(50, space.y - automation_height - 24)};
    ui_canvas(DM_TEXT_NOTES_CANVAS, r.w, r.h);
    UiMouse mouse = ui_mouse();
    bool hovered = ui_hover();

    if (hovered) {
        ed_hover(e, DM_TEXT_CLICK_ADD_DRAG);

        if (mouse.clicked || mouse.right_clicked) {
            ed_focus_timeline(e, CONTEXT_ROLL);
        }
    }

    View *v = &e->roll_view;
    float label = dm_channel_drums(&e->project, e->pattern_channel) ? 160 : 64, row = 20,
          scale = 80 * v->zoom, total = fmaxf((float)p->length * scale, r.w - label);
    /* When the editor grows or shrinks, the pitch at the middle of the view stays there, unless
       something placed the view since the last frame (opening a pattern, navigation). */
    if (!e->focus_pattern && e->roll_grid_height > 0 && v->scroll_y == e->roll_scroll_seen) {
        v->scroll_y += (e->roll_grid_height - (r.h - 28)) * .5f;
    }

    e->roll_grid_height = r.h - 28;
    ed_focus_first_note(e, p, (UiRect){r.x + label, r.y + 28, r.w - label, r.h - 28});
    bool velocity_wheel = ed_note_velocity_wheel(e, p, r, v, label, row, scale, mouse, hovered);
    ed_grid_scroll(e, v, (UiRect){r.x + label, r.y, r.w - label, r.h}, total, 128 * row, row,
                   velocity_wheel);
    scale = 80 * v->zoom;
    total = fmaxf((float)p->length * scale, r.w - label);
    v->scroll_x = fmaxf(0, v->scroll_x);
    ui_clip(r);
    ui_rect(r, BG, 0);
    piano_keys(e, r, v, label, row, mouse);

    if (mouse.released && e->held[SDL_SCANCODE_UNKNOWN] >= 0) {
        dm_audio_preview(e->audio, e->held_channels[SDL_SCANCODE_UNKNOWN],
                         e->held[SDL_SCANCODE_UNKNOWN], 0, false);
        e->held[SDL_SCANCODE_UNKNOWN] = -1;
    }

    UiRect grid = {r.x + label, r.y + 28, r.w - label, r.h - 28};
    ui_clip(grid);
    int first = (int)floor(v->scroll_x / (scale * .25));

    for (int i = first; i < first + (int)(grid.w / (scale * .25)) + 3; ++i) {
        float x = grid.x + i * .25f * scale - v->scroll_x;
        ui_line((UiPoint){x, grid.y}, (UiPoint){x, r.y + r.h}, ed_grid_color(e, i * .25), 1);
    }

    UiRect hit_box = {0};
    if (!dm_graph_resolution(p, DM_EVENT_BEND)) {
        pitch_underlay(e, grid, v, scale);
    }

    int hit = piano_notes(e, p, r, grid, v, scale, row, mouse, &hit_box);
    ui_unclip();
    ed_ruler(e, r, v, scale, label);
    ui_unclip();
    ed_piano_mouse(e, p, r, grid, v, scale, row, mouse, hovered, hit, hit_box);
    note_context(e, p, hit);
    ed_horizontal_scroll(v, (UiRect){grid.x, r.y, grid.w, r.h}, total);
    ui_scrollbar(DM_TEXT_PIANO_VERTICAL, (UiRect){r.x + r.w + 2, r.y + 28, 12, r.h - 28}, true,
                 &v->scroll_y, r.h - 28, 128 * row);
    ed_brush_drag(e, p, grid, v, scale, row, mouse);
    note_drag(e, p, scale, row, mouse);

    if (e->drag == DRAG_NOTE && !e->drag_edge) {
        ui_gesture_cursor(false);
    }

    note_placement_tip(e, p, grid, v, row, mouse, hovered);
    note_bend(e, grid, v, scale, row, mouse);
    ed_marquee_outline(e, FOCUS_NOTES, mouse);

    ed_automation_panel(e, r, v, scale, label, mouse);
    e->roll_scroll_seen = v->scroll_y;
    ui_child_end();
}
