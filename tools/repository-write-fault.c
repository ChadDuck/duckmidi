/* LD_PRELOAD fixture: fail one anchored stage write with ENOSPC, without production hooks. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t write(int fd, const void *bytes, size_t size) {
    static unsigned stages;
    ssize_t (*next)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
    const char *selected = getenv("DUCKMIDI_FAIL_STAGE_WRITE");

    if (selected && size) {
        char descriptor[64], path[8192];
        snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
        ssize_t length = readlink(descriptor, path, sizeof(path) - 1);

        if (length > 0) {
            path[length] = 0;

            if (strstr(path, "/.duckmidi-") && ++stages == strtoul(selected, NULL, 10)) {
                errno = ENOSPC;
                return -1;
            }
        }
    }

    return next(fd, bytes, size);
}
