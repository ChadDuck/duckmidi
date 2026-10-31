#include "editor_internal.h"
#include "text/en.h"

static void rename_dialog(Editor *e);
static void lane_dialog(Editor *e);

static void rename_dialog(Editor *e) {
    if (e->rename_pattern < 0 || (size_t)e->rename_pattern >= e->project.pattern_count) {
        return;
    }

    bool open = true;

    if (ui_content_dialog(DM_TEXT_RENAME_PATTERN, &open, 380)) {
        if (ui_appearing()) {
            ui_focus_next();
        }

        ui_input(DM_TEXT_NAME, e->rename_text, sizeof(e->rename_text));

        if (ui_button(DM_TEXT_RENAME, ui_available().x, 0) || ui_dialog_enter()) {
            ed_checkpoint(e);
            dm_pattern_name(&e->project, e->rename_pattern, e->rename_text);
            e->transient_pattern = -1;
            ed_changed(e);
            open = false;
        }
    }

    ui_end();

    if (!open) {
        e->rename_pattern = -1;
    }
}

static void lane_dialog(Editor *e) {
    if (!e->lane_settings) {
        return;
    }

    int c = e->lane_channel, lane = e->lane_index;

    if (ui_dialog(DM_TEXT_AUTOMATION_LANE, &e->lane_settings, 420, 210)) {
        if (lane) {
            if (ui_appearing()) {
                ui_focus_next();
            }

            ui_input(DM_TEXT_NAME, e->rename_text, sizeof(e->rename_text));

            if (ui_button(DM_TEXT_APPLY_NAME, 0, 0) || ui_dialog_enter()) {
                ed_checkpoint(e);
                snprintf(e->project.lane_names[c][lane - 1], 64, "%s", e->rename_text);
                ed_changed(e);
                e->lane_settings = false;
            }
        }

        ui_disabled(e->project.lane_count[c] >= DM_LANES);

        if (ui_button(DM_TEXT_ADD_SUBROW, 0, 0)) {
            ed_checkpoint(e);
            ++e->project.lane_count[c];
            e->channel_expanded[c] = true;
            ed_changed(e);
            e->lane_settings = false;
        }

        ui_disabled_end();

        if (lane && ui_button(DM_TEXT_REMOVE_SUBROW, 0, 0)) {
            ed_checkpoint(e);

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

        ui_end();
    }
}

static void audio_export_controls(Editor *e) {
    DmAudioOptions *o = &e->audio_options;
    bool rendering = dm_audio_exporting(e->audio);
    ui_disabled(dm_audio_busy(e->audio) || e->dialog_busy);
    ui_width(240);

    if (ui_combo("Codec", dm_audio_codec_name(o->codec))) {
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

        if (ui_combo(DM_TEXT_BIT_DEPTH, o->bits == 24 ? "24-bit" : "16-bit")) {
            if (ui_select("16-bit", o->bits == 16, 0, 0)) {
                o->bits = 16;
            }

            if (ui_select("24-bit", o->bits == 24, 0, 0)) {
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

            if (ui_combo("Bitrate", label)) {
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

    if (ui_button("Export", ui_available().x, 0)) {
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
    } else {
        ui_wrap(e->status);
    }
}

void ed_export_dialog(Editor *e) {
    if (e->export_window && !e->export_shown) {
        ed_export_reset_results(e);
    }

    e->export_shown = e->export_window;

    if (e->export_window) {
        if (ui_dialog(DM_TEXT_EXPORT, &e->export_window, 650, 650)) {
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
        }

        ui_end();
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
            dm_spectrum_analyze(stereo, e->spectrum_targets);
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

void ed_dialogs(Editor *e) {
    dm_audio_analyzer_enable(e->audio, e->about && dm_audio_playing(e->audio));

    if (!e->about) {
        memset(e->spectrum, 0, sizeof(e->spectrum));
        memset(e->spectrum_targets, 0, sizeof(e->spectrum_targets));
        e->spectrum_tick = 0;
    }

    lane_dialog(e);
    rename_dialog(e);
    ed_pattern_color_dialog(e);
    ed_combine_dialog(e);
    ed_song_event_dialog(e);
    ed_midi_event_window(e);
    ed_settings(e);
    ed_instrument(e);
    ed_preferences(e);
    ed_shortcuts(e);

    if (e->about) {
        if (ui_fixed_dialog(DM_TEXT_ABOUT_DUCKMIDI, &e->about, 560, 158)) {
            about_spectrum(e);
            UiPoint at = ui_cursor();
            ui_piano_icon((UiRect){at.x + ui_available().x - 72, at.y + 2, 66, 36});
            ui_text(DM_TEXT_ABOUT_APP_VERSION);
            ui_love_text(DM_TEXT_ABOUT_TAGLINE);
            ui_separator();
            ui_text(DM_TEXT_C17_CORE_DEAR);
            ui_text(DM_TEXT_DISTRIBUTED_UNDER_MIT_LICENSE);
        }

        ui_end();
    }

    ed_export_dialog(e);
    ed_percussion_dialog(e);
    ed_generate_dialog(e);
    ed_export_warning_dialog(e);
    ed_export_review_dialog(e);

    if (e->confirm) {
        bool open = true;

        if (ui_fixed_dialog(DM_TEXT_UNSAVED_PROJECT, &open, 430, 160)) {
            ui_confirmation_message(DM_TEXT_SAVE_CHANGES_BEFORE);
            ui_center_row(316);

            if (ui_button(DM_TEXT_SAVE, 100, 0)) {
                e->confirm = false;
                ed_save(e, false);
            }

            ui_same();

            if (ui_button(DM_TEXT_DISCARD, 100, 0)) {
                ed_pending_execute(e);
            }

            ui_same();

            if (ui_button(DM_TEXT_CANCEL, 100, 0)) {
                e->confirm = false;
                e->pending_action = 0;
            }
        }

        ui_end();

        if (!open) {
            e->confirm = false;
            e->pending_action = 0;
        }
    }
}
