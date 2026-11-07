#define DME_BUILD_PLUGIN
#include "../include/duckmidi_export_plugin.h"
#include <stdlib.h>

static void *DME_CALL create(void) {
    return calloc(1, sizeof(unsigned));
}

static void DME_CALL destroy(void *instance) {
    free(instance);
}

static int32_t DME_CALL open_tool(void *instance) {
    unsigned *opens = instance;
    ++*opens;
    return DME_OK;
}

static const void *DME_CALL interface_at(uint32_t version) {
    static const uint32_t fixture_interface = 1;
    return version == 1 ? &fixture_interface : NULL;
}

static const DmeTool tool = {sizeof(DmeTool), DME_SAMPLES_TOOL_ID, "Fixture Samples", create,
                             destroy,         open_tool,           interface_at};

static const DmeTool *DME_CALL tool_at(uint32_t index) {
    return index == 0 ? &tool : NULL;
}

static const DmePlugin plugin = {.abi = DME_ABI,
                                 .size = sizeof(DmePlugin),
                                 .id = "org.duckmidi.fixture-tool",
                                 .name = "Fixture tool plugin",
                                 .tool_count = 1,
                                 .tool = tool_at};

DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &plugin : NULL;
}
