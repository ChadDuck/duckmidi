#include "crash.h"
#include "core/duck.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts(dm_app_version);
        return 0;
    }

    if (argc != 3) {
        return 2;
    }

#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    struct rlimit limit = {0, 0};
    setrlimit(RLIMIT_CORE, &limit);
#endif
    if (!dm_crash_install(argv[1])) {
        return 3;
    }

    for (int i = 0; i < 40; ++i) {
        char message[64];
        snprintf(message, sizeof(message), "breadcrumb %02d", i);
        dm_crash_note(message);
    }

    if (!strcmp(argv[2], "abort")) {
        raise(SIGABRT);
    } else {
        *(volatile int *)(uintptr_t)1 = 42;
    }

    return 4;
}
