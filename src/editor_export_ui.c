#include "editor_export_internal.h"
#include "text/en.h"

static const char *choice_label(const DmeField *field, int value) {
    for (uint32_t i = 0; i < field->choice_count; i++) {
        if (field->choices[i].value == value) {
            return field->choices[i].label;
        }
    }

    return DM_TEXT_UNAVAILABLE_CHOICE;
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

    if (ui_child("sources", left, 180, true)) {
        for (uint32_t i = 0; i < field->row_count; i++) {
            if (ui_select(field->rows[i].label, *selected == (int)i, 0, 0)) {
                *selected = (int)i;
            }
        }
    }

    ui_child_end();
    ui_same();

    if (ui_child("mapping", width - left - 10, 180, true)) {
        const DmeMappingRow *row = &field->rows[*selected];
        ui_wrap(row->label);
        ui_separator();
        ui_dim(DM_TEXT_DEFAULT_MAPPING);
        ui_wrap(choice_label(field, row->default_value));
        ui_space(6);
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
            ui_width(280);
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

/* Repository installs: after a folder is chosen, the window shows it and the Keep original files
   choice above the action row. */
static void repository_settings(DmExportPlugins *h) {
    if (!*h->repository) {
        return;
    }

    ui_dim(h->repository);
    ui_check(DM_TEXT_KEEP_ORIGINAL_FILES, &h->keep_originals);
    ui_same();
    ui_dim(DM_TEXT_KEEP_ORIGINAL_FILES_HELP);
}

void ed_export_controls(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);
    DmExportFormat *f = ed_export_selected(e);

    if (!h || !f) {
        ui_dim(DM_TEXT_SELECTED_EXPORTER_UNAVAILABLE);
        return;
    }

    bool ready = ed_export_context(e), busy = ed_export_busy(e);
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

    if (f->api->install) {
        repository_settings(h);
    }

    /* Actions, Save and Export to Repository share one row of equal buttons. */
    unsigned buttons = f->api->install ? 2 : 1;

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

    if (f->api->install) {
        ui_same();

        if (ui_button(f->api->install_label, button_width, 0)) {
            ed_dialog(e, DIALOG_REPOSITORY);
        }

        ui_tip(DM_TEXT_EXPORT_REPOSITORY_HELP);
    }

    /* Revert sits between the action row and the result below it. */
    if (f->api->install && ed_export_has_changes(e)) {
        if (ui_button(DM_TEXT_REVERT_REPOSITORY_EXPORT, ui_available().x, 0)) {
            ed_export_revert(e);
        }

        ui_tip(DM_TEXT_REVERT_REPOSITORY_EXPORT_HELP);
    }

    ui_disabled_end();

    if (h->work.thread) {
        ui_progress(SDL_GetAtomicInt(&h->work.progress) / 100.0f, 300, 0,
                    h->work.inspection ? DM_TEXT_READING_ARRANGEMENT : "Exporting");

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
        } else if (review.count) {
            ui_wrap(review.description);
            ui_space(8);

            if (h->review_selected < 0 || (uint32_t)h->review_selected >= review.count) {
                h->review_selected = 0;
            }

            float width = ui_available().x, height = fmaxf(100, ui_available().y - 48),
                  left = width * .40f;

            if (ui_child("affected-sources", left, height, true)) {
                for (uint32_t i = 0; i < review.count; i++) {
                    if (ui_select(review.rows[i].label, h->review_selected == (int)i, 0, 0)) {
                        h->review_selected = (int)i;
                    }
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
