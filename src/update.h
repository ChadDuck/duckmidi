/* Update checks against the duckmidi GitHub releases, with a staged install that runs after the
   editor closes. Network work runs on a background thread; see update.c. */
#ifndef DM_UPDATE_H
#define DM_UPDATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DM_UPDATE_IDLE,
    DM_UPDATE_CHECKING,
    DM_UPDATE_CURRENT,     /* This version is the latest release. */
    DM_UPDATE_AVAILABLE,   /* A newer release can be downloaded. */
    DM_UPDATE_DOWNLOADING, /* Downloading and unpacking the release. */
    DM_UPDATE_READY,       /* Unpacked; installs when duckmidi closes. */
    DM_UPDATE_FAILED
} DmUpdateState;

typedef struct {
    DmUpdateState state;
    char version[32];  /* Latest release version, without the leading v. */
    char message[512]; /* Failure reason. */
    float progress;    /* Download progress, 0 to 1. */
} DmUpdateStatus;

typedef struct DmUpdate DmUpdate;

/* base is the folder holding duckmidi.exe, ending with a separator. */
DmUpdate *dm_update_create(const char *base, const char *current_version);
void dm_update_destroy(DmUpdate *u);
bool dm_update_check(DmUpdate *u);
bool dm_update_download(DmUpdate *u);
DmUpdateStatus dm_update_status(DmUpdate *u);

/* Starts the install script, which waits for this process to exit. sample_hash is the sample
   project's fingerprint when it was first opened (0 if unknown); an edited sample is kept.
   new_sample_hash receives the installed sample's fingerprint, or 0 if it was kept. */
bool dm_update_install(DmUpdate *u, bool restart, uint64_t sample_hash, uint64_t *new_sample_hash,
                       char *error, size_t capacity);

/* Release helpers, independent of the network. */
int dm_version_compare(const char *a, const char *b);
bool dm_release_parse(const char *json, bool full, char *version, size_t version_capacity,
                      char *url, size_t url_capacity);
uint64_t dm_file_fingerprint(const char *path);
#endif
