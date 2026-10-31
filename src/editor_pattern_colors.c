#include "editor_internal.h"
#include "text/en.h"

/* Pattern colors are chosen per pattern. Index 0 follows the application color. */
static const uint32_t palette[DM_PATTERN_COLORS] = {
    COLOR(232, 104, 104), COLOR(240, 150, 82),  COLOR(242, 193, 78),  COLOR(222, 220, 104),
    COLOR(170, 214, 92),  COLOR(96, 200, 120),  COLOR(76, 201, 170),  COLOR(80, 196, 222),
    COLOR(96, 164, 236),  COLOR(120, 136, 240), COLOR(150, 120, 232), COLOR(186, 120, 226),
    COLOR(222, 110, 200), COLOR(240, 130, 166), COLOR(184, 140, 104), COLOR(160, 170, 180)};

uint32_t ed_palette_color(Editor *e, unsigned color) {
    if (!color || color > DM_PATTERN_COLORS) {
        return e->accent;
    }

    return palette[color - 1];
}

uint32_t ed_pattern_color(Editor *e, int id) {
    if (id < 0 || (size_t)id >= e->project.pattern_count) {
        return e->accent;
    }

    return ed_palette_color(e, e->project.patterns[id].color);
}

void ed_pattern_color_request(Editor *e, int pattern) {
    if (pattern < 0) {
        bool any = false;

        for (size_t i = 0; i < e->project.clip_count; ++i) {
            any |= e->project.clips[i].selected != 0;
        }

        if (!any) {
            return;
        }
    } else if ((size_t)pattern >= e->project.pattern_count) {
        return;
    }

    e->pattern_color_target = pattern;
    e->pattern_color_popup = true;
}

static bool target_has_color(Editor *e, unsigned color) {
    if (e->pattern_color_target >= 0) {
        return e->project.patterns[e->pattern_color_target].color == color;
    }

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        DmClip clip = e->project.clips[i];

        if (clip.selected && e->project.patterns[clip.pattern].color != color) {
            return false;
        }
    }

    return true;
}

static void apply_color(Editor *e, unsigned color) {
    if (target_has_color(e, color)) {
        return;
    }

    ed_checkpoint(e);

    if (e->pattern_color_target >= 0) {
        e->project.patterns[e->pattern_color_target].color = (uint8_t)color;
    } else {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            if (e->project.clips[i].selected) {
                e->project.patterns[e->project.clips[i].pattern].color = (uint8_t)color;
            }
        }
    }

    ed_commit(e);
}

void ed_pattern_color_dialog(Editor *e) {
    bool request = e->pattern_color_popup;
    e->pattern_color_popup = false;

    if (e->pattern_color_target >= (int)e->project.pattern_count) {
        e->pattern_color_target = -1;
    }

    if (!ui_mouse_popup(DM_TEXT_PATTERN_COLOR, request)) {
        return;
    }

    ui_text(DM_TEXT_PATTERN_COLOR);

    for (unsigned color = 0; color <= DM_PATTERN_COLORS; ++color) {
        ui_id((int)color);

        /* Application color, then two rows of eight palette colors. */
        if (color != 1 && color != 9) {
            ui_same();
        }

        if (ui_port_color("##pattern-color", ed_palette_color(e, color), target_has_color(e, color),
                          25)) {
            apply_color(e, color);
            ui_close_popup();
        }

        ui_tip(color ? DM_TEXT_PATTERN_COLOR_SWATCH : DM_TEXT_APPLICATION_COLOR);
        ui_pop_id();
    }

    ui_popup_end();
}
