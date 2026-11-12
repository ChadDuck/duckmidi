#include "editor/internal.h"
#include "text/en.h"

static bool pattern_context(Editor *e, size_t i);
static void delete_pattern(Editor *e, size_t i);
static void pattern_browser_header(Editor *e);

static void mixer_peak_update(Editor *e, uint64_t now);

static void delete_pattern(Editor *e, size_t i) {
    ed_checkpoint(e);
    dm_pattern_remove(&e->project, (int)i);
    e->selected_pattern = e->project.pattern_count ? 0 : -1;
    ed_bind_pattern_channel(e);
    ed_commit(e);
}

/* Right click opens a pattern card's menu. Patterns hold song data, so the right button never
   erases them here; deleting is a menu choice. */
static bool pattern_context(Editor *e, size_t i) {
    if (!ui_context(DM_TEXT_PATTERN_ACTIONS)) {
        return false;
    }

    if (ui_menu_item(DM_TEXT_RENAME, NULL, true)) {
        e->rename_pattern = (int)i;
        snprintf(e->rename_text, sizeof(e->rename_text), "%s", e->project.patterns[i].name);
    }

    char binding[96];

    if (ui_menu_item(DM_TEXT_DUPLICATE, ed_binding_text(e, ACT_DUPLICATE, binding, sizeof(binding)),
                     true)) {
        ed_duplicate_pattern(e, (int)i);
    }

    if (ui_menu_item(DM_TEXT_CHANGE_COLOR,
                     ed_binding_text(e, ACT_NOTE_COLOR, binding, sizeof(binding)), true)) {
        ed_pattern_color_request(e, (int)i);
    }

    bool removed = false;

    if (ui_menu_item(DM_TEXT_DELETE_PATTERN, NULL, true)) {
        delete_pattern(e, i);
        removed = true;
    }

    ui_popup_end();
    return removed;
}

/* Draws a pattern's notes as short lines for cards and placements. Notes outside the visible area
   are skipped, and only the first note of each key in a pixel column is drawn, so dense patterns
   stay cheap to draw every frame. */
void ed_note_thumbnail(const DmPattern *p, NoteThumbnail t) {
    static uint64_t drawn[4096][2];
    int columns = (int)fminf(4096, fmaxf(1, ceilf(t.visible.w) + 1));
    memset(drawn, 0, (size_t)columns * sizeof(*drawn));

    for (size_t n = 0; n < p->count; ++n) {
        DmNote note = p->notes[n];
        float x = t.x + (float)(note.beat - t.offset) * t.x_scale;
        float width = fmaxf(2, (float)note.length * t.x_scale);
        float y = t.y - (note.key - 48) * t.y_scale;

        if (y < t.top || y > t.bottom || x > t.visible.x + t.visible.w || x + width < t.visible.x) {
            continue;
        }

        int column = (int)fmaxf(0, x - t.visible.x);

        if (column < columns) {
            uint64_t bit = 1ull << (note.key & 63);

            if (drawn[column][note.key >> 6] & bit) {
                continue;
            }

            drawn[column][note.key >> 6] |= bit;
        }

        ui_rect((UiRect){x, y, width, 2}, t.color, 0);
    }
}

void ed_duplicate_pattern(Editor *e, int pattern) {
    if (pattern < 0 || (size_t)pattern >= e->project.pattern_count) {
        return;
    }

    ed_checkpoint(e);
    int id = dm_pattern_copy(&e->project, pattern);

    if (id >= 0) {
        ed_open_pattern(e, id, true);
        ed_focus_edit(e, FOCUS_PATTERNS);
        ed_commit(e);
    }
}

void ed_choose_pattern_card(Editor *e, int id, bool selected) {
    bool keep = id == e->selected_pattern && (e->preview_override || e->preview_source_channel < 0);
    int program = e->preview_program, source = e->preview_source_channel;
    bool override = e->preview_override;
    ed_release_piano(e);

    if (selected && ed_gesture_held(e, GESTURE_DESELECT, ui_mouse())) {
        ed_clear_pattern_selection(e);
    } else {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            e->project.clips[i].selected = 0;
        }

        ed_open_pattern(e, id, false);
        ed_bind_pattern_channel(e);

        if (keep) {
            e->preview_program = program;
            e->preview_source_channel = source;
            e->preview_override = override;
        }

        e->context_note = -1;
        e->automation_center =
            e->project.patterns[id].count ? e->project.patterns[id].notes[0].key : 60;
    }
}

static void remap_pattern_refs(Editor *e, int from, int to) {
    int *refs[] = {&e->selected_pattern, &e->rename_pattern, &e->pattern_color_target,
                   &e->pattern_drag};

    for (size_t i = 0; i < sizeof(refs) / sizeof(*refs); ++i) {
        int index = *refs[i];

        if (to >= 0) {
            *refs[i] = dm_pattern_moved_index(index, from, to);
        } else {
            *refs[i] = index == from ? -1 : index - (index > from);
        }
    }
}

static long pattern_number(const char *name, const char *prefix) {
    size_t length = strlen(prefix);

    if (strncmp(name, prefix, length)) {
        return 0;
    }

    const char *digits = name + length;
    char *end;
    long number = strtol(digits, &end, 10);
    return *digits >= '1' && *digits <= '9' && !*end ? number : 0;
}

static void create_pattern(Editor *e, bool automation) {
    e->automation_browser = automation;
    DmProject *p = &e->project;
    const char *prefix =
        automation ? DM_TEXT_AUTOMATION_NUMBER_PREFIX : DM_TEXT_PATTERN_NUMBER_PREFIX;
    int count = (int)p->pattern_count;
    bool *used = calloc((size_t)count + 2, sizeof(*used));

    if (!used) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PATTERN_EDIT);
        return;
    }

    for (int i = 0; i < count; ++i) {
        long number = pattern_number(p->patterns[i].name, prefix);

        if (number > 0 && number <= count + 1) {
            used[number] = true;
        }
    }

    int number = 1;

    while (used[number]) {
        ++number;
    }

    free(used);
    int before = count;

    for (int i = 0; i < count; ++i) {
        if ((p->patterns[i].kind != 0) == automation &&
            pattern_number(p->patterns[i].name, prefix) > number) {
            before = i;
            break;
        }
    }

    char name[64];
    snprintf(name, sizeof(name), "%s%d", prefix, number);
    bool pushed = ed_checkpoint(e);
    int id = dm_pattern_add(p, name);

    if (id < 0) {
        ed_checkpoint_abandon(e, pushed);
        return;
    }

    if (automation) {
        p->patterns[id].kind = 1;
        p->patterns[id].length = dm_bar(p);
    }

    int to = dm_pattern_move(p, id, before);
    remap_pattern_refs(e, id, to);
    ed_open_pattern(e, to, true);
    ed_bind_pattern_channel(e);
    ed_commit(e);
}

void ed_step_pattern(Editor *e, bool automation, int direction) {
    e->automation_browser = automation;
    DmProject *p = &e->project;

    if (!p->pattern_count) {
        if (direction > 0) {
            create_pattern(e, automation);
        }

        return;
    }

    bool *used = calloc(p->pattern_count, sizeof(*used));

    if (!used) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PATTERN_EDIT);
        return;
    }

    for (size_t i = 0; i < p->clip_count; ++i) {
        int id = p->clips[i].pattern;

        if ((size_t)id < p->pattern_count) {
            used[id] = true;
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        int id = p->automation[i].pattern;

        if ((size_t)id < p->pattern_count) {
            used[id] = true;
        }
    }

    int count = (int)p->pattern_count;
    int selected = e->selected_pattern;
    bool in_category =
        selected >= 0 && selected < count && (p->patterns[selected].kind != 0) == automation;

    for (int i = 0; i < count; ++i) {
        used[i] |= p->patterns[i].mode != 0 || p->patterns[i].count != 0;
    }

    if (direction > 0) {
        /* Keep creating cards while the selection is on an empty one. */
        if (!in_category || used[selected]) {
            for (int i = in_category ? selected + 1 : 0; i < count; ++i) {
                if (used[i] && (p->patterns[i].kind != 0) == automation) {
                    free(used);
                    ed_open_pattern(e, i, false);
                    ed_bind_pattern_channel(e);
                    return;
                }
            }
        }

        free(used);
        create_pattern(e, automation);
        return;
    }

    /* UIDs follow creation order even when a card moves or fills a gap in the list. */
    int remove = -1;

    for (size_t i = 0; i < p->pattern_count; ++i) {
        const DmPattern *pattern = &p->patterns[i];

        if ((pattern->kind != 0) == automation && !used[i] &&
            (remove < 0 || pattern->uid > p->patterns[remove].uid)) {
            remove = (int)i;
        }
    }

    if (remove < 0) {
        for (int i = (in_category ? selected : count) - 1; i >= 0; --i) {
            if (used[i] && (p->patterns[i].kind != 0) == automation) {
                ed_open_pattern(e, i, false);
                ed_bind_pattern_channel(e);
                break;
            }
        }

        free(used);
        return;
    }

    free(used);
    ed_checkpoint(e);
    dm_pattern_remove(p, remove);
    remap_pattern_refs(e, remove, -1);
    const DmPattern *current = ed_pattern(e);

    if (!current || (current->kind != 0) != automation) {
        int last = (int)p->pattern_count - 1;
        int next = current ? e->selected_pattern : last;

        for (int i = last; i >= 0; --i) {
            if ((p->patterns[i].kind != 0) == automation) {
                next = i;

                if (i < remove) {
                    break;
                }
            }
        }

        ed_open_pattern(e, next, false);
        ed_bind_pattern_channel(e);
        e->automation_browser = automation;
    }

    ed_commit(e);
}

static void pattern_browser_header(Editor *e) {
    ui_align_frame();
    ui_text(DM_TEXT_PATTERNS);
    ui_same();

    if (ui_button(DM_TEXT_NEW, 70, 0)) {
        create_pattern(e, e->automation_browser);
    }

    ui_same();
    float available = ui_available().x;
    float selector = fminf(128, available);
    UiPoint position = ui_cursor();
    ui_position(position.x + available - selector, position.y);
    ui_width(selector);

    if (ui_combo("##Pattern category",
                 e->automation_browser ? DM_TEXT_AUTOMATION : DM_TEXT_PATTERNS_TITLE)) {
        if (ui_select(DM_TEXT_PATTERNS_TITLE, !e->automation_browser, 0, 0)) {
            e->automation_browser = false;
        }

        if (ui_select(DM_TEXT_AUTOMATION, e->automation_browser, 0, 0)) {
            e->automation_browser = true;
        }

        ui_combo_end();
    }

    ui_separator();
}

#define CARD_HEIGHT 92

/* Draws a pattern card's face. Cards match playlist placements: the pattern color fills it. */
static void pattern_card_face(Editor *e, int i, UiRect r, bool selected, bool hovered) {
    uint32_t color = ed_pattern_color(e, i);
    ui_rect(r, color, 5);

    if (hovered) {
        ui_rect(r, 0x28ffffffu, 5);
    }

    ui_border(r, selected ? TEXT : color, 5, selected ? 2 : 1);
    ui_clip(r);
    ui_label((UiPoint){r.x + 9, r.y + 10}, ui_text_on(color), e->project.patterns[i].name);
    DmPattern *p = &e->project.patterns[i];

    ed_note_thumbnail(p, (NoteThumbnail){.visible = r,
                                         .color = ui_text_on(color),
                                         .x = r.x + 9,
                                         .x_scale = (float)((r.w - 18) / p->length),
                                         .y = r.y + 68,
                                         .y_scale = 1.1f,
                                         .top = r.y + 32,
                                         .bottom = r.y + 81});

    if (p->kind) {
        ed_automation_preview(e, i, (UiRect){r.x + 9, r.y + 33, r.w - 18, 48}, ui_text_on(color),
                              r.x + 9, (float)((r.w - 18) / p->length), 0);
    }

    ui_unclip();
}

/* The card's button, which also starts a drag. While dragged, the card floats under the mouse
   with the grab cursor. Returns true when its menu deleted the pattern. */
static bool pattern_card_button(Editor *e, int i, float card, float height, bool *hovered) {
    bool selected = e->pattern_highlight && e->selected_pattern == i;
    ui_accent(selected);

    if (ui_button("##pattern", card, height)) {
        ed_choose_pattern_card(e, i, selected);
    }

    ui_accent_end();
    *hovered = ui_hover();

    if (pattern_context(e, (size_t)i)) {
        return true;
    }

    if (ui_drag_pattern(i, !e->pattern_drag_on_playlist)) {
        e->pattern_drag = i;
        ui_gesture_cursor(false);

        if (!e->pattern_drag_on_playlist) {
            UiPoint at = ui_cursor();
            ui_dummy(card, CARD_HEIGHT);
            pattern_card_face(e, i, (UiRect){at.x, at.y, card, CARD_HEIGHT}, selected, false);
        }

        ui_drag_pattern_end();
    }

    return false;
}

/* Next grid cell: cards fill two columns. */
static void pattern_cell(int *cell) {
    if ((*cell)++ % 2) {
        ui_same();
    }
}

/* The dragged card keeps its button alive, since ImGui ends a drag whose source disappears: as
   the outlined open space where it will land, or as a thin row while it is elsewhere. */
static void dragged_cell(Editor *e, int dragged, float card, bool open, int *cell) {
    pattern_cell(cell);
    ui_id(dragged);
    UiPoint at = ui_cursor();
    ui_invisible_button("##pattern", card, open ? CARD_HEIGHT : 1);

    if (open) {
        ui_border((UiRect){at.x, at.y, card, CARD_HEIGHT}, DIM, 5, 1);
    }

    if (ui_drag_pattern(dragged, !e->pattern_drag_on_playlist)) {
        bool selected = e->pattern_highlight && e->selected_pattern == dragged;
        ui_gesture_cursor(false);

        if (!e->pattern_drag_on_playlist) {
            UiPoint floating = ui_cursor();
            ui_dummy(card, CARD_HEIGHT);
            pattern_card_face(e, dragged, (UiRect){floating.x, floating.y, card, CARD_HEIGHT},
                              selected, false);
        }

        ui_drag_pattern_end();
    }

    ui_pop_id();
}

/* The card being dragged in this category, or -1. */
static int dragged_card(Editor *e) {
    int i = e->pattern_drag;
    return i >= 0 && (size_t)i < e->project.pattern_count &&
                   (e->project.patterns[i].kind != 0) == e->automation_browser
               ? i
               : -1;
}

/* Grid slot under the mouse among the cards left once the dragged card is lifted out. */
static int drop_slot(UiPoint origin, UiPoint mouse, float card, int count) {
    float column = card + ui_item_spacing_x(), row = CARD_HEIGHT + ui_item_spacing_y();
    int y = (int)floorf(fmaxf(0, mouse.y - origin.y) / row);
    int x = mouse.x - origin.x >= column - ui_item_spacing_x() * .5f ? 1 : 0;
    return (int)fminf((float)count, (float)(y * 2 + x));
}

/* Dropping a dragged card in the pattern section moves it to the open slot. */
static void move_pattern_card(Editor *e, int dragged, int slot) {
    int before = (int)e->project.pattern_count, seen = 0;

    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        if ((int)i == dragged || (e->project.patterns[i].kind != 0) != e->automation_browser) {
            continue;
        }

        if (seen++ == slot) {
            before = (int)i;
            break;
        }
    }

    /* Past the last card of this category: after it, ahead of any later pattern. */
    if (before == (int)e->project.pattern_count) {
        for (size_t i = e->project.pattern_count; i-- > 0;) {
            if ((int)i != dragged && (e->project.patterns[i].kind != 0) == e->automation_browser) {
                before = (int)i + 1;
                break;
            }
        }

        if (before == (int)e->project.pattern_count && seen == 0) {
            return; /* The only card in its category. */
        }
    }

    int to = before > dragged ? before - 1 : before;

    if (to == dragged) {
        return;
    }

    ed_checkpoint(e);
    dm_pattern_move(&e->project, dragged, before);
    remap_pattern_refs(e, dragged, to);
    e->pattern_drag = -1;
    ed_commit(e);
}

void ed_pattern_panel(Editor *e, float width, float height) {
    ui_child(DM_TEXT_PATTERNS_TITLE, width, height, true);

    if (ui_panel_clicked()) {
        ed_focus_edit(e, FOCUS_PATTERNS);
    }

    pattern_browser_header(e);
    int cell = 0, slot = 0, count = 0;
    float card = (ui_available().x - 8) / 2;
    UiMouse mouse = ui_mouse();
    UiPoint origin = ui_cursor();
    int dragged = dragged_card(e);

    /* While a card is dragged, the others close up behind it; over this section they open a
       space where it will land. */
    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        count += (int)i != dragged && (e->project.patterns[i].kind != 0) == e->automation_browser;
    }

    bool over = dragged >= 0 && ed_inside(mouse.pos, ui_window_rect());
    int gap = over ? drop_slot(origin, mouse.pos, card, count) : -1;
    bool removed = false;

    for (size_t i = 0; i < e->project.pattern_count && !removed; ++i) {
        if ((int)i == dragged || (e->project.patterns[i].kind != 0) != e->automation_browser) {
            continue;
        }

        if (slot++ == gap) {
            dragged_cell(e, dragged, card, true, &cell);
        }

        pattern_cell(&cell);
        ui_id((int)i);
        UiPoint at = ui_cursor();
        bool hovered;
        removed = pattern_card_button(e, (int)i, card, CARD_HEIGHT, &hovered);

        if (!removed) {
            bool selected = e->pattern_highlight && e->selected_pattern == (int)i;
            pattern_card_face(e, (int)i, (UiRect){at.x, at.y, card, CARD_HEIGHT}, selected,
                              hovered && dragged < 0);

            if (hovered && dragged < 0) {
                ed_hover(e, DM_TEXT_SELECT_PATTERN_CLICK);
            }
        }

        ui_pop_id();
    }

    /* The open space after the last card, or a thin row while the card is outside the section. */
    if (dragged >= 0 && !removed && (!over || gap == count)) {
        dragged_cell(e, dragged, card, over, &cell);
    }

    if (dragged >= 0 && !removed && over && mouse.released) {
        move_pattern_card(e, dragged, gap);
    }

    if (ui_background_clicked()) {
        ed_clear_pattern_selection(e);
    }

    ui_child_end();
}

bool ed_mixer_apply(Editor *e, DmPortMix before, DmPortMix mix) {
    if (!dm_port_mix_set(&e->project, mix)) {
        return false;
    }

    if (mix.volume != before.volume) {
        dm_audio_part_control(e->audio, mix.port, mix.channel, MIDI_CC_VOLUME, mix.volume);
    }

    if (mix.pan != before.pan) {
        dm_audio_part_control(e->audio, mix.port, mix.channel, MIDI_CC_PAN, mix.pan);
    }

    int preview = ed_context(e) == CONTEXT_ROLL ? e->pattern_channel : e->selected_channel;

    if (preview == mix.channel && e->note_ports[preview] == mix.port) {
        ed_keyboard_channel(e);
    }

    return true;
}

static void mixer_store(Editor *e, DmPortMix before, DmPortMix mix) {
    bool checkpoint = ed_checkpoint(e);

    if (!ed_mixer_apply(e, before, mix)) {
        ed_checkpoint_abandon(e, checkpoint);
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PORT_MIXER);
        return;
    }

    ed_midi_bindings_reset_pickup(e);
    ed_commit(e);
}

static void mixer_midi_learn(Editor *e, EdMidiTargetKind kind, int channel, int port) {
    EdMidiTarget target = {kind, (uint8_t)channel, (uint8_t)port};

    if (ed_midi_binding_learning(e, target)) {
        ui_border(ui_item_rect(), e->accent, 3, 2);
    }

    static const char *const ids[] = {
        [ED_MIDI_VOLUME] = "midi-volume",
        [ED_MIDI_PAN] = "midi-pan",
        [ED_MIDI_MUTE] = "midi-mute",
        [ED_MIDI_SOLO] = "midi-solo",
    };

    if (!ui_context(ids[kind])) {
        return;
    }

    EdMidiBinding *binding = ed_midi_binding_find(e, target);

    if (binding) {
        char source[DM_MIDI_NAME + 50];
        ed_midi_source_label(&binding->source, source, sizeof(source));
        ui_dim(source);
    }

    if (ui_menu_item(DM_TEXT_MIDI_BIND_LEARN, NULL, e->midi_input != NULL)) {
        ed_midi_binding_learn(e, target, false);
    }

    if (ui_menu_item(DM_TEXT_MIDI_BIND_LEARN_ANY, NULL, e->midi_input != NULL)) {
        ed_midi_binding_learn(e, target, true);
    }

    if (binding && ui_menu_item(DM_TEXT_MIDI_BIND_CLEAR, NULL, true)) {
        ed_midi_binding_clear(e, target);
    }

    ui_popup_end();
}

static void mixer_controls(Editor *e, int c, int port) {
    DmPortMix mix = dm_port_mix(&e->project, port, c), original = mix;
    DmPortMix reset = {port, c, 100, 64, false, false, original.program};

    if (!port) {
        DmChannel value = e->reset_channels[c];
        reset.volume = value.volume;
        reset.pan = value.pan;
        reset.mute = value.mute;
        reset.solo = value.solo;
    }

    int part = port * 16 + c;
    DmPortMix shown = mix;

    if (e->mixer_live && e->mixer_live_volume[part] < 128) {
        shown.volume = e->mixer_live_volume[part];
    }

    if (e->mixer_live && e->mixer_live_pan[part] < 128) {
        shown.pan = e->mixer_live_pan[part];
    }

    DmPortMix displayed = shown;
    char text[32];
    UiPoint start = ui_cursor(), area = ui_available();
    float width = area.x, height = fmaxf(40, area.y - 110);
    snprintf(text, sizeof(text), "%d", port);
    ui_center_text(text);
    snprintf(text, sizeof(text), DM_TEXT_PORT, port);
    ui_tip(text);
    ui_rect((UiRect){start.x + (width - 16) * .5f, start.y + 19, 16, 2}, ed_port_color(e, port), 1);
    UiPoint v = {start.x + (width - 34) * .5f, start.y + 28};
    ui_position(v.x, v.y);
    ui_vslider("##volume", &shown.volume, height);

    if (ui_item_middle()) {
        shown.volume = reset.volume;
    }

    ui_tip(DM_TEXT_INITIAL_VOLUME_MIDDLE_CLICK);
    mixer_midi_learn(e, ED_MIDI_VOLUME, c, port);
    float peak = e->mixer_peaks[part];
    ui_rect((UiRect){v.x + 30, v.y, 4, height}, BG, 2);
    ui_rect((UiRect){v.x + 30, v.y + height * (1 - fminf(peak, 1)), 4, height * fminf(peak, 1)},
            peak > .95f ? GOLD : ed_port_color(e, port), 2);
    ui_position(start.x, v.y + height + 4);
    ui_accent(mix.mute);

    if (ui_button(DM_TEXT_M, (width - 2) * .5f, 24)) {
        mix.mute = !mix.mute;
    }

    ui_accent_end();

    if (ui_item_middle()) {
        mix.mute = reset.mute;
    }

    ui_tip(DM_TEXT_MUTE_THIS_PORT);
    mixer_midi_learn(e, ED_MIDI_MUTE, c, port);
    ui_same();
    ui_accent(mix.solo);

    if (ui_button(DM_TEXT_S, (width - 2) * .5f, 24)) {
        mix.solo = !mix.solo;
    }

    ui_accent_end();

    if (ui_item_middle()) {
        mix.solo = reset.solo;
    }

    ui_tip(DM_TEXT_SOLO_THIS_PORT);
    mixer_midi_learn(e, ED_MIDI_SOLO, c, port);
    ui_position(start.x, v.y + height + 32);
    ui_mixer_pan(&shown.pan, width);

    if (ui_item_middle()) {
        shown.pan = reset.pan;
    }

    snprintf(text, sizeof(text), DM_TEXT_PAN_MIDDLE_CLICK_RESET, shown.pan);
    ui_tip(text);
    mixer_midi_learn(e, ED_MIDI_PAN, c, port);
    ui_position(start.x, v.y + height + 58);
    ui_mixer_value(&shown.volume, width);

    /* During playback the controls show the song's values; an edit sets the channel's own. */
    if (shown.volume != displayed.volume) {
        mix.volume = shown.volume;
    }

    if (shown.pan != displayed.pan) {
        mix.pan = shown.pan;
    }

    if (mix.volume != original.volume || mix.pan != original.pan || mix.mute != original.mute ||
        mix.solo != original.solo) {
        mixer_store(e, original, mix);
    }
}

bool ed_channel_has_clips(Editor *e, int channel) {
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].channel == channel) {
            return true;
        }
    }

    return false;
}

int ed_available_channel(Editor *e) {
    for (int c = 0; c < 16; ++c) {
        if (!e->project.channels[c].enabled) {
            return c;
        }
    }

    return -1;
}

static void mixer_peak_update(Editor *e, uint64_t now) {
    bool sample = now - e->mixer_peak_tick >= 25;

    if (sample) {
        e->mixer_peak_tick = now;
    }

    for (int c = 0; c < DM_PARTS; ++c) {
        if (sample) {
            e->mixer_peak_targets[c] = dm_audio_peak(e->audio, c);
        }

        e->mixer_peaks[c] = dm_visual_smooth(e->mixer_peaks[c], e->mixer_peak_targets[c],
                                             ui_delta_time(), .025f, .16f);
    }
}

/* The color of the pattern on the channel's arrangement row at the song position (or the row's
   first pattern before it starts), so the mixer label matches the row; the application color
   when the row has no patterns or they use the default color. */
static uint32_t channel_row_color(Editor *e, int channel) {
    double now = dm_audio_playing(e->audio) && !e->pattern_mode
                     ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                     : e->cursor_beat[0];
    const DmClip *current = NULL, *first = NULL;

    for (size_t i = 0; i < e->project.clip_count; i++) {
        const DmClip *clip = &e->project.clips[i];

        if (clip->channel != channel || clip->lane) {
            continue;
        }

        if (!first || clip->beat < first->beat) {
            first = clip;
        }

        if (clip->beat <= now && (!current || clip->beat >= current->beat)) {
            current = clip;
        }
    }

    current = current ? current : first;
    return current ? ed_pattern_color(e, current->pattern) : e->accent;
}

void ed_mixer(Editor *e, float width, float height) {
    mixer_peak_update(e, SDL_GetTicks());
    e->mixer_live =
        !e->pattern_mode && dm_audio_mix_state(e->audio, e->mixer_live_volume, e->mixer_live_pan);
    ui_child(DM_TEXT_MIXER, width, height, false);

    if (ui_panel_clicked()) {
        ed_focus_edit(e, FOCUS_OTHER);
    }

    ui_align_frame();
    ui_text(DM_TEXT_MIXER_TITLE);
    ui_separator();
    ui_compact_begin();
    ui_child(DM_TEXT_CHANNEL_STRIPS, 0, 0, true);
    ui_mixer_wheel();
    UiPoint start = ui_cursor();
    /* Measured as if the horizontal scrollbar were hidden, so the strips keep their size and
       position when it appears in the space below them. */
    float strip_height = fmaxf(180, ui_available().y + ui_scrollbar_reserved() - 16), x = start.x;

    for (int c = 0; c < 16; c++) {
        if (!e->project.channels[c].enabled) {
            continue;
        }

        bool used[DM_NOTE_PORTS];
        memcpy(used, ed_channel_ports(e, c), sizeof(used));
        DmPattern *editing = ed_pattern(e);

        if (editing && e->pattern_channel == c) {
            for (size_t n = 0; n < editing->count; n++) {
                used[editing->notes[n].port] = true;
            }
        }

        unsigned count = 0;

        for (unsigned port = 0; port < DM_NOTE_PORTS; port++) {
            count += used[port];
        }

        /* An empty channel still gets the strip of its default route. */
        if (!count) {
            used[0] = true;
            count = 1;
        }

        float group_width = count * 64.0f + 12;
        uint32_t row_color = channel_row_color(e, c);
        ui_id(c);
        ui_channel_color(row_color);
        ui_position(x, start.y);
        ui_mixer_child("channel group", group_width, strip_height, true);
        ui_rect((UiRect){x + 6, start.y, group_width - 12, 2}, row_color, 1);
        char text[32];
        snprintf(text, sizeof(text), "%02d%s", c + 1,
                 dm_channel_drums(&e->project, c) ? DM_TEXT_DR : "");

        if (ui_mixer_header(text, group_width - 12)) {
            ed_release_piano(e);
            e->selected_channel = c;
            e->channel_selected = true;
            ed_open_instrument(e, c, false);
        }

        if (ui_item_middle()) {
            ed_checkpoint(e);
            e->project.channels[c] = e->reset_channels[c];
            e->project.channels[c].enabled = true;
            ed_sample_clear_route(&e->project, -1, c);
            ed_commit(e);
        }

        ui_tip(dm_channel_drums(&e->project, c) ? DM_TEXT_GENERAL_MIDI_PERCUSSION
                                                : dm_program_name(e->project.channels[c].program));
        UiPoint row = ui_cursor();
        unsigned column = 0;

        for (int port = 0; port < DM_NOTE_PORTS; port++) {
            if (used[port]) {
                ui_id(port);
                ui_position(row.x + column * 64, row.y);
                ui_channel_color(ed_port_color(e, port));
                ui_mixer_child("port strip", 64, strip_height - 40, false);
                mixer_controls(e, c, port);
                ui_child_end();
                ui_channel_color_end();
                ui_pop_id();
                column++;
            }
        }

        ui_child_end();
        ui_channel_color_end();
        ui_pop_id();
        x += group_width + 6;
    }

    ui_position(x, start.y + 1);
    ui_space(1);
    ui_child_end();
    ui_compact_end();
    ui_child_end();
}

int ed_channels(Editor *e, int *list) {
    int n = 0;

    for (int c = 0; c < 16; ++c) {
        if (e->project.channels[c].enabled) {
            list[n++] = c;
        }
    }

    return n;
}

/* Ports used on each channel. Scanning every note is costly for large songs, so the result is
   kept until the project changes. */
const bool *ed_channel_ports(Editor *e, int channel) {
    if (!e->ports_cached || e->ports_revision != e->export_revision ||
        e->ports_session != e->project_session) {
        for (int c = 0; c < 16; ++c) {
            dm_channel_ports(&e->project, c, e->channel_ports[c]);
        }

        e->ports_revision = e->export_revision;
        e->ports_session = e->project_session;
        e->ports_cached = true;
    }

    return e->channel_ports[channel];
}
