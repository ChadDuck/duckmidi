#include "editor_internal.h"

static void grid_pan(Editor *e, View *v, UiRect canvas, UiMouse mouse, int context);
static void ruler_ticks(Editor *e, UiRect r, View *v, float scale, float label);
static void ruler_bars(Editor *e, UiRect r, View *v, float scale, float label);
static void timeline_autoscroll(Editor *e, UiRect r, View *v, float scale, float label,
                                UiMouse mouse, double length);
static bool pattern_copy(Editor *e, DmClip clip);
static double fallback_pattern_origin(Editor *e, int previous, int first);
static void timeline_begin(Editor *e, UiRect r, float label, int context, bool hovered,
                           UiMouse mouse);
static void timeline_region(Editor *e, UiRect r, View *v, float scale, float label, int context,
                            bool hovered, UiMouse mouse);

static void grid_pan(Editor *e, View *v, UiRect canvas, UiMouse mouse, int context) {
    if (mouse.middle_clicked && ui_hover() && ed_inside(mouse.pos, canvas)) {
        e->pan_context = context;
    }

    if (e->pan_context != context) {
        return;
    }

    if (!mouse.middle) {
        e->pan_context = 0;
        return;
    }

    ui_pan_cursor();
    v->scroll_x -= mouse.delta.x;
    v->scroll_y -= mouse.delta.y;
}

void ed_grid_scroll(Editor *e, View *v, UiRect canvas, float content_width, float content_height,
                    float row, bool consumed) {
    UiMouse m = ui_mouse();

    if (consumed) {
        m.wheel = 0;
    }

    if (ui_hover() && ed_inside(m.pos, canvas) && m.wheel) {
        if (m.ctrl || m.pos.y < canvas.y + 28) {
            float old = v->zoom;
            v->zoom = ed_clampf(v->zoom * powf(1.15f, m.wheel), .15f, 8);
            v->scroll_x = fmaxf(0, (v->scroll_x + m.pos.x - canvas.x) * (v->zoom / old) -
                                       (m.pos.x - canvas.x));
        } else if (m.shift) {
            v->scroll_y -= m.wheel * row * 3;
        } else {
            v->scroll_x -= m.wheel * 70;
        }
    }

    grid_pan(e, v, canvas, m, v == &e->roll_view ? 2 : 1);
    v->scroll_x = fmaxf(0, v->scroll_x);
    (void)content_width;
    v->scroll_y = ed_clampf(v->scroll_y, 0, fmaxf(0, content_height - canvas.h + 28));
}

void ed_horizontal_scroll(View *v, UiRect canvas, float total) {
    total = fmaxf(total, v->scroll_x + canvas.w * 3);
    ui_scrollbar("horizontal", (UiRect){canvas.x, canvas.y + canvas.h + 4, canvas.w, 12}, false,
                 &v->scroll_x, canvas.w, total);
}

static void ruler_ticks(Editor *e, UiRect r, View *v, float scale, float label) {
    double step = v == &e->roll_view ? .25 : 1;

    while (step * scale < 8) {
        step *= 2;
    }

    double first = floor(v->scroll_x / (scale * step)) * step;

    for (int i = 0; i < (int)(r.w / (scale * step)) + 3; ++i) {
        double beat = first + i * step;
        float x = r.x + label + (float)(beat * scale) - v->scroll_x;

        if (x < r.x + label || x > r.x + r.w) {
            continue;
        }

        DmMeter meter = dm_meter(&e->project, beat);
        bool major = fabs(remainder(beat - meter.origin, meter.bar)) < .001;
        float top = major ? 18 : fabs(remainder(beat, 1)) < .001 ? 20 : 24;
        ui_line((UiPoint){x, r.y + top}, (UiPoint){x, r.y + 28}, major ? DIM : GRID, 1);
    }
}

static void ruler_bars(Editor *e, UiRect r, View *v, float scale, float label) {
    double left = v->scroll_x / scale, right = (v->scroll_x + r.w - label) / scale;
    DmMeter meter = dm_meter(&e->project, left);
    double beat = meter.origin + ceil((left - meter.origin) / meter.bar - 1e-9) * meter.bar;
    beat = fmin(beat, meter.next_change);

    for (int guard = 0; beat <= right && guard < 1000; ++guard) {
        meter = dm_meter(&e->project, beat + 1e-8);
        double number = meter.completed + floor((beat - meter.origin) / meter.bar + 1e-8) + 1;
        float x = r.x + label + (float)beat * scale - v->scroll_x;
        char text[32];
        snprintf(text, sizeof(text), "%.0f", number);
        ui_label((UiPoint){x + 4, r.y + 5}, DIM, text);
        ui_line((UiPoint){x, r.y + 18}, (UiPoint){x, r.y + 28}, DIM, 1);
        double stride = fmax(1, ceil(40 / (meter.bar * scale)));
        beat = fmin(beat + meter.bar * stride, meter.next_change);
    }
}

double ed_view_playhead(Editor *e, int context, bool playing, double beat, bool *visible) {
    double playhead = e->cursor_beat[context];
    *visible = true;

    if (!e->scrub && playing) {
        if (e->pattern_mode == context) {
            playhead = beat;
        } else if (context && !e->pattern_mode) {
            *visible = false;
            double latest = -1;

            for (size_t i = 0; i < e->project.clip_count; ++i) {
                DmClip clip = e->project.clips[i];

                if (clip.pattern == e->selected_pattern && clip.beat > latest &&
                    beat >= clip.beat && beat < clip.beat + dm_clip_length(&e->project, clip)) {
                    playhead = beat - clip.beat + clip.offset;
                    *visible = true;
                    latest = clip.beat;
                }
            }
        }
    }

    return playhead;
}

void ed_ruler(Editor *e, UiRect r, View *v, float scale, float label) {
    ui_rect((UiRect){r.x, r.y, r.w, 28}, COLOR(26, 26, 26), 0);
    int region = v == &e->roll_view;

    if (e->loop_enabled[region]) {
        float left = r.x + label + (float)e->loop_start[region] * scale - v->scroll_x;
        float right = r.x + label + (float)e->loop_end[region] * scale - v->scroll_x;
        ui_clip((UiRect){r.x + label, r.y, r.w - label, r.h});
        ui_rect((UiRect){left, r.y, right - left, 28}, (e->accent & 0xffffffu) | 0x55000000u, 0);
        ui_line((UiPoint){left, r.y + 28}, (UiPoint){right, r.y + 28}, e->accent, 2);
        ui_unclip();
    }

    ruler_ticks(e, r, v, scale, label);
    ruler_bars(e, r, v, scale, label);
    int context = v == &e->roll_view;
    bool visible;
    double playhead =
        ed_view_playhead(e, context, dm_audio_playing(e->audio),
                         dm_seconds_beat(&e->project, dm_audio_position(e->audio)), &visible);
    float x = r.x + label + (float)playhead * scale - v->scroll_x;

    if (visible && x >= r.x + label && x < r.x + r.w) {
        ui_line((UiPoint){x, r.y + 14}, (UiPoint){x, r.y + r.h}, GREEN, 2);
        ui_triangle((UiPoint){x - 6, r.y + 3}, (UiPoint){x + 6, r.y + 3}, (UiPoint){x, r.y + 14},
                    GREEN);
    }
}

static void timeline_autoscroll(Editor *e, UiRect r, View *v, float scale, float label,
                                UiMouse mouse, double length) {
    (void)e;
    float right = r.x + r.w;
    float outside = mouse.pos.x < r.x + label ? mouse.pos.x - r.x - label
                    : mouse.pos.x > right     ? mouse.pos.x - right
                                              : 0;
    float elapsed = ui_delta_time();
    v->scroll_x = fmaxf(0, v->scroll_x + outside * 8 * elapsed);
    (void)length;
    (void)scale;
}

static bool pattern_copy(Editor *e, DmClip clip) {
    return clip.pattern == e->selected_pattern && clip.channel == e->pattern_channel;
}

/* Song beat where the pattern's beat 0 falls for a placement; sliced placements start later in
   the pattern. */
static double clip_origin(DmClip clip) {
    return clip.beat - clip.offset;
}

static double fallback_pattern_origin(Editor *e, int previous, int first) {
    if (e->selected_clip >= 0 && (size_t)e->selected_clip < e->project.clip_count &&
        pattern_copy(e, e->project.clips[e->selected_clip])) {
        return clip_origin(e->project.clips[e->selected_clip]);
    }

    int index = previous >= 0 ? previous : first;
    return index >= 0 ? clip_origin(e->project.clips[index]) : 0;
}

double ed_pattern_seek_origin(Editor *e) {
    if (!ed_pattern(e)) {
        return 0;
    }

    double beat = e->cursor_beat[0], length = ed_pattern(e)->length;
    int current = -1, following = -1, first = -1, previous = -1;
    const DmClip *clips = e->project.clips;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        DmClip clip = clips[i];

        if (!pattern_copy(e, clip)) {
            continue;
        }

        if (first < 0 || clip.beat < clips[first].beat) {
            first = (int)i;
        }

        if (clip.beat <= beat && (previous < 0 || clip.beat > clips[previous].beat)) {
            previous = (int)i;
        }

        if (beat >= clip.beat && beat < clip.beat + dm_clip_length(&e->project, clip) &&
            (current < 0 || clip.beat >= clips[current].beat)) {
            current = (int)i;
            length = dm_clip_length(&e->project, clip);
        }
    }

    if (current < 0) {
        return fallback_pattern_origin(e, previous, first);
    }

    if (beat - clips[current].beat <= length * .5) {
        return clip_origin(clips[current]);
    }

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        DmClip clip = clips[i];

        if (pattern_copy(e, clip) && clip.beat > clips[current].beat &&
            (following < 0 || clip.beat < clips[following].beat)) {
            following = (int)i;
        }
    }

    return clip_origin(clips[following >= 0 ? following : current]);
}

void ed_scrub_publish(Editor *e, double beat, int context, bool playing) {
    double absolute = context == 2 && !e->pattern_mode ? e->scrub_origin + beat : beat;
    e->cursor_beat[e->pattern_mode] = e->start_beat[e->pattern_mode] = absolute;
    ed_transport_publish(e, dm_beat_seconds(&e->project, absolute), playing);
}

static void timeline_begin(Editor *e, UiRect r, float label, int context, bool hovered,
                           UiMouse mouse) {
    if (hovered && mouse.clicked && mouse.pos.y < r.y + 28 && mouse.pos.x >= r.x + label) {
        ed_release_piano(e);
        e->scrub = true;
        e->scrub_context = context;
        e->scrub_playing = dm_audio_playing(e->audio);

        if (context == 1) {
            e->pattern_mode = false;
        }

        e->scrub_origin = context == 2 && !e->pattern_mode ? ed_pattern_seek_origin(e) : 0;
        e->context = context;
        e->scrub_target = -1;
    }
}

static void timeline_region(Editor *e, UiRect r, View *v, float scale, float label, int context,
                            bool hovered, UiMouse mouse) {
    int mode = context == 2;

    if (hovered && mouse.right_clicked && mouse.pos.y < r.y + 28 && mouse.pos.x >= r.x + label) {
        ed_release_piano(e);
        e->loop_drag = true;
        e->loop_context = context;
        e->loop_anchor = ed_snap((mouse.pos.x - r.x - label + v->scroll_x) / scale, .25, mouse.alt);
        e->pattern_mode = mode;
        e->context = context;
        ed_focus_edit(e, mode ? FOCUS_NOTES : FOCUS_PLAYLIST);
    }

    if (!e->loop_drag || e->loop_context != context) {
        return;
    }

    if (mouse.right) {
        double length = mode && ed_pattern(e) ? ed_pattern_length(e) : dm_duration(&e->project);
        timeline_autoscroll(e, r, v, scale, label, mouse, length);
        double beat = fmin(
            length, ed_snap((mouse.pos.x - r.x - label + v->scroll_x) / scale, .25, mouse.alt));
        e->loop_start[mode] = fmin(e->loop_anchor, beat);
        e->loop_end[mode] = fmax(e->loop_anchor, beat);
        e->loop_enabled[mode] = e->loop_end[mode] - e->loop_start[mode] >= .001;
    } else {
        e->loop_drag = false;
        ed_transport_publish(e, dm_beat_seconds(&e->project, e->cursor_beat[mode]),
                             dm_audio_playing(e->audio));
    }
}

bool ed_clear_loop(Editor *e) {
    int mode = e->edit_focus == FOCUS_PLAYLIST                                   ? 0
               : (e->edit_focus == FOCUS_NOTES || e->edit_focus == FOCUS_EVENTS) ? 1
                                                                                 : -1;

    if (mode < 0 || !e->loop_enabled[mode]) {
        return false;
    }

    e->loop_enabled[mode] = false;
    e->loop_drag = false;

    if (mode) {
        e->pattern_mode = false;
    }

    ed_transport_publish(e, dm_beat_seconds(&e->project, e->cursor_beat[e->pattern_mode]),
                         dm_audio_playing(e->audio));
    return true;
}

void ed_timeline_drag(Editor *e, UiRect r, View *v, float scale, float label, int context,
                      bool hovered) {
    UiMouse mouse = ui_mouse();
    timeline_region(e, r, v, scale, label, context, hovered, mouse);
    timeline_begin(e, r, label, context, hovered, mouse);

    if (!e->scrub || e->scrub_context != context) {
        return;
    }

    if (mouse.left) {
        double length =
            context == 2 && ed_pattern(e) ? ed_pattern_length(e) : dm_duration(&e->project);
        timeline_autoscroll(e, r, v, scale, label, mouse, length);
        double beat = ed_snap((mouse.pos.x - r.x - label + v->scroll_x) / scale,
                              context == 2 ? .25 : 1, mouse.alt);
        beat = fmin(beat, length);
        e->cursor_beat[context == 2] = e->start_beat[context == 2] = beat;

        if (fabs(beat - e->scrub_target) > 1e-8) {
            ed_scrub_publish(e, beat, context, true);
            e->scrub_target = beat;
        }
    }

    if (mouse.released || !mouse.left) {
        e->scrub = false;
        ed_scrub_publish(e, e->cursor_beat[context == 2], context, e->scrub_playing);
    }
}
