#include "editor/internal.h"
#include "text/en.h"

static bool content_selected(Editor *e, EditFocus focus);
static void delete_pattern_selection(Editor *e);
static int transpose_delta(DmPattern *p, int steps);
static int copy_pattern_between(DmProject *to, const DmProject *from, int source);
static bool copy_notes(Editor *e, DmProject *to);
static bool copy_clips(Editor *e, DmProject *to);
static void paste_notes(Editor *e, DmProject *from, double at);
static void paste_clips(Editor *e, const DmProject *from, double at);
static void delete_selected_notes(DmPattern *p);
static void store_clipboard(Editor *e, DmBytes bytes);

bool ed_point_editing(Editor *e) {
    if (ed_velocity_editing(e)) {
        return false;
    }

    return !ed_pattern(e) || !ed_pattern(e)->kind || !ed_pattern(e)->mode;
}

static bool content_selected(Editor *e, EditFocus focus) {
    bool any = false;

    if (focus == FOCUS_NOTES && ed_pattern(e)) {
        for (size_t i = 0; i < ed_pattern(e)->count; ++i) {
            any |= ed_pattern(e)->notes[i].selected;
        }
    } else if (focus == FOCUS_PLAYLIST) {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            any |= e->project.clips[i].selected;
        }
    }

    return any;
}

void ed_remove_placement(Editor *e, size_t index) {
    memmove(e->project.clips + index, e->project.clips + index + 1,
            (e->project.clip_count - index - 1) * sizeof(DmClip));
    --e->project.clip_count;
    int *references[] = {&e->selected_clip, &e->playlist_context_clip, &e->drag_index};

    for (int i = 0; i < 3; ++i) {
        if (*references[i] == (int)index) {
            *references[i] = -1;
        } else if (*references[i] > (int)index) {
            --*references[i];
        }
    }
}

static void delete_pattern_selection(Editor *e) {
    if (!e->pattern_highlight || !ed_pattern(e)) {
        return;
    }

    ed_checkpoint(e);
    dm_pattern_remove(&e->project, e->selected_pattern);
    e->selected_pattern = -1;
    e->pattern_mode = false;
    ed_clear_pattern_selection(e);
    ed_commit(e);
    return;
}

void ed_delete_selection(Editor *e) {
    if (!e->active) {
        return;
    }

    EditFocus focus = ed_selection_focus(e);

    if (focus == FOCUS_OTHER) {
        return;
    }

    if (focus == FOCUS_EVENTS) {
        if (!ed_point_editing(e)) {
            return;
        }

        if (e->event_selection.size) {
            ed_delete_events(e);
        } else if (e->automation_all) {
            ed_automation_clear(e);
        } else {
            ed_remove_selected_node(e);
        }

        return;
    }

    if (focus == FOCUS_PATTERNS) {
        delete_pattern_selection(e);
        return;
    }

    bool any = content_selected(e, focus);

    if (!any) {
        return;
    }

    ed_checkpoint(e);

    if (focus == FOCUS_NOTES) {
        DmPattern *p = ed_pattern(e);

        if (!p) {
            return;
        }

        delete_selected_notes(p);
    } else {
        for (size_t i = e->project.clip_count; i > 0; --i) {
            if (e->project.clips[i - 1].selected) {
                ed_remove_placement(e, i - 1);
            }
        }
    }

    ed_commit(e);

    if (focus == FOCUS_PLAYLIST) {
        ed_clear_pattern_selection(e);
    }
}

void ed_select_all(Editor *e) {
    EditFocus focus = ed_selection_focus(e);

    /* The pattern section has no multiple selection, and the mixer has nothing to select. */
    if (focus == FOCUS_PATTERNS || focus == FOCUS_OTHER) {
        return;
    }

    if (ed_selection_focus(e) == FOCUS_EVENTS && ed_velocity_editing(e)) {
        for (size_t i = 0; i < ed_pattern(e)->count; ++i) {
            ed_pattern(e)->notes[i].selected = 1;
        }

        return;
    }

    if (!e->active) {
        return;
    }

    if (ed_selection_focus(e) == FOCUS_EVENTS) {
        if (!ed_point_editing(e)) {
            return;
        }

        e->automation_node = -1;
        e->event_selection.size = 0;
        e->automation_all = true;
        return;
    }

    e->automation_node = -1;

    if (e->context == 2 && ed_pattern(e)) {
        DmPattern *p = ed_pattern(e);

        for (size_t i = 0; i < p->count; ++i) {
            p->notes[i].selected = 1;
        }
    } else if (e->context == 1) {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            e->project.clips[i].selected = 1;
        }
    }
}

void ed_duplicate_clips(Editor *e) {
    size_t count = e->project.clip_count, selected = 0;
    double first = INFINITY, end = 0;

    for (size_t i = 0; i < count; ++i) {
        DmClip c = e->project.clips[i];

        if (!c.selected) {
            continue;
        }

        ++selected;
        first = fmin(first, c.beat);
        end = fmax(end, c.beat + dm_clip_length(&e->project, c));
    }

    if (!selected || !dm_reserve((void **)&e->project.clips, &e->project.clip_capacity,
                                 count + selected, sizeof(DmClip))) {
        return;
    }

    double span = fmax(1, ceil(end - 1e-9) - floor(first + 1e-9));
    ed_checkpoint(e);

    for (size_t i = 0; i < count; ++i) {
        DmClip c = e->project.clips[i];

        if (!c.selected) {
            continue;
        }

        e->project.clips[i].selected = 0;
        c.length = dm_clip_length(&e->project, c);
        c.beat += span;
        dm_clip_add(&e->project, c);
    }

    ed_commit(e);
}

void ed_duplicate_notes(Editor *e) {
    DmPattern *p = ed_pattern(e);

    if (!e->active || e->context != 2 || !p) {
        return;
    }

    size_t selected = 0, count = p->count;
    double first = HUGE_VAL, end = 0;

    for (size_t i = 0; i < count; ++i) {
        if (!p->notes[i].selected) {
            continue;
        }

        ++selected;
        first = fmin(first, p->notes[i].beat);
        end = fmax(end, p->notes[i].beat + p->notes[i].length);
    }

    if (!selected) {
        return;
    }

    double span = fmax(1, ceil(end - 1e-9) - floor(first + 1e-9));
    double bar = dm_meter(&e->project, first).bar;

    if (span >= bar) {
        span = ceil(span / bar) * bar;
    }

    if (!dm_reserve((void **)&p->notes, &p->capacity, count + selected, sizeof(*p->notes))) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_DUPLICATED);
        return;
    }

    ed_checkpoint(e);

    for (size_t i = 0; i < count; ++i) {
        if (!p->notes[i].selected) {
            continue;
        }

        DmNote copy = p->notes[i];
        p->notes[i].selected = 0;
        copy.beat += span;
        ed_add_note(e, p, copy);
    }

    ed_commit(e);
}

bool ed_deselect_notes(DmPattern *p) {
    bool selected = false;

    if (p) {
        for (size_t i = 0; i < p->count; ++i) {
            selected |= p->notes[i].selected != 0;
            p->notes[i].selected = 0;
        }
    }

    return selected;
}

void ed_marquee_begin(Editor *e, EditFocus focus, UiMouse mouse) {
    const DmPattern *pattern = focus == FOCUS_PLAYLIST ? NULL : ed_pattern(e);
    size_t count = pattern ? pattern->count : e->project.clip_count;
    DmBytes *selection = &e->marquee_selection;

    if (count && !dm_reserve((void **)&selection->data, &selection->capacity, count, 1)) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        selection->data[i] = pattern ? pattern->notes[i].selected : e->project.clips[i].selected;
    }

    selection->size = count;
    ed_focus_edit(e, focus);
    e->marquee = true;
    e->marquee_start = mouse.pos;
}

bool ed_marquee_retains(Editor *e, size_t index, bool shift) {
    return shift && index < e->marquee_selection.size && e->marquee_selection.data[index];
}

void ed_deselect(Editor *e) {
    if (e->context == 2) {
        ed_deselect_notes(ed_pattern(e));
    } else {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            e->project.clips[i].selected = 0;
        }
    }
}

void ed_clear_pattern_selection(Editor *e) {
    e->pattern_highlight = false;
    e->linked_highlight = false;
    e->roll_auto_hidden = true;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        e->project.clips[i].selected = 0;
    }
}

bool ed_deselect_focused(Editor *e) {
    EditFocus focus = ed_selection_focus(e);

    if (focus == FOCUS_EVENTS || focus == FOCUS_NOTES) {
        bool selected = e->automation_all || e->automation_node >= 0 || e->event_selection.size;
        ed_deselect_events(e);

        return ed_deselect_notes(ed_pattern(e)) || selected;
    }

    bool selected = e->pattern_highlight;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        selected |= e->project.clips[i].selected != 0;
    }

    if (!selected) {
        return false;
    }

    ed_clear_pattern_selection(e);
    return true;
}

static int transpose_delta(DmPattern *p, int steps) {
    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].selected) {
            steps = (int)fmax(-p->notes[i].key, fmin(127 - p->notes[i].key, steps));
        }
    }

    return steps;
}

void ed_transpose(Editor *e, int steps) {
    ed_checkpoint(e);
    DmPattern *p = ed_pattern(e);

    if (e->context == 2 && p) {
        steps = transpose_delta(p, steps);
        DmNote *before = malloc((p->count + 1) * sizeof(*before));

        if (before) {
            memcpy(before, p->notes, p->count * sizeof(*before));
        }

        for (size_t i = 0; i < p->count; ++i) {
            if (p->notes[i].selected) {
                p->notes[i].key = (uint8_t)(p->notes[i].key + steps);
            }
        }

        if (before) {
            ed_follow_note_curves(e, e->selected_pattern, before, p->count, 0);
            free(before);
        }
    } else {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            if (e->project.clips[i].selected) {
                int c = e->project.clips[i].channel + (steps > 0 ? -1 : 1);

                while (c >= 0 && c < 16 && !e->project.channels[c].enabled) {
                    c += steps > 0 ? -1 : 1;
                }

                if (c >= 0 && c < 16) {
                    e->project.clips[i].channel = c;
                    int lane = e->project.clips[i].lane;

                    if (lane) {
                        e->project.lane_count[c] = (int)fmax(e->project.lane_count[c], lane);
                        e->channel_expanded[c] = true;
                    }
                }
            }
        }

        if (e->selected_clip >= 0 && (size_t)e->selected_clip < e->project.clip_count &&
            e->project.clips[e->selected_clip].selected) {
            ed_select_placement_route(e, e->selected_clip);
        }
    }

    ed_commit(e);
}

void ed_quantize_notes(Editor *e) {
    DmPattern *p = ed_pattern(e);
    DmNote *before = malloc((p->count + 1) * sizeof(*before));

    if (!before) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_NOTE);
        return;
    }

    if (p->count) {
        memcpy(before, p->notes, p->count * sizeof(*before));
    }

    ed_checkpoint(e);
    dm_quantize(p, .25);

    for (size_t i = 0; i < p->count; ++i) {
        DmNote note = before[i];
        double shift = p->notes[i].beat - note.beat;
        note.selected = 1;

        if (note.bend_id && ed_note_bend_offset(e, &note, 1, shift) > shift) {
            p->notes[i] = before[i];
        }
    }

    ed_follow_note_curves(e, e->selected_pattern, before, p->count, 0);
    free(before);
    dm_pattern_fit(&e->project, ed_pattern(e));
    ed_commit(e);
}

static int copy_pattern_between(DmProject *to, const DmProject *from, int source) {
    int id = dm_pattern_add(to, from->patterns[source].name);

    if (id < 0) {
        return -1;
    }

    const DmPattern *pat = &from->patterns[source];

    for (size_t i = 0; i < pat->count; ++i) {
        if (!dm_note_add(&to->patterns[id], pat->notes[i])) {
            return -1;
        }
    }

    DmPattern *target = &to->patterns[id];
    target->length = pat->length;
    target->kind = pat->kind;
    target->mode = pat->mode;
    memcpy(target->graph_lanes, pat->graph_lanes, sizeof(target->graph_lanes));
    target->event = pat->event;
    target->speed = pat->speed;
    target->tension = pat->tension;
    target->skew = pat->skew;
    target->pulse = pat->pulse;
    target->amount = pat->amount;
    target->center = pat->center;
    target->pitch_center = pat->pitch_center;
    target->reference = pat->reference;
    target->inverted = pat->inverted;
    target->control_scope = pat->control_scope;
    target->port = pat->port;
    target->color = pat->color;

    for (size_t i = 0; i < from->automation_count; ++i) {
        if (from->automation[i].pattern == source) {
            DmAutomation a = from->automation[i];
            a.pattern = id;

            if (!dm_automation_add(to, a)) {
                return -1;
            }
        }
    }

    return id;
}

/* The pattern section copies whole patterns, with their automation points, and pastes them as
   new patterns. */
static bool copy_pattern_card(Editor *e, DmProject *to) {
    if (!e->pattern_highlight || !ed_pattern(e)) {
        return false;
    }

    snprintf(to->name, sizeof(to->name), "%s", DM_TEXT_PATTERNS_TITLE);
    return copy_pattern_between(to, &e->project, e->selected_pattern) >= 0;
}

static bool pattern_name_taken(const DmProject *p, int id) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        if ((int)i != id && !strcmp(p->patterns[i].name, p->patterns[id].name)) {
            return true;
        }
    }

    return false;
}

static void paste_patterns(Editor *e, const DmProject *from) {
    int last = -1;

    for (size_t i = 0; i < from->pattern_count; ++i) {
        int id = copy_pattern_between(&e->project, from, (int)i);

        if (id < 0) {
            break;
        }

        DmPattern *pat = &e->project.patterns[id];
        snprintf(pat->base_name, sizeof(pat->base_name), "%s", from->patterns[i].base_name);

        for (uint32_t n = 2; pattern_name_taken(&e->project, id) && n < 100000; ++n) {
            char postfix[16];
            snprintf(postfix, sizeof(postfix), "-%u", n);
            snprintf(pat->name, sizeof(pat->name), "%.*s%s", (int)(63 - strlen(postfix)),
                     pat->base_name, postfix);
            pat->duplicate_index = n;
        }

        last = id;
    }

    if (last >= 0) {
        ed_open_pattern(e, last, true);
        ed_focus_edit(e, FOCUS_PATTERNS);
    }
}

static bool copy_notes(Editor *e, DmProject *to) {
    DmPattern *p = ed_pattern(e);

    if (!p) {
        return false;
    }

    snprintf(to->name, sizeof(to->name), DM_TEXT_NOTES);
    int id = dm_pattern_add(to, DM_TEXT_CLIPBOARD);

    if (id < 0) {
        return false;
    }

    double first = INFINITY;

    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].selected) {
            first = fmin(first, p->notes[i].beat);
        }
    }

    if (!isfinite(first)) {
        return false;
    }

    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].selected) {
            DmNote n = p->notes[i];
            n.beat -= first;

            if (!dm_note_add(&to->patterns[id], n)) {
                return false;
            }
        }
    }

    return true;
}

static bool copy_clips(Editor *e, DmProject *to) {
    memcpy(to->lane_count, e->project.lane_count, sizeof(to->lane_count));
    memcpy(to->lane_names, e->project.lane_names, sizeof(to->lane_names));
    snprintf(to->name, sizeof(to->name), DM_TEXT_CLIPS);
    double first = INFINITY;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].selected) {
            first = fmin(first, e->project.clips[i].beat);
        }
    }

    if (!isfinite(first)) {
        return false;
    }

    int *map = malloc(e->project.pattern_count * sizeof(int));

    if (!map) {
        return false;
    }

    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        map[i] = -1;
    }

    bool ok = true;

    for (size_t i = 0; i < e->project.clip_count && ok; ++i) {
        DmClip clip = e->project.clips[i];

        if (!clip.selected) {
            continue;
        }

        if (map[clip.pattern] < 0) {
            map[clip.pattern] = copy_pattern_between(to, &e->project, clip.pattern);
        }

        uint32_t uid = e->project.patterns[clip.pattern].uid;
        clip.length = dm_clip_length(&e->project, clip);
        clip.pattern = map[clip.pattern];

        if (clip.pattern < 0) {
            ok = false;
            break;
        }

        to->patterns[clip.pattern].uid = uid;

        if (uid > to->next_pattern_uid) {
            to->next_pattern_uid = uid;
        }

        clip.beat -= first;
        ok = dm_clip_add(to, clip);
    }

    free(map);
    return ok;
}

static void paste_notes(Editor *e, DmProject *from, double at) {
    DmPattern *p = ed_pattern(e);

    if (!p) {
        return;
    }

    uint32_t next_bend_id = ed_next_bend_id(e);

    /* Use IDs above both projects' IDs so rewritten roots cannot match a later source note. */
    for (size_t i = 0; i < from->patterns[0].count; ++i) {
        uint32_t id = from->patterns[0].notes[i].bend_id;

        if (next_bend_id && id >= next_bend_id) {
            next_bend_id = id == UINT32_MAX ? 0 : id + 1;
        }
    }

    for (size_t i = 0; i < from->patterns[0].count; ++i) {
        DmNote n = from->patterns[0].notes[i];
        uint32_t old_id = n.bend_id;
        n.beat += at;
        n.selected = 1;

        if (old_id && next_bend_id) {
            n.bend_id = next_bend_id++;
        } else {
            n.bend_id = 0;
        }

        bool added = ed_add_note(e, p, n);

        for (size_t j = 0; old_id && j < from->automation_count; ++j) {
            if (from->automation[j].bend_note == old_id && from->automation[j].port == n.port) {
                from->automation[j].bend_note = added ? n.bend_id : 0;
            }
        }
    }
}

static void paste_clips(Editor *e, const DmProject *from, double at) {
    int *map = malloc(from->pattern_count * sizeof(int));

    if (!map) {
        return;
    }

    bool linked = e->clipboard_session == e->project_session &&
                  e->clipboard_port == e->project.port &&
                  e->clipboard_sequence == e->project.sequence;

    for (size_t i = 0; i < from->pattern_count; ++i) {
        map[i] = -1;

        if (linked) {
            for (size_t j = 0; j < e->project.pattern_count; ++j) {
                if (e->project.patterns[j].uid == from->patterns[i].uid) {
                    map[i] = (int)j;
                    break;
                }
            }
        }

        if (map[i] < 0) {
            map[i] = copy_pattern_between(&e->project, from, (int)i);
        }

        if (map[i] < 0) {
            free(map);
            return;
        }
    }

    for (size_t i = 0; i < from->clip_count; ++i) {
        DmClip c = from->clips[i];
        c.pattern = map[c.pattern];
        c.beat += at;
        c.selected = 1;

        if (c.lane > e->project.lane_count[c.channel]) {
            e->project.lane_count[c.channel] = c.lane;
        }

        if (c.lane) {
            e->channel_expanded[c.channel] = true;
        }

        dm_clip_add(&e->project, c);
    }

    free(map);
}

void ed_paste(Editor *e) {
    EditFocus focus = ed_selection_focus(e);

    if ((focus == FOCUS_EVENTS && !ed_point_editing(e)) || focus == FOCUS_OTHER) {
        return;
    }

    if (!e->clipboard.size) {
        return;
    }

    DmProject from;
    dm_project_init(&from);

    if (dm_unpack(&from, e->clipboard.data, e->clipboard.size, e->status, sizeof(e->status))) {
        if (focus == FOCUS_PLAYLIST && !strcmp(from.name, DM_TEXT_CLIPS)) {
            for (size_t i = 0; i < from.clip_count; ++i) {
                if (!e->project.channels[from.clips[i].channel].enabled) {
                    ed_message(e, DM_TEXT_CREATE_CHANNELS_BEFORE_PASTING);
                    dm_project_free(&from);
                    return;
                }
            }
        }

        bool pushed = ed_checkpoint(e);
        ed_deselect(e);
        double at = e->cursor_beat[e->context == 2];

        if (focus == FOCUS_PATTERNS) {
            if (!strcmp(from.name, DM_TEXT_PATTERNS_TITLE)) {
                paste_patterns(e, &from);
            }
        } else if (focus == FOCUS_EVENTS && ed_point_editing(e) &&
                   !strcmp(from.name, DM_TEXT_EVENTS)) {
            ed_paste_events(e, &from, at);
        } else if (e->context == 2 && !strcmp(from.name, DM_TEXT_NOTES)) {
            paste_notes(e, &from, at);

            if (from.automation_count && !ed_paste_note_bends(e, &from, at)) {
                ed_checkpoint_abandon(e, pushed);
                dm_project_free(&from);
                return;
            }
        } else if (e->context == 1 && !strcmp(from.name, DM_TEXT_CLIPS)) {
            paste_clips(e, &from, at);
        }

        ed_commit(e);
    }

    dm_project_free(&from);
}

void ed_clipboard(Editor *e, int mode) {
    if (!e->active) {
        return;
    }

    if (mode == 2) {
        ed_paste(e);
        return;
    }

    EditFocus focus = ed_selection_focus(e);

    if (focus == FOCUS_OTHER) {
        return; /* The mixer and other panels have nothing to copy. */
    }

    DmProject copy;
    dm_project_init(&copy);
    bool ok = focus == FOCUS_EVENTS     ? ed_point_editing(e) && ed_copy_events(e, &copy)
              : focus == FOCUS_PATTERNS ? copy_pattern_card(e, &copy)
              : e->context == 2         ? copy_notes(e, &copy)
                                        : copy_clips(e, &copy);

    if (ok) {
        DmBytes bytes = {0};

        if (dm_pack(&copy, &bytes)) {
            store_clipboard(e, bytes);

            if (mode == 1) {
                ed_delete_selection(e);
            }
        }
    }

    dm_project_free(&copy);
}

static void delete_selected_notes(DmPattern *p) {
    size_t kept = 0;

    for (size_t i = 0; i < p->count; ++i) {
        if (!p->notes[i].selected) {
            p->notes[kept++] = p->notes[i];
        }
    }

    p->count = kept;
}

static void store_clipboard(Editor *e, DmBytes bytes) {
    dm_bytes_free(&e->clipboard);
    e->clipboard = bytes;
    e->clipboard_session = e->project_session;
    e->clipboard_port = e->project.port;
    e->clipboard_sequence = e->project.sequence;
}

void ed_clipboard_note_bends(Editor *e, bool cut) {
    if (!e->active || ed_selection_focus(e) != FOCUS_NOTES || !ed_pattern(e) ||
        ed_pattern(e)->kind) {
        return;
    }

    DmProject copy;
    dm_project_init(&copy);
    bool *selected = calloc(e->project.automation_count + 1, sizeof(*selected));
    double first = HUGE_VAL;
    DmPattern *p = ed_pattern(e);

    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].selected) {
            first = fmin(first, p->notes[i].beat);
        }
    }

    DmBytes bytes = {0};

    if (!selected || !copy_notes(e, &copy) || !ed_copy_note_bends(e, &copy, first, selected) ||
        !dm_pack(&copy, &bytes)) {
        free(selected);
        dm_bytes_free(&bytes);
        dm_project_free(&copy);
        return;
    }

    store_clipboard(e, bytes);

    if (cut && ed_checkpoint(e)) {
        ed_remove_events(e, selected, -2);
        delete_selected_notes(p);
        ed_deselect_events(e);
        ed_commit(e);
    }

    free(selected);
    dm_project_free(&copy);
}
