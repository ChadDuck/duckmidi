#include "editor/export_internal.h"
#include "text/en.h"

static const char *choice_label(const DmeField *field, int value) {
    for (uint32_t i = 0; i < field->choice_count; i++) {
        if (field->choices[i].value == value) {
            return field->choices[i].label;
        }
    }

    return DM_TEXT_UNAVAILABLE_CHOICE;
}

static bool same_string(DmeString a, DmeString b) {
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, (size_t)a.length));
}

static void custom_mapping(Editor *e, DmExportFormat *format, const DmeMappingRow *row) {
    const DmeCustomMapping *mapping = row->custom;

    if (!mapping || !format->api->set_custom) {
        return;
    }

    ui_disabled(!mapping->available);
    DmeCustomSelection selected = mapping->selection;
    bool enabled = selected.enabled != 0;
    bool changed = ui_check(DM_TEXT_EXPORT_CUSTOM, &enabled);
    selected.enabled = enabled;
    ui_disabled_end();

    if (!mapping->available) {
        ui_wrap(mapping->unavailable_reason.data);
        return;
    }

    if (enabled) {
        bool show_all = selected.show_all != 0;
        changed |= ui_check(DM_TEXT_EXPORT_SHOW_ALL, &show_all);
        selected.show_all = show_all;
        const DmeSampleCatalog *catalog = mapping->catalog;
        const DmeSampleCatalogSet *set = NULL;
        const DmeSampleCatalogPreset *preset = NULL;

        for (uint32_t i = 0; catalog && i < catalog->set_count; i++) {
            if (same_string(catalog->sets[i].id, selected.sample.set_id)) {
                set = &catalog->sets[i];
            }
        }

        ui_text(DM_TEXT_EXPORT_SAMPLE_SET);
        ui_width(ui_available().x);

        if (ui_combo("##sample-set", set ? set->name.data : DM_TEXT_EXPORT_SAMPLE_NO_SET)) {
            for (uint32_t i = 0; catalog && i < catalog->set_count; i++) {
                const DmeSampleCatalogSet *item = &catalog->sets[i];

                if (ui_select(item->name.data, set == item, 0, 0)) {
                    set = item;
                    selected.sample = (DmeSampleSelection){.size = sizeof(selected.sample),
                                                           .generation = item->generation,
                                                           .locator = item->locator,
                                                           .set_id = item->id};
                    selected.region_set = selected.replace_zone = 0;
                    changed = true;
                }
            }

            ui_combo_end();
        }

        if (set && !set->available) {
            ui_wrap(set->error.data);
        }

        for (uint32_t i = 0; set && i < set->preset_count; i++) {
            const DmeSampleCatalogPreset *item = &catalog->presets[set->first_preset + i];

            if (same_string(item->id, selected.sample.preset_id)) {
                preset = item;
            }
        }

        ui_text(DM_TEXT_EXPORT_SAMPLE_PRESET);
        float trailing = ui_frame_height() + 2 * ui_item_spacing_y();
        if (selected.operation == DME_SAMPLE_REPLACE) {
            trailing += ui_text_height(DM_TEXT_EXPORT_SAMPLE_TARGET, 0) + ui_frame_height() +
                        2 * ui_item_spacing_y();
            if (preset && preset->zone_count > 1) {
                trailing += ui_text_height(DM_TEXT_EXPORT_SAMPLE_REGION, 0) + ui_frame_height() +
                            2 * ui_item_spacing_y();
            }
        }

        if (ui_child("custom-presets", 0, fmaxf(60, ui_available().y - trailing), true)) {
            for (uint32_t i = 0; set && set->available && i < set->preset_count; i++) {
                const DmeSampleCatalogPreset *item = &catalog->presets[set->first_preset + i];
                bool drums = (row->source.flags & DME_SAMPLE_PERCUSSION) != 0;
                bool matches = !drums && !(item->flags & DME_SAMPLE_PERCUSSION) &&
                               item->suggested_program == row->source.program + 1;

                for (uint32_t z = 0;
                     drums && (item->flags & DME_SAMPLE_PERCUSSION) && z < item->zone_count; z++) {
                    const DmeSampleZoneParams *zone = &catalog->zones[item->first_zone + z];
                    matches |= row->source.drum_key >= zone->key_lo &&
                               row->source.drum_key <= zone->key_hi;
                }

                if (!show_all && !matches) {
                    continue;
                }

                ui_id((int)i);

                if (ui_select(item->name.data, preset == item, 0, 0)) {
                    selected.sample.preset_id = item->id;
                    selected.sample.flags = item->flags & DME_SAMPLE_PERCUSSION;
                    selected.sample.generation = set->generation;
                    selected.region_set = selected.replace_zone = 0;
                    changed = true;
                }

                ui_pop_id();
            }
        }

        ui_child_end();

        ui_width(ui_available().x);
        if (ui_combo("##sample-operation", selected.operation == DME_SAMPLE_ADD
                                               ? DM_TEXT_EXPORT_SAMPLE_ADD
                                               : DM_TEXT_EXPORT_SAMPLE_REPLACE)) {
            if (ui_select(DM_TEXT_EXPORT_SAMPLE_ADD, selected.operation == DME_SAMPLE_ADD, 0, 0)) {
                selected.operation = DME_SAMPLE_ADD;
                changed = true;
            }

            if (ui_select(DM_TEXT_EXPORT_SAMPLE_REPLACE, selected.operation == DME_SAMPLE_REPLACE,
                          0, 0)) {
                selected.operation = DME_SAMPLE_REPLACE;
                changed = true;
            }

            ui_combo_end();
        }

        if (selected.operation == DME_SAMPLE_REPLACE) {
            if (preset && preset->zone_count > 1) {
                char chosen[80];
                snprintf(chosen, sizeof(chosen), DM_TEXT_EXPORT_SAMPLE_REGION_NUMBER,
                         selected.replace_zone + 1);
                ui_text(DM_TEXT_EXPORT_SAMPLE_REGION);
                ui_width(ui_available().x);

                if (ui_combo("##sample-region",
                             selected.region_set ? chosen : DM_TEXT_EXPORT_SAMPLE_CHOOSE_REGION)) {
                    for (uint32_t i = 0; i < preset->zone_count; i++) {
                        const DmeSampleZoneParams *zone = &catalog->zones[preset->first_zone + i];
                        char label[128];
                        snprintf(label, sizeof(label), DM_TEXT_EXPORT_SAMPLE_REGION_LABEL, i + 1,
                                 zone->key_lo, zone->key_hi, zone->velocity_lo, zone->velocity_hi);

                        if (ui_select(label, selected.region_set && selected.replace_zone == i, 0,
                                      0)) {
                            selected.region_set = 1;
                            selected.replace_zone = i;
                            changed = true;
                        }
                    }

                    ui_combo_end();
                }
            }

            const char *label = DM_TEXT_EXPORT_SAMPLE_NO_TARGET;

            for (uint32_t i = 0; i < mapping->target_count; i++) {
                if (same_string(mapping->targets[i].id, selected.physical_target)) {
                    label = mapping->targets[i].label.data;
                }
            }

            ui_text(DM_TEXT_EXPORT_SAMPLE_TARGET);
            ui_width(ui_available().x);

            if (ui_combo("##sample-target", label)) {
                for (uint32_t i = 0; i < mapping->target_count; i++) {
                    const DmeSampleTarget *item = &mapping->targets[i];

                    if (ui_select(item->label.data, same_string(item->id, selected.physical_target),
                                  0, 0)) {
                        selected.physical_target = item->id;
                        changed = true;
                    }

                    ui_tip(item->label.data);
                }

                ui_combo_end();
            }
        }
    }

    if (changed) {
        char error[512] = {0};

        if (format->api->set_custom(format->instance, row->id, &selected, error, sizeof(error)) !=
            DME_OK) {
            ed_export_error(e, *error ? error : DM_TEXT_INVALID_EXPORTER_SETTING);
        }
    }
}

static void mapping(Editor *e, DmExportFormat *format, const DmeField *field, unsigned index) {
    ui_text(field->label);

    if (field->help) {
        ui_wrap(field->help);
    }

    if (!field->row_count) {
        ui_dim(DM_TEXT_NO_USED_SOURCES_IN);
        return;
    }

    int *selected = &format->selected_rows[index];

    if (*selected < 0 || (uint32_t)*selected >= field->row_count) {
        *selected = 0;
    }

    float width = ui_available().x, left = width * .47f;

    /* An enabled Custom panel needs its rows: the default mapping (two lines and a separator),
       Custom, Show all, the set label and list, the preset label, three preset rows and the
       operation, plus Replace's target and region. Both panels take any extra height the user
       gives the window. */
    const DmeCustomMapping *custom = field->rows[*selected].custom;
    bool tall = custom && custom->available && custom->selection.enabled;
    float rows = tall ? 12 + (custom->selection.operation == DME_SAMPLE_REPLACE ? 4 : 0) : 0;
    float minimum = fmaxf(206, rows * (ui_frame_height() + ui_item_spacing_y()));
    float height = ui_flex(minimum, fmaxf(minimum, 22 * (ui_frame_height() + ui_item_spacing_y())));

    if (ui_child("sources", left, height, true)) {
        for (uint32_t i = 0; i < field->row_count; i++) {
            ui_id((int)i);

            if (ui_select(field->rows[i].label, *selected == (int)i, 0, 0)) {
                *selected = (int)i;
            }

            ui_pop_id();
        }
    }

    ui_child_end();
    ui_same();

    if (ui_child("mapping", width - left - 10, height, true)) {
        const DmeMappingRow *row = &field->rows[*selected];
        ui_dim(DM_TEXT_DEFAULT_MAPPING);
        ui_wrap(choice_label(field, row->default_value));

        /* Similar instruments: the plugin's closest choices to the default mapping that still
           reach the source's notes, as the export review recommends. The list stays the same
           while the user picks from it; the picked one is the label. Manual selection shows the
           same value, so it follows a pick here. */
        ui_separator();
        custom_mapping(e, format, row);
        if (!row->custom || !row->custom->selection.enabled) {
            ui_space(4);
            ui_text(DM_TEXT_SIMILAR_INSTRUMENTS);
            const char *similar =
                row->similar_count ? DM_TEXT_CHOOSE_SIMILAR : DM_TEXT_NO_SIMILAR_INSTRUMENT;

            for (uint32_t i = 0; i < row->similar_count; i++) {
                if (row->similar[i].value == row->value) {
                    similar = row->similar[i].label;
                }
            }

            ui_width(ui_available().x);
            ui_disabled(!row->similar_count);

            if (ui_combo("##similar-instrument", similar)) {
                for (uint32_t i = 0; i < row->similar_count; i++) {
                    if (ui_select(row->similar[i].label, row->similar[i].value == row->value, 0,
                                  0)) {
                        ed_export_set(e, (size_t)(e->export_type - 1), row->id,
                                      (DmeValue){DME_CHOICE, row->similar[i].value, NULL});
                    }
                }

                ui_combo_end();
            }

            ui_disabled_end();
            ui_text(DM_TEXT_MANUAL_SELECTION);
            ui_width(ui_available().x);

            if (ui_combo("##mapped-instrument", choice_label(field, row->value))) {
                for (uint32_t i = 0; i < field->choice_count; i++) {
                    if (ui_select(field->choices[i].label, row->value == field->choices[i].value, 0,
                                  0)) {
                        ed_export_set(e, (size_t)(e->export_type - 1), row->id,
                                      (DmeValue){DME_CHOICE, field->choices[i].value, NULL});
                    }
                }

                ui_combo_end();
            }

            ui_tip(choice_label(field, row->value));
        }
    }

    ui_child_end();
}

void ed_export_render_fields(Editor *e, DmExportFormat *format, const DmeUi *ui) {
    for (uint32_t i = 0; i < ui->count; i++) {
        const DmeField *f = &ui->fields[i];
        bool changed = false;
        DmeValue value = f->value;

        if (f->type == DME_ACTION) {
            continue;
        }

        char text[4096];
        ui_id((int)i);

        switch (f->type) {
        case DME_BOOL: {
            bool flag = f->value.integer != 0;
            changed = ui_check(f->label, &flag);
            value.integer = flag;
            break;
        }
        case DME_INT: {
            int number = f->value.integer;
            ui_width(120);
            changed = ui_int(f->label, &number, f->minimum, f->maximum);
            value.integer = number;
            break;
        }
        case DME_TEXT:
            snprintf(text, sizeof(text), "%s", f->value.text);
            ui_width(300);
            changed = ui_input(f->label, text, f->text_capacity);
            value.text = text;
            break;
        case DME_CHOICE:
            ui_width(300);

            if (ui_combo(f->label, choice_label(f, f->value.integer))) {
                for (uint32_t j = 0; j < f->choice_count; j++) {
                    if (ui_select(f->choices[j].label, f->choices[j].value == f->value.integer, 0,
                                  0)) {
                        value.integer = f->choices[j].value;
                        changed = true;
                    }
                }

                ui_combo_end();
            }
            break;
        case DME_MAPPING:
            mapping(e, format, f, i);
            break;
        case DME_NOTICE:
            ui_wrap(f->help ? f->help : f->label);
            break;
        case DME_ACTION:
            break; /* Actions share the footer row with Save. */
        }

        if (changed) {
            ed_export_set(e, (size_t)(e->export_type - 1), f->id, value);
        }

        if (f->type != DME_MAPPING && f->type != DME_NOTICE && f->help) {
            ui_tip(f->help);
        }

        ui_pop_id();
    }
}

bool ed_export_busy(Editor *e) {
    return e->export_plugins &&
           (e->export_plugins->work.thread || e->export_plugins->review_pending);
}

/* Repository installs: Keep original files sits above the action row. */
static void repository_settings(DmExportPlugins *h) {
    ui_check(DM_TEXT_KEEP_ORIGINAL_FILES, &h->keep_originals);
    ui_tip(DM_TEXT_KEEP_ORIGINAL_FILES_HELP);
}

bool ed_export_history_visible(Editor *e) {
    DmExportFormat *format = ed_export_selected(e);
    return e->export_plugins && e->export_plugins->history_open && format && format->history;
}

void ed_export_history_toggle(Editor *e) {
    DmExportFormat *format = ed_export_selected(e);
    DmExportPlugins *h = e->export_plugins;

    if (!format || !format->history || !h) {
        return;
    }

    ui_same();
    UiPoint at = ui_cursor();
    ui_position(at.x + fmaxf(0, ui_available().x - 30), at.y);

    if (ui_button(h->history_open ? "<##repository-history" : ">##repository-history", 30, 0)) {
        h->history_open = !h->history_open;
    }

    ui_tip(DM_TEXT_REPOSITORY_HISTORY);

    if (h->history_open) {
        ui_attach_space(h->history_width);
    }
}

void ed_export_history_panel(Editor *e, UiRect main) {
    if (!ed_export_history_visible(e)) {
        return;
    }

    DmExportPlugins *h = e->export_plugins;
    DmExportFormat *format = ed_export_selected(e);
    bool busy = ed_export_busy(e) || e->dialog_busy;
    DmeRepositoryView view = {0};
    bool valid = format->history_job &&
                 h->registry.history_runtime->describe(format->history_job, &view) == DME_OK;

    if (!busy && !format->history_job) {
        ed_export_repository_action(e, DME_REPOSITORY_INSPECT, format->history_scope, NULL);
        busy = ed_export_busy(e);
    }

    /* The section follows the exporter's repository folder; it is not chosen here. */
    const DmeRepositoryScope *selected = NULL;

    for (uint32_t i = 0; valid && e->repository_history && i < view.scope_count; i++) {
        if (view.scopes[i].id == format->history_scope) {
            selected = &view.scopes[i];
        }
    }

    /* A drawer of the Export window: the arrow in Export format shows and hides it. */
    bool open = ui_attached_drawer("##repository-history-drawer", main, h->history_width);

    if (open) {
        ui_disabled(busy);
        ui_text(DM_TEXT_REPOSITORY_HISTORY);
        ui_separator();

        if (selected) {
            char section[256];
            snprintf(section, sizeof(section), DM_TEXT_REPOSITORY_SECTION_FORMAT,
                     (int)selected->target_name.length, selected->target_name.data);
            ui_text(section);
        }

        if (ui_check(DM_TEXT_REPOSITORY_HISTORY_ENABLE, &e->repository_history)) {
            ed_export_history_reset(e);
            ed_preference_save(e);
        }

        ui_tip(DM_TEXT_REPOSITORY_HISTORY_ENABLE_HELP);

        if (valid && view.notice.length) {
            ui_wrap(view.notice.data);
        }

        ui_separator();

        if (!e->repository_history) {
            ui_wrap(DM_TEXT_REPOSITORY_HISTORY_OFF);
            ui_disabled_end();
        }
    }

    if (!e->repository_history) {
        ui_end();
        h->history_confirm = 0;
        return;
    }

    if (open) {
        /* Selection follows the logical key, so it survives reverts and new exports. */
        const DmeRepositoryRow *chosen = NULL;

        for (uint32_t i = 0; valid && i < view.row_count; i++) {
            chosen = !strcmp(h->history_key, view.rows[i].key.data) ? &view.rows[i] : chosen;
        }

        /* The list ends where the selected row's paths and the two actions begin, so the
           actions sit at the drawer's bottom edge. */
        float paths_height = 70, spacing = ui_item_spacing_y();
        float tail = spacing + 2 * ui_frame_height() + spacing;

        if (chosen) {
            tail += paths_height + spacing;
        }

        float height = fmaxf(90, ui_available().y - tail);

        if (ui_child("repository-current", 0, height, true)) {
            for (uint32_t i = 0; valid && i < view.row_count; i++) {
                const DmeRepositoryRow *row = &view.rows[i];
                ui_id((int)i);
                /* The label wraps beside the Revert button, which is centred on the row. */
                float button = 68, frame = ui_frame_height();
                float width = fmaxf(40, ui_available().x - button - ui_item_spacing_x());
                float rows = fmaxf(frame, ui_text_height(row->label.data, width));
                UiPoint at = ui_cursor();

                if (ui_select_wrapped("##row", row->label.data, row == chosen, width, rows)) {
                    snprintf(h->history_key, sizeof(h->history_key), "%s", row->key.data);
                }

                ui_same();
                ui_position(ui_cursor().x, at.y + (rows - frame) / 2);
                ui_disabled(!row->can_revert);

                if (ui_button(DM_TEXT_REPOSITORY_REVERT, button, 0)) {
                    ed_export_repository_action(e, DME_REPOSITORY_REVERT, view.selected_scope,
                                                row->key.data);
                }

                ui_disabled_end();
                ui_pop_id();
            }

            if (valid && !view.row_count) {
                ui_wrap(DM_TEXT_REPOSITORY_NO_CHANGES);
            }
        }

        ui_child_end();

        if (chosen) {
            if (ui_child("repository-paths", 0, paths_height, true)) {
                for (uint32_t i = 0; i < chosen->path_count; i++) {
                    ui_wrap(chosen->paths[i].data);
                }
            }

            ui_child_end();
        }

        ui_disabled(!selected || (selected->capabilities & DME_REPOSITORY_MISSING));

        if (ui_button(DM_TEXT_REPOSITORY_INITIALIZE, ui_available().x, 0)) {
            h->history_confirm = DME_REPOSITORY_INITIALIZE;
        }

        if (ui_button(DM_TEXT_REPOSITORY_REVERT_ALL, ui_available().x, 0)) {
            h->history_confirm = DME_REPOSITORY_REVERT_ALL;
        }

        ui_disabled_end();
        ui_disabled_end();
    }

    ui_end();
    bool confirm = h->history_confirm != 0;

    if (ui_modal(DM_TEXT_REPOSITORY_CONFIRM, &confirm, 460, 180)) {
        ui_wrap(h->history_confirm == DME_REPOSITORY_INITIALIZE
                    ? DM_TEXT_REPOSITORY_INITIALIZE_CONFIRM
                    : DM_TEXT_REPOSITORY_REVERT_CONFIRM);

        if (ui_button(DM_TEXT_CANCEL, 0, 0)) {
            confirm = false;
        }

        ui_same();
        ui_disabled(ed_export_busy(e) || e->dialog_busy);

        if (ui_button(DM_TEXT_APPLY, 0, 0)) {
            ed_export_repository_action(e, (uint32_t)h->history_confirm, format->history_scope,
                                        NULL);
            confirm = false;
        }

        ui_disabled_end();

        if (!confirm) {
            ui_close_popup();
        }

        ui_popup_end();
    }

    if (!confirm) {
        h->history_confirm = 0;
    }
}

void ed_export_controls(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);
    DmExportFormat *f = ed_export_selected(e);

    if (!h || !f) {
        ui_dim(DM_TEXT_SELECTED_EXPORTER_UNAVAILABLE);
        return;
    }

    bool ready = ed_export_context(e), busy = ed_export_busy(e);

    if (ready && !busy && f->history && !f->history_job &&
        *ed_export_repository(e, (size_t)(e->export_type - 1))) {
        ed_export_repository_action(e, DME_REPOSITORY_INSPECT, 0, NULL);
        busy = ed_export_busy(e);
    }
    DmeUi ui = {0};
    bool valid = false;

    if (ready) {
        valid = ed_export_settings(e, (size_t)(e->export_type - 1), &ui);
    } else if (busy && !h->work.inspection) {
        char error[256];
        valid = f->api->describe(f->instance, &ui) == DME_OK &&
                dm_export_ui_valid(&ui, error, sizeof(error));
    }

    ui_disabled(!ready || busy || e->dialog_busy);

    if (valid) {
        ed_export_render_fields(e, f, &ui);
    }

    if (f->history) {
        repository_settings(h);
    }

    /* Actions, Save and Export to Repository share one row of equal buttons. */
    unsigned buttons = f->history ? 2 : 1;

    for (uint32_t i = 0; valid && i < ui.count; i++) {
        buttons += ui.fields[i].type == DME_ACTION;
    }

    float button_width =
        fmaxf(1, (ui_available().x - (buttons - 1) * ui_item_spacing_x()) / buttons);

    for (uint32_t i = 0; valid && i < ui.count; i++) {
        if (ui.fields[i].type != DME_ACTION) {
            continue;
        }

        const DmeField *action = &ui.fields[i];
        ui_id((int)i);

        if (ui_button(action->label, button_width, 0)) {
            ed_export_set(e, (size_t)(e->export_type - 1), action->id,
                          (DmeValue){DME_ACTION, 0, NULL});
        }

        if (action->help) {
            ui_tip(action->help);
        }

        ui_pop_id();
        ui_same();
    }

    if (ui_button(f->api->save_label, button_width, 0)) {
        ed_dialog(e, DIALOG_EXPORT_PLUGIN);
    }

    /* Without a saved folder the button only chooses one; the next click exports. */
    if (f->history) {
        size_t index = (size_t)(e->export_type - 1);
        bool chosen = *ed_export_repository(e, index) != 0;
        ui_same();

        if (ui_button(chosen ? f->api->install_label : DM_TEXT_REPOSITORY_CHOOSE_FOLDER,
                      button_width, 0)) {
            if (chosen) {
                ed_export_to_repository(e);
            } else if (ed_export_folder_picker(e, index)) {
                ed_dialog(e, DIALOG_PLUGIN_FOLDER);
            }
        }

        ui_tip(chosen ? DM_TEXT_EXPORT_REPOSITORY_HELP : DM_TEXT_REPOSITORY_CHOOSE_FOLDER_HELP);
    }

    ui_disabled_end();

    if (h->work.thread) {
        ui_progress(SDL_GetAtomicInt(&h->work.progress) / 100.0f, 300, 0,
                    h->work.inspection                            ? DM_TEXT_READING_ARRANGEMENT
                    : h->work.history_runtime && !h->work.install ? DM_TEXT_REPOSITORY_WORKING
                                                                  : DM_TEXT_EXPORTING_STATUS);

        if (ui_button(DM_TEXT_CANCEL_EXPORT_TASK, 0, 0)) {
            SDL_SetAtomicInt(&h->work.cancel, 1);
        }
    }

    if (!busy && *h->message &&
        (h->message_format < 0 || h->message_format == e->export_type - 1)) {
        ui_separator();
        ui_wrap(h->message);
    }

    if (!busy && !ready && ui_button(DM_TEXT_RETRY_READING_ARRANGEMENT, 0, 0)) {
        h->context_valid = false;
        e->export_revision++;
    }
}

void ed_export_review_dialog(Editor *e) {
    DmExportPlugins *h = e->export_plugins;

    if (!h) {
        return;
    }

    bool open = h->review_pending;
    int decision = -1;

    if (ui_modal(DM_TEXT_REVIEW_EXPORT_MAPPINGS, &open, 900, 590)) {
        DmeReview review = {0};
        char error[512] = {0};

        if (h->work.api->review(h->work.job, &review) != DME_OK ||
            !dm_export_review_valid(&review, error, sizeof(error))) {
            ui_wrap(*error ? error : DM_TEXT_CANNOT_READ_MAPPING_REVIEW);

            if (ui_button(DM_TEXT_CANCEL_EXPORT, 0, 0)) {
                decision = 0;
            }
        } else if (!review.count) {
            if (ui_child("preflight-notices", 0, ui_available().y - 48, true)) {
                ui_wrap(h->work.summary);
            }

            ui_child_end();
            if (ui_button(DM_TEXT_CANCEL_EXPORT, 0, 0)) {
                decision = 0;
            }

            ui_same();
            if (ui_button(DM_TEXT_EXPORT_PREFLIGHT_CONTINUE, 0, 0)) {
                decision = 1;
            }
        } else if (review.count) {
            ui_wrap(review.description);
            if (*h->work.summary) {
                if (ui_child("preflight-notices", 0, 100, true)) {
                    ui_wrap(h->work.summary);
                }

                ui_child_end();
            }
            ui_space(8);

            if (h->review_selected < 0 || (uint32_t)h->review_selected >= review.count) {
                h->review_selected = 0;
            }

            float width = ui_available().x, height = fmaxf(100, ui_available().y - 48),
                  left = width * .40f;

            if (ui_child("affected-sources", left, height, true)) {
                for (uint32_t i = 0; i < review.count; i++) {
                    ui_id((int)i);

                    if (ui_select(review.rows[i].label, h->review_selected == (int)i, 0, 0)) {
                        h->review_selected = (int)i;
                    }

                    ui_pop_id();
                }
            }

            ui_child_end();
            ui_same();

            if (ui_child("substitute-mapping", width - left - 10, height, true)) {
                const DmeReviewRow *row = &review.rows[h->review_selected];
                ui_wrap(row->label);
                ui_separator();
                ui_dim(DM_TEXT_CURRENT_MAPPING);
                ui_wrap(row->current_mapping);
                ui_space(6);
                ui_wrap(row->description);
                ui_separator();
                ui_text(DM_TEXT_RECOMMENDED_SUBSTITUTES);

                for (uint32_t i = 0; i < row->recommended_count; i++) {
                    if (ui_select(row->recommended[i].label,
                                  row->value == row->recommended[i].value, 0, 0)) {
                        h->work.api->review_set(h->work.job, row->id, row->recommended[i].value,
                                                error, sizeof(error));
                    }
                }

                if (!row->recommended_count) {
                    ui_wrap(DM_TEXT_NO_STOCK_SUBSTITUTE_COVERS);
                }

                ui_space(8);
                ui_text(DM_TEXT_MANUAL_MAPPING);
                ui_width(ui_available().x);
                const char *label = DM_TEXT_CHOOSE_MAPPING;

                for (uint32_t i = 0; i < row->choice_count; i++) {
                    if (row->choices[i].value == row->value) {
                        label = row->choices[i].label;
                    }
                }

                if (ui_combo("##review-mapping", label)) {
                    for (uint32_t i = 0; i < row->choice_count; i++) {
                        if (ui_select(row->choices[i].label, row->value == row->choices[i].value, 0,
                                      0)) {
                            h->work.api->review_set(h->work.job, row->id, row->choices[i].value,
                                                    error, sizeof(error));
                        }
                    }

                    ui_combo_end();
                }

                if (!row->supported) {
                    ui_wrap(DM_TEXT_THIS_SELECTED_SUBSTITUTE_STILL);
                }

                if (*error) {
                    ui_wrap(error);
                }
            }

            ui_child_end();
            float button = (ui_available().x - 2 * ui_item_spacing_x()) / 3;

            if (ui_button(DM_TEXT_CANCEL_EXPORT, button, 0)) {
                decision = 0;
            }

            ui_same();

            if (ui_button(DM_TEXT_EXPORT_ANYWAYS, button, 0)) {
                decision = 1;
            }

            ui_same();

            if (ui_button(DM_TEXT_EXPORT_WITH_CHANGES, button, 0)) {
                decision = 2;
            }
        }

        if (decision >= 0) {
            ui_close_popup();
        }

        ui_popup_end();
    }

    if (!open && h->review_pending) {
        decision = 0;
    }

    if (decision >= 0) {
        ed_export_review_decide(e, decision);
    }
}

void ed_export_plugin_errors(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);

    if (h && *h->registry.errors) {
        ui_separator();
        ui_wrap(h->registry.errors);
    }
}

bool ed_export_warning_open(Editor *e) {
    return e->export_plugins && e->export_plugins->warning_open;
}

void ed_export_warning_close(Editor *e) {
    if (e->export_plugins) {
        e->export_plugins->warning_open = false;
    }
}

void ed_export_warning_dialog(Editor *e) {
    DmExportPlugins *h = e->export_plugins;

    if (!h) {
        return;
    }

    if (ui_modal(DM_TEXT_EXPORT_WARNINGS, &h->warning_open, 680, 480)) {
        if (ui_child("warning-details", 0, ui_available().y - 45, true)) {
            ui_wrap(h->warning);
        }

        ui_child_end();

        if (ui_button("OK", 100, 0)) {
            h->warning_open = false;
            ui_close_popup();
        }

        ui_popup_end();
    }
}
