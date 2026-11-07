#include "internal.h"

/* Locale-independent JSON floating point: compiler/host may use comma decimals. */
static void number(Buffer *b, double value) {
    char text[64];
    snprintf(text, sizeof(text), "%.12g", value);

    for (char *s = text; *s; s++) {
        if (*s == ',') {
            *s = '.';
        }
    }

    bytes(b, text, strlen(text));
}

static void envelope(Buffer *b, const M64Instrument *d, const EsNativeRegion *custom) {
    if (custom) {
        format(b, "[");

        for (unsigned i = 0; i < custom->point_count; i++) {
            format(b, "[%d,%d],", custom->points[i][0], custom->points[i][1]);
        }

        format(b, "\"hang\"]");
        return;
    }

    format(b, "[[%u,32700],[%u,%u],\"hang\"]", d->attack, d->decay, d->sustain);
}

static void generated_bank(Compile *c) {
    Buffer *b = &c->result->files[M64_BANK_JSON];
    format(b, "{\n  \"sample_bank\": \"%s\",\n  \"envelopes\": {\n",
           c->custom && c->custom->sample_bank ? c->custom->sample_bank : "instruments");
    unsigned total = c->program_count + c->drum_count;

    for (unsigned i = 0; i < total; i++) {
        const M64Instrument *d = i < c->program_count
                                     ? program_definition(c, c->programs[i])
                                     : drum_definition(c, c->percussion[i - c->program_count]);
        format(b, "    \"env%u\": ", i);
        const EsNativeRegion *custom = NULL;

        if (c->custom && i < c->program_count && c->programs[i] >= 4096 &&
            c->programs[i] < M64_CUSTOM_STOCK_BASE) {
            custom = &c->custom->samples->regions[(c->programs[i] - 4096) >> 8];
        } else if (c->custom && i >= c->program_count &&
                   c->percussion[i - c->program_count] >= 128 &&
                   c->percussion[i - c->program_count] < 256) {
            custom =
                &c->custom->samples
                     ->regions[c->custom_drum_regions[c->percussion[i - c->program_count] - 128]];
        }

        envelope(b, d, custom);
        format(b, "%s\n", i + 1 < total ? "," : "");
    }

    format(b, "  },\n  \"instruments\": {\n");

    for (unsigned i = 0; i < c->program_count; i++) {
        const M64Instrument *d = program_definition(c, c->programs[i]);
        const Register *r = &c->registers[i];
        const M64Instrument *normal = r->enabled && r->normal_hi < 0 ? &r->high : d;
        format(
            b,
            "    \"inst%u\": {\"release_rate\": %u, \"envelope\": \"env%u\", \"sound\": {\"sample\": \"%s\", \"tuning\": ",
            i, d->release_rate, i, normal->sample);
        double origin = pow(2.0, -21.0 / 12.0) * ((c->programs[i] & 128) ? 2.0 : 1.0);
        number(b, normal->tuning * origin);
        format(b, "}");

        if (r->enabled && r->normal_hi >= 0) {
            format(b, ", \"normal_range_hi\": %d, \"sound_hi\": {\"sample\": \"%s\", \"tuning\": ",
                   r->normal_hi, r->high.sample);
            number(b, r->high.tuning * origin);
            format(b, "}");
        }

        format(b, "}%s\n", i + 1 < c->program_count || c->drum_count ? "," : "");
    }

    if (c->drum_count) {
        format(b, "    \"percussion\": [\n");

        for (unsigned i = 0; i < c->drum_count; i++) {
            const M64Instrument *d = drum_definition(c, c->percussion[i]);
            unsigned drum = c->percussion[i];
            unsigned pan =
                drum >= 128 && drum < 256
                    ? es_native_pan(
                          &c->custom->samples->regions[c->custom_drum_regions[drum - 128]])
                    : 64;
            format(
                b,
                "      {\"release_rate\": %u, \"pan\": %u, \"envelope\": \"env%u\", \"sound\": {\"sample\": \"%s\", \"tuning\": ",
                d->release_rate, pan, c->program_count + i, d->sample);
            number(b, d->tuning);
            format(b, "}}%s\n", i + 1 < c->drum_count ? "," : "");
        }

        format(b, "    ]\n");
    }

    format(b, "  },\n  \"instrument_list\": [");

    for (unsigned i = 0; i < c->program_count; i++) {
        format(b, "%s\"inst%u\"", i ? ", " : "", i);
    }

    format(b, "]\n}\n");
    format(&c->result->files[M64_SEQUENCE_JSON], "{\"%s\": [\"%s\"]}\n", c->request->sequence_name,
           c->request->bank_name);
}

/* sm64coopdx writes no bank or sequence table entry: the mod names bank 0x2A. */
bool bank_json(Compile *c) {
    if (!c->coopdx) {
        generated_bank(c);
    }

    Buffer *m = &c->result->files[M64_MANIFEST];
    const char *target = c->coopdx                                     ? "sm64coopdx"
                         : c->request->target == M64_TARGET_HACKERSM64 ? "HackerSM64"
                                                                       : "n64decomp/sm64 US";

    if (c->coopdx) {
        format(m,
               "{\n  \"target\": \"%s\",\n  \"sound_bank\": \"30_extended\",\n  \"bank_id\": %u,\n",
               target, COOPDX_BANK);
    } else {
        format(m, "{\n  \"target\": \"%s\",\n  \"sample_bank\": \"instruments\",\n", target);
    }

    format(
        m,
        "  \"sample_assets_included\": %s,\n  \"sequence_bytes\": %llu,\n  \"sequence_address_max_offset\": 65535,\n  \"additional_timing_grid_ticks\": %u,\n  \"tempo_scale\": ",
        c->custom && c->custom->samples->count ? "true" : "false",
        (unsigned long long)c->result->files[M64_SEQUENCE].size, c->quality_grid);
    number(m, c->tempo_scale);

    if (!c->coopdx) {
        format(
            m,
            ",\n  \"sequence_memory_warning_bytes\": 16128,\n  \"sequence_load_bytes\": %llu,\n  \"minimum_temporary_sequence_pool_bytes\": %llu",
            (unsigned long long)sequence_load_bytes(c),
            (unsigned long long)(sequence_load_bytes(c) + 16));
    }

    format(m, ",\n  \"programs\": [\n");
    bool comma = false;

    for (unsigned p = 0; p < 4096; p++) {
        if (c->slots[p] >= 0) {
            const M64Instrument *d = program_definition(c, p);
            format(
                m,
                "%s    {\"gm_program\": %u, \"channel\": %d, \"high_octave_variant\": %s, \"slot\": %d, \"sample\": \"%s\", \"c4_tuning\": ",
                comma ? ",\n" : "", p & 127, c->request->channel_programs ? (int)(p >> 8) : -1,
                (p & 128) ? "true" : "false", c->slots[p], d->sample);
            number(m, d->tuning);
            const Register *r = &c->registers[c->slots[p]];

            if (c->coopdx) {
                format(m, ", \"bank_instrument\": %d", c->fixed[c->slots[p]]);
            } else if (r->enabled) {
                format(
                    m,
                    ", \"register\": {\"normal_range_hi\": %d, \"sample\": \"%s\", \"c4_tuning\": ",
                    r->normal_hi, r->high.sample);
                number(m, r->high.tuning);
                format(m, "}");
            }

            format(m, "}");
            comma = true;
        }
    }

    format(m, "\n  ],\n  \"drums\": [\n");

    for (unsigned i = 0; i < c->drum_count; i++) {
        unsigned key = c->percussion[i];
        const M64Instrument *d = drum_definition(c, key);
        format(m, "%s    {\"gm_note\": %u, \"slot\": %u, \"sample\": \"%s\", \"tuning\": ",
               i ? ",\n" : "", key, i, d->sample);
        number(m, d->tuning);

        if (c->coopdx) {
            format(m, ", \"bank_instrument\": %d, \"note\": %d", c->drum_instrument[key],
                   c->drum_pitch[key]);
        }

        format(m, "}");
    }

    format(m, "\n  ],\n  \"diagnostics\": [\n");

    for (size_t i = 0; i < c->result->count; i++) {
        Diagnostic *d = &c->result->diagnostics[i];
        format(
            m,
            "%s    {\"severity\": %u, \"code\": %u, \"source_id\": %llu, \"tick\": %llu, \"channel\": %d, \"message\": \"",
            i ? ",\n" : "", d->view.severity, d->view.code, (unsigned long long)d->view.source_id,
            (unsigned long long)d->view.tick, d->view.channel);

        for (const char *s = d->text; *s; s++) {
            if (*s == '"' || *s == '\\') {
                put(m, '\\');
            }

            put(m, (unsigned char)*s);
        }

        format(m, "\"}");
    }

    format(m, "\n  ]\n}\n");

    for (unsigned i = 0; i < M64_ARTIFACT_COUNT; i++) {
        if (c->result->files[i].failed) {
            c->result->status = M64_NOMEM;
            return false;
        }
    }

    return !cancelled(c, 3, 100);
}
