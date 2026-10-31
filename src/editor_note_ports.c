#include "editor_internal.h"
#include "text/en.h"

enum { VISIBLE_PORTS = 8 };

uint32_t ed_port_color(Editor *e, unsigned port) {
    static const uint32_t colors[VISIBLE_PORTS] = {
        COLOR(83, 190, 244), COLOR(242, 151, 73), COLOR(183, 130, 243), COLOR(242, 110, 135),
        COLOR(76, 211, 184), COLOR(168, 211, 91), COLOR(233, 140, 216), COLOR(116, 156, 248)};
    return port ? colors[(port - 1) % VISIBLE_PORTS] : e->accent;
}

unsigned ed_note_port(Editor *e) {
    DmPattern *p = ed_pattern(e);
    return p && p->kind ? (unsigned)p->port : e->note_ports[e->pattern_channel];
}

static void channel_ports(Editor *e, bool *used) {
    memset(used, 0, DM_NOTE_PORTS * sizeof(*used));

    for (size_t i = 0; i < e->project.clip_count; i++) {
        if (e->project.clips[i].channel == e->pattern_channel) {
            DmPattern *p = &e->project.patterns[e->project.clips[i].pattern];

            for (size_t j = 0; j < p->count; j++) {
                used[p->notes[j].port] = true;
            }
        }
    }

    DmPattern *p = ed_pattern(e);

    if (p) {
        for (size_t j = 0; j < p->count; j++) {
            used[p->notes[j].port] = true;
        }
    }
}

float ed_port_buttons_width(Editor *e) {
    bool used[DM_NOTE_PORTS];
    channel_ports(e, used);
    unsigned count = 0;

    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (used[p]) {
            count++;
        }
    }

    if (!count) {
        e->note_ports[e->pattern_channel] = 0;
        return 0;
    }

    count = 1;

    for (unsigned p = 1; p <= VISIBLE_PORTS; p++) {
        if (used[p]) {
            count++;
        }
    }

    return count * (22 + ui_item_spacing_x());
}

/* Returns the selected channel's notes on a port to the default port and discards that port's
   events and mixer settings for the channel. */
bool ed_delete_note_port(Editor *e, unsigned port) {
    int channel = e->pattern_channel;
    DmProject *project = &e->project;

    if (!port || port >= DM_NOTE_PORTS) {
        return false;
    }

    ed_checkpoint(e);
    ed_release_piano(e);

    for (size_t c = 0; c < project->clip_count; ++c) {
        if (project->clips[c].channel != channel) {
            continue;
        }

        int pattern = project->clips[c].pattern;
        DmPattern *p = &project->patterns[pattern];

        for (size_t n = 0; n < p->count; ++n) {
            if (p->notes[n].port == port) {
                p->notes[n].port = 0;
            }
        }

        size_t kept = 0;

        for (size_t i = 0; i < project->automation_count; ++i) {
            DmAutomation a = project->automation[i];

            if (a.pattern != pattern || a.port != port) {
                project->automation[kept++] = a;
            }
        }

        project->automation_count = kept;

        if (p->kind && p->port == (int)port) {
            p->port = 0;
        }
    }

    size_t kept = 0;

    for (size_t i = 0; i < project->port_mix_count; ++i) {
        DmPortMix mix = project->port_mix[i];

        if (mix.channel != channel || mix.port != (int)port) {
            project->port_mix[kept++] = mix;
        }
    }

    project->port_mix_count = kept;
    e->note_ports[channel] = 0;
    e->automation_node = -1;
    e->event_selection.size = 0;
    ed_commit(e);
    return true;
}

static void port_menu(Editor *e, unsigned port, bool request) {
    if (!ui_mouse_popup(DM_TEXT_NOTE_PORT_ACTIONS, request)) {
        return;
    }

    char title[80];
    snprintf(title, sizeof(title), DM_TEXT_NOTE_PORT, port);
    ui_text(title);
    ui_dim(DM_TEXT_DELETE_PORT_WARNING);
    ui_separator();

    if (ui_menu_item(DM_TEXT_DELETE_PORT, NULL, true)) {
        ed_delete_note_port(e, port);
        ui_close_popup();
    }

    ui_popup_end();
}

void ed_port_buttons(Editor *e) {
    bool used[DM_NOTE_PORTS], colored = false;
    channel_ports(e, used);

    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        colored |= used[p];
    }

    if (!colored) {
        e->note_ports[e->pattern_channel] = 0;
        return;
    }

    used[0] = true;
    static unsigned menu_port;
    bool request = false;

    for (unsigned port = 0; port <= VISIBLE_PORTS; port++) {
        if (!used[port]) {
            continue;
        }

        ui_id((int)port);

        if (ui_port_color("##event-port", ed_port_color(e, port), ed_note_port(e) == port, 22)) {
            /* Shift+click on a colored port offers to delete it. */
            if (ui_mouse().shift && port) {
                menu_port = port;
                request = true;
            } else {
                ed_release_piano(e);
                e->note_ports[e->pattern_channel] = (uint8_t)port;
                DmPattern *p = ed_pattern(e);

                if (p && p->kind && p->port != (int)port) {
                    ed_checkpoint(e);
                    p->port = (int)port;
                    ed_commit(e);
                }

                e->automation_node = -1;
                e->event_selection.size = 0;
            }
        }

        char hint[96];
        snprintf(hint, sizeof(hint), port ? DM_TEXT_PORT_SWATCH_HINT : DM_TEXT_DEFAULT_PORT_HINT,
                 port);
        ui_tip(hint);
        ui_pop_id();
        ui_same();
    }

    port_menu(e, menu_port, request);
}

bool ed_color_notes(Editor *e, unsigned port) {
    DmPattern *p = ed_pattern(e);

    if (!p || p->kind || port >= DM_NOTE_PORTS) {
        return false;
    }

    bool changed = false;

    for (size_t i = 0; i < p->count; i++) {
        changed |= p->notes[i].selected && p->notes[i].port != port;
    }

    if (!changed) {
        return false;
    }

    ed_checkpoint(e);

    for (size_t i = 0; i < p->count; i++) {
        if (p->notes[i].selected) {
            p->notes[i].port = (uint8_t)port;
        }
    }

    e->note_ports[e->pattern_channel] = (uint8_t)port;
    ed_commit(e);
    return true;
}

void ed_note_color_dialog(Editor *e) {
    bool request = e->note_color_popup;
    e->note_color_popup = false;

    if (ui_mouse_popup(DM_TEXT_NOTE_COLORS, request)) {
        UiPoint top = ui_cursor();
        float width = (VISIBLE_PORTS + 1) * 25 + VISIBLE_PORTS * ui_item_spacing_x();
        ui_text(DM_TEXT_ASSIGN_NOTE_PORT);
        ui_same();
        ui_position(top.x + width - 24, top.y);
        ui_info(DM_TEXT_INDEPENDENT_MIDI_PORTS_MAY);

        for (unsigned port = 0; port <= VISIBLE_PORTS; port++) {
            ui_id((int)port);

            if (port) {
                ui_same();
            }

            if (ui_port_color("##note-port", ed_port_color(e, port), ed_note_port(e) == port, 25)) {
                ed_color_notes(e, port);
                ui_close_popup();
            }

            char hint[80];
            snprintf(hint, sizeof(hint),
                     port ? DM_TEXT_NOTE_PORT : DM_TEXT_DEFAULT_REMOVE_EXPLICIT_COLOR, port);
            ui_tip(hint);
            ui_pop_id();
        }

        ui_popup_end();
    }
}
