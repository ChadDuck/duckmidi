/* Exercise the bundled FluidSynth with generated PCM and the generated test SF2. */
#include <SDL3/SDL.h>
#include <fluidsynth.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RATE = 48000,
    BLOCK = 64,
    FRAMES = 14400,
    LOOP_START = 480,
    LOOP_END = 960,
    VIRTUAL_BANK = 257,
    VOICES = 64,
    PROGRAMS = 6,
};

typedef struct {
    const char *name;
    int number, mode, exclusive, layers, attack, release;
    fluid_sample_t *pcm[2];
    fluid_preset_t *preset;
} SpikePreset;

typedef struct {
    const char *name;
    SpikePreset program[PROGRAMS];
    size_t count;
} SpikeBank;

typedef struct {
    float left[BLOCK], right[BLOCK];
} Block;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)
#define OK(call) CHECK((call) == FLUID_OK)

static const char *preset_name(fluid_preset_t *preset) {
    const SpikePreset *p = fluid_preset_get_data(preset);
    return p->name;
}

static int preset_bank(fluid_preset_t *preset) {
    (void)preset;
    return VIRTUAL_BANK;
}

static int preset_number(fluid_preset_t *preset) {
    const SpikePreset *p = fluid_preset_get_data(preset);
    return p->number;
}

static int note_on(fluid_preset_t *preset, fluid_synth_t *synth, int chan, int key, int vel) {
    const SpikePreset *p = fluid_preset_get_data(preset);

    for (int i = 0; i < p->layers; ++i) {
        fluid_voice_t *voice = fluid_synth_alloc_voice(synth, p->pcm[i], chan, key, vel);

        if (!voice) {
            return FLUID_FAILED;
        }

        fluid_voice_gen_set(voice, GEN_SAMPLEMODE, p->mode);
        fluid_voice_gen_set(voice, GEN_EXCLUSIVECLASS, p->exclusive);
        fluid_voice_gen_set(voice, GEN_VOLENVATTACK, p->attack);
        fluid_voice_gen_set(voice, GEN_VOLENVRELEASE, p->release);

        if (p->number == 3) {
            fluid_voice_gen_set(voice, GEN_SCALETUNE, 0);
        }

        if (p->layers == 2) {
            fluid_voice_gen_set(voice, GEN_PAN, i == 0 ? -500 : 500);
        }

        fluid_synth_start_voice(synth, voice);
    }

    return FLUID_OK;
}

static const char *bank_name(fluid_sfont_t *sfont) {
    const SpikeBank *bank = fluid_sfont_get_data(sfont);
    return bank->name;
}

static fluid_preset_t *find_preset(fluid_sfont_t *sfont, int bank_num, int program) {
    const SpikeBank *bank = fluid_sfont_get_data(sfont);

    if (bank_num != VIRTUAL_BANK || program < 0 || (size_t)program >= bank->count) {
        return NULL;
    }

    return bank->program[program].preset;
}

static int free_bank(fluid_sfont_t *sfont) {
    SpikeBank *bank = fluid_sfont_get_data(sfont);

    for (size_t i = 0; i < bank->count; ++i) {
        delete_fluid_preset(bank->program[i].preset);
    }

    return delete_fluid_sfont(sfont);
}

static fluid_sfont_t *load_bank(fluid_sfloader_t *loader, const char *path) {
    if (strcmp(path, "spike:revision") != 0) {
        return NULL;
    }

    SpikeBank *bank = fluid_sfloader_get_data(loader);
    fluid_sfont_t *sfont = new_fluid_sfont(bank_name, find_preset, NULL, NULL, free_bank);
    CHECK(sfont);
    OK(fluid_sfont_set_data(sfont, bank));

    for (size_t i = 0; i < bank->count; ++i) {
        SpikePreset *p = &bank->program[i];
        p->number = (int)i;
        p->preset = new_fluid_preset(sfont, preset_name, preset_bank, preset_number, note_on,
                                     delete_fluid_preset);
        CHECK(p->preset);
        OK(fluid_preset_set_data(p->preset, p));
    }

    return sfont;
}

static int register_bank(fluid_synth_t *synth, fluid_sfloader_t *loader, SpikeBank *bank) {
    OK(fluid_sfloader_set_data(loader, bank));
    int id = fluid_synth_sfload(synth, "spike:revision", 0);
    CHECK(id >= 0);
    OK(fluid_sfloader_set_data(loader, NULL));
    return id;
}

static double sinusoid(int frame, int frequency) {
    return sin(6.283185307179586 * frequency * frame / RATE);
}

static fluid_sample_t *make_sample(int frequency) {
    enum { COPY_PADDING = 8 };

    short data[FRAMES];

    for (int i = 0; i < FRAMES; ++i) {
        int hz = i < LOOP_END ? frequency : frequency * 2;
        data[i] = i < LOOP_START ? 0 : (short)(16000 * sinusoid(i, hz));
    }

    fluid_sample_t *sample = new_fluid_sample();
    CHECK(sample);
    OK(fluid_sample_set_sound_data(sample, data, NULL, FRAMES, RATE, 1));
    /* With copy enabled, 2.5.4 adds eight frames before the PCM. Loop offsets
       are buffer indices and the rendered interval is half-open. */
    OK(fluid_sample_set_loop(sample, LOOP_START + COPY_PADDING, LOOP_END + COPY_PADDING));
    OK(fluid_sample_set_pitch(sample, 69, 0));
    return sample;
}

static fluid_synth_t *make_synth(fluid_settings_t *settings, fluid_sfloader_t **loader) {
    fluid_synth_t *synth = new_fluid_synth(settings);
    CHECK(synth);
    *loader = new_fluid_sfloader(load_bank, delete_fluid_sfloader);
    CHECK(*loader);
    fluid_synth_add_sfloader(synth, *loader);
    return synth;
}

static void render(fluid_synth_t *synth, int blocks, Block *block) {
    for (int i = 0; i < blocks; ++i) {
        OK(fluid_synth_write_float(synth, BLOCK, block->left, 0, 1, block->right, 0, 1));
    }
}

static double peak(const float *pcm) {
    double value = 0;

    for (int i = 0; i < BLOCK; ++i) {
        value = fmax(value, fabs(pcm[i]));
    }

    return value;
}

static double tone(fluid_synth_t *synth, int right, int frequency) {
    Block block;
    double sine = 0, cosine = 0;

    /* 40 ms gives an integer number of cycles for the unbent test tones. */
    for (int b = 0; b < 30; ++b) {
        render(synth, 1, &block);
        const float *pcm = right ? block.right : block.left;

        for (int i = 0; i < BLOCK; ++i) {
            int frame = b * BLOCK + i;
            sine += pcm[i] * sinusoid(frame, frequency);
            cosine += pcm[i] * sinusoid(frame + RATE / (4 * frequency), frequency);
        }
    }

    return hypot(sine, cosine) / (15 * BLOCK);
}

static void clear(fluid_synth_t *synth) {
    Block block;
    OK(fluid_synth_all_sounds_off(synth, -1));
    render(synth, 4, &block);
    CHECK(fluid_synth_get_active_voice_count(synth) == 0);
}

static int voice_class(fluid_synth_t *synth, int channel, int key) {
    fluid_voice_t *voices[VOICES] = {0};
    fluid_synth_get_voicelist(synth, voices, VOICES, -1);

    for (size_t i = 0; i < VOICES && voices[i]; ++i) {
        if (fluid_voice_get_channel(voices[i]) == channel &&
            fluid_voice_get_key(voices[i]) == key) {
            return (int)fluid_voice_gen_get(voices[i], GEN_EXCLUSIVECLASS);
        }
    }

    return -1;
}

static void check_loop(fluid_synth_t *synth, int id, int program, int tail_hz) {
    Block block;
    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, program));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    render(synth, 60, &block);
    CHECK(tone(synth, 0, 400) > 0.03);
    OK(fluid_synth_noteoff(synth, 0, 69));
    render(synth, 16, &block);
    CHECK(tone(synth, 0, tail_hz) > 0.01);
    render(synth, 500, &block);
    CHECK(peak(block.left) < 0.00001);
    clear(synth);
}

static void check_endpoints(fluid_synth_t *synth, int id) {
    Block block;
    float output[480 * 3], right[480 * 3];
    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, 0));
    OK(fluid_synth_set_interp_method(synth, 0, FLUID_INTERP_NONE));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    render(synth, 60, &block);
    CHECK(peak(block.left) > 0.03);
    OK(fluid_synth_write_float(synth, (int)(sizeof(output) / sizeof(*output)), output, 0, 1, right,
                               0, 1));

    for (size_t i = 480; i < sizeof(output) / sizeof(*output); ++i) {
        CHECK(fabs(output[i] - output[i - 480]) < 0.00005);
    }

    clear(synth);
    OK(fluid_synth_set_interp_method(synth, 0, FLUID_INTERP_4THORDER));
}

static void check_choke(fluid_synth_t *synth, int id) {
    Block block;
    OK(fluid_synth_program_select(synth, 9, id, VIRTUAL_BANK, 3));
    OK(fluid_synth_start(synth, 1000, fluid_synth_get_channel_preset(synth, 9), 0, 9, 46, 120));
    /* The route consumes ordinary note-off for a one-shot. */
    render(synth, 120, &block);
    CHECK(peak(block.left) > 0.01 && voice_class(synth, 9, 46) == 129);
    OK(fluid_synth_start(synth, 1001, fluid_synth_get_channel_preset(synth, 9), 0, 9, 42, 120));
    CHECK(voice_class(synth, 9, 46) == 0 && voice_class(synth, 9, 42) == 129);
    render(synth, 230, &block);
    CHECK(peak(block.left) < 0.00001);
    clear(synth);

    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, 0));
    OK(fluid_synth_program_select(synth, 1, id, VIRTUAL_BANK, 0));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    OK(fluid_synth_noteon(synth, 1, 69, 120));
    render(synth, 30, &block);
    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, 4));
    OK(fluid_synth_noteon(synth, 0, 73, 120));
    CHECK(voice_class(synth, 0, 69) == 128 && voice_class(synth, 0, 73) == 130);
    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, 0));
    OK(fluid_synth_noteon(synth, 0, 71, 120));
    CHECK(voice_class(synth, 0, 69) == 0 && voice_class(synth, 0, 71) == 128);
    CHECK(voice_class(synth, 1, 69) == 128 && voice_class(synth, 0, 73) == 130);
    clear(synth);
}

static void check_controls(fluid_synth_t *synth, int primary, int id) {
    Block block;
    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, 0));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    OK(fluid_synth_cc(synth, 0, 64, 127));
    OK(fluid_synth_program_select(synth, 0, primary, 0, 0));
    OK(fluid_synth_noteoff(synth, 0, 69));
    render(synth, 300, &block);
    CHECK(tone(synth, 0, 400) > 0.03);
    OK(fluid_synth_pitch_bend(synth, 0, 16383));
    CHECK(tone(synth, 0, 449) > 0.03);
    OK(fluid_synth_pitch_bend(synth, 0, 8192));
    OK(fluid_synth_cc(synth, 0, 64, 0));
    render(synth, 500, &block);
    CHECK(peak(block.left) < 0.00001);
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    CHECK(tone(synth, 0, 440) > 0.03);
    clear(synth);

    OK(fluid_synth_program_select(synth, 0, id, VIRTUAL_BANK, 5));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    render(synth, 8, &block);
    double early = peak(block.left);
    render(synth, 40, &block);
    CHECK(peak(block.left) > early * 3);
    OK(fluid_synth_noteoff(synth, 0, 69));
    render(synth, 40, &block);
    CHECK(peak(block.left) < 0.00001);
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    render(synth, 48, &block);
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    OK(fluid_synth_noteoff(synth, 0, 69));
    render(synth, 40, &block);
    CHECK(peak(block.left) < 0.00001);
    clear(synth);
}

static void check_preview_and_swap(fluid_synth_t *synth, fluid_synth_t *preview, int first,
                                   fluid_sfloader_t *loader, SpikeBank *next, int audition) {
    Block block;
    OK(fluid_synth_program_select(synth, 0, first, VIRTUAL_BANK, 0));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    OK(fluid_synth_program_select(preview, 0, audition, VIRTUAL_BANK, 2));
    OK(fluid_synth_noteon(preview, 0, 69, 120));
    CHECK(tone(preview, 0, 400) > 0.03 && tone(preview, 1, 600) > 0.03);
    CHECK(tone(preview, 0, 600) < 0.003 && tone(preview, 1, 400) < 0.003);
    clear(preview);
    CHECK(tone(synth, 0, 400) > 0.03);

    OK(fluid_synth_noteoff(synth, 0, 69));
    int next_id = register_bank(synth, loader, next);
    OK(fluid_synth_program_select(synth, 0, next_id, VIRTUAL_BANK, 0));
    OK(fluid_synth_sfunload(synth, first, 0));
    CHECK(!fluid_synth_get_sfont_by_id(synth, first));
    /* Keep PCM alive until after synth teardown, independently of preset metadata. */
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    CHECK(tone(synth, 0, 400) > 0.01 && tone(synth, 0, 600) > 0.03);
    render(synth, 500, &block);
    CHECK(tone(synth, 0, 400) < 0.003 && tone(synth, 0, 600) > 0.03);
    clear(synth);
}

static void benchmark(fluid_synth_t *synth, const char *name, int id, int bank, int program) {
    const int callbacks = 10000;
    const double timer_hz = (double)SDL_GetPerformanceFrequency();
    Block block;
    OK(fluid_synth_program_select(synth, 0, id, bank, program));
    OK(fluid_synth_noteon(synth, 0, 69, 120));
    int onset = -1;

    for (int b = 0; b < 40 && onset < 0; ++b) {
        render(synth, 1, &block);

        for (int i = 0; i < BLOCK; ++i) {
            if (fabs(block.left[i]) > 0.0001) {
                onset = b * BLOCK + i;
                break;
            }
        }
    }

    CHECK(onset >= 0);
    render(synth, 100, &block);
    Uint64 start = SDL_GetPerformanceCounter();

    for (int i = 0; i < callbacks; ++i) {
        render(synth, 1, &block);
    }

    double average_us = 1e6 * (SDL_GetPerformanceCounter() - start) / timer_hz / callbacks;
    printf("%s: first audible frame %d, %.3f us per %d-frame render\n", name, onset, average_us,
           BLOCK);
    clear(synth);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    fluid_settings_t *settings = new_fluid_settings();
    CHECK(settings);
    OK(fluid_settings_setnum(settings, "synth.sample-rate", RATE));
    OK(fluid_settings_setint(settings, "synth.polyphony", VOICES));
    OK(fluid_settings_setint(settings, "synth.reverb.active", 0));
    OK(fluid_settings_setint(settings, "synth.chorus.active", 0));
    fluid_sfloader_t *loader, *preview_loader;
    fluid_synth_t *synth = make_synth(settings, &loader);
    fluid_synth_t *preview = make_synth(settings, &preview_loader);
    int primary = fluid_synth_sfload(synth, argv[1], 1);
    CHECK(primary >= 0);
    CHECK(fluid_synth_sfload(preview, argv[1], 1) >= 0);

    fluid_sample_t *mono = make_sample(400);
    fluid_sample_t *alternate = make_sample(600);
    fluid_sample_t *preview_left = make_sample(400);
    fluid_sample_t *preview_right = make_sample(600);
    SpikeBank bank = {"Revision one",
                      {{"Loop through release", 0, 1, 128, 1, -12000, -1200, {mono}, NULL},
                       {"Loop until release", 0, 3, 0, 1, -12000, -1200, {mono}, NULL},
                       {"Stereo", 0, 1, 0, 2, -12000, -1200, {mono, alternate}, NULL},
                       {"One-shot kit", 0, 0, 129, 1, -12000, -7200, {mono}, NULL},
                       {"Other choke preset", 0, 1, 130, 1, -12000, -1200, {mono}, NULL},
                       {"Unlooped melody", 0, 0, 0, 1, -3600, -7200, {mono}, NULL}},
                      PROGRAMS};
    SpikeBank next = {
        "Revision two", {{"New loop", 0, 1, 131, 1, -12000, -1200, {alternate}, NULL}}, 1};
    SpikeBank draft = {"Isolated draft",
                       {{"Mono", 0, 1, 0, 1, -12000, -1200, {preview_left}, NULL},
                        {"Other mono", 0, 1, 0, 1, -12000, -1200, {preview_right}, NULL},
                        {"Stereo", 0, 1, 0, 2, -12000, -1200, {preview_left, preview_right}, NULL}},
                       3};
    int id = register_bank(synth, loader, &bank);
    int audition = register_bank(preview, preview_loader, &draft);

    check_loop(synth, id, 0, 400);
    check_loop(synth, id, 1, 800);
    check_endpoints(synth, id);
    puts("Loop modes and exact loop period passed.");
    check_choke(synth, id);
    puts("One-shot tail, choke scope and panic passed.");
    check_controls(synth, primary, id);
    puts("Program Change, CC64, pitch bend, attack, release and repeated notes passed.");
    benchmark(synth, "Generated SF2", primary, 0, 0);
    benchmark(synth, "Virtual mono (480 silent source frames)", id, VIRTUAL_BANK, 0);
    benchmark(synth, "Virtual stereo (480 silent source frames)", id, VIRTUAL_BANK, 2);
    check_preview_and_swap(synth, preview, id, loader, &next, audition);
    puts("Stereo isolation and revision swap with overlapping release passed.");
    delete_fluid_synth(preview);
    delete_fluid_synth(synth);
    delete_fluid_sample(preview_left);
    delete_fluid_sample(preview_right);
    delete_fluid_sample(mono);
    delete_fluid_sample(alternate);
    delete_fluid_settings(settings);
    return 0;
}
