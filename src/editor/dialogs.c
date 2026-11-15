#include "editor/internal.h"
#include "devtools/devtools.h"
#include "text/en.h"

static void rename_dialog(Editor *e);
static void lane_dialog(Editor *e);

static void rename_dialog(Editor *e) {
    if (e->rename_pattern < 0 || (size_t)e->rename_pattern >= e->project.pattern_count) {
        return;
    }

    bool open = true;

    if (ui_dialog(DM_TEXT_RENAME_PATTERN, &open, (UiDialog){.width = 380, .escape_closes = true})) {
        if (ui_appearing()) {
            ui_focus_next();
        }

        ui_input(DM_TEXT_NAME, e->rename_text, sizeof(e->rename_text));

        if (ui_button(DM_TEXT_RENAME, ui_available().x, 0) || ui_dialog_enter()) {
            if (ed_checkpoint(e)) {
                dm_pattern_name(&e->project, e->rename_pattern, e->rename_text);
                ed_changed(e);
                open = false;
            }
        }
    }

    ui_dialog_end();

    if (!open) {
        e->rename_pattern = -1;
    }
}

static void lane_dialog(Editor *e) {
    if (!e->lane_settings) {
        return;
    }

    int c = e->lane_channel, lane = e->lane_index;

    /* Fields span the window; Add subrow and Apply name share a row, and the destructive
       Remove subrow takes its own full-width row, as in the Instrument window. */
    if (ui_dialog(DM_TEXT_AUTOMATION_LANE, &e->lane_settings,
                  (UiDialog){.width = 420, .escape_closes = true})) {
        float half = (ui_available().x - ui_item_spacing_x()) * .5f;

        if (lane) {
            if (ui_appearing()) {
                ui_focus_next();
            }

            ui_width(ui_available().x - ui_text_width(DM_TEXT_NAME) - ui_item_spacing_x());
            ui_input(DM_TEXT_NAME, e->rename_text, sizeof(e->rename_text));
        }

        ui_disabled(e->project.lane_count[c] >= DM_LANES);

        if (ui_button(DM_TEXT_ADD_SUBROW, lane ? half : ui_available().x, 0)) {
            if (ed_checkpoint(e)) {
                ++e->project.lane_count[c];
                e->channel_expanded[c] = true;
                ed_changed(e);
                e->lane_settings = false;
            }
        }

        ui_disabled_end();

        if (lane) {
            ui_same();

            if (ui_button(DM_TEXT_APPLY_NAME, half, 0) || ui_dialog_enter()) {
                /* The position-based default name is stored as no name, so it keeps
                   following the subrow's position. */
                char automatic[64];
                snprintf(automatic, sizeof(automatic), "%s%d", DM_TEXT_AUTOMATION_NUMBER_PREFIX,
                         lane);
                bool numbered = !strcmp(e->rename_text, automatic);
                if (ed_checkpoint(e)) {
                    snprintf(e->project.lane_names[c][lane - 1], 64, "%s",
                             numbered ? "" : e->rename_text);
                    ed_changed(e);
                    e->lane_settings = false;
                }
            }
        }

        if (lane && ui_button(DM_TEXT_REMOVE_SUBROW, ui_available().x, 0)) {
            if (ed_checkpoint(e)) {
                for (size_t i = e->project.clip_count; i > 0; --i) {
                    DmClip clip = e->project.clips[i - 1];

                    if (clip.channel == c && clip.lane == lane) {
                        ed_remove_placement(e, i - 1);
                    }
                }

                for (size_t i = 0; i < e->project.clip_count; ++i) {
                    DmClip *clip = &e->project.clips[i];

                    if (clip->channel == c && clip->lane > lane) {
                        --clip->lane;
                    }
                }

                memmove(e->project.lane_names[c][lane - 1], e->project.lane_names[c][lane],
                        (e->project.lane_count[c] - lane) * 64);
                --e->project.lane_count[c];

                if (!e->project.lane_count[c]) {
                    e->channel_expanded[c] = false;
                }

                e->project.lane_names[c][e->project.lane_count[c]][0] = 0;
                e->lane_settings = false;
                ed_commit(e);
            }
        }
    }

    ui_dialog_end();
}

static void audio_export_controls(Editor *e) {
    DmAudioOptions *o = &e->audio_options;
    bool rendering = dm_audio_exporting(e->audio);
    ui_disabled(dm_audio_busy(e->audio) || e->dialog_busy);
    ui_width(240);

    if (ui_combo(DM_TEXT_CODEC, dm_audio_codec_name(o->codec))) {
        for (int codec = 0; codec < DM_AUDIO_CODECS; codec++) {
            if (ui_select(dm_audio_codec_name(codec), o->codec == codec, 0, 0)) {
                *o = dm_audio_options_default();
                o->codec = (DmAudioCodec)codec;
                o->variable = codec == DM_AUDIO_VORBIS || codec == DM_AUDIO_OPUS;
            }
        }

        ui_combo_end();
    }

    if (o->codec == DM_AUDIO_WAV || o->codec == DM_AUDIO_FLAC) {
        ui_width(160);

        if (ui_combo(DM_TEXT_BIT_DEPTH, o->bits == 24 ? DM_TEXT_BITS_24 : DM_TEXT_BITS_16)) {
            if (ui_select(DM_TEXT_BITS_16, o->bits == 16, 0, 0)) {
                o->bits = 16;
            }

            if (ui_select(DM_TEXT_BITS_24, o->bits == 24, 0, 0)) {
                o->bits = 24;
            }

            ui_combo_end();
        }

        if (o->codec == DM_AUDIO_FLAC) {
            ui_int(DM_TEXT_COMPRESSION_LEVEL, &o->compression, 0, 12);
        }
    } else {
        ui_width(240);

        if (ui_combo(DM_TEXT_RATE_CONTROL,
                     o->variable ? DM_TEXT_VARIABLE_BITRATE : DM_TEXT_CONSTANT_BITRATE)) {
            if (ui_select(DM_TEXT_CONSTANT_BITRATE, !o->variable, 0, 0)) {
                o->variable = false;
            }

            if (ui_select(DM_TEXT_VARIABLE_BITRATE, o->variable, 0, 0)) {
                o->variable = true;
            }

            ui_combo_end();
        }

        if (o->variable && o->codec != DM_AUDIO_OPUS) {
            int max = o->codec == DM_AUDIO_MP3 ? 9 : o->codec == DM_AUDIO_AAC ? 5 : 10;
            ui_int(o->codec == DM_AUDIO_MP3 ? DM_TEXT_QUALITY_0_BEST
                                            : DM_TEXT_QUALITY_HIGHER_BETTER,
                   &o->quality, o->codec == DM_AUDIO_AAC ? 1 : 0, max);
        } else {
            static const int rates[] = {32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
            char label[32];
            snprintf(label, sizeof(label), "%d kbps", o->bitrate);
            ui_width(160);

            if (ui_combo(DM_TEXT_BITRATE, label)) {
                for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
                    snprintf(label, sizeof(label), "%d kbps", rates[i]);

                    if (ui_select(label, o->bitrate == rates[i], 0, 0)) {
                        o->bitrate = rates[i];
                    }
                }

                ui_combo_end();
            }
        }
    }

    ui_text("48 kHz stereo, with a two-second release tail.");

    if (ui_button(DM_TEXT_EXPORT, ui_available().x, 0)) {
        ed_dialog(e, DIALOG_WAVE);
    }

    ui_disabled_end();
    ui_disabled(rendering || !*e->audio_exported);

    if (ui_button(DM_TEXT_OPEN_FILE_LOCATION, ui_available().x, 0)) {
        ed_open_audio_location(e);
    }

    ui_disabled_end();

    if (rendering) {
        float gap = fmaxf(0, ui_available().y - 84);
        ui_space(gap);
        ui_separator();
        ui_progress(dm_audio_export_progress(e->audio) / 100.0f, ui_available().x, 0,
                    DM_TEXT_RENDERING_AUDIO);

        if (ui_button(DM_TEXT_CANCEL_AUDIO_EXPORT, ui_available().x, 0)) {
            dm_audio_cancel(e->audio);
        }
    } else if (*e->audio_export_message) {
        ui_wrap(e->audio_export_message);
    }
}

void ed_export_dialog(Editor *e) {
    if (e->export_window && !e->export_shown) {
        ed_export_reset_results(e);
    }

    e->export_shown = e->export_window;

    if (e->export_window) {
        UiRect export_rect = {0};
        bool visible = ui_dialog(DM_TEXT_EXPORT, &e->export_window,
                                 (UiDialog){.width = 760,
                                            .height = 400,
                                            .size = UI_FLEXIBLE,
                                            .drawer = ed_export_history_visible(e),
                                            .layout = e->export_type});
        if (visible) {
            ui_text(DM_TEXT_EXPORT_COMPOSITION);
            ui_separator();
            size_t count = ed_export_count(e);

            if (e->export_type < -1 || e->export_type > (int)count) {
                e->export_type = 0;
            }

            ui_width(220);
            ui_disabled(ed_export_busy(e) || e->dialog_busy || dm_audio_exporting(e->audio));

            if (ui_combo(DM_TEXT_EXPORT_FORMAT,
                         e->export_type < 0 ? DM_TEXT_DIGITAL_AUDIO
                         : e->export_type   ? ed_export_name(e, (size_t)(e->export_type - 1))
                                            : "MIDI")) {
                if (ui_select("MIDI", e->export_type == 0, 0, 0)) {
                    e->export_type = 0;
                }

                if (ui_select(DM_TEXT_DIGITAL_AUDIO, e->export_type == -1, 0, 0)) {
                    e->export_type = -1;
                }

                for (size_t i = 0; i < count; i++) {
                    if (ui_select(ed_export_name(e, i), e->export_type == (int)i + 1, 0, 0)) {
                        e->export_type = (int)i + 1;
                    }
                }

                ui_combo_end();
            }

            ui_disabled_end();
            ed_export_history_toggle(e);
            ui_separator();

            if (e->export_type == -1) {
                audio_export_controls(e);
            } else if (e->export_type > 0) {
                ed_export_controls(e);
            } else {
                const char *metadata_labels[] = {DM_TEXT_KEEP_EDITOR_METADATA,
                                                 DM_TEXT_KEEP_DUCKMIDI_EDITOR,
                                                 DM_TEXT_STRIP_EDITOR_METADATA};
                ui_width(360);

                if (ui_combo(DM_TEXT_EDITOR_METADATA, metadata_labels[e->export_metadata])) {
                    for (int i = 0; i < 3; ++i) {
                        if (ui_select(metadata_labels[i], e->export_metadata == (DmMetadataMode)i,
                                      0, 0)) {
                            e->export_metadata = (DmMetadataMode)i;
                        }
                    }

                    ui_combo_end();
                }

                const char *metadata_help[] = {DM_TEXT_INCLUDES_DUCKMIDI_OTHER,
                                               DM_TEXT_REMOVES_OTHER_EDITORS,
                                               DM_TEXT_REMOVES_DUCKMIDI_OTHER};
                ui_dim(metadata_help[e->export_metadata]);
                ui_dim(DM_TEXT_KEEPS_TEMPO_LYRICS);
                const char *export_labels[] = {DM_TEXT_EXPORT_MIDI};
                float export_width = ui_button_row_width(export_labels, 1);

                if (ui_button(export_labels[0], export_width, 0)) {
                    ed_dialog(e, DIALOG_MIDI);
                    e->export_window = false;
                }
            }

            ed_export_plugin_errors(e);
            export_rect = ui_window_rect();
        }

        ui_dialog_end();

        if (e->export_window && export_rect.h > 0) {
            ed_export_history_panel(e, export_rect);
        }
    }
}

static void about_spectrum(Editor *e) {
    bool playing = dm_audio_playing(e->audio);
    uint64_t now = SDL_GetTicks();

    if (!playing) {
        memset(e->spectrum_targets, 0, sizeof(e->spectrum_targets));
        e->spectrum_tick = 0;
    }

    if (playing && now - e->spectrum_tick >= 16) {
        float stereo[DM_SPECTRUM_FRAMES * 2];

        if (dm_audio_analyzer_read(e->audio, stereo)) {
            float content;

            if (e->spectrum_top <= 0) {
                e->spectrum_top = DM_SPECTRUM_TOP;
            }

            dm_spectrum_analyze(stereo, e->spectrum_top, e->spectrum_targets, &content);
            float elapsed = e->spectrum_tick ? (now - e->spectrum_tick) / 1000.f : 0;
            e->spectrum_top = dm_spectrum_follow_top(e->spectrum_top, content, elapsed);
        }

        e->spectrum_tick = now;
    }

    for (int i = 0; i < DM_SPECTRUM_BANDS; ++i) {
        e->spectrum[i] = dm_visual_smooth(e->spectrum[i], e->spectrum_targets[i], ui_delta_time(),
                                          .025f, playing ? .12f : .18f);

        if (e->spectrum[i] < .001f) {
            e->spectrum[i] = 0;
        }
    }

    ui_spectrum(e->spectrum, DM_SPECTRUM_BANDS, 100, e->accent);
}

#ifdef DM_DEVTOOLS
static void profiler_dialog(Editor *e) {
    if (!e->profiler) {
        return;
    }

    if (ui_dialog(DM_TEXT_FRAME_PROFILER, &e->profiler, (UiDialog){.width = 600, .tool = true})) {
        enum { WINDOW_SECONDS = 2 };

        struct {
            double average, worst;
            int calls;
        } stats[DM_PROFILE_COUNT];

        int frames = dm_profile_frames(WINDOW_SECONDS);

        for (int i = 0; i < DM_PROFILE_COUNT; ++i) {
            dm_profile_stats(i, WINDOW_SECONDS, &stats[i].average, &stats[i].worst,
                             &stats[i].calls);
        }

        double frame_ms = stats[DM_PROFILE_FRAME].average * frames;
        double fps = frame_ms > 0 ? stats[DM_PROFILE_FRAME].calls * 1000 / frame_ms : 0;
        char text[128];
        snprintf(text, sizeof(text), DM_TEXT_PROFILER_FPS, fps);
        ui_text(text);
        ui_dim(DM_TEXT_PROFILER_INCLUSIVE);
        ui_separator();
        ui_group_begin();
        ui_text(DM_TEXT_PROFILER_SECTION);

        for (int i = 0; i < DM_PROFILE_COUNT; ++i) {
            ui_text(dm_profile_name(i));
        }

        ui_group_end();
        ui_same();
        ui_group_begin();
        ui_text(DM_TEXT_PROFILER_AVERAGE);

        for (int i = 0; i < DM_PROFILE_COUNT; ++i) {
            snprintf(text, sizeof(text), "%.3f", stats[i].average);
            ui_text(text);
        }

        ui_group_end();
        ui_same();
        ui_group_begin();
        ui_text(DM_TEXT_PROFILER_WORST);

        for (int i = 0; i < DM_PROFILE_COUNT; ++i) {
            snprintf(text, sizeof(text), "%.3f", stats[i].worst);
            ui_text(text);
        }

        ui_group_end();
        ui_same();
        ui_group_begin();
        ui_text(DM_TEXT_PROFILER_CALLS);

        for (int i = 0; i < DM_PROFILE_COUNT; ++i) {
            snprintf(text, sizeof(text), "%.2f", frames ? (double)stats[i].calls / frames : 0);
            ui_text(text);
        }

        ui_group_end();
    }

    ui_dialog_end();
}

static void format_targets(unsigned targets, char *out, size_t cap) {
    static const struct {
        unsigned bit;
        const char *name;
    } names[] = {
        {TARGET_NOTE, "Note"},
        {TARGET_PLACEMENT, "Placement"},
        {TARGET_EVENT_POINT, "Event point"},
        {TARGET_BEND, "Bend"},
        {TARGET_EVENT_LANE, "Event lane"},
        {TARGET_CANVAS, "Grid"},
        {TARGET_CHANNEL_LABEL, "Channel label"},
        {TARGET_TIMELINE, "Ruler"},
        {TARGET_EVENT_LIST, "Event list"},
        {TARGET_PATTERN_CARD, "Pattern card"},
        {TARGET_LANE_BUTTON, "Lane button"},
        {TARGET_PORT, "Port"},
        {TARGET_TRANSPORT, "Transport"},
        {TARGET_SAMPLE, "Sample"},
    };

    out[0] = 0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!(targets & names[i].bit)) {
            continue;
        }

        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s", len ? ", " : "", names[i].name);
    }

    if (!out[0]) {
        snprintf(out, cap, "%s", DM_TEXT_INSPECTOR_NONE);
    }
}

static void append_token(char *out, size_t cap, const char *sep, const char *token) {
    size_t len = strlen(out);
    snprintf(out + len, cap - len, "%s%s", len ? sep : "", token);
}

static void inspector_row(const Editor *e, const DmGestureRecord *rec) {
    char binding[64] = {0};
    ed_gesture_format(e->gestures[rec->gesture], binding, sizeof(binding));
    const char *helper =
        rec->helper == DM_GESTURE_HELD ? DM_TEXT_INSPECTOR_HELD : DM_TEXT_INSPECTOR_PRESSED;
    char count[16] = {0};

    if (rec->count > 1) {
        snprintf(count, sizeof(count), " x%d", rec->count);
    }

    char line[256];

    if (rec->result) {
        snprintf(line, sizeof(line), "  %s (%s) [%s]%s", ed_gestures[rec->gesture].label, binding,
                 helper, count);
    } else {
        snprintf(line, sizeof(line), "  %s (%s) [%s]%s - %s", ed_gestures[rec->gesture].label,
                 binding, helper, count, rec->reason);
    }

    ui_text(line);
}

static void gesture_inspector_dialog(Editor *e) {
    if (!e->gesture_inspector) {
        return;
    }

    if (ui_dialog(DM_TEXT_GESTURE_INSPECTOR, &e->gesture_inspector,
                  (UiDialog){.width = 600, .tool = true})) {
        UiMouse m = ui_mouse();
        char target[160], mods[64] = {0}, buttons[64] = {0}, line[256];
        format_targets(e->hover_target, target, sizeof(target));

        if (m.ctrl) {
            append_token(mods, sizeof(mods), "+", "Ctrl");
        }

        if (m.shift) {
            append_token(mods, sizeof(mods), "+", "Shift");
        }

        if (m.alt) {
            append_token(mods, sizeof(mods), "+", "Alt");
        }

        if (!mods[0]) {
            snprintf(mods, sizeof(mods), "%s", DM_TEXT_INSPECTOR_NONE);
        }

        if (m.left) {
            append_token(buttons, sizeof(buttons), ", ", "Left");
        }

        if (m.right) {
            append_token(buttons, sizeof(buttons), ", ", "Right");
        }

        if (m.middle) {
            append_token(buttons, sizeof(buttons), ", ", "Middle");
        }

        if (m.wheel != 0) {
            append_token(buttons, sizeof(buttons), ", ", "Wheel");
        }

        if (!buttons[0]) {
            snprintf(buttons, sizeof(buttons), "%s", DM_TEXT_INSPECTOR_NONE);
        }

        snprintf(line, sizeof(line), DM_TEXT_INSPECTOR_HOVER, target);
        ui_text(line);
        snprintf(line, sizeof(line), DM_TEXT_INSPECTOR_MODIFIERS, mods);
        ui_text(line);
        snprintf(line, sizeof(line), DM_TEXT_INSPECTOR_BUTTONS, buttons);
        ui_text(line);
        ui_separator();
        ui_text(DM_TEXT_INSPECTOR_WON);
        int count = dm_gesture_inspector_count();
        bool any = false;

        for (int i = 0; i < count; ++i) {
            DmGestureRecord rec;

            if (dm_gesture_inspector_entry(i, &rec) && rec.result) {
                inspector_row(e, &rec);
                any = true;
            }
        }

        if (!any) {
            ui_dim(DM_TEXT_INSPECTOR_NONE);
        }

        ui_separator();
        ui_text(DM_TEXT_INSPECTOR_LOST);
        any = false;

        for (int i = 0; i < count; ++i) {
            DmGestureRecord rec;

            if (dm_gesture_inspector_entry(i, &rec) && !rec.result) {
                inspector_row(e, &rec);
                any = true;
            }
        }

        if (!any) {
            ui_dim(DM_TEXT_INSPECTOR_NONE);
        }

        ui_separator();
        ui_text(DM_TEXT_INSPECTOR_NOT_ASKED);
        any = false;

        for (int i = 0; i < GESTURE_COUNT; ++i) {
            if ((ed_gestures[i].targets & e->hover_target) && !dm_gesture_inspector_queried(i)) {
                char binding[64] = {0};
                ed_gesture_format(e->gestures[i], binding, sizeof(binding));
                snprintf(line, sizeof(line), "  %s (%s)", ed_gestures[i].label, binding);
                ui_text(line);
                any = true;
            }
        }

        if (!any) {
            ui_dim(DM_TEXT_INSPECTOR_NONE);
        }
    }

    ui_dialog_end();
}
#endif

void ed_dialogs(Editor *e) {
#ifdef DM_DEVTOOLS
    profiler_dialog(e);
    gesture_inspector_dialog(e);
#endif
    dm_audio_analyzer_enable(e->audio, e->about && dm_audio_playing(e->audio));

    if (!e->about) {
        memset(e->spectrum, 0, sizeof(e->spectrum));
        memset(e->spectrum_targets, 0, sizeof(e->spectrum_targets));
        e->spectrum_tick = 0;
    }

    if (e->remove_note_row) {
        static const char *const buttons[] = {DM_TEXT_REMOVE_NOTE_ROW, DM_TEXT_CANCEL};

        if (ui_confirm(DM_TEXT_REMOVE_NOTE_ROW_TITLE, &e->remove_note_row,
                       DM_TEXT_REMOVE_NOTE_ROW_WARNING, buttons, 2) == 0) {
            ed_remove_note_row(e, e->remove_note_row_channel);
        }
    }

    lane_dialog(e);
    rename_dialog(e);
    ed_pattern_color_dialog(e);
    ed_combine_dialog(e);
    ed_song_event_dialog(e);
    ed_graph_mode_dialog(e);
    ed_midi_event_window(e);
    ed_settings(e);
    ed_instrument(e);
    ed_samples_draw(e);
    ed_preferences(e);
    ed_shortcuts(e);
    ed_update_dialog(e);

    if (e->about) {
        if (ui_dialog(DM_TEXT_ABOUT_DUCKMIDI, &e->about, (UiDialog){.width = 560})) {
            about_spectrum(e);
            UiPoint at = ui_cursor();
            ui_piano_icon((UiRect){at.x + ui_available().x - 72, at.y + 2, 66, 36});
            char version[64];
            snprintf(version, sizeof(version), DM_TEXT_ABOUT_APP_VERSION, dm_app_version);
            ui_text(version);
            ui_love_text(DM_TEXT_ABOUT_TAGLINE);
            ui_separator();
            ui_text(DM_TEXT_C17_CORE_DEAR);
            ui_text(DM_TEXT_DISTRIBUTED_UNDER_MIT_LICENSE);
        }

        ui_dialog_end();
    }

    ed_export_dialog(e);
    ed_percussion_dialog(e);
    ed_generate_dialog(e);
    ed_export_warning_dialog(e);
    ed_export_review_dialog(e);
    ed_autosave_dialog(e);

    if (e->confirm) {
        static const char *const buttons[] = {DM_TEXT_SAVE, DM_TEXT_DISCARD, DM_TEXT_CANCEL};
        bool open = true;
        int choice =
            ui_confirm(DM_TEXT_UNSAVED_PROJECT, &open, DM_TEXT_SAVE_CHANGES_BEFORE, buttons, 3);

        if (choice == 0) {
            e->confirm = false;
            ed_save(e, false);
        } else if (choice == 1) {
            ed_pending_execute(e);
        } else if (!open) {
            e->confirm = false;
            e->pending_action = 0;
        }
    }
}
