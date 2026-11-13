#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "crash.h"
#include "core/duck.h"
#include "text/en.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#ifdef __linux__
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif
#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define DM_CRASH_BACKTRACE 1
#endif
#endif
#endif

enum { CRASH_NOTES = 32, CRASH_NOTE_SIZE = 512, CRASH_KEEP = 10, CRASH_FRAMES = 64 };

static char notes[CRASH_NOTES][CRASH_NOTE_SIZE];
static volatile sig_atomic_t note_count, reporting;
static char logs[DM_PATH], report_path[DM_PATH], platform[256];
static char line[4096];
static size_t used;
#ifdef _WIN32
static wchar_t wide_path[DM_PATH];
static HANDLE report;
static CONTEXT context;
static STACKFRAME64 frame;

static union {
    SYMBOL_INFO info;
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
} symbol;
#else
static int report = -1;
static unsigned char alternate_stack[128 * 1024];
#ifdef DM_CRASH_BACKTRACE
static void *frames[CRASH_FRAMES];
#endif
#endif

static void flush_line(void) {
#ifdef _WIN32
    DWORD written;
    WriteFile(report, line, (DWORD)used, &written, NULL);
#else
    size_t offset = 0;

    while (offset < used) {
        ssize_t n = write(report, line + offset, used - offset);

        if (n <= 0) {
            break;
        }

        offset += (size_t)n;
    }
#endif
    used = 0;
}

static void append(const char *text) {
    while (*text) {
        if (used == sizeof(line)) {
            flush_line();
        }

        line[used++] = *text++;
    }
}

static void number(uint64_t value, unsigned base, unsigned width) {
    char digits[32];
    unsigned n = 0;

    do {
        digits[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);

    while (n < width) {
        digits[n++] = '0';
    }

    while (n) {
        char one[] = {digits[--n], 0};
        append(one);
    }
}

/* Module file name only: reports are pasted into public issues, and full paths carry the user's
   account name. */
static void append_module(const char *path) {
    const char *name = path;

    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }

    append(name);
}

static void address(uintptr_t pc) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION memory;
    static char module[DM_PATH];
    uintptr_t base =
        VirtualQuery((void *)pc, &memory, sizeof(memory)) ? (uintptr_t)memory.AllocationBase : 0;
    DWORD n = base ? GetModuleFileNameA((HMODULE)base, module, sizeof(module)) : 0;

    if (n && n < sizeof(module)) {
        append_module(module);
    } else {
        append(DM_TEXT_CRASH_UNKNOWN_MODULE);
    }
#else
    Dl_info info;
    uintptr_t base = 0;

    if (dladdr((void *)pc, &info)) {
        base = (uintptr_t)info.dli_fbase;
        append_module(info.dli_fname ? info.dli_fname : DM_TEXT_CRASH_UNKNOWN_MODULE);
    } else {
        append(DM_TEXT_CRASH_UNKNOWN_MODULE);
    }
#endif
    append("+0x");
    number(pc - base, 16, 1);
    append(" (0x");
    number(pc, 16, 1);
    append(")");
#ifdef _WIN32
    symbol.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol.info.MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement;

    if (SymFromAddr(GetCurrentProcess(), pc, &displacement, &symbol.info)) {
        append(" ");
        append(symbol.info.Name);
    }
#endif
    append("\n");
}

static void begin_report(const char *name, uint64_t code, uintptr_t fault, uint64_t thread) {
    /* Build the UTC filename without stdio, allocation or locale state in the handler. */
    unsigned year, month, day, hour, minute, second;
#ifdef _WIN32
    SYSTEMTIME now;
    GetSystemTime(&now);
    year = now.wYear;
    month = now.wMonth;
    day = now.wDay;
    hour = now.wHour;
    minute = now.wMinute;
    second = now.wSecond;
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t days = now.tv_sec / 86400 + 719468;
    int64_t era = days / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = (unsigned)(yoe + era * 400) + (month <= 2);
    hour = (unsigned)(now.tv_sec / 3600 % 24);
    minute = (unsigned)(now.tv_sec / 60 % 60);
    second = (unsigned)(now.tv_sec % 60);
#endif
    used = 0;
    append(logs);
    append("/crash-");
    number(year, 10, 4);
    append("-");
    number(month, 10, 2);
    append("-");
    number(day, 10, 2);
    append("_");
    number(hour, 10, 2);
    append("-");
    number(minute, 10, 2);
    append("-");
    number(second, 10, 2);
    append(".txt");
    line[used] = 0;
    memcpy(report_path, line, used + 1);
    used = 0;
#ifdef _WIN32
    MultiByteToWideChar(CP_UTF8, 0, report_path, -1, wide_path, DM_PATH);
    report = CreateFileW(wide_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
#else
    report = open(report_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
    append(DM_TEXT_APP_NAME " ");
    append(dm_app_version);
    append(DM_TEXT_CRASH_OS);
    append(platform);
    append(DM_TEXT_CRASH_EXCEPTION);
    append(name);
    append(" code=0x");
    number(code, 16, 1);
    append(DM_TEXT_CRASH_THREAD);
    number(thread, 10, 1);
    append(DM_TEXT_CRASH_FAULT);
    address(fault);
    append(DM_TEXT_CRASH_BASE);
#ifdef _WIN32
    number((uintptr_t)GetModuleHandleW(NULL), 16, 1);
#else
    Dl_info app;
    number(dladdr((void *)dm_crash_install, &app) ? (uintptr_t)app.dli_fbase : 0, 16, 1);
#endif
    append(DM_TEXT_CRASH_STACK);
}

static void end_report(void) {
    append(DM_TEXT_CRASH_BREADCRUMBS);
    sig_atomic_t count = note_count;
    sig_atomic_t start = count > CRASH_NOTES ? count - CRASH_NOTES : 0;

    for (sig_atomic_t i = start; i < count; ++i) {
        append(notes[i % CRASH_NOTES]);
        append("\n");
    }

    flush_line();
#ifdef _WIN32
    CloseHandle(report);
#else
    close(report);
#endif
}

#ifdef _WIN32
static const char *exception_name(DWORD code) {
    switch (code) {
    case SIGABRT:
        return "SIGABRT";
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    default:
        return DM_TEXT_CRASH_UNKNOWN_EXCEPTION;
    }
}

static LONG WINAPI exception_report(EXCEPTION_POINTERS *exception) {
    if (InterlockedExchange((volatile LONG *)&reporting, 1)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    begin_report(exception_name(exception->ExceptionRecord->ExceptionCode),
                 exception->ExceptionRecord->ExceptionCode,
                 (uintptr_t)exception->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    context = *exception->ContextRecord;
    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
#elif defined(_M_ARM64) || defined(__aarch64__)
    DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrStack.Offset = context.Sp;
    frame.AddrFrame.Offset = context.Fp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrFrame.Offset = context.Ebp;
#endif
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;

    /* The first StackWalk64 call returns the faulting frame itself. */
    for (int i = 0; i < CRASH_FRAMES; ++i) {
        if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) ||
            !frame.AddrPC.Offset) {
            break;
        }

        append("  #");
        number(i, 10, 1);
        append(" ");
        address((uintptr_t)frame.AddrPC.Offset);
    }

    end_report();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void abort_report(int sig) {
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode = (DWORD)sig;
    record.ExceptionAddress = (void *)abort_report;
    EXCEPTION_POINTERS exception = {&record, &context};
    exception_report(&exception);
    TerminateProcess(GetCurrentProcess(), (UINT)sig);
}
#else
static const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return DM_TEXT_CRASH_UNKNOWN_SIGNAL;
    }
}

static void signal_report(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;

    if (!reporting) {
        reporting = 1;
        uint64_t thread;
#ifdef __linux__
        thread = (uint64_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
        thread = 0;
        pthread_threadid_np(NULL, &thread);
#else
        thread = (uint64_t)getpid();
#endif
        begin_report(signal_name(sig), (uint64_t)sig, (uintptr_t)info->si_addr, thread);
#ifdef DM_CRASH_BACKTRACE
        int count = backtrace(frames, CRASH_FRAMES);

        for (int i = 0; i < count; ++i) {
            append("  #");
            number(i, 10, 1);
            append(" ");
            address((uintptr_t)frames[i]);
        }

        flush_line();
        backtrace_symbols_fd(frames, count, report);
#else
        append("  #0 ");
        address((uintptr_t)info->si_addr);
#endif
        end_report();
    }

    struct sigaction action = {0};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, NULL);
    raise(sig);
}
#endif

static int newest_first(const void *a, const void *b) {
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

bool dm_crash_install(const char *settings_folder) {
    if (snprintf(logs, sizeof(logs), "%s%slogs", settings_folder,
                 *settings_folder && settings_folder[strlen(settings_folder) - 1] != '/' &&
                         settings_folder[strlen(settings_folder) - 1] != '\\'
                     ? "/"
                     : "") >= (int)sizeof(logs) - 40 ||
        !SDL_CreateDirectory(logs)) {
        return false;
    }

    int count = 0;
    char **files = SDL_GlobDirectory(logs, "crash-*.txt", 0, &count);

    if (files) {
        qsort(files, count, sizeof(*files), newest_first);

        for (int i = CRASH_KEEP; i < count; ++i) {
            char path[DM_PATH];

            if (snprintf(path, sizeof(path), "%s/%s", logs, files[i]) < (int)sizeof(path)) {
                SDL_RemovePath(path);
            }
        }

        SDL_free(files);
    }
#ifdef _WIN32
    SYSTEM_INFO system;
    GetNativeSystemInfo(&system);
    snprintf(platform, sizeof(platform), "Windows / %s",
             system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64   ? "x86_64"
             : system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                                                                             : "x86");
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    /* __fastfail (0xc0000409) bypasses in-process handlers by design. */
    SetUnhandledExceptionFilter(exception_report);
    signal(SIGABRT, abort_report);
#else
    struct utsname system;

    if (uname(&system) == 0) {
        snprintf(platform, sizeof(platform), "%.80s %.80s / %.80s", system.sysname, system.release,
                 system.machine);
    }

    stack_t stack = {0};
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);

    if (sigaltstack(&stack, NULL) != 0) {
        return false;
    }
#ifdef DM_CRASH_BACKTRACE
    backtrace(frames, CRASH_FRAMES); /* Load unwind support before a signal arrives. */
#endif
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction action = {0};
    action.sa_sigaction = signal_report;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) {
        if (sigaction(signals[i], &action, NULL) != 0) {
            return false;
        }
    }
#endif
    return true;
}

void dm_crash_note(const char *text) {
    sig_atomic_t index = note_count % CRASH_NOTES;
    snprintf(notes[index], sizeof(notes[index]), "[%llu] %s", (unsigned long long)SDL_GetTicks(),
             text);

    if (note_count == SIG_ATOMIC_MAX) {
        note_count = CRASH_NOTES;
    } else {
        ++note_count;
    }
}
