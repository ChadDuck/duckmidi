#ifndef M64_PLUGIN_H
#define M64_PLUGIN_H
#include "duckmidi_export_plugin.h"
#include "duckmidi_midi.h"
#include "m64export/m64export.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char sequence[97], bank[97];
    char custom_sequence[97];
    int sequence_choice, loop, percussion;
    int loop_points; /* Use the arrangement's M64 loop points when it has them. */
    int memory;      /* Sequence memory variant: 0 Console-Compatible, 1 Emulator Only. */
    bool has_loop_points;
    int melodic[16][128], drums[128];
    bool used[16][128], used_drums[128], session_known, custom_sequence_set;
    uint64_t session;
    char program_names[128][80], drum_names[128][80];
    DmeField fields[11];
    char port_notice[768]; /* Moved or merged colored ports, shown as a notice. */
    bool port_interference;
    DmeChoice choices[128];
    char choice_labels[128][220];
    DmeMappingRow rows[2048];
    char row_ids[2048][40], row_labels[2048][180];
} M64PluginUi;
typedef struct M64ReviewUi M64ReviewUi;

/* Where each colored port's channel messages play in M64; see ports.c. */
#define M64_PORTS 128

typedef struct {
    uint8_t out[MIDI_CHANNELS][M64_PORTS];
    bool merged, interference;
    unsigned moved_count, conflict_count;
    char notice[768];
} M64PortLayout;

bool m64_port_layout(const DmeEvent *events, uint64_t count, M64PortLayout *layout);

typedef struct {
    M64Request request;
    char sequence[97], bank[97];
    M64Instrument melodic[16 * 128], drums[128];
    int presets[16][128], drum_presets[128];
    bool loop_points, memory_maximum;
    char port_notice[768]; /* Merged ports that interfere, reported as a warning. */
    M64ReviewUi *review_ui;
} M64PluginJob;

void *DME_CALL m64_ui_create(void);
void DME_CALL m64_ui_destroy(void *);
int32_t DME_CALL m64_ui_inspect(void *, const DmeContext *, char *, uint64_t);
int32_t DME_CALL m64_ui_describe(void *, DmeUi *);
int32_t DME_CALL m64_ui_set(void *, const char *, const DmeValue *, char *, uint64_t);
void DME_CALL m64_ui_reset(void *);
bool m64_review_build(M64PluginJob *, const DmeContext *, const M64Result *);
void m64_review_free(M64PluginJob *);
/* Shared SM64 sample choices; see samples.c. */
int m64_canonical_preset(bool percussion, int index);
void m64_preset_label(bool percussion, int index, char *out, size_t capacity);
unsigned m64_preset_choices(bool percussion, DmeChoice *choices, char (*labels)[220]);
int32_t DME_CALL m64_review_describe(void *, DmeReview *);
int32_t DME_CALL m64_review_set(void *, const char *, int32_t, char *, uint64_t);
int32_t DME_CALL m64_review_apply(void *, void *, char *, uint64_t);

/* SM64 US sequence memory; see memory.c. */
#define M64_DATA_C "src/audio/data.c"
#define M64_DATA_H "src/audio/data.h"
#define M64_BUFFERS_C "src/buffers/buffers.c"
#define M64_SEQUENCES_JSON "sound/sequences.json"
#define M64_HACKER_CONFIG "include/config/config_audio.h"

typedef struct {
    char *data;
    size_t size, capacity;
    bool failed;
} M64Text;

typedef struct {
    uint64_t required; /* Bytes of temporary sequence memory the song needs. */
    bool needed, fits; /* Stock pools are too small; the stock layout can hold the song. */
    uint32_t pool;     /* Temporary sequence pool for every US preset below it. */
    uint32_t low_rate; /* Pool of the low-rate presets shared with JP. */
    uint32_t growth, heap, buffer;
} M64MemoryPlan;

M64MemoryPlan m64_memory_plan(uint64_t required, bool maximum);
char *m64_memory_guide(uint64_t required, const char *sequence, bool maximum);
bool m64_hacker_expanded(const uint8_t *config, uint64_t size);
bool m64_patch_hacker_data_h(const uint8_t *data, uint64_t size, uint64_t required, bool expanded,
                             M64Text *out, bool *changed, char *error, size_t capacity);
uint64_t m64_fingerprint(const uint8_t *data, uint64_t size);
bool m64_stock_memory_file(const char *path, const uint8_t *data, uint64_t size);
void m64_text_free(M64Text *text);
bool m64_patch_data_c(const uint8_t *data, uint64_t size, const M64MemoryPlan *plan, M64Text *out,
                      char *error, size_t capacity);
bool m64_patch_data_h(const uint8_t *data, uint64_t size, const M64MemoryPlan *plan, M64Text *out,
                      char *error, size_t capacity);
bool m64_patch_buffers_c(const uint8_t *data, uint64_t size, const M64MemoryPlan *plan,
                         M64Text *out, char *error, size_t capacity);
bool m64_patch_sequences(const uint8_t *data, uint64_t size, const char *sequence, const char *bank,
                         M64Text *out, char *error, size_t capacity);
#endif
