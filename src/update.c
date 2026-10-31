/* Update checks and installs.

   The check reads the latest release of the duckmidi GitHub repository and compares its tag with
   this version. A download picks the release ZIP that matches this build (Full when the bundled
   SoundFont is present, Core otherwise) and unpacks it into update/ beside the executable with
   the tar tool built into Windows. The install is a PowerShell script that waits for duckmidi to
   exit, removes the old program files, copies the new build in, and leaves settings, layout and
   projects alone. */
#include "update.h"
#include "duck.h"
#include "text/en.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

#define RELEASE_HOST "api.github.com"
#define RELEASE_PATH "/repos/ChadDuck/duckmidi/releases/latest"
#define DOWNLOAD_LIMIT (512u * 1024u * 1024u)

struct DmUpdate {
    SDL_Mutex *lock;
    SDL_Thread *thread;
    DmUpdateStatus status;
    char base[DM_PATH], current[32], url[1024], root[DM_PATH];
    bool full;
};

/* Versions */

static void version_parts(const char *text, int parts[3]) {
    parts[0] = parts[1] = parts[2] = 0;

    if (*text == 'v' || *text == 'V') {
        ++text;
    }

    for (int i = 0; i < 3 && *text; ++i) {
        parts[i] = atoi(text);

        while (isdigit((unsigned char)*text)) {
            ++text;
        }

        if (*text != '.') {
            break;
        }

        ++text;
    }
}

int dm_version_compare(const char *a, const char *b) {
    int left[3], right[3];
    version_parts(a, left);
    version_parts(b, right);

    for (int i = 0; i < 3; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }

    return 0;
}

/* Release JSON */

/* Copies the string value after "key": starting at text; returns the position after it. */
static const char *json_string(const char *text, const char *key, char *out, size_t capacity) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *at = strstr(text, pattern);

    if (!at) {
        return NULL;
    }

    at = strchr(at + strlen(pattern), '"');

    if (!at) {
        return NULL;
    }

    size_t n = 0;

    for (++at; *at && *at != '"'; ++at) {
        if (*at == '\\' && at[1]) {
            ++at;
        }

        if (n + 1 < capacity) {
            out[n++] = *at;
        }
    }

    out[n] = 0;
    return *at ? at + 1 : NULL;
}

static bool contains_word(const char *text, const char *word) {
    size_t n = strlen(word);

    for (; *text; ++text) {
        if (!SDL_strncasecmp(text, word, n)) {
            return true;
        }
    }

    return false;
}

bool dm_release_parse(const char *json, bool full, char *version, size_t version_capacity,
                      char *url, size_t url_capacity) {
    char tag[64];

    if (!json_string(json, "tag_name", tag, sizeof(tag))) {
        return false;
    }

    snprintf(version, version_capacity, "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
    const char *assets = strstr(json, "\"assets\"");
    char candidate[1024], fallback[1024] = {0};
    *url = 0;

    for (const char *at = assets; at;) {
        at = json_string(at, "browser_download_url", candidate, sizeof(candidate));

        if (!at) {
            break;
        }

        size_t n = strlen(candidate);

        if (n < 4 || SDL_strcasecmp(candidate + n - 4, ".zip")) {
            continue;
        }

        const char *name = strrchr(candidate, '/');
        name = name ? name + 1 : candidate;

        if (contains_word(name, full ? "full" : "core")) {
            snprintf(url, url_capacity, "%s", candidate);
            return true;
        }

        if (!*fallback) {
            snprintf(fallback, sizeof(fallback), "%s", candidate);
        }
    }

    snprintf(url, url_capacity, "%s", fallback);
    return *url != 0;
}

uint64_t dm_file_fingerprint(const char *path) {
    DmBytes data = {0};
    char error[256];

    if (!dm_read(path, &data, error, sizeof(error))) {
        return 0;
    }

    uint64_t hash = 1469598103934665603ULL;

    for (size_t i = 0; i < data.size; ++i) {
        hash = (hash ^ data.data[i]) * 1099511628211ULL;
    }

    dm_bytes_free(&data);
    return hash;
}

/* Status shared with the worker */

static void set_state(DmUpdate *u, DmUpdateState state, const char *message) {
    SDL_LockMutex(u->lock);
    u->status.state = state;

    if (message) {
        snprintf(u->status.message, sizeof(u->status.message), "%s", message);
    }

    SDL_UnlockMutex(u->lock);
}

static void set_progress(DmUpdate *u, float progress) {
    SDL_LockMutex(u->lock);
    u->status.progress = progress;
    SDL_UnlockMutex(u->lock);
}

DmUpdateStatus dm_update_status(DmUpdate *u) {
    SDL_LockMutex(u->lock);
    DmUpdateStatus status = u->status;
    SDL_UnlockMutex(u->lock);
    return status;
}

void dm_update_dismiss(DmUpdate *u) {
    if (!u) {
        return;
    }

    SDL_LockMutex(u->lock);

    if (u->status.state == DM_UPDATE_CURRENT || u->status.state == DM_UPDATE_FAILED) {
        u->status.state = DM_UPDATE_IDLE;
        u->status.message[0] = 0;
    }

    SDL_UnlockMutex(u->lock);
}

DmUpdate *dm_update_create(const char *base, const char *current_version) {
    DmUpdate *u = calloc(1, sizeof(*u));

    if (!u || !(u->lock = SDL_CreateMutex())) {
        free(u);
        return NULL;
    }

    snprintf(u->base, sizeof(u->base), "%s", base);
    snprintf(u->current, sizeof(u->current), "%s", current_version);
    char font[DM_PATH];
    snprintf(font, sizeof(font), "%sassets/TimGM6mb.sf2", base);
    u->full = SDL_GetPathInfo(font, NULL);
    return u;
}

static void join(DmUpdate *u) {
    if (u->thread) {
        SDL_WaitThread(u->thread, NULL);
        u->thread = NULL;
    }
}

void dm_update_destroy(DmUpdate *u) {
    if (!u) {
        return;
    }

    join(u);
    SDL_DestroyMutex(u->lock);
    free(u);
}

static bool busy(DmUpdate *u) {
    DmUpdateState state = dm_update_status(u).state;
    return state == DM_UPDATE_CHECKING || state == DM_UPDATE_DOWNLOADING;
}

#ifdef _WIN32

static bool wide(const char *text, wchar_t *out, int capacity) {
    return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, capacity) > 0;
}

/* GETs an HTTPS URL into memory, following redirects. */
static bool http_get(DmUpdate *u, const char *url, DmBytes *out, bool report) {
    wchar_t wide_url[1100], host[256], path[1024];
    URL_COMPONENTS parts = {sizeof(parts)};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;

    if (!wide(url, wide_url, 1100) || !WinHttpCrackUrl(wide_url, 0, 0, &parts)) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"duckmidi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, host, parts.nPort, 0) : NULL;
    HINTERNET request =
        connection
            ? WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : NULL;
    bool ok = request &&
              WinHttpSendRequest(request,
                                 L"Accept: application/vnd.github+json, application/octet-stream",
                                 (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, NULL);
    DWORD code = 0, length = 0, size = sizeof(code);
    ok = ok &&
         WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) &&
         code == 200;
    size = sizeof(length);

    if (ok && !WinHttpQueryHeaders(
                  request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                  WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX)) {
        length = 0;
    }

    while (ok) {
        DWORD available = 0, read = 0;

        if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
        }

        if (!available) {
            break;
        }

        uint8_t chunk[65536];
        DWORD want = available < sizeof(chunk) ? available : (DWORD)sizeof(chunk);
        ok = WinHttpReadData(request, chunk, want, &read) && dm_bytes(out, chunk, read) &&
             out->size <= DOWNLOAD_LIMIT;

        if (report && length) {
            set_progress(u, (float)out->size / (float)length);
        }
    }

    if (request) {
        WinHttpCloseHandle(request);
    }

    if (connection) {
        WinHttpCloseHandle(connection);
    }

    if (session) {
        WinHttpCloseHandle(session);
    }

    return ok;
}

/* Runs a command line without a console window and waits for it. */
static bool run_hidden(wchar_t *command, bool wait) {
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup,
                        &process)) {
        return false;
    }

    DWORD code = 0;

    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

static int check_worker(void *user) {
    DmUpdate *u = user;
    DmBytes body = {0};
    char version[32], url[1024];

    if (!http_get(u, "https://" RELEASE_HOST RELEASE_PATH, &body, false) ||
        !dm_bytes(&body, (const uint8_t *)"", 1)) {
        dm_bytes_free(&body);
        set_state(u, DM_UPDATE_FAILED, DM_TEXT_UPDATE_CHECK_FAILED);
        return 0;
    }

    bool parsed = dm_release_parse((const char *)body.data, u->full, version, sizeof(version), url,
                                   sizeof(url));
    dm_bytes_free(&body);

    if (!parsed) {
        set_state(u, DM_UPDATE_FAILED, DM_TEXT_UPDATE_NO_RELEASE);
        return 0;
    }

    SDL_LockMutex(u->lock);
    snprintf(u->status.version, sizeof(u->status.version), "%s", version);
    snprintf(u->url, sizeof(u->url), "%s", url);
    u->status.state =
        dm_version_compare(version, u->current) > 0 ? DM_UPDATE_AVAILABLE : DM_UPDATE_CURRENT;
    SDL_UnlockMutex(u->lock);
    return 0;
}

/* The release may unpack into its own folder; the build is wherever duckmidi.exe is. */
static bool find_root(const char *folder, char *root, size_t capacity) {
    char path[DM_PATH];
    snprintf(path, sizeof(path), "%sduckmidi.exe", folder);

    if (SDL_GetPathInfo(path, NULL)) {
        snprintf(root, capacity, "%s", folder);
        return true;
    }

    int count = 0;
    char **entries = SDL_GlobDirectory(folder, "*", 0, &count);
    bool found = false;

    for (int i = 0; entries && i < count && !found; ++i) {
        snprintf(path, sizeof(path), "%s%s/duckmidi.exe", folder, entries[i]);

        if (SDL_GetPathInfo(path, NULL)) {
            snprintf(root, capacity, "%s%s/", folder, entries[i]);
            found = true;
        }
    }

    SDL_free(entries);
    return found;
}

static int download_worker(void *user) {
    DmUpdate *u = user;
    DmBytes zip = {0};
    char folder[DM_PATH], archive[DM_PATH], unpacked[DM_PATH], error[256];
    snprintf(folder, sizeof(folder), "%supdate/", u->base);
    snprintf(archive, sizeof(archive), "%srelease.zip", folder);
    snprintf(unpacked, sizeof(unpacked), "%srelease/", folder);
    SDL_CreateDirectory(unpacked);

    if (!http_get(u, u->url, &zip, true) ||
        !dm_write(archive, zip.data, zip.size, error, sizeof(error))) {
        dm_bytes_free(&zip);
        set_state(u, DM_UPDATE_FAILED, DM_TEXT_UPDATE_DOWNLOAD_FAILED);
        return 0;
    }

    dm_bytes_free(&zip);
    wchar_t command[3 * DM_PATH], wide_archive[DM_PATH], wide_unpacked[DM_PATH];
    wide(archive, wide_archive, DM_PATH);
    wide(unpacked, wide_unpacked, DM_PATH);
    swprintf(command, 3 * DM_PATH, L"tar.exe -xf \"%ls\" -C \"%ls\"", wide_archive, wide_unpacked);
    char root[DM_PATH];

    if (!run_hidden(command, true) || !find_root(unpacked, root, sizeof(root))) {
        set_state(u, DM_UPDATE_FAILED, DM_TEXT_UPDATE_UNPACK_FAILED);
        return 0;
    }

    SDL_LockMutex(u->lock);
    snprintf(u->root, sizeof(u->root), "%s", root);
    u->status.state = DM_UPDATE_READY;
    u->status.progress = 1;
    SDL_UnlockMutex(u->lock);
    return 0;
}

bool dm_update_check(DmUpdate *u) {
    if (!u || busy(u)) {
        return false;
    }

    join(u);
    set_state(u, DM_UPDATE_CHECKING, "");
    u->thread = SDL_CreateThread(check_worker, "update check", u);
    return u->thread != NULL;
}

bool dm_update_download(DmUpdate *u) {
    if (!u || dm_update_status(u).state != DM_UPDATE_AVAILABLE) {
        return false;
    }

    join(u);
    set_progress(u, 0);
    set_state(u, DM_UPDATE_DOWNLOADING, "");
    u->thread = SDL_CreateThread(download_worker, "update download", u);
    return u->thread != NULL;
}

/* Writes a PowerShell single-quoted string. */
static void quoted(FILE *f, const char *text) {
    fputc('\'', f);

    for (; *text; ++text) {
        if (*text == '\'') {
            fputc('\'', f);
        }

        fputc(*text, f);
    }

    fputc('\'', f);
}

bool dm_update_install(DmUpdate *u, bool restart, uint64_t sample_hash, uint64_t *new_sample_hash,
                       char *error, size_t capacity) {
    *new_sample_hash = 0;

    if (!u || dm_update_status(u).state != DM_UPDATE_READY) {
        snprintf(error, capacity, "%s", DM_TEXT_UPDATE_NOT_READY);
        return false;
    }

    /* The sample project is replaced only while it is unedited; an edited one is kept and the
       new sample, if it changed, is added beside it. */
    char sample[DM_PATH], incoming[DM_PATH], script[DM_PATH], beside[DM_PATH];
    snprintf(sample, sizeof(sample), "%sprojects/funky-beat.duck", u->base);
    snprintf(incoming, sizeof(incoming), "%sprojects/funky-beat.duck", u->root);
    snprintf(beside, sizeof(beside), "%sprojects/funky-beat-%s.duck", u->base, u->status.version);
    snprintf(script, sizeof(script), "%supdate/install.ps1", u->base);
    uint64_t current = dm_file_fingerprint(sample), next = dm_file_fingerprint(incoming);
    bool replace = next && (!current || (sample_hash && current == sample_hash));
    bool add_beside = next && !replace && next != current && next != sample_hash;
    FILE *f = fopen(script, "wb");

    if (!f) {
        snprintf(error, capacity, "%s", DM_TEXT_UPDATE_SCRIPT_FAILED);
        return false;
    }

    fprintf(f, "$ErrorActionPreference = 'Stop'\r\n");
    fprintf(f, "Wait-Process -Id %lu -ErrorAction SilentlyContinue\r\n",
            (unsigned long)GetCurrentProcessId());
    fprintf(f, "$install = ");
    quoted(f, u->base);
    fprintf(f, "\r\n$new = ");
    quoted(f, u->root);
    fprintf(f, "\r\n$staging = ");
    char staging[DM_PATH];
    snprintf(staging, sizeof(staging), "%supdate", u->base);
    quoted(f, staging);
    fprintf(f, "\r\n# Remove the old program files; settings, layout and projects stay.\r\n"
               "Get-ChildItem -LiteralPath $install -File | Where-Object { $_.Extension -in "
               "'.exe', '.dll' } | Remove-Item -Force\r\n"
               "$plugins = Join-Path $install 'plugins'\r\n"
               "if (Test-Path -LiteralPath $plugins) { Remove-Item -LiteralPath $plugins -Recurse "
               "-Force }\r\n"
               "Get-ChildItem -LiteralPath $new | Where-Object { $_.Name -ne 'projects' } | "
               "Copy-Item -Destination $install -Recurse -Force\r\n"
               "New-Item -ItemType Directory -Force -Path (Join-Path $install 'projects') | "
               "Out-Null\r\n");

    if (replace || add_beside) {
        fprintf(f, "Copy-Item -LiteralPath ");
        quoted(f, incoming);
        fprintf(f, " -Destination ");
        quoted(f, replace ? sample : beside);
        fprintf(f, " -Force\r\n");
    }

    if (restart) {
        fprintf(f, "Start-Process -FilePath (Join-Path $install 'duckmidi.exe') -WorkingDirectory "
                   "$install\r\n");
    }

    fprintf(f, "Remove-Item -LiteralPath $staging -Recurse -Force\r\n");
    bool written = !ferror(f);
    written = !fclose(f) && written;
    wchar_t command[2 * DM_PATH], wide_script[DM_PATH];

    if (!written || !wide(script, wide_script, DM_PATH)) {
        snprintf(error, capacity, "%s", DM_TEXT_UPDATE_SCRIPT_FAILED);
        return false;
    }

    swprintf(command, 2 * DM_PATH,
             L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "
             L"\"%ls\"",
             wide_script);

    if (!run_hidden(command, false)) {
        snprintf(error, capacity, "%s", DM_TEXT_UPDATE_SCRIPT_FAILED);
        return false;
    }

    *new_sample_hash = replace ? next : 0;
    return true;
}

#else

bool dm_update_check(DmUpdate *u) {
    if (u) {
        set_state(u, DM_UPDATE_FAILED, DM_TEXT_UPDATE_WINDOWS_ONLY);
    }

    return false;
}

bool dm_update_download(DmUpdate *u) {
    (void)u;
    return false;
}

bool dm_update_install(DmUpdate *u, bool restart, uint64_t sample_hash, uint64_t *new_sample_hash,
                       char *error, size_t capacity) {
    (void)u;
    (void)restart;
    (void)sample_hash;
    *new_sample_hash = 0;
    snprintf(error, capacity, "%s", DM_TEXT_UPDATE_WINDOWS_ONLY);
    return false;
}

#endif
