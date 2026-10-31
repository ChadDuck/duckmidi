/* The Updates window: when to check, manual checks, download and install. Nothing reaches the
   network until the user applies "On Launch" or clicks Check for Update. */
#include "editor_internal.h"
#include "text/en.h"
#include "update.h"

#define UPDATE_ON_LAUNCH 1
#define UPDATE_NEVER 0

static DmUpdate *updater(Editor *e) {
    if (!e->update) {
        e->update = dm_update_create(e->base_path, DM_APP_VERSION);
    }

    return e->update;
}

static void start_check(Editor *e) {
    if (updater(e)) {
        dm_update_check(e->update);
    }
}

void ed_update_launch(Editor *e) {
    /* The first-launch question waits until a file or project is open. */
    if (!e->update_prompted) {
        e->update_prompt_pending = true;
        e->update_choice = 1; /* Never, which is also the saved setting until Apply. */
    } else if (e->update_mode == UPDATE_ON_LAUNCH) {
        start_check(e);
    }
}

void ed_update_open(Editor *e) {
    e->update_window = true;
    e->update_choice = e->update_mode == UPDATE_ON_LAUNCH ? 0 : 1;
}

/* Closing the window without applying keeps Never and does not ask again. */
static void close_window(Editor *e) {
    e->update_window = false;

    if (!e->update_prompted) {
        e->update_prompted = true;
        e->update_mode = UPDATE_NEVER;
        ed_preference_save(e);
    }
}

static void status_section(Editor *e, float width) {
    if (!e->update) {
        return;
    }

    DmUpdateStatus s = dm_update_status(e->update);
    char text[256];

    switch (s.state) {
    case DM_UPDATE_CHECKING:
        ui_wrap(DM_TEXT_UPDATE_CHECKING);
        break;
    case DM_UPDATE_CURRENT:
        snprintf(text, sizeof(text), DM_TEXT_UPDATE_CURRENT, DM_APP_VERSION);
        ui_wrap(text);
        break;
    case DM_UPDATE_AVAILABLE:
        snprintf(text, sizeof(text), DM_TEXT_UPDATE_AVAILABLE, s.version);
        ui_wrap(text);

        if (ui_button(DM_TEXT_UPDATE_DOWNLOAD, width, 0)) {
            dm_update_download(e->update);
        }
        break;
    case DM_UPDATE_DOWNLOADING:
        snprintf(text, sizeof(text), DM_TEXT_UPDATE_DOWNLOADING, s.version);
        ui_progress(s.progress, width, 0, text);
        break;
    case DM_UPDATE_READY:
        snprintf(text, sizeof(text), DM_TEXT_UPDATE_READY, s.version);
        ui_wrap(text);

        if (ui_button(DM_TEXT_UPDATE_RESTART, width, 0)) {
            e->update_install = true;
            e->update_restart = true;
            ed_request(e, PENDING_QUIT, NULL);
        }

        if (ui_button(e->update_install ? DM_TEXT_UPDATE_SCHEDULED : DM_TEXT_UPDATE_ON_CLOSE, width,
                      0)) {
            e->update_install = true;
        }
        break;
    case DM_UPDATE_FAILED:
        ui_wrap(s.message);
        break;
    case DM_UPDATE_IDLE:
        break;
    }
}

void ed_update_dialog(Editor *e) {
    if (e->update_prompt_pending && e->active) {
        e->update_prompt_pending = false;
        e->update_window = true;
    }

    /* An update found by the launch check opens the window once. */
    if (!e->update_window && e->update && !e->update_announced &&
        dm_update_status(e->update).state == DM_UPDATE_AVAILABLE) {
        e->update_announced = true;
        e->update_window = true;
        e->update_choice = e->update_mode == UPDATE_ON_LAUNCH ? 0 : 1;
    }

    if (!e->update_window) {
        return;
    }

    bool open = true;

    if (ui_fitted_dialog(DM_TEXT_UPDATES, &open, 420)) {
        float width = ui_available().x;
        ui_wrap(DM_TEXT_UPDATE_QUESTION);
        static const char *const choices[] = {DM_TEXT_UPDATE_ON_LAUNCH, DM_TEXT_UPDATE_NEVER};
        ui_width(width);

        if (ui_combo("##update-when", choices[e->update_choice])) {
            for (int i = 0; i < 2; ++i) {
                if (ui_select(choices[i], i == e->update_choice, 0, 0)) {
                    e->update_choice = i;
                }
            }

            ui_combo_end();
        }

        if (ui_button(DM_TEXT_APPLY, width, 0)) {
            e->update_mode = e->update_choice == 0 ? UPDATE_ON_LAUNCH : UPDATE_NEVER;
            e->update_prompted = true;
            ed_preference_save(e);
            e->update_window = false; /* A found update reopens the window. */

            if (e->update_mode == UPDATE_ON_LAUNCH) {
                start_check(e);
            }
        }

        if (ui_button(DM_TEXT_UPDATE_CHECK_NOW, width, 0)) {
            start_check(e);
        }

        if (e->update && dm_update_status(e->update).state != DM_UPDATE_IDLE) {
            ui_separator();
        }

        status_section(e, width);
    }

    ui_end();

    if (!open) {
        close_window(e);
    }
}

/* Starts a scheduled install as duckmidi exits. */
void ed_update_finish(Editor *e) {
    if (!e->update) {
        return;
    }

    if (e->update_install && dm_update_status(e->update).state == DM_UPDATE_READY) {
        uint64_t sample = 0;
        char error[256];

        if (dm_update_install(e->update, e->update_restart, e->sample_hash, &sample, error,
                              sizeof(error)) &&
            sample) {
            e->sample_hash = sample;
            ed_preference_save(e);
        }
    }

    dm_update_destroy(e->update);
    e->update = NULL;
}
