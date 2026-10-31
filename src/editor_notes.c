#include "editor_internal.h"
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
static int insert_bend_node(Editor *e, int segment, UiRect grid, View *v, float scale,
                            UiMouse mouse);
static void move_bend_node(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse);
static void note_placement_tip(Editor *e, DmPattern *p, UiRect grid, View *v, float row,
                               UiMouse mouse, bool hovered);

bool ed_sounding(Editor *e, int key) {
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
    return mouse.left && e->drag == 0 && (mouse.clicked || e->held[SDL_SCANCODE_UNKNOWN] >= 0);
}

static void piano_keys(Editor *e, UiRect r, View *v, float label, float row, UiMouse mouse) {
    for (int i = 0; i < 128; ++i) {
        float y = r.y + 28 + i * row - v->scroll_y;

        if (y + row < r.y + 28 || y > r.y + r.h) {
            continue;
        }

        int key = 127 - i, step = key % 12;
        bool black = ed_black_key(step);
        ui_rect((UiRect){r.x, y, r.w, row}, black ? COLOR(14, 14, 14) : COLOR(22, 22, 22), 0);
        ui_rect((UiRect){r.x, y, label - 2, row - 1},
                ed_sounding(e, key) ? GREEN
                : black             ? COLOR(31, 31, 31)
                                    : COLOR(62, 62, 62),
                0);
        ui_line((UiPoint){r.x + label, y + row}, (UiPoint){r.x + r.w, y + row}, GRID, 1);
        static const char *note_names[] = {DM_TEXT_C,       DM_TEXT_C_SHARP, DM_TEXT_D,
                                           DM_TEXT_D_SHARP, DM_TEXT_E,       DM_TEXT_F,
                                           DM_TEXT_F_SHARP, DM_TEXT_G,       DM_TEXT_G_SHARP,
                                           DM_TEXT_A,       DM_TEXT_A_SHARP, DM_TEXT_B};

        if (step == 0 || row >= 18) {
            char text[32];
            snprintf(text, sizeof(text), "%s%d", note_names[step], key / 12 - 1);
            ui_label_fit((UiPoint){r.x + 9, y + 3}, label - 18, ed_sounding(e, key) ? BG : TEXT,
                         e->pattern_channel == MIDI_DRUM_CHANNEL ? dm_drum_name(key) : text);
        }

        if (piano_mouse_active(e, mouse) && ed_inside(mouse.pos, (UiRect){r.x, y, label, row}) &&
            ed_inside(mouse.pos, r)) {
            piano_glissando(e, key);
        }
    }
}

static void note_context(Editor *e, DmPattern *p, int hit) {
    UiMouse mouse = ui_mouse();

    if (mouse.right_clicked && mouse.shift) {
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
                ed_checkpoint(e);
                p->notes[hit].velocity = (uint8_t)velocity;
                ed_commit(e);
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
            e->automation_cc = -2;
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

void ed_begin_edit_move(Editor *e) {
    if (!e->edit_moved) {
        ed_checkpoint(e);
    }

    e->edit_moved = true;
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

static void note_drag(Editor *e, DmPattern *p, float scale, float row, UiMouse mouse) {
    if (e->drag == 2 && e->drag_index >= 0 && (size_t)e->drag_index < p->count) {
        if (e->drag_edge) {
            ui_resize_cursor(false);
        }

        double delta = (mouse.pos.x - e->marquee_start.x) / scale;

        if (mouse.left && ed_gesture_moved(e, mouse)) {
            ed_begin_edit_move(e);

            if (e->drag_edge) {
                ed_resize_note_group(e, p, delta, mouse.alt);
            } else {
                double offset = ed_snap(e->drag_beat + delta, .25, mouse.alt) - e->drag_beat;
                int pitch_delta = -(int)roundf((mouse.pos.y - e->marquee_start.y) / row);

                for (size_t i = 0; i < e->drag_note_count; ++i) {
                    DmNote original = e->drag_notes[i];

                    if (original.selected) {
                        offset = fmax(offset, -original.beat);
                        pitch_delta =
                            (int)fmax(-original.key, fmin(127 - original.key, pitch_delta));
                    }
                }

                for (size_t i = 0; i < e->drag_note_count; ++i) {
                    DmNote original = e->drag_notes[i];

                    if (original.selected) {
                        p->notes[i].beat = original.beat + offset;
                        p->notes[i].key = (uint8_t)(original.key + pitch_delta);
                    }
                }
            }

            if (!e->drag_edge) {
                ed_edit_feedback(e, p->notes[e->drag_index].key);
            }

            fit_dragged_notes(e, p);
            ed_changed(e);
        }

        if (mouse.released) {
            e->drag = 0;

            if (e->edit_moved) {
                if (!e->drag_edge) {
                    ed_follow_note_curves(e, e->selected_pattern, e->drag_notes,
                                          e->drag_note_count);
                }

                ed_publish(e);
            }
        }
    }
}

static void note_bend(Editor *e, UiRect grid, View *v, float scale, float row, UiMouse mouse) {
    if (e->drag == 3) {
        ui_clip(grid);
        ui_line(e->marquee_start, mouse.pos, 0x8897dbff, 3);
        ui_unclip();

        if (mouse.released) {
            double start = fmax(0, (e->marquee_start.x - grid.x + v->scroll_x) / scale),
                   end = fmax(0, (mouse.pos.x - grid.x + v->scroll_x) / scale);
            int semitones = (int)roundf((e->marquee_start.y - mouse.pos.y) / row);
            semitones = (int)ed_clampf(e->drag_key + semitones, 0, 127) - e->drag_key;
            start = ed_snap(start, .25, false);
            end = ed_snap(end, .25, false);
            e->bend_range = (int)fmax(12, abs(semitones));
            int value = (int)ed_clampf(
                MIDI_BEND_CENTER + semitones * MIDI_BEND_CENTER / e->bend_range, 0, MIDI_BEND_MAX);
            ed_automation_line(e, start, end, MIDI_BEND_CENTER, value, -2);
            e->drag = 0;
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
    e->drag = 2;
    e->drag_index = hit;
    e->drag_beat = n->beat;
    e->drag_length = n->length;
    e->drag_key = n->key;
    e->drag_edge = edge;
    free(e->drag_notes);
    e->drag_notes = malloc(p->count * sizeof(*p->notes));
    e->drag_note_count = e->drag_notes ? p->count : 0;

    if (!e->drag_notes) {
        e->drag = 0;
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_NOTE);
        return false;
    }

    memcpy(e->drag_notes, p->notes, p->count * sizeof(*p->notes));
    e->marquee_start = mouse.pos;
    return true;
}

static void piano_hit_click(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                            UiMouse mouse, int hit, UiRect hit_box, double beat) {
    DmNote *n = &p->notes[hit];

    if (e->tool == 3) {
        ed_checkpoint(e);
        memmove(n, n + 1, (p->count - hit - 1) * sizeof(*n));
        --p->count;
        ed_commit(e);
    } else if (e->tool == 2) {
        ed_checkpoint(e);
        ed_split_note(p, (size_t)hit, beat);
        ed_commit(e);
    } else {
        if (!mouse.shift && !n->selected) {
            ed_deselect(e);
        }

        n->selected = 1;
        int edge = mouse.pos.x < hit_box.x + fminf(9, hit_box.w * .3f)               ? -1
                   : mouse.pos.x > hit_box.x + hit_box.w - fminf(9, hit_box.w * .3f) ? 1
                                                                                     : 0;

        if (!start_note_drag(e, p, hit, mouse, edge)) {
            return;
        }

        if (!e->drag_edge && !mouse.shift) {
            ed_edit_feedback(e, n->key);
        }

        if (mouse.shift && !e->drag_edge) {
            e->marquee_start.x =
                grid.x +
                (float)ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, .25, false) * scale -
                v->scroll_x;
            e->marquee_start.y = grid.y + (127 - n->key) * row - v->scroll_y + row * .5f;
            e->drag = 3;
            e->automation_open = true;
            e->automation_cc = -2;
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

    if (ed_add_note(e, p, (DmNote){beat, .25, key, 100, 1, (uint8_t)ed_note_port(e)})) {
        ed_changed(e);
    }
}

void ed_brush_drag(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                   UiMouse mouse) {
    if (e->drag != 10) {
        return;
    }

    if (mouse.left) {
        int quarter = (int)floor(fmax(0, (mouse.pos.x - grid.x + v->scroll_x) / scale) * 4);
        int key =
            mouse.ctrl
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
        e->drag = 0;
        ed_publish(e);
    }
}

void ed_brush_begin(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse) {
    ed_checkpoint(e);
    ed_deselect(e);
    e->drag = 10;
    e->brush_quarter = (int)floor(fmax(0, (mouse.pos.x - grid.x + v->scroll_x) / scale) * 4);
    e->brush_pitch = e->brush_anchor =
        (int)ed_clampf(127 - floorf((mouse.pos.y - grid.y + v->scroll_y) / row), 0, 127);
    ed_brush_drag(e, p, grid, v, scale, row, mouse);
}

void ed_piano_click(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, int hit, UiRect hit_box) {
    if (mouse.clicked && ed_inside(mouse.pos, grid)) {
        if (e->tool == 4) {
            ed_brush_begin(e, p, grid, v, scale, row, mouse);
            return;
        }

        if (mouse.ctrl) {
            ed_deselect(e);
            e->marquee = true;
            e->marquee_start = mouse.pos;
            return;
        }

        e->automation_node = -1;
        double beat = ed_snap((mouse.pos.x - grid.x + v->scroll_x) / scale, .25, mouse.alt);
        int key =
            (int)ed_clampf(127 - floorf((mouse.pos.y - r.y - 28 + v->scroll_y) / row), 0, 127);

        if (hit >= 0) {
            piano_hit_click(e, p, grid, v, scale, row, mouse, hit, hit_box, beat);
        } else if (e->tool == 1) {
            ed_deselect(e);
            e->marquee = true;
            e->marquee_start = mouse.pos;
        } else {
            ed_checkpoint(e);
            ed_deselect(e);
            remove_overlapping_notes(e, p, key, beat);

            if (ed_add_note(e, p,
                            (DmNote){beat, .25, (uint8_t)key, 100, 1, (uint8_t)ed_note_port(e)})) {
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

static void preview_instrument(Editor *e, float right) {
    if (!e->preview_override && e->preview_source_channel >= 0) {
        e->preview_program = ed_channel_program(e, e->preview_source_channel);
    }

    float ports = ed_port_buttons_width(e);
    float width = fminf(220, fmaxf(100, ui_available().x - 190 - ports));
    UiPoint at = ui_cursor();

    if (ui_available().x < width + 188 + ports) {
        ui_newline();
        at = ui_cursor();
    }

    if (ui_available().x < width + 188 + ports && ports) {
        ed_port_buttons(e);
        ui_newline();
        at = ui_cursor();
        ports = 0;
    }

    ui_position(fmaxf(at.x, right - width - 188 - ports), at.y);

    if (ports) {
        ed_port_buttons(e);
    }

    ui_width(width);
    const char *name =
        e->preview_program == 128 ? DM_TEXT_PERCUSSION : dm_program_name(e->preview_program);

    if (ui_bar_combo("##Preview instrument", name)) {
        for (int i = 0; i <= 128; ++i) {
            const char *label = i == 128 ? DM_TEXT_PERCUSSION : dm_program_name(i);

            if (ui_select(label, e->preview_program == i, 0, 0)) {
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
        if ((e->preview_program == 128 || e->pattern_channel == MIDI_DRUM_CHANNEL ||
             (e->channel_selected && e->selected_channel == MIDI_DRUM_CHANNEL)) &&
            ui_menu_item(DM_TEXT_PERCUSSION_KEYBOARD_MAPPING, NULL, true)) {
            ed_percussion_open(e);
        }

        if (ui_menu_item(DM_TEXT_QUANTIZE, NULL, true)) {
            ed_checkpoint(e);
            dm_quantize(p, .25);
            dm_pattern_fit(&e->project, p);
            ed_commit(e);
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

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) ||
            a.type != MIDI_PITCH_BEND || a.curve < 0) {
            continue;
        }

        float pitch = ed_automation_pitch(e, a);
        UiPoint point = {grid.x + (float)a.beat * scale - v->scroll_x,
                         grid.y + (127 - pitch) * 20 - v->scroll_y + 10};

        if (has) {
            ed_automation_connection(previous, point, before, a,
                                     ed_curve_units(e, before, grid, true),
                                     (e->accent & 0xffffffu) | 0x88000000u);
        }

        ui_circle(point, e->automation_node == (int)i ? 6 : 3,
                  e->automation_node == (int)i ? TEXT : (e->accent & 0xffffffu) | 0x99000000u);
        previous = point;
        has = true;
        before = a;
    }
}

static int piano_notes(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale,
                       float row, UiMouse mouse, UiRect *hit_result) {
    int hit = -1;
    UiRect hit_box = {0};

    for (size_t i = 0; i < p->count; ++i) {
        DmNote n = p->notes[i];
        UiRect box = {grid.x + (float)n.beat * scale - v->scroll_x,
                      r.y + 28 + (127 - n.key) * row - v->scroll_y + 1,
                      fmaxf(3, (float)n.length * scale), row - 2};

        if (!ed_intersects(box, grid)) {
            continue;
        }

        uint32_t color = ed_port_color(e, n.port);
        ui_rect(box, color, 3);
        /* Notes use the application or port color, so selection is outlined in white. */
        ui_border(box, n.selected ? TEXT : color, 3, n.selected ? 2 : 1);
        ui_line((UiPoint){box.x + 3, box.y + box.h - 3},
                (UiPoint){box.x + box.w * fminf(1, n.velocity / 127.0f), box.y + box.h - 3},
                0x8c000000u, 2);
        UiRect target = {box.x - 4, box.y, fmaxf(12, box.w + 8), box.h};

        if (ed_inside(mouse.pos, target) && ed_inside(mouse.pos, grid)) {
            if (mouse.pos.x < box.x + 9 || mouse.pos.x > box.x + box.w - 9) {
                ui_resize_cursor(false);
            }

            hit = (int)i;
            hit_box = box;
        }

        if (e->marquee && e->context == 2) {
            UiRect selection = ed_marquee_box(e, mouse);
            p->notes[i].selected = ed_intersects(box, selection);
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
        ed_begin_edit_move(e);
        float pitch = roundf(127 - (mouse.pos.y - grid.y + v->scroll_y - 10) / 20);
        DmAutomation edited = ed_expand_pitch_range(e, *a, pitch);
        e->automation_node = -1;
        ed_select_edited_node(e, edited);
        a = &e->project.automation[e->automation_node];
        a->beat = ed_snap(e->node_beat + (mouse.pos.x - e->marquee_start.x) / scale, .25, false);
        ed_set_event_value(
            a, (int)ed_clampf(MIDI_BEND_CENTER +
                                  (pitch - (a->curve ? a->anchor : ed_nearest_note(e, 0))) *
                                      MIDI_BEND_CENTER / ed_automation_range(e, *a),
                              0, MIDI_BEND_MAX));
        ed_edit_feedback(e, ed_automation_pitch(e, *a));
        ed_changed(e);
    }

    if (mouse.released && !e->edit_moved) {
        e->drag = 0;
        return;
    }

    if (mouse.released) {
        e->automation_cc = -2;
        ed_curve_restore_position(e, a->curve);
        ed_sort_automation(e);
        e->automation_node = -1;
        e->drag = 0;
        ed_commit(e);
    }
}

bool ed_bend_mouse(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse) {
    if (e->drag == 7) {
        move_bend_node(e, grid, v, scale, mouse);
        return true;
    }

    if (mouse.ctrl) {
        return false;
    }

    if (!ed_inside(mouse.pos, grid) || !mouse.clicked || (!mouse.alt && !mouse.shift)) {
        return false;
    }

    int hit = bend_hit(e, grid, v, scale, mouse, false);

    if (hit >= 0 && mouse.shift) {
        ed_focus_edit(e, FOCUS_EVENTS);
        e->automation_all = false;
        e->automation_node = hit;
        e->automation_cc = -2;
        return true;
    }

    if (!mouse.alt) {
        return false;
    }

    int segment = hit < 0 ? bend_hit(e, grid, v, scale, mouse, true) : -1;

    if (hit < 0 && segment < 0) {
        return false;
    }

    e->edit_moved = false;
    ed_focus_edit(e, FOCUS_EVENTS);

    if (hit < 0) {
        ed_begin_edit_move(e);
        hit = insert_bend_node(e, segment, grid, v, scale, mouse);
    }

    if (hit < 0) {
        return true;
    }

    e->automation_node = hit;
    e->automation_cc = -2;
    e->node_beat = e->project.automation[hit].beat;
    e->marquee_start = mouse.pos;
    e->drag = 7;
    return true;
}

void ed_piano_mouse(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, bool hovered, int hit, UiRect hit_box) {
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

    ui_clip(grid);
    bool handle = ed_curve_handles(e, grid, v, scale, mouse, true, hovered);
    ui_unclip();
    bool note_gesture = hit >= 0 && mouse.clicked && (mouse.alt || mouse.shift) && !mouse.ctrl;
    bool bend = handle || (!note_gesture && (hovered || e->drag == 7) &&
                           ed_bend_mouse(e, grid, v, scale, mouse));

    if (hovered && !bend) {
        ed_piano_click(e, p, r, grid, v, scale, row, mouse, hit, hit_box);
    }

    if (hovered && mouse.right && !mouse.shift && hit >= 0 && ed_inside(mouse.pos, grid)) {
        if (!e->erase_active) {
            ed_checkpoint(e);
        }

        e->erase_active = true;
        memmove(&p->notes[hit], &p->notes[hit + 1], (p->count - hit - 1) * sizeof(DmNote));
        --p->count;
        ed_commit(e);
    }
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
        ed_clampf((127 - (low + high) * .5f) * 20 + 10 - grid.h * .5f, 0, fmaxf(0, 2560 - grid.h));
    e->automation_center = (low + high) * .5f;
}

static void note_placement_tip(Editor *e, DmPattern *p, UiRect grid, View *v, float row,
                               UiMouse mouse, bool hovered) {
    if (hovered && mouse.left && ed_inside(mouse.pos, grid) && !mouse.ctrl && !mouse.alt &&
        e->tool != 1) {
        int key = 127 - (int)floorf((mouse.pos.y - grid.y + v->scroll_y) / row);

        if ((e->drag == 2 || e->drag == 3) && e->drag_index >= 0 &&
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
            e->context = 2;
        }
    }

    View *v = &e->roll_view;
    float label = e->pattern_channel == MIDI_DRUM_CHANNEL ? 160 : 64, row = 20,
          scale = 80 * v->zoom, total = fmaxf((float)p->length * scale, r.w - label);
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
    pitch_underlay(e, grid, v, scale);
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

    if (e->drag == 2 && !e->drag_edge) {
        ui_gesture_cursor(false);
    }

    note_placement_tip(e, p, grid, v, row, mouse, hovered);
    note_bend(e, grid, v, scale, row, mouse);
    ed_marquee_outline(e, 2, mouse);

    ed_automation_panel(e, r, v, scale, label, mouse);
    ui_child_end();
}
