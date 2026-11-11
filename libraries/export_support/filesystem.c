#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "internal.h"
#include "../../src/text/en.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
#include <unistd.h>
#endif

#ifndef _WIN32
static int checkout_descriptor(EsRoot *root);
#endif

void *es_checkout_lock(EsRoot *root, char *error, size_t cap) {
#ifdef _WIN32
    uint8_t digest[32];
    es_sha256(es_root_identity(root), strlen(es_root_identity(root)), digest);
    wchar_t name[128];
    swprintf(
        name, sizeof(name) / sizeof(*name),
        L"Local\\duckmidi-repository-%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
        digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14],
        digest[15]);
    HANDLE mutex = CreateMutexW(NULL, FALSE, name);
    DWORD result = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;

    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        return mutex;
    }

    if (mutex) {
        CloseHandle(mutex);
    }
#else
    int fd = checkout_descriptor(root);

    if (fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB)) {
        return (void *)(intptr_t)(fd + 1);
    }

    if (fd >= 0) {
        close(fd);
    }
#endif
    es_fail(error, cap,
            DM_TEXT_REPOSITORY_ANOTHER_WRITER_OWNS_THIS_PHYSICAL_CHECKOUT_OR_ITS_FILESYSTEM);
    return NULL;
}

void es_checkout_unlock(void *lock) {
    if (!lock) {
        return;
    }

#ifdef _WIN32
    ReleaseMutex((HANDLE)lock);
    CloseHandle((HANDLE)lock);
#else
    int fd = (int)(intptr_t)lock - 1;
    flock(fd, LOCK_UN);
    close(fd);
#endif
}

#define ES_FILE_LIMIT (256u * 1024u * 1024u)
#define ES_PATH_LIMIT 8192

typedef struct {
#ifdef _WIN32
    HANDLE handle;
    wchar_t *path;
#else
    int fd;
#endif
} Directory;

/* An anchored subdirectory handle, kept open for the root's lifetime so repeated access to
   one tree walks each component once. A closed entry is reopened on its next use. */
typedef struct {
    char *path;
    Directory directory;
    bool open;
} CachedDirectory;

#define ES_DIRECTORY_CACHE 8192

struct EsRoot {
    Directory directory;
    char *name;
    char identity[96];
    CachedDirectory **cache;
    size_t cache_count, cache_capacity;
    EsMap cache_index;
};

#ifndef _WIN32
static int checkout_descriptor(EsRoot *root) {
    return openat(root->directory.fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
#endif

/* The resolved parent directory of a relative path. The handle belongs to the root's cache.
   missing is set when an ancestor does not exist; leaf then holds the missing remainder. */
typedef struct {
    const Directory *parent;
    char leaf[ES_PATH_LIMIT];
    bool missing;
} Parent;

static SDL_SpinLock coordinator_init;
static SDL_Mutex *coordinator;

bool es_fail(char *error, size_t cap, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, cap, format, args);
    va_end(args);
    return false;
}

bool es_reserve(void **data, size_t *capacity, size_t count, size_t width) {
    if (count <= *capacity) {
        return true;
    }

    size_t next = *capacity ? *capacity : 16;

    while (next < count && next <= SIZE_MAX / 2) {
        next *= 2;
    }

    if (next < count || next > SIZE_MAX / width) {
        return false;
    }

    void *grown = realloc(*data, next * width);

    if (!grown) {
        return false;
    }

    *data = grown;
    *capacity = next;
    return true;
}

bool es_relative(const char *path) {
    if (!path || !*path || strlen(path) >= ES_PATH_LIMIT || strchr(path, '\\') ||
        strchr(path, ':') || path[0] == '/') {
        return false;
    }

    for (const char *p = path; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);

        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.') ||
            p[n - 1] == '.' || p[n - 1] == ' ') {
            return false;
        }

        for (size_t i = 0; i < n; ++i) {
            if ((unsigned char)p[i] < 32 || strchr("*?\"<>|", p[i])) {
                return false;
            }
        }

        if (end && !end[1]) {
            return false;
        }

        p += n + (end != NULL);
    }

    return true;
}

#ifdef _WIN32
static wchar_t *wide(const char *name) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
    wchar_t *out = size > 0 ? malloc((size_t)size * sizeof(*out)) : NULL;

    if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out, size)) {
        free(out);
        out = NULL;
    }

    return out;
}

static wchar_t *child_name(const Directory *dir, const char *leaf) {
    wchar_t *part = wide(leaf);

    if (!part) {
        return NULL;
    }

    size_t a = wcslen(dir->path), b = wcslen(part);
    wchar_t *path = a + b + 2 <= 32767 ? malloc((a + b + 2) * sizeof(*path)) : NULL;

    if (path) {
        memcpy(path, dir->path, a * sizeof(*path));
        path[a] = L'\\';
        memcpy(path + a + 1, part, (b + 1) * sizeof(*path));
    }

    free(part);
    return path;
}

static bool normal(HANDLE h, bool directory, BY_HANDLE_FILE_INFORMATION *info) {
    return h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_DISK &&
           GetFileInformationByHandle(h, info) &&
           !(info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           ((info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
           (directory || info->nNumberOfLinks == 1);
}

static bool directory_open(const wchar_t *path, Directory *out, bool *missing) {
    HANDLE h = CreateFileW(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD failure = GetLastError();
    BY_HANDLE_FILE_INFORMATION info;

    if (missing) {
        *missing = h == INVALID_HANDLE_VALUE &&
                   (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND);
    }

    if (!normal(h, true, &info)) {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }

        return false;
    }

    DWORD length = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
    wchar_t *name = length && length < 32767 ? malloc((length + 1) * sizeof(*name)) : NULL;

    if (!name || !GetFinalPathNameByHandleW(h, name, length + 1, FILE_NAME_NORMALIZED)) {
        free(name);
        CloseHandle(h);
        return false;
    }

    *out = (Directory){h, name};
    return true;
}

static void directory_close(Directory *dir) {
    CloseHandle(dir->handle);
    free(dir->path);
}

static bool directory_child(const Directory *dir, const char *name, Directory *out, bool *missing) {
    wchar_t *path = child_name(dir, name);
    bool ok = path && directory_open(path, out, missing);
    free(path);
    return ok;
}
#else
static void directory_close(Directory *dir) {
    close(dir->fd);
}

static bool directory_child(const Directory *dir, const char *name, Directory *out, bool *missing) {
    out->fd = openat(dir->fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    if (missing) {
        *missing = out->fd < 0 && errno == ENOENT;
    }

    return out->fd >= 0;
}
#endif

EsRoot *es_root_open(const char *path, char *error, size_t cap) {
    EsRoot *root = calloc(1, sizeof(*root));

    if (!root) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_ANCHOR);
        return NULL;
    }

#ifdef _WIN32
    wchar_t *input = path ? wide(path) : NULL;
    bool ok = input && directory_open(input, &root->directory, NULL);
    free(input);

    if (ok) {
        BY_HANDLE_FILE_INFORMATION info;
        ok = GetFileInformationByHandle(root->directory.handle, &info) != 0;
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root->directory.path, -1, NULL,
                                    0, NULL, NULL);
        root->name = n > 0 ? malloc((size_t)n) : NULL;
        ok = ok && root->name &&
             WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root->directory.path, -1,
                                 root->name, n, NULL, NULL);
        snprintf(root->identity, sizeof(root->identity), "%08lx:%08lx%08lx",
                 info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow);
        wchar_t volume[MAX_PATH];

        if (ok && GetVolumePathNameW(root->directory.path, volume, MAX_PATH)) {
            size_t root_length = wcslen(root->directory.path), volume_length = wcslen(volume);

            while (root_length && root->directory.path[root_length - 1] == L'\\') {
                root_length--;
            }

            while (volume_length && volume[volume_length - 1] == L'\\') {
                volume_length--;
            }

            ok = root_length != volume_length ||
                 _wcsnicmp(root->directory.path, volume, root_length) != 0;
        }

        if (!ok) {
            directory_close(&root->directory);
        }
    }
#else
    root->name = path ? realpath(path, NULL) : NULL;
    root->directory.fd = root->name ? open(root->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    struct stat info;
    bool ok =
        root->directory.fd >= 0 && strcmp(root->name, "/") && !fstat(root->directory.fd, &info);

    if (ok) {
        snprintf(root->identity, sizeof(root->identity), "%" PRIx64 ":%" PRIx64,
                 (uint64_t)info.st_dev, (uint64_t)info.st_ino);
    } else if (root->directory.fd >= 0) {
        close(root->directory.fd);
    }
#endif

    if (!ok) {
        free(root->name);
        free(root);
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_OPEN_REPOSITORY_DIRECTORY_S,
                path ? path : "");
        return NULL;
    }

    return root;
}

void es_root_close(EsRoot *root) {
    if (root) {
        for (size_t i = 0; i < root->cache_count; ++i) {
            if (root->cache[i]->open) {
                directory_close(&root->cache[i]->directory);
            }

            free(root->cache[i]->path);
            free(root->cache[i]);
        }

        free(root->cache);
        es_map_free(&root->cache_index);
        directory_close(&root->directory);
        free(root->name);
        free(root);
    }
}

/* Closes cached handles at and below path so the directory can be removed or replaced. */
static void forget_directory(EsRoot *root, const char *path) {
    size_t length = strlen(path);

    for (size_t i = 0; i < root->cache_count; ++i) {
        CachedDirectory *entry = root->cache[i];

        if (entry->open && !strncmp(entry->path, path, length) &&
            (!entry->path[length] || entry->path[length] == '/')) {
            directory_close(&entry->directory);
            entry->open = false;
        }
    }
}

static const Directory *cached_child(EsRoot *root, const Directory *parent, const char *prefix,
                                     const char *name, bool *missing) {
    size_t index;
    CachedDirectory *entry = NULL;

    if (es_map_get(&root->cache_index, prefix, &index)) {
        entry = root->cache[index];

        if (entry->open) {
            *missing = false;
            return &entry->directory;
        }
    } else {
        if (root->cache_count >= ES_DIRECTORY_CACHE ||
            !es_reserve((void **)&root->cache, &root->cache_capacity, root->cache_count + 1,
                        sizeof(*root->cache))) {
            *missing = false;
            return NULL;
        }

        entry = calloc(1, sizeof(*entry));
        char *copy = entry ? malloc(strlen(prefix) + 1) : NULL;

        if (!copy) {
            free(entry);
            *missing = false;
            return NULL;
        }

        strcpy(copy, prefix);
        entry->path = copy;

        if (!es_map_put(&root->cache_index, entry->path, root->cache_count)) {
            free(copy);
            free(entry);
            *missing = false;
            return NULL;
        }

        root->cache[root->cache_count++] = entry;
    }

    entry->open = directory_child(parent, name, &entry->directory, missing);
    return entry->open ? &entry->directory : NULL;
}

const char *es_root_name(const EsRoot *root) {
    return root->name;
}

const char *es_root_identity(const EsRoot *root) {
    return root->identity;
}

static bool parent_open(EsRoot *root, const char *path, Parent *parent, char *error, size_t cap) {
    *parent = (Parent){.parent = &root->directory};

    if (!es_relative(path)) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_UNSAFE_REPOSITORY_RELATIVE_PATH_S,
                       path ? path : "");
    }

    char prefix[ES_PATH_LIMIT];
    snprintf(prefix, sizeof(prefix), "%s", path);
    const char *part = path;

    for (const char *slash; (slash = strchr(part, '/')) != NULL; part = slash + 1) {
        size_t end = (size_t)(slash - path), offset = (size_t)(part - path);
        char name[ES_PATH_LIMIT];
        memcpy(name, part, end - offset);
        name[end - offset] = 0;
        prefix[end] = 0;
        const Directory *next = cached_child(root, parent->parent, prefix, name, &parent->missing);
        prefix[end] = '/';

        if (!next) {
            if (parent->missing) {
                snprintf(parent->leaf, sizeof(parent->leaf), "%s", part);
                return true;
            }

            return es_fail(error, cap, DM_TEXT_REPOSITORY_UNREADABLE_OR_LINKED_PARENT_DIRECTORY_S,
                           path);
        }

        parent->parent = next;
    }

    snprintf(parent->leaf, sizeof(parent->leaf), "%s", part);
    return true;
}

#ifdef _WIN32
static int64_t file_time(FILETIME time) {
    return (int64_t)(((uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime) * 100u);
}

static EsStat find_stat(const WIN32_FIND_DATAW *info) {
    return (EsStat){.exists = true,
                    .directory = (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    .size = (int64_t)((uint64_t)info->nFileSizeHigh << 32 | info->nFileSizeLow),
                    .mtime = file_time(info->ftLastWriteTime),
                    .mode = info->dwFileAttributes & FILE_ATTRIBUTE_READONLY ? 0444 : 0644};
}
#else
static EsStat posix_stat(const struct stat *info) {
#ifdef __APPLE__
    int64_t nanoseconds = (int64_t)info->st_mtimespec.tv_nsec;
#else
    int64_t nanoseconds = (int64_t)info->st_mtim.tv_nsec;
#endif
    return (EsStat){.exists = true,
                    .directory = S_ISDIR(info->st_mode),
                    .size = (int64_t)info->st_size,
                    .mtime = (int64_t)info->st_mtime * 1000000000 + nanoseconds,
                    .mode = (uint32_t)info->st_mode & 0777};
}
#endif

/* Reads a directory entry's metadata. Links and special files are refused. */
bool es_fs_stat(EsRoot *root, const char *path, EsStat *out, char *error, size_t cap) {
    Parent parent;
    *out = (EsStat){0};

    if (!parent_open(root, path, &parent, error, cap)) {
        return false;
    }

    if (parent.missing) {
        return true;
    }

    bool ok = true;
#ifdef _WIN32
    wchar_t *name = child_name(parent.parent, parent.leaf);
    WIN32_FIND_DATAW info;
    HANDLE find =
        name ? FindFirstFileExW(name, FindExInfoBasic, &info, FindExSearchNameMatch, NULL, 0)
             : INVALID_HANDLE_VALUE;
    DWORD failure = GetLastError();
    free(name);

    if (find != INVALID_HANDLE_VALUE) {
        FindClose(find);
        ok = !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        *out = find_stat(&info);
    } else {
        ok = failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND;
    }
#else
    struct stat info;

    if (!fstatat(parent.parent->fd, parent.leaf, &info, AT_SYMLINK_NOFOLLOW)) {
        ok = S_ISREG(info.st_mode) || S_ISDIR(info.st_mode);
        *out = posix_stat(&info);
    } else {
        ok = errno == ENOENT;
    }
#endif
    return ok ||
           es_fail(error, cap, DM_TEXT_REPOSITORY_UNREADABLE_OR_LINKED_REPOSITORY_ENTRY_S, path);
}

char *es_fs_destination_key(EsRoot *root, const char *path, char *error, size_t cap) {
    Parent parent;

    if (!parent_open(root, path, &parent, error, cap)) {
        return NULL;
    }

    char *key = NULL;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    wchar_t *existing = parent.missing ? NULL : child_name(parent.parent, parent.leaf);
    HANDLE file =
        existing ? CreateFileW(existing, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                 : INVALID_HANDLE_VALUE;
    DWORD failure = GetLastError();
    free(existing);

    if (file != INVALID_HANDLE_VALUE) {
        if (normal(file, false, &info)) {
            key = malloc(80);

            if (key) {
                snprintf(key, 80, "file:%08lx:%08lx%08lx", info.dwVolumeSerialNumber,
                         info.nFileIndexHigh, info.nFileIndexLow);
            }
        }

        CloseHandle(file);
        return key;
    }

    if (!parent.missing && failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_IDENTIFY_REPOSITORY_DESTINATION_S, path);
        return NULL;
    }

    wchar_t *name = wide(parent.leaf);

    struct {
        ULONG flags;
    } sensitive = {0};

    GetFileInformationByHandleEx(parent.parent->handle, (FILE_INFO_BY_HANDLE_CLASS)23, &sensitive,
                                 sizeof(sensitive));
    wchar_t *normalized = name;

    if (name && !(sensitive.flags & 1)) {
        int n =
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name, -1, NULL, 0, NULL, NULL, 0);
        normalized = n > 0 ? malloc((size_t)n * sizeof(*normalized)) : NULL;

        if (normalized && !LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name, -1,
                                         normalized, n, NULL, NULL, 0)) {
            free(normalized);
            normalized = NULL;
        }
    }

    int n = normalized ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized, -1, NULL, 0,
                                             NULL, NULL)
                       : 0;

    if (n > 0 && GetFileInformationByHandle(parent.parent->handle, &info)) {
        key = malloc((size_t)n + 64);

        if (key) {
            int prefix = snprintf(key, 64, "%08lx:%08lx%08lx:", info.dwVolumeSerialNumber,
                                  info.nFileIndexHigh, info.nFileIndexLow);

            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized, -1, key + prefix, n,
                                     NULL, NULL)) {
                free(key);
                key = NULL;
            }
        }
    }

    if (normalized != name) {
        free(normalized);
    }

    free(name);
#else
    struct stat info;

    if (!fstat(parent.parent->fd, &info)) {
        size_t n = strlen(parent.leaf) + 64;
        key = malloc(n);

        if (key) {
            snprintf(key, n, "%" PRIx64 ":%" PRIx64 ":%s", (uint64_t)info.st_dev,
                     (uint64_t)info.st_ino, parent.leaf);
        }
    }
#endif

    if (!key) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_IDENTIFY_REPOSITORY_DESTINATION_S, path);
    }

    return key;
}

bool es_fs_type(EsRoot *root, const char *path, bool *exists, bool *directory, char *error,
                size_t cap) {
    EsStat info;
    bool ok = es_fs_stat(root, path, &info, error, cap);
    *exists = info.exists;
    *directory = info.directory;
    return ok;
}

bool es_fs_read(EsRoot *root, const char *path, EsFile *out, char *error, size_t cap) {
    Parent parent;
    *out = (EsFile){0};

    if (!parent_open(root, path, &parent, error, cap)) {
        return false;
    }

    if (parent.missing) {
        return true;
    }

    bool missing = false, ok = false;
    size_t size = 0;
#ifdef _WIN32
    wchar_t *name = child_name(parent.parent, parent.leaf);
    HANDLE file = name ? CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                       : INVALID_HANDLE_VALUE;
    DWORD failure = GetLastError();
    free(name);
    missing = file == INVALID_HANDLE_VALUE && failure == ERROR_FILE_NOT_FOUND;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER length;

    if (normal(file, false, &info) && GetFileSizeEx(file, &length) && length.QuadPart >= 0 &&
        (uint64_t)length.QuadPart <= ES_FILE_LIMIT) {
        size = (size_t)length.QuadPart;
        out->data = malloc(size + 1);
        DWORD read = 0;
        ok = out->data && ReadFile(file, out->data, (DWORD)size, &read, NULL) && read == size;
        out->mode = info.dwFileAttributes & FILE_ATTRIBUTE_READONLY ? 0444 : 0644;
    }

    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
#else
    int file = openat(parent.parent->fd, parent.leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    missing = file < 0 && errno == ENOENT;
    struct stat info;

    if (file >= 0 && !fstat(file, &info) && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
        info.st_size >= 0 && (uint64_t)info.st_size <= ES_FILE_LIMIT) {
        size = (size_t)info.st_size;
        out->data = malloc(size + 1);
        size_t at = 0;

        while (out->data && at < size) {
            ssize_t n = read(file, out->data + at, size - at);

            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n <= 0) {
                break;
            }

            at += (size_t)n;
        }

        struct stat after;
        ok = out->data && at == size && !fstat(file, &after) && after.st_size == info.st_size &&
             after.st_mtime == info.st_mtime;
        out->mode = (uint32_t)info.st_mode & 0777;
    }

    if (file >= 0) {
        close(file);
    }
#endif

    if (missing) {
        return true;
    }

    if (!ok) {
        es_file_free(out);
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_CANNOT_READ_REGULAR_UNALIASED_REPOSITORY_FILE_S, path);
    }

    out->data[size] = 0;
    out->size = size;
    out->exists = true;
    return true;
}

static void stage_name(char name[96], uint64_t transaction, size_t index) {
    snprintf(name, 96, ".duckmidi-%" PRIu64 "-%zu.tmp", transaction, index);
}

bool es_fs_stage_available(EsRoot *root, const char *path, uint64_t transaction, size_t index,
                           char *error, size_t cap) {
    char temporary_path[ES_PATH_LIMIT], leaf[96];
    stage_name(leaf, transaction, index);
    const char *slash = strrchr(path, '/');
    int size = slash ? snprintf(temporary_path, sizeof(temporary_path), "%.*s/%s",
                                (int)(slash - path), path, leaf)
                     : snprintf(temporary_path, sizeof(temporary_path), "%s", leaf);
    bool exists = false, directory = false;
    return size > 0 && size < (int)sizeof(temporary_path) &&
           es_fs_type(root, temporary_path, &exists, &directory, error, cap) &&
           (!exists ||
            es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_STAGING_PATH_ALREADY_EXISTS_S,
                    temporary_path));
}

static bool stage_prefix(const EsFile *stage, const EsFile *expected) {
    return expected && expected->exists && stage->size <= expected->size &&
           (!stage->size || !memcmp(stage->data, expected->data, stage->size));
}

bool es_fs_stage_remove(EsRoot *root, const char *path, uint64_t transaction, size_t index,
                        const EsFile *expected, const EsFile *alternate, char *error, size_t cap) {
    char temporary_path[ES_PATH_LIMIT], leaf[96];
    stage_name(leaf, transaction, index);
    const char *slash = strrchr(path, '/');
    int size = slash ? snprintf(temporary_path, sizeof(temporary_path), "%.*s/%s",
                                (int)(slash - path), path, leaf)
                     : snprintf(temporary_path, sizeof(temporary_path), "%s", leaf);
    EsFile stage = {0};

    if (size < 0 || size >= (int)sizeof(temporary_path) ||
        !es_fs_read(root, temporary_path, &stage, error, cap)) {
        return false;
    }

    bool known = !stage.exists || stage_prefix(&stage, expected) || stage_prefix(&stage, alternate);
    es_file_free(&stage);

    if (!known) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_RECOVERY_STAGE_WAS_CHANGED_OUTSIDE_DUCKMIDI_S,
                       temporary_path);
    }

    Parent parent;

    if (!parent_open(root, path, &parent, error, cap)) {
        return false;
    }

    bool ok = true;

    if (!parent.missing) {
#ifdef _WIN32
        wchar_t *name = child_name(parent.parent, leaf);
        ok = name && (DeleteFileW(name) || GetLastError() == ERROR_FILE_NOT_FOUND);
        free(name);
#else
        ok = !unlinkat(parent.parent->fd, leaf, 0) || errno == ENOENT;
#endif
    }

    return ok ||
           es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_REMOVE_RECORDED_REPOSITORY_STAGE_S, path);
}

bool es_fs_write(EsRoot *root, const char *path, const EsFile *data, uint64_t transaction,
                 size_t index, char *error, size_t cap) {
    Parent parent;

    if (!parent_open(root, path, &parent, error, cap)) {
        return false;
    }

    if (parent.missing) {
        return !data->exists ||
               es_fail(error, cap, DM_TEXT_REPOSITORY_MISSING_PREPARED_PARENT_S, path);
    }

    char stage[96];
    stage_name(stage, transaction, index);
    bool ok = false;
#ifdef _WIN32
    wchar_t *destination = child_name(parent.parent, parent.leaf);
    wchar_t *temporary = child_name(parent.parent, stage);

    if (destination && temporary && !data->exists) {
        ok = DeleteFileW(destination) || GetLastError() == ERROR_FILE_NOT_FOUND;
    } else if (destination && temporary && data->size <= ES_FILE_LIMIT) {
        HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        DWORD wrote = 0;
        ok = file != INVALID_HANDLE_VALUE &&
             WriteFile(file, data->data, (DWORD)data->size, &wrote, NULL) && wrote == data->size &&
             FlushFileBuffers(file);

        if (file != INVALID_HANDLE_VALUE) {
            ok = CloseHandle(file) && ok;
        }

        if (ok) {
            ok = MoveFileExW(temporary, destination,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        }
    }

    free(temporary);
    free(destination);
#else
    if (!data->exists) {
        ok = !unlinkat(parent.parent->fd, parent.leaf, 0) || errno == ENOENT;
    } else if (data->size <= ES_FILE_LIMIT) {
        int file = openat(parent.parent->fd, stage,
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        size_t at = 0;

        while (file >= 0 && at < data->size) {
            ssize_t n = write(file, data->data + at, data->size - at);

            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n <= 0) {
                break;
            }

            at += (size_t)n;
        }

        ok = file >= 0 && at == data->size && !fchmod(file, data->mode ? data->mode : 0644) &&
             !fsync(file);

        if (file >= 0) {
            ok = !close(file) && ok;
        }

        if (ok) {
            ok = !renameat(parent.parent->fd, stage, parent.parent->fd, parent.leaf);
        }
    }

    if (ok && fsync(parent.parent->fd) && errno != EINVAL && errno != ENOTSUP) {
        ok = false;
    }
#endif
    return ok || es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_APPLY_REPOSITORY_FILE_S, path);
}

bool es_fs_mkdir(EsRoot *root, const char *path, char *error, size_t cap) {
    Parent parent;

    if (!parent_open(root, path, &parent, error, cap)) {
        return false;
    }

    bool ok = false;

    if (!parent.missing) {
#ifdef _WIN32
        wchar_t *name = child_name(parent.parent, parent.leaf);
        ok = name && CreateDirectoryW(name, NULL);
        free(name);
#else
        ok = !mkdirat(parent.parent->fd, parent.leaf, 0755);

        if (ok && fsync(parent.parent->fd) && errno != EINVAL && errno != ENOTSUP) {
            ok = false;
        }
#endif
    }

    return ok || es_fail(error, cap,
                         DM_TEXT_REPOSITORY_CANNOT_CREATE_RECORDED_REPOSITORY_DIRECTORY_S, path);
}

bool es_fs_rmdir(EsRoot *root, const char *path, char *error, size_t cap) {
    Parent parent;

    if (!parent_open(root, path, &parent, error, cap)) {
        return false;
    }

    forget_directory(root, path);
    bool ok = parent.missing;

    if (!parent.missing) {
#ifdef _WIN32
        wchar_t *name = child_name(parent.parent, parent.leaf);
        ok = name &&
             (RemoveDirectoryW(name) || GetLastError() == ERROR_FILE_NOT_FOUND ||
              GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_DIR_NOT_EMPTY);
        free(name);
#else
        ok = !unlinkat(parent.parent->fd, parent.leaf, AT_REMOVEDIR) || errno == ENOENT ||
             errno == ENOTEMPTY;

        if (ok && fsync(parent.parent->fd) && errno != EINVAL && errno != ENOTSUP) {
            ok = false;
        }
#endif
    }

    return ok ||
           es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_REMOVE_RECORDED_EMPTY_DIRECTORY_S, path);
}

typedef struct {
    EsRoot *root;
    EsFsVisit visit;
    void *user;
    bool recursive;
    char *error;
    size_t cap;
} Scan;

static bool scan_directory(Scan *scan, const char *prefix, unsigned depth);

/* Entry metadata comes from the enumeration itself; no per-entry open is needed. */
static bool scan_entry(Scan *scan, const char *prefix, const char *name, const EsStat *info,
                       bool linked, unsigned depth) {
    char path[ES_PATH_LIMIT];

    if (snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", name) >=
        (int)sizeof(path)) {
        return es_fail(scan->error, scan->cap,
                       DM_TEXT_REPOSITORY_AUDIO_INVENTORY_PATH_EXCEEDS_ITS_LIMIT);
    }

    if (linked) {
        return es_fail(scan->error, scan->cap,
                       DM_TEXT_REPOSITORY_UNREADABLE_OR_LINKED_REPOSITORY_ENTRY_S, path);
    }

    if (info->directory) {
        return !scan->recursive || scan_directory(scan, path, depth + 1);
    }

    return scan->visit(scan->user, path, info) ||
           es_fail(scan->error, scan->cap, DM_TEXT_REPOSITORY_AUDIO_INVENTORY_EXCEEDS_ITS_LIMIT);
}

static bool scan_directory(Scan *scan, const char *prefix, unsigned depth) {
    if (depth > 32) {
        return es_fail(scan->error, scan->cap, DM_TEXT_REPOSITORY_REPOSITORY_PATH_NESTING_LIMIT_S,
                       prefix);
    }

    const Directory *dir = &scan->root->directory;

    if (*prefix) {
        EsStat info;

        if (!es_fs_stat(scan->root, prefix, &info, scan->error, scan->cap)) {
            return false;
        }

        if (!info.exists) {
            return true;
        }

        if (!info.directory) {
            return es_fail(scan->error, scan->cap, DM_TEXT_REPOSITORY_EXPECTED_AUDIO_DIRECTORY_S,
                           prefix);
        }

        Parent parent;
        bool missing = false;

        if (!parent_open(scan->root, prefix, &parent, scan->error, scan->cap)) {
            return false;
        }

        dir = parent.missing
                  ? NULL
                  : cached_child(scan->root, parent.parent, prefix, parent.leaf, &missing);

        if (!dir) {
            return missing ||
                   es_fail(scan->error, scan->cap,
                           DM_TEXT_REPOSITORY_CANNOT_INVENTORY_ANCHORED_AUDIO_DIRECTORY_S, prefix);
        }
    }

    bool ok = true;
#ifdef _WIN32
    wchar_t *pattern = child_name(dir, "*");
    WIN32_FIND_DATAW info;
    HANDLE find = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &info, FindExSearchNameMatch,
                                             NULL, FIND_FIRST_EX_LARGE_FETCH)
                          : INVALID_HANDLE_VALUE;
    free(pattern);
    ok = find != INVALID_HANDLE_VALUE || GetLastError() == ERROR_FILE_NOT_FOUND;

    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(info.cFileName, L".") || !wcscmp(info.cFileName, L"..")) {
                continue;
            }

            int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info.cFileName, -1, NULL, 0,
                                        NULL, NULL);
            char *name = n > 0 ? malloc((size_t)n) : NULL;
            EsStat entry = find_stat(&info);
            ok = name &&
                 WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info.cFileName, -1, name, n,
                                     NULL, NULL) &&
                 scan_entry(scan, prefix, name, &entry,
                            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0, depth);
            free(name);
        } while (ok && FindNextFileW(find, &info));

        ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
        FindClose(find);
    }
#else
    int copy = openat(dir->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *listing = copy >= 0 ? fdopendir(copy) : NULL;
    ok = listing != NULL;

    if (!listing && copy >= 0) {
        close(copy);
    }

    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(listing);

        if (!entry) {
            ok = !errno;
            break;
        }

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        struct stat info;
        ok = !fstatat(dir->fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW);

        if (ok) {
            EsStat stat = posix_stat(&info);
            ok = scan_entry(scan, prefix, entry->d_name, &stat,
                            !S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode), depth);
        }
    }

    if (listing) {
        closedir(listing);
    }
#endif
    return ok ||
           (*scan->error
                ? false
                : es_fail(scan->error, scan->cap,
                          DM_TEXT_REPOSITORY_CANNOT_INVENTORY_ANCHORED_AUDIO_DIRECTORY_S, prefix));
}

bool es_fs_scan(EsRoot *root, const char *path, bool recursive, EsFsVisit visit, void *user,
                char *error, size_t cap) {
    if (error && cap) {
        *error = 0;
    }

    Scan scan = {root, visit, user, recursive, error, cap};
    return scan_directory(&scan, path ? path : "", 0);
}

typedef struct {
    EsRoot *root;
    Directory history;
    char *database;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
} Writer;

void *es_writer_lock(const char *plugins, char *error, size_t cap) {
    SDL_LockSpinlock(&coordinator_init);

    if (!coordinator) {
        coordinator = SDL_CreateMutex();
    }

    SDL_UnlockSpinlock(&coordinator_init);

    if (!coordinator || !SDL_TryLockMutex(coordinator)) {
        es_fail(error, cap,
                DM_TEXT_REPOSITORY_ANOTHER_REPOSITORY_OPERATION_IS_RUNNING_TRY_AGAIN_WHEN_IT);
        return NULL;
    }

    Writer *writer = calloc(1, sizeof(*writer));

    if (!writer) {
        SDL_UnlockMutex(coordinator);
        return NULL;
    }

#ifdef _WIN32
    writer->handle = INVALID_HANDLE_VALUE;
#else
    writer->fd = -1;
    writer->history.fd = -1;
#endif
    writer->root = es_root_open(plugins, error, cap);
    bool exists = false, directory = false;
    bool ok = writer->root && es_fs_type(writer->root, "history", &exists, &directory, error, cap);

    if (ok && !exists) {
        ok = es_fs_mkdir(writer->root, "history", error, cap);
    } else if (ok) {
        ok = directory;
    }

    Directory dir;

    if (ok) {
        ok = directory_child(&writer->root->directory, "history", &dir, NULL);

        if (ok) {
#ifdef __linux__
            struct statfs volume;
            ok = !fstatfs(dir.fd, &volume) && volume.f_type != 0x6969 &&
                 volume.f_type != 0xff534d42 && volume.f_type != 0xfe534d42 &&
                 volume.f_type != 0x517B && volume.f_type != 0x564c;
#endif
#ifdef _WIN32
            wchar_t volume[MAX_PATH];
            ok = ok && GetVolumePathNameW(dir.path, volume, MAX_PATH) &&
                 GetDriveTypeW(volume) != DRIVE_REMOTE &&
                 wcsncmp(dir.path, L"\\\\?\\UNC\\", 8) != 0;
            wchar_t *path = child_name(&dir, "writer.lock");
            writer->handle = path ? CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                                OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                                  : INVALID_HANDLE_VALUE;
            free(path);
            BY_HANDLE_FILE_INFORMATION info;
            ok = ok && normal(writer->handle, false, &info);
#else
            writer->fd =
                openat(dir.fd, "writer.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
            struct stat info;
            ok = ok && writer->fd >= 0 && !fstat(writer->fd, &info) && S_ISREG(info.st_mode) &&
                 info.st_nlink == 1 && info.st_uid == getuid() &&
                 !fcntl(writer->fd, F_SETLK, &lock);
#endif
            if (ok) {
                char database[8192];
#ifdef __linux__
                snprintf(database, sizeof(database), "/proc/self/fd/%d/repositories.sqlite3",
                         dir.fd);
#else
                snprintf(database, sizeof(database), "%s/history/repositories.sqlite3",
                         es_root_name(writer->root));
#endif
                writer->database = SDL_strdup(database);
                ok = writer->database != NULL;
            }

            if (ok) {
                writer->history = dir;
            } else {
                directory_close(&dir);
            }
        }
    }

    if (!ok) {
        es_writer_unlock(writer);
        es_fail(error, cap,
                DM_TEXT_REPOSITORY_CANNOT_ACQUIRE_LOCAL_PLUGIN_HISTORY_WRITER_S_HISTORY_WRITER,
                plugins);
        return NULL;
    }

    return writer;
}

const char *es_writer_database(void *opaque) {
    Writer *writer = opaque;
    return writer ? writer->database : NULL;
}

void es_writer_unlock(void *opaque) {
    Writer *writer = opaque;

    if (writer) {
#ifdef _WIN32
        if (writer->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(writer->handle);
        }
#else
        if (writer->fd >= 0) {
            close(writer->fd);
        }
#endif
        es_root_close(writer->root);

        if (writer->database) {
            directory_close(&writer->history);
        }

        SDL_free(writer->database);
        free(writer);
        SDL_UnlockMutex(coordinator);
    }
}
