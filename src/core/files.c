/* File access: whole-file reads, atomic writes through a temporary file, and ZIP archives. */
#include "core/duck.h"
#include "text/en.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
static wchar_t *wide_path(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);

    if (!n) {
        return NULL;
    }

    wchar_t *w = malloc((size_t)n * sizeof(*w));

    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    }

    return w;
}
#endif

static FILE *file_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *w = wide_path(path);

    if (!w) {
        return NULL;
    }

    FILE *f = _wfopen(w, mode[0] == 'r' ? L"rb" : L"wbx");
    free(w);
    return f;
#else
    return fopen(path, mode);
#endif
}

bool dm_read(const char *path, DmBytes *out, char *error, size_t cap) {
    FILE *f = file_open(path, "rb");

    if (!f) {
        snprintf(error, cap, DM_TEXT_CANNOT_OPEN_FILE, strerror(errno));
        return false;
    }

    bool ok = fseek(f, 0, SEEK_END) == 0;
    long size = ok ? ftell(f) : -1;
    rewind(f);
    ok = size > 0 && size <= 1024 * 1024 * 1024 &&
         dm_reserve((void **)&out->data, &out->capacity, (size_t)size, 1);

    if (ok) {
        out->size = (size_t)size;
        ok = fread(out->data, 1, out->size, f) == out->size;
    }

    fclose(f);

    if (!ok) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_FILE_UNREADABLE_EMPTY);
    }

    return ok;
}

bool dm_output_begin(DmOutput *out, const char *path, char *error, size_t cap) {
    static atomic_uint serial = 0;
    memset(out, 0, sizeof(*out));

    if (strlen(path) >= sizeof(out->path)) {
        snprintf(error, cap, DM_TEXT_OUTPUT_PATH_TOO);
        return false;
    }

    snprintf(out->path, sizeof(out->path), "%s", path);
    unsigned sequence = atomic_fetch_add(&serial, 1);
    snprintf(out->temporary, sizeof(out->temporary), DM_TEXT_DUCK_TMP, path,
             (unsigned long long)time(NULL), (unsigned long)clock(), sequence);
    out->file = file_open(out->temporary, "wbx");

    if (!out->file) {
        snprintf(error, cap, DM_TEXT_CANNOT_CREATE_TEMPORARY, strerror(errno));
        return false;
    }

    return true;
}

bool dm_output_write(DmOutput *out, const void *data, size_t size, char *error, size_t cap) {
    if (!out->file || out->failed) {
        return false;
    }

    if (fwrite(data, 1, size, out->file) != size) {
        out->failed = true;
        snprintf(error, cap, DM_TEXT_WRITING_FAILED, strerror(errno));
        return false;
    }

    return true;
}

static bool output_replace(const char *temporary, const char *path) {
#ifdef _WIN32
    wchar_t *from = wide_path(temporary), *to = wide_path(path);
    bool ok =
        from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(from);
    free(to);
    return ok;
#else
    return rename(temporary, path) == 0;
#endif
}

static void output_remove(const char *path) {
#ifdef _WIN32
    wchar_t *wide = wide_path(path);

    if (wide) {
        DeleteFileW(wide);
        free(wide);
    }
#else
    remove(path);
#endif
}

bool dm_output_finish(DmOutput *out, bool commit, char *error, size_t cap) {
    if (!out->file) {
        return false;
    }

    bool ok = !out->failed;

    if (fflush(out->file)) {
        ok = false;
        snprintf(error, cap, DM_TEXT_CANNOT_FLUSH_OUTPUT, strerror(errno));
    }

    if (fclose(out->file)) {
        ok = false;
        snprintf(error, cap, DM_TEXT_CANNOT_CLOSE_OUTPUT, strerror(errno));
    }

    out->file = NULL;

    if (!commit) {
        ok = false;
    }

    if (ok) {
        ok = output_replace(out->temporary, out->path);

        if (!ok) {
#ifdef _WIN32
            DWORD code = GetLastError();
            char reason[256] = {0};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                           0, reason, sizeof(reason), NULL);
            reason[strcspn(reason, "\r\n")] = 0;
            snprintf(error, cap, DM_TEXT_CANNOT_REPLACE_DESTINATION_WINDOWS,
                     *reason ? reason : DM_TEXT_FILE_SYSTEM_OPERATION_FAILED, (unsigned long)code);
#else
            snprintf(error, cap, DM_TEXT_CANNOT_REPLACE_DESTINATION_ORIGINAL, strerror(errno));
#endif
        }
    }

    if (!ok) {
        output_remove(out->temporary);

        if (commit && !*error) {
            snprintf(error, cap, DM_TEXT_WRITING_FAILED_ORIGINAL);
        }
    }

    return ok;
}

bool dm_write(const char *path, const void *data, size_t size, char *error, size_t cap) {
    DmOutput out;

    if (!dm_output_begin(&out, path, error, cap)) {
        return false;
    }

    bool ok = dm_output_write(&out, data, size, error, cap);
    return dm_output_finish(&out, ok, error, cap);
}

static bool zip_le(DmBytes *out, uint32_t value, int width) {
    unsigned char b[4];

    for (int i = 0; i < width; i++) {
        b[i] = (unsigned char)value;
        value >>= 8;
    }

    return dm_bytes(out, b, (size_t)width);
}

bool dm_zip(const DmArchiveEntry *entries, size_t count, DmBytes *out, char *error, size_t cap) {
    DmBytes zip = {0};
    uint32_t offsets[64], checksums[64];
    bool ok = entries && count && count <= 64;

    /* Entries carry the local time of the export, in the DOS format ZIP uses. */
    time_t now = time(NULL);
    struct tm local = {0};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    uint32_t dos_time = (uint32_t)(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2);
    uint32_t dos_date = (uint32_t)((local.tm_year < 80 ? 0 : local.tm_year - 80) << 9 |
                                   (local.tm_mon + 1) << 5 | local.tm_mday);

    for (size_t i = 0; i < count && ok; i++) {
        const DmArchiveEntry *e = &entries[i];
        size_t name = e->name ? strlen(e->name) : 0;
        ok = name && name <= 65535 && e->size <= UINT32_MAX && zip.size <= UINT32_MAX &&
             (!e->size || e->data);

        if (!ok) {
            break;
        }

        offsets[i] = (uint32_t)zip.size;
        uint32_t crc = UINT32_MAX;

        for (size_t j = 0; j < e->size; j++) {
            crc ^= e->data[j];

            for (int bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
            }
        }

        checksums[i] = ~crc;
        ok = zip_le(&zip, 0x04034b50, 4) && zip_le(&zip, 20, 2) && zip_le(&zip, 0x800, 2) &&
             zip_le(&zip, 0, 2) && zip_le(&zip, dos_time, 2) && zip_le(&zip, dos_date, 2) &&
             zip_le(&zip, checksums[i], 4) && zip_le(&zip, (uint32_t)e->size, 4) &&
             zip_le(&zip, (uint32_t)e->size, 4) && zip_le(&zip, (uint32_t)name, 2) &&
             zip_le(&zip, 0, 2) && dm_bytes(&zip, e->name, name) &&
             dm_bytes(&zip, e->data, e->size);
    }

    size_t central = zip.size;

    for (size_t i = 0; i < count && ok; i++) {
        const DmArchiveEntry *e = &entries[i];
        size_t name = strlen(e->name);
        ok = zip_le(&zip, 0x02014b50, 4) && zip_le(&zip, 20, 2) && zip_le(&zip, 20, 2) &&
             zip_le(&zip, 0x800, 2) && zip_le(&zip, 0, 2) && zip_le(&zip, dos_time, 2) &&
             zip_le(&zip, dos_date, 2) && zip_le(&zip, checksums[i], 4) &&
             zip_le(&zip, (uint32_t)e->size, 4) && zip_le(&zip, (uint32_t)e->size, 4) &&
             zip_le(&zip, (uint32_t)name, 2) && zip_le(&zip, 0, 2) && zip_le(&zip, 0, 2) &&
             zip_le(&zip, 0, 2) && zip_le(&zip, 0, 2) && zip_le(&zip, 0, 4) &&
             zip_le(&zip, offsets[i], 4) && dm_bytes(&zip, e->name, name);
    }

    if (zip.size > UINT32_MAX) {
        ok = false;
    }

    size_t central_size = zip.size - central;
    ok = ok && zip_le(&zip, 0x06054b50, 4) && zip_le(&zip, 0, 2) && zip_le(&zip, 0, 2) &&
         zip_le(&zip, (uint32_t)count, 2) && zip_le(&zip, (uint32_t)count, 2) &&
         zip_le(&zip, (uint32_t)central_size, 4) && zip_le(&zip, (uint32_t)central, 4) &&
         zip_le(&zip, 0, 2);

    if (!ok) {
        dm_bytes_free(&zip);
        snprintf(error, cap, DM_TEXT_CANNOT_ASSEMBLE_EXPORT_ARCHIVE);
        return false;
    }

    dm_bytes_free(out);
    *out = zip;
    return true;
}
