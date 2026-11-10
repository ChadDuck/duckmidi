#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "sample_library.h"
#include "../src/duck.h"
#include "../src/text/en.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <inttypes.h>
#include <libavutil/intreadwrite.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static const char manifest_file[] = "manifest.json";

typedef struct {
#ifdef _WIN32
    HANDLE handle;
    wchar_t path[DM_PATH];
#else
    int fd;
#endif
} LibraryDir;

#ifdef _WIN32
#define FILE_SYNC(file) _commit(_fileno(file))

static bool wide_path(const char *path, wchar_t *wide, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, cap) > 0;
}

static bool child_path(const LibraryDir *dir, const char *name, wchar_t *path) {
    size_t length = wcslen(dir->path);

    if (length + 1 >= DM_PATH) {
        return false;
    }

    memcpy(path, dir->path, length * sizeof(*path));
    path[length++] = L'\\';
    return wide_path(name, path + length, (int)(DM_PATH - length));
}

static bool normal_handle(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info;

    return handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
           GetFileInformationByHandle(handle, &info) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

static bool open_directory(const wchar_t *path, LibraryDir *out) {
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);

    if (!normal_handle(handle, true)) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }

        return false;
    }

    DWORD length = GetFinalPathNameByHandleW(handle, out->path, DM_PATH, FILE_NAME_NORMALIZED);

    if (!length || length >= DM_PATH) {
        CloseHandle(handle);
        return false;
    }

    out->handle = handle;
    return true;
}

static bool open_root(const char *path, LibraryDir *out) {
    wchar_t wide[DM_PATH];

    if (!wide_path(path, wide, DM_PATH)) {
        return false;
    }

    size_t len = wcslen(wide);

    while (len > 3 && (wide[len - 1] == L'/' || wide[len - 1] == L'\\')) {
        wide[--len] = 0;
    }

    return open_directory(wide, out);
}

static bool open_child(const LibraryDir *parent, const char *name, LibraryDir *out) {
    wchar_t wide[DM_PATH];
    return child_path(parent, name, wide) && open_directory(wide, out);
}

static void close_directory(LibraryDir *dir) {
    CloseHandle(dir->handle);
}

static bool make_directory(const LibraryDir *parent, const char *name, bool exclusive) {
    wchar_t wide[DM_PATH];

    return child_path(parent, name, wide) &&
           (CreateDirectoryW(wide, NULL) || (!exclusive && GetLastError() == ERROR_ALREADY_EXISTS));
}

static HANDLE open_handle(const LibraryDir *dir, const char *name, DWORD access, DWORD share,
                          DWORD action, bool *missing) {
    wchar_t wide[DM_PATH];

    if (!child_path(dir, name, wide)) {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE handle =
        CreateFileW(wide, access, share, NULL, action, FILE_FLAG_OPEN_REPARSE_POINT, NULL);

    if (handle == INVALID_HANDLE_VALUE && missing) {
        *missing = GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    if (handle != INVALID_HANDLE_VALUE && !normal_handle(handle, false)) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }

    return handle;
}

static FILE *open_file(const LibraryDir *dir, const char *name, bool write, bool *missing) {
    HANDLE handle = open_handle(dir, name, write ? GENERIC_WRITE : GENERIC_READ,
                                write ? 0 : FILE_SHARE_READ | FILE_SHARE_DELETE,
                                write ? CREATE_NEW : OPEN_EXISTING, missing);

    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    int fd = _open_osfhandle((intptr_t)handle, (write ? _O_WRONLY : _O_RDONLY) | _O_BINARY);

    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }

    FILE *file = _fdopen(fd, write ? "wb" : "rb");

    if (!file) {
        _close(fd);
    }

    return file;
}

static bool rename_entry(const LibraryDir *dir, const char *from, const char *to, bool replace) {
    wchar_t source[DM_PATH], destination[DM_PATH];
    DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);

    if (!child_path(dir, from, source) || !child_path(dir, to, destination)) {
        return false;
    }

    /* A reader or scanner can briefly deny delete sharing on newly closed files. */
    for (unsigned attempt = 0; attempt < 21; ++attempt) {
        if (MoveFileExW(source, destination, flags)) {
            return true;
        }

        DWORD error = GetLastError();

        if (attempt == 20 || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
                              error != ERROR_LOCK_VIOLATION)) {
            return false;
        }

        SDL_Delay(5);
    }

    return false;
}

static void remove_entry(const LibraryDir *dir, const char *name, bool directory) {
    wchar_t wide[DM_PATH];

    if (child_path(dir, name, wide)) {
        if (directory) {
            RemoveDirectoryW(wide);
        } else {
            DeleteFileW(wide);
        }
    }
}

static bool sync_directory(const LibraryDir *dir) {
    (void)dir;
    return true; /* Rename uses MOVEFILE_WRITE_THROUGH. */
}
#else
#define FILE_SYNC(file) fsync(fileno(file))

static bool open_root(const char *path, LibraryDir *out) {
    size_t len = strlen(path);
    char root[DM_PATH];

    if (!len || len >= sizeof(root)) {
        return false;
    }

    while (len > 1 && path[len - 1] == '/') {
        --len;
    }

    memcpy(root, path, len);
    root[len] = 0;
    out->fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return out->fd >= 0;
}

static bool open_child(const LibraryDir *parent, const char *name, LibraryDir *out) {
    out->fd = openat(parent->fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return out->fd >= 0;
}

static void close_directory(LibraryDir *dir) {
    close(dir->fd);
}

static bool make_directory(const LibraryDir *parent, const char *name, bool exclusive) {
    return mkdirat(parent->fd, name, 0700) == 0 || (!exclusive && errno == EEXIST);
}

static int open_regular(const LibraryDir *dir, const char *name, int flags, bool *missing) {
    int fd = openat(dir->fd, name, flags | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);

    if (fd < 0) {
        if (missing) {
            *missing = errno == ENOENT;
        }

        return -1;
    }

    struct stat info;

    if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        close(fd);
        return -1;
    }

    return fd;
}

static FILE *open_file(const LibraryDir *dir, const char *name, bool write, bool *missing) {
    int fd = open_regular(dir, name, write ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, missing);

    if (fd < 0) {
        return NULL;
    }

    FILE *file = fdopen(fd, write ? "wb" : "rb");

    if (!file) {
        close(fd);
    }

    return file;
}

static bool rename_entry(const LibraryDir *dir, const char *from, const char *to, bool replace) {
    if (replace) {
        return renameat(dir->fd, from, dir->fd, to) == 0;
    }

#ifdef __APPLE__
    int result = renameatx_np(dir->fd, from, dir->fd, to, RENAME_EXCL);
#else
    int result = renameat2(dir->fd, from, dir->fd, to, RENAME_NOREPLACE);
#endif

    if (!result) {
        return true;
    }

    /* Some filesystems lack an exclusive directory rename. An exclusive mkdir reserves an empty
     * target that normal rename can replace. */
    if ((errno != ENOSYS && errno != ENOTSUP && errno != EINVAL) ||
        !make_directory(dir, to, true)) {
        return false;
    }

    if (!renameat(dir->fd, from, dir->fd, to)) {
        return true;
    }

    unlinkat(dir->fd, to, AT_REMOVEDIR);
    return false;
}

static void remove_entry(const LibraryDir *dir, const char *name, bool directory) {
    unlinkat(dir->fd, name, directory ? AT_REMOVEDIR : 0);
}

static bool sync_directory(const LibraryDir *dir) {
    return fsync(dir->fd) == 0 || errno == EINVAL || errno == ENOTSUP;
}
#endif

static bool fail(char *error, size_t cap, const char *message) {
    snprintf(error, cap, "%s", message);
    return false;
}

static bool canceled(const DmSampleImportCallbacks *cb) {
    return cb && cb->canceled && cb->canceled(cb->user);
}

static uint32_t fingerprint(const void *bytes, size_t length) {
    const uint8_t *data = bytes;
    uint32_t hash = 2166136261u;

    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ data[i]) * 16777619u;
    }

    return hash ? hash : 1;
}

static uint32_t file_fingerprint(FILE *file, const char *json, size_t length) {
    uint64_t stamp = 0;
#ifdef _WIN32
    FILETIME time;

    if (GetFileTime((HANDLE)_get_osfhandle(_fileno(file)), NULL, NULL, &time)) {
        stamp = (uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime;
    }
#else
    struct stat info;

    if (!fstat(fileno(file), &info)) {
#ifdef __APPLE__
        stamp = (uint64_t)info.st_mtimespec.tv_sec * 1000000000u + info.st_mtimespec.tv_nsec;
#else
        stamp = (uint64_t)info.st_mtim.tv_sec * 1000000000u + info.st_mtim.tv_nsec;
#endif
    }
#endif
    uint32_t hash = fingerprint(json, length) ^ fingerprint(&stamp, sizeof(stamp));
    return hash ? hash : 1;
}

static bool read_manifest_in(const LibraryDir *dir, const char *name, DmSampleManifest *out) {
    FILE *file = open_file(dir, manifest_file, false, NULL);

    if (!file) {
        return false;
    }

    char *json = malloc(DM_SAMPLE_MANIFEST_MAX + 1);
    size_t length = json ? fread(json, 1, DM_SAMPLE_MANIFEST_MAX + 1, file) : 0;
    bool ok = json && !ferror(file) && length <= DM_SAMPLE_MANIFEST_MAX &&
              dm_sample_manifest_parse(json, length, out, NULL, 0);

    if (ok) {
        out->generation = file_fingerprint(file, json, length);
        snprintf(out->directory, sizeof(out->directory), "%s", name);
    }

    fclose(file);
    free(json);
    return ok;
}

static bool read_manifest(const LibraryDir *root, const char *name, DmSampleManifest *out) {
    LibraryDir dir;

    if (!dm_sample_name_valid(name) || !open_child(root, name, &dir)) {
        return false;
    }

    bool ok = read_manifest_in(&dir, name, out);
    close_directory(&dir);
    return ok;
}

typedef struct {
    const LibraryDir *root;
    const char *id;
    DmSampleManifest found;
    bool duplicate;
} FindSet;

static SDL_EnumerationResult SDLCALL find_set_file(void *user, const char *path, const char *name) {
    (void)path;
    FindSet *find = user;
    DmSampleManifest candidate = {0};

    if (read_manifest(find->root, name, &candidate) && !strcmp(candidate.id, find->id)) {
        if (find->found.assets) {
            find->duplicate = true;
        } else {
            find->found = candidate;
            candidate = (DmSampleManifest){0};
        }
    }

    dm_sample_manifest_free(&candidate);
    return SDL_ENUM_CONTINUE;
}

static bool find_set_file_at(const char *path, const LibraryDir *root, const char *id,
                             DmSampleManifest *out) {
    FindSet find = {.root = root, .id = id};
    bool ok = dm_sample_id_valid(id) && SDL_EnumerateDirectory(path, find_set_file, &find) &&
              find.found.assets && !find.duplicate;

    if (ok) {
        *out = find.found;
    } else {
        dm_sample_manifest_free(&find.found);
    }

    return ok;
}

static bool close_output(FILE *file, bool ok) {
    ok = ok && fflush(file) == 0 && FILE_SYNC(file) == 0;
    return fclose(file) == 0 && ok;
}

static bool write_bytes(const LibraryDir *dir, const char *name, const void *bytes, size_t size) {
    FILE *file = open_file(dir, name, true, NULL);
    return file && close_output(file, fwrite(bytes, 1, size, file) == size);
}

static bool write_wav(const LibraryDir *dir, const DmSampleAsset *asset, const DmSampleAudio *audio,
                      const DmSampleImportCallbacks *cb) {
    if (!audio->pcm || audio->frames != asset->frames || audio->rate != (int)asset->rate ||
        audio->channels != asset->channels) {
        return false;
    }

    FILE *file = open_file(dir, asset->file, true, NULL);

    if (!file) {
        return false;
    }

    size_t count = (size_t)asset->frames * asset->channels;
    uint32_t bytes = (uint32_t)count * 3;
    bool loop = audio->embedded_loop_start < audio->embedded_loop_end &&
                audio->embedded_loop_end <= audio->frames;
    bool metadata = audio->embedded_root >= 0 || loop;
    uint32_t sampler_bytes = metadata ? (loop ? 68 : 44) : 0;
    bool signed_pitch = metadata && audio->embedded_root >= 0 && audio->embedded_cents < 0 &&
                        audio->embedded_cents >= -50;
    uint32_t metadata_bytes = sampler_bytes + (signed_pitch ? 16 : 0);
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4);
    AV_WL32(header + 4, 36 + bytes + (bytes & 1u) + metadata_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    AV_WL32(header + 16, 16);
    AV_WL16(header + 20, 1);
    AV_WL16(header + 22, asset->channels);
    AV_WL32(header + 24, asset->rate);
    AV_WL32(header + 28, asset->rate * asset->channels * 3);
    AV_WL16(header + 32, asset->channels * 3);
    AV_WL16(header + 34, 24);
    memcpy(header + 36, "data", 4);
    AV_WL32(header + 40, bytes);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    uint8_t buffer[3 * 2048];

    for (size_t start = 0; ok && start < count;) {
        size_t length = SDL_min(count - start, sizeof(buffer) / 3);

        if (canceled(cb)) {
            ok = false;
            break;
        }

        for (size_t i = 0; i < length; ++i) {
            float value = audio->pcm[start + i];
            double scaled =
                isfinite(value) ? SDL_clamp((double)value * 8388608, -8388608, 8388607) : 0;
            AV_WL24(buffer + i * 3, (uint32_t)(int32_t)lround(scaled));
        }

        ok = fwrite(buffer, 3, length, file) == length;
        start += length;
    }

    if (ok && (bytes & 1u)) {
        ok = fputc(0, file) == 0;
    }

    if (ok && metadata) {
        uint8_t sampler[68] = {0};
        memcpy(sampler, "smpl", 4);
        AV_WL32(sampler + 4, sampler_bytes - 8);
        AV_WL32(sampler + 16, 1000000000u / asset->rate);
        AV_WL32(sampler + 20, audio->embedded_root >= 0 ? (uint32_t)audio->embedded_root : 128);
        double cents = audio->embedded_cents;
        AV_WL32(sampler + 24, (uint32_t)fmax(0, fmin(4294967295.0, cents * 4294967296.0 / 100)));
        AV_WL32(sampler + 36, loop ? 1 : 0);

        if (loop) {
            AV_WL32(sampler + 52, (uint32_t)audio->embedded_loop_start);
            AV_WL32(sampler + 56, (uint32_t)audio->embedded_loop_end - 1);
        }

        ok = fwrite(sampler, 1, sampler_bytes, file) == sampler_bytes;

        if (ok && signed_pitch) {
            uint8_t instrument[16] = {'i', 'n', 's', 't', 7, 0, 0, 0};
            instrument[8] = (uint8_t)audio->embedded_root;
            instrument[9] = (uint8_t)(int8_t)lround(-audio->embedded_cents);
            instrument[12] = instrument[14] = 127;
            ok = fwrite(instrument, 1, sizeof(instrument), file) == sizeof(instrument);
        }
    }

    return close_output(file, ok);
}

/* SFZ companions sit beside the WAVs they play. A single-sample preset is named after its sample
   (piano.wav, piano.sfz); a layered preset after itself. Repeated names get the preset ID. */
#define SFZ_NAME_CAP (DM_SAMPLE_NAME_CAP + DM_SAMPLE_ID_CAP + 8)

static void sfz_stem(char *stem, size_t cap, const DmSampleManifest *m, uint32_t index) {
    const DmSamplePreset *preset = &m->presets[index];
    uint32_t asset = m->zones[preset->first_zone].asset;
    bool single = true;

    for (uint32_t i = 1; i < preset->zone_count; ++i) {
        single &= m->zones[preset->first_zone + i].asset == asset;
    }

    if (single) {
        const char *file = m->assets[asset].file;
        snprintf(stem, cap, "%.*s", (int)(strlen(file) - 4), file);
    } else {
        snprintf(stem, cap, "%s", dm_sample_name_valid(preset->name) ? preset->name : preset->id);
    }
}

static void sfz_name(char *name, size_t cap, const DmSampleManifest *m, uint32_t index) {
    char stem[DM_SAMPLE_NAME_CAP], other[DM_SAMPLE_NAME_CAP];
    sfz_stem(stem, sizeof(stem), m, index);
    bool repeated = false;

    for (uint32_t i = 0; i < index && !repeated; ++i) {
        sfz_stem(other, sizeof(other), m, i);
        repeated = !SDL_strcasecmp(stem, other);
    }

    if (repeated) {
        snprintf(name, cap, "%s-%s.sfz", stem, m->presets[index].id);
    } else {
        snprintf(name, cap, "%s.sfz", stem);
    }
}

/* Fixed-point decimal output is independent of the current locale. */
static bool sfz_number(FILE *file, const char *name, double value) {
    uint32_t scaled = (uint32_t)lround(fabs(value) * 10000);
    return fprintf(file, " %s=%s%" PRIu32 ".%04" PRIu32, name, value < 0 ? "-" : "", scaled / 10000,
                   scaled % 10000) >= 0;
}

static bool sfz_region(FILE *file, const DmSamplePreset *preset, const DmSampleZone *zone,
                       const DmSampleAsset *asset) {
    const char *mode = preset->drums ? "one_shot" : "no_loop";

    if (zone->loop_mode == DM_SAMPLE_CONTINUOUS_LOOP) {
        mode = "loop_continuous";
    } else if (zone->loop_mode == DM_SAMPLE_SUSTAIN_LOOP) {
        mode = "loop_sustain";
    }

    const DmSampleEnvelope *e = &zone->envelope;
    const DmeSampleToneParams *tone = &zone->tone;
    int tracking = tone->custom_tracking ? (int)lround(tone->tracking) : preset->drums ? 0 : 100;
    bool ok =
        fprintf(file,
                "<region> sample=%s lokey=%u hikey=%u lovel=%u hivel=%u "
                "pitch_keycenter=%u pitch_keytrack=%d loop_mode=%s",
                asset->file, zone->key_lo, zone->key_hi, zone->velocity_lo, zone->velocity_hi,
                zone->root, tracking, mode) >= 0 &&
        sfz_number(file, "tune", tone->fine - zone->cents) &&
        sfz_number(file, "volume", zone->gain_db) &&
        sfz_number(file, "pan", 100 * zone->pan) && sfz_number(file, "ampeg_delay", e->delay) &&
        sfz_number(file, "ampeg_attack", e->attack) && sfz_number(file, "ampeg_hold", e->hold) &&
        sfz_number(file, "ampeg_decay", e->decay) &&
        sfz_number(file, "ampeg_sustain", 100 * e->sustain) &&
        sfz_number(file, "ampeg_release", e->release);

    if (ok && (zone->playback_set || zone->playback.start || zone->playback.end)) {
        size_t end = zone->playback.end ? zone->playback.end : asset->frames;
        ok = fprintf(file, " offset=%zu end=%zu", zone->playback.start, end - 1) >= 0;
    }

    if (ok && tone->coarse) {
        ok = fprintf(file, " transpose=%d", (int)tone->coarse) >= 0;
    }

    if (ok && (tone->cutoff || tone->resonance || tone->filter_depth)) {
        ok = sfz_number(file, "cutoff", tone->cutoff ? tone->cutoff : 20000) &&
             sfz_number(file, "resonance", tone->resonance);
    }

    if (ok && tone->filter_depth) {
        ok = fprintf(file, " fileg_depth=%d", (int)lround(tone->filter_depth)) >= 0 &&
             sfz_number(file, "fileg_delay", e->delay) &&
             sfz_number(file, "fileg_attack", e->attack) &&
             sfz_number(file, "fileg_hold", e->hold) &&
             sfz_number(file, "fileg_decay", e->decay) &&
             sfz_number(file, "fileg_sustain", e->sustain * 100) &&
             sfz_number(file, "fileg_release", e->release);
    }

    if (ok && tone->vibrato_depth) {
        ok = sfz_number(file, "pitchlfo_freq", tone->vibrato_rate ? tone->vibrato_rate : 5) &&
             fprintf(file, " pitchlfo_depth=%d", (int)lround(tone->vibrato_depth)) >= 0;
    }

    if (ok && (tone->chorus || tone->reverb)) {
        ok = sfz_number(file, "effect1", tone->reverb * 100) &&
             sfz_number(file, "effect2", tone->chorus * 100);
    }

    if (ok && zone->loop_mode != DM_SAMPLE_UNLOOPED) {
        ok = fprintf(file, " loop_start=%zu loop_end=%zu", zone->loop.start, zone->loop.end - 1) >=
             0;
    }

    if (ok && (zone->cut_group || zone->cut_by)) {
        ok = fprintf(file, " group=%u off_by=%u off_mode=fast", zone->cut_group, zone->cut_by) >= 0;
    }

    return ok && fputc('\n', file) != EOF;
}

static bool write_sfz(const LibraryDir *dir, const DmSampleManifest *m, uint32_t index) {
    const DmSamplePreset *preset = &m->presets[index];
    char name[SFZ_NAME_CAP];
    sfz_name(name, sizeof(name), m, index);
    FILE *file = open_file(dir, name, true, NULL);

    if (!file) {
        return false;
    }

    bool ok = fputs("// Envelope, filter, vibrato, effects, and choke response vary by player.\n",
                    file) >= 0;

    for (uint32_t i = 0; ok && i < preset->zone_count; ++i) {
        const DmSampleZone *zone = &m->zones[preset->first_zone + i];
        ok = sfz_region(file, preset, zone, &m->assets[zone->asset]);
    }

    return close_output(file, ok);
}

static void discard_set(const LibraryDir *root, const char *name, const DmSampleManifest *m) {
    LibraryDir dir;

    if (!open_child(root, name, &dir)) {
        return;
    }

    for (uint32_t i = 0; i < m->asset_count; ++i) {
        if (dm_sample_asset_filename_valid(m->assets[i].file)) {
            remove_entry(&dir, m->assets[i].file, false);
        }
    }

    for (uint32_t i = 0; i < m->preset_count; ++i) {
        char file[SFZ_NAME_CAP];
        sfz_name(file, sizeof(file), m, i);
        remove_entry(&dir, file, false);
    }

    remove_entry(&dir, manifest_file, false);
    close_directory(&dir);
    remove_entry(root, name, true);
}

static bool write_set(const LibraryDir *dir, const DmSampleManifest *m, const DmSampleAudio *audio,
                      const DmSampleImportCallbacks *cb, const char *json, size_t length) {
    for (uint32_t i = 0; i < m->asset_count; ++i) {
        if (canceled(cb) || !dm_sample_asset_filename_valid(m->assets[i].file) ||
            !write_wav(dir, &m->assets[i], &audio[i], cb)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < m->preset_count; ++i) {
        if (canceled(cb) || !write_sfz(dir, m, i)) {
            return false;
        }
    }

    return !canceled(cb) && write_bytes(dir, manifest_file, json, length) && sync_directory(dir);
}

/* A saved set folder holds only its manifest, WAVs and SFZ companions. */
static SDL_EnumerationResult SDLCALL owned_entry(void *user, const char *path, const char *name) {
    (void)path;
    const DmSampleManifest *m = user;

    if (!strcmp(name, manifest_file)) {
        return SDL_ENUM_CONTINUE;
    }

    for (uint32_t i = 0; i < m->preset_count; ++i) {
        char sfz[SFZ_NAME_CAP];
        sfz_name(sfz, sizeof(sfz), m, i);

        if (!strcmp(name, sfz)) {
            return SDL_ENUM_CONTINUE;
        }
    }

    for (uint32_t i = 0; i < m->asset_count; ++i) {
        if (!strcmp(name, m->assets[i].file)) {
            return SDL_ENUM_CONTINUE;
        }
    }

    return SDL_ENUM_FAILURE;
}

static bool owned_set(const char *root, const DmSampleManifest *m) {
    char path[DM_PATH];
    int length = snprintf(path, sizeof(path), "%s/%s", root, m->directory);

    if (length <= 0 || length >= (int)sizeof(path) ||
        !SDL_EnumerateDirectory(path, owned_entry, (void *)m)) {
        return false;
    }

    LibraryDir library, set;

    if (!open_root(root, &library)) {
        return false;
    }

    bool ok = open_child(&library, m->directory, &set);
    close_directory(&library);

    if (!ok) {
        return false;
    }

    for (uint32_t i = 0; ok && i < m->asset_count; ++i) {
        FILE *file = dm_sample_asset_filename_valid(m->assets[i].file)
                         ? open_file(&set, m->assets[i].file, false, NULL)
                         : NULL;
        ok = file != NULL;

        if (file) {
            fclose(file);
        }
    }

    close_directory(&set);
    return ok;
}

bool dm_sample_library_publish(const char *path, DmSampleManifest *m, const DmSampleAudio *audio,
                               const DmSampleImportCallbacks *cb, char *error, size_t cap) {
    if (!dm_sample_name_valid(m->name)) {
        return fail(error, cap, DM_TEXT_SAMPLE_STORAGE_NAME);
    }

    if (canceled(cb)) {
        return fail(error, cap, DM_TEXT_SAMPLE_IMPORT_CANCELED);
    }

    size_t length;
    char *json = dm_sample_manifest_write(m, &length);

    if (!json) {
        return fail(error, cap, DM_TEXT_SAMPLE_MANIFEST_INVALID);
    }

    LibraryDir root;

    if (!open_root(path, &root)) {
        free(json);
        return fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_PATH);
    }

    DmSampleManifest previous = {0}, destination = {0};
    bool existing = find_set_file_at(path, &root, m->id, &previous);
    bool target = read_manifest(&root, m->name, &destination);
    LibraryDir occupied;
    bool exists = open_child(&root, m->name, &occupied);

    if (exists) {
        close_directory(&occupied);
    }

    bool allowed = !exists || (target && existing && !strcmp(destination.id, m->id) &&
                               !SDL_strcasecmp(previous.directory, m->name));
    dm_sample_manifest_free(&destination);
    static atomic_uint serial;
    unsigned number = atomic_fetch_add(&serial, 1);
    char stage[64], backup[64];
    uint64_t time = SDL_GetTicksNS();
    snprintf(stage, sizeof(stage), ".save-%" PRIu64 "-%u", time, number);
    snprintf(backup, sizeof(backup), ".replace-%" PRIu64 "-%u", time, number);
    bool clean = !existing || owned_set(path, &previous);
    bool staged = allowed && clean && make_directory(&root, stage, true);
    LibraryDir dir;
    bool ok = staged && open_child(&root, stage, &dir);

    if (ok) {
        ok = write_set(&dir, m, audio, cb, json, length);
        close_directory(&dir);
    }

    bool moved =
        ok && !canceled(cb) && existing && rename_entry(&root, previous.directory, backup, false);
    bool committed =
        ok && !canceled(cb) && (!existing || moved) && rename_entry(&root, stage, m->name, false);

    if (committed) {
        sync_directory(&root);
        m->generation = fingerprint(json, length);
        DmSampleManifest saved = {0};

        if (read_manifest(&root, m->name, &saved)) {
            m->generation = saved.generation;
            dm_sample_manifest_free(&saved);
        }

        snprintf(m->directory, sizeof(m->directory), "%s", m->name);

        if (moved) {
            discard_set(&root, backup, &previous);
        }
    } else {
        if (moved) {
            rename_entry(&root, backup, previous.directory, false);
        }

        if (staged) {
            discard_set(&root, stage, m);
        }
    }

    dm_sample_manifest_free(&previous);
    close_directory(&root);
    free(json);
    return committed ? true
                     : fail(error, cap,
                            canceled(cb) ? DM_TEXT_SAMPLE_IMPORT_CANCELED
                            : !allowed   ? DM_TEXT_SAMPLE_STORAGE_COLLISION
                            : !clean     ? DM_TEXT_SAMPLE_STORAGE_EXTRA
                                         : DM_TEXT_SAMPLE_LIBRARY_WRITE);
}

bool dm_sample_library_writable(const char *root) {
    LibraryDir library;

    if (!open_root(root, &library)) {
        return false;
    }

    SDL_Time now;
    uint64_t time = SDL_GetCurrentTime(&now) ? (uint64_t)now : SDL_GetTicksNS();
    static atomic_uint serial;
    char name[64];
    snprintf(name, sizeof(name), ".write-%" PRIu64 "-%u", time, atomic_fetch_add(&serial, 1));
    bool ok = make_directory(&library, name, true);

    if (ok) {
        remove_entry(&library, name, true);
    }

    close_directory(&library);
    return ok;
}

bool dm_sample_library_manifest(const char *path, const char *id, DmSampleManifest *out,
                                char *error, size_t cap) {
    LibraryDir root;

    if (!open_root(path, &root)) {
        return fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_PATH);
    }

    bool ok = find_set_file_at(path, &root, id, out);
    close_directory(&root);
    return ok ? true : fail(error, cap, DM_TEXT_SAMPLE_MANIFEST_INVALID);
}

static Sint64 SDLCALL file_seek(void *user, Sint64 offset, SDL_IOWhence whence) {
    FILE *file = user;

    if (offset > LONG_MAX || offset < LONG_MIN || fseek(file, (long)offset, whence)) {
        return -1;
    }

    return ftell(file);
}

static size_t SDLCALL file_read(void *user, void *bytes, size_t size, SDL_IOStatus *status) {
    FILE *file = user;
    size_t count = fread(bytes, 1, size, file);

    if (count < size) {
        *status = feof(file) ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_ERROR;
    }

    return count;
}

static bool decode_asset(FILE *file, const DmSampleAsset *asset, const DmSampleImportCallbacks *cb,
                         DmSampleAudio *out, char *error, size_t cap) {
    if (!file) {
        return fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_ASSET);
    }

    SDL_IOStreamInterface interface;
    SDL_INIT_INTERFACE(&interface);
    interface.seek = file_seek;
    interface.read = file_read;
    SDL_IOStream *io = SDL_OpenIO(&interface, file);
    DmSampleAudio audio;
    bool ok = io && dm_sample_decode_stream(io, asset->file, cb, &audio, error, cap);

    if (io) {
        SDL_CloseIO(io);
    } else {
        fail(error, cap, DM_TEXT_SAMPLE_IMPORT_MEMORY);
    }

    fclose(file);

    if (!ok) {
        return false;
    }

    if (strcmp(audio.format, "wav") || strncmp(audio.codec, "pcm_", 4) ||
        audio.frames != asset->frames || audio.rate != (int)asset->rate ||
        audio.channels != asset->channels) {
        dm_sample_audio_free(&audio);
        return fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_ASSET);
    }

    *out = audio;
    return true;
}

static FILE *open_asset_file(const LibraryDir *set, const DmSampleAsset *asset) {
    return dm_sample_asset_filename_valid(asset->file) ? open_file(set, asset->file, false, NULL)
                                                       : NULL;
}

bool dm_sample_library_audio(const char *root, const DmSampleManifest *m, uint32_t index,
                             const DmSampleImportCallbacks *cb, DmSampleAudio *out, char *error,
                             size_t cap) {
    if (index >= m->asset_count || !dm_sample_asset_filename_valid(m->assets[index].file)) {
        return fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_ASSET);
    }

    LibraryDir library, set;
    FILE *file = NULL;

    if (open_root(root, &library)) {
        if (open_child(&library, m->directory, &set)) {
            file = open_asset_file(&set, &m->assets[index]);
            close_directory(&set);
        }

        close_directory(&library);
    }

    return decode_asset(file, &m->assets[index], cb, out, error, cap);
}

struct DmSampleLibraryReader {
    LibraryDir set;
    const DmSampleManifest *manifest;
};

DmSampleLibraryReader *dm_sample_library_reader_open(const char *path, const char *id,
                                                     DmSampleManifest *out, char *error,
                                                     size_t cap) {
    return dm_sample_library_reader_open_in(path, NULL, id, out, error, cap);
}

DmSampleLibraryReader *dm_sample_library_reader_open_in(const char *path, const char *directory,
                                                        const char *id, DmSampleManifest *out,
                                                        char *error, size_t cap) {
    LibraryDir root;

    if (!open_root(path, &root)) {
        fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_PATH);
        return NULL;
    }

    DmSampleManifest found = {0}, current = {0};
    DmSampleLibraryReader *reader = calloc(1, sizeof(*reader));
    bool hinted = false;

    /* A cataloged folder avoids parsing every set's manifest to find one ID. */
    if (reader && directory && dm_sample_name_valid(directory) &&
        read_manifest(&root, directory, &found) && !strcmp(found.id, id)) {
        hinted = true;
    } else {
        dm_sample_manifest_free(&found);
    }

    bool opened = reader && (hinted || find_set_file_at(path, &root, id, &found)) &&
                  open_child(&root, found.directory, &reader->set);
    bool ok = opened && read_manifest_in(&reader->set, found.directory, &current) &&
              !strcmp(current.id, id);
    close_directory(&root);
    dm_sample_manifest_free(&found);

    if (!ok) {
        if (opened) {
            close_directory(&reader->set);
        }

        free(reader);
        dm_sample_manifest_free(&current);
        fail(error, cap, DM_TEXT_SAMPLE_MANIFEST_INVALID);
        return NULL;
    }

    *out = current;
    reader->manifest = out;
    return reader;
}

bool dm_sample_library_reader_audio(const DmSampleLibraryReader *reader, const DmSampleManifest *m,
                                    uint32_t index, const DmSampleImportCallbacks *cb,
                                    DmSampleAudio *out, char *error, size_t cap) {
    if (!reader || reader->manifest != m || index >= m->asset_count) {
        return fail(error, cap, DM_TEXT_SAMPLE_LIBRARY_ASSET);
    }

    return decode_asset(open_asset_file(&reader->set, &m->assets[index]), &m->assets[index], cb,
                        out, error, cap);
}

void dm_sample_library_reader_close(DmSampleLibraryReader *reader) {
    if (reader) {
        close_directory(&reader->set);
        free(reader);
    }
}

typedef struct {
    DmSampleCatalog catalog;
    size_t capacity;
    bool writable;
    const DmSampleImportCallbacks *callbacks;
} CatalogScan;

static SDL_EnumerationResult SDLCALL scan_set(void *data, const char *dir, const char *name) {
    CatalogScan *scan = data;

    if (canceled(scan->callbacks)) {
        return SDL_ENUM_FAILURE;
    }

    if (!dm_sample_name_valid(name)) {
        return SDL_ENUM_CONTINUE;
    }

    DmSampleManifest m;

    LibraryDir root;

    if (!open_root(dir, &root)) {
        return SDL_ENUM_CONTINUE;
    }

    bool read = read_manifest(&root, name, &m);
    close_directory(&root);

    if (!read) {
        return SDL_ENUM_CONTINUE;
    }

    size_t index = 0;

    while (index < scan->catalog.count && strcmp(scan->catalog.entries[index].id, m.id)) {
        ++index;
    }

    bool ok = true;

    if (index == scan->catalog.count) {
        ok = index < 1024 && dm_reserve((void **)&scan->catalog.entries, &scan->capacity, index + 1,
                                        sizeof(*scan->catalog.entries));
        scan->catalog.count += ok;
    }

    if (ok) {
        DmSampleCatalogEntry *entry = &scan->catalog.entries[index];
        memcpy(entry->id, m.id, sizeof(entry->id));
        memcpy(entry->name, m.name, sizeof(entry->name));
        snprintf(entry->directory, sizeof(entry->directory), "%s", name);
        entry->generation = m.generation;
        char manifest_path[DM_PATH];
        SDL_PathInfo info;
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s/manifest.json", dir, name);
        entry->modified = SDL_GetPathInfo(manifest_path, &info) ? (uint64_t)info.modify_time : 0;
        entry->writable_root = scan->writable;
    }

    dm_sample_manifest_free(&m);
    return ok ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
}

static int compare_sets(const void *a, const void *b) {
    const DmSampleCatalogEntry *left = a, *right = b;
    return strcmp(left->id, right->id);
}

bool dm_sample_library_catalog(const char *program_root, const char *writable_root,
                               const DmSampleImportCallbacks *cb, DmSampleCatalog *out, char *error,
                               size_t cap) {
    CatalogScan scan = {.callbacks = cb};
    const char *roots[] = {program_root, writable_root};

    for (size_t i = 0; i < sizeof(roots) / sizeof(*roots); ++i) {
        SDL_PathInfo info;
        scan.writable = i == 1;

        if (roots[i] && SDL_GetPathInfo(roots[i], &info) && info.type == SDL_PATHTYPE_DIRECTORY &&
            !SDL_EnumerateDirectory(roots[i], scan_set, &scan)) {
            dm_sample_catalog_free(&scan.catalog);
            return fail(error, cap,
                        canceled(cb) ? DM_TEXT_SAMPLE_IMPORT_CANCELED
                                     : DM_TEXT_SAMPLE_LIBRARY_SCAN);
        }
    }

    if (scan.catalog.count > 1) {
        qsort(scan.catalog.entries, scan.catalog.count, sizeof(*scan.catalog.entries),
              compare_sets);
    }

    *out = scan.catalog;
    return true;
}

void dm_sample_catalog_free(DmSampleCatalog *catalog) {
    free(catalog->entries);
    *catalog = (DmSampleCatalog){0};
}
