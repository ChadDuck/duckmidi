/* Test-only adapter to the separately fetched GPL Mupen64Plus RSP HLE core.
   Not linked into duckmidi or included in deliveries. */
#include "hle_internal.h"
#include "ucodes.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static struct hle_t state;
static unsigned char dmem[4096], imem[4096];

void rsp_break(struct hle_t *hle, unsigned int bits) {
    (void)hle;
    (void)bits;
}

void HleWarnMessage(void *user, const char *format, ...) {
    (void)user;
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
}

__declspec(dllexport) void m64_rsp_init(unsigned char *ram) {
    memset(&state, 0, sizeof(state));
    state.dram = ram;
    state.dmem = dmem;
    state.imem = imem;
}

__declspec(dllexport) void m64_rsp_run(unsigned int address, unsigned int size) {
    memcpy(dmem + 0xff0, &address, 4);
    memcpy(dmem + 0xff4, &size, 4);
    alist_process_audio(&state);
}
