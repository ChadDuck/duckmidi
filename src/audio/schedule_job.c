#include "audio/internal.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    bool valid;
    DmProject project;
    bool has_preview;
    DmPatternPreview preview;
    uint64_t session;
    uint64_t revision;
    uint64_t seq;
} ScheduleRequest;

typedef struct {
    bool valid;
    bool ok;
    DmSchedule schedule;
    DmProject project;
    bool has_preview;
    DmPatternPreview preview;
    uint64_t session;
    uint64_t revision;
    char error[512];
} ScheduleResult;

struct ScheduleJob {
    SDL_Mutex *lock;
    SDL_Condition *cond;
    SDL_Thread *thread;
    bool quit;
    uint64_t latest_seq;
    /* Requests up to this one were cancelled by a synchronous publish or a reset: their builds
       are dropped. Newer requests waiting do not drop a finished build; the editor publishes it
       when it is newer than what plays. */
    uint64_t cancelled_seq;
    ScheduleRequest pending;
    ScheduleResult result;
};

static void clear_request(ScheduleRequest *r) {
    if (!r->valid) {
        return;
    }

    dm_project_free(&r->project);
    *r = (ScheduleRequest){0};
}

static void clear_result(ScheduleResult *r) {
    if (!r->valid) {
        return;
    }

    dm_schedule_free(&r->schedule);
    dm_project_free(&r->project);
    *r = (ScheduleResult){0};
}

static int SDLCALL schedule_worker(void *userdata) {
    ScheduleJob *j = userdata;

    for (;;) {
        SDL_LockMutex(j->lock);

        while (!j->quit && !j->pending.valid) {
            SDL_WaitCondition(j->cond, j->lock);
        }

        if (j->quit) {
            SDL_UnlockMutex(j->lock);
            break;
        }

        ScheduleRequest req = j->pending;
        j->pending = (ScheduleRequest){0};
        SDL_UnlockMutex(j->lock);

        DmSchedule next = {0};
        char error[512] = "";
        bool ok = dm_audio_build_schedule(&req.project, req.has_preview ? &req.preview : NULL,
                                          &next, error, sizeof(error));

        SDL_LockMutex(j->lock);

        if (j->quit || req.seq <= j->cancelled_seq) {
            SDL_UnlockMutex(j->lock);
            dm_schedule_free(&next);
            dm_project_free(&req.project);
            continue;
        }

        clear_result(&j->result);
        j->result.valid = true;
        j->result.ok = ok;
        j->result.schedule = next;
        j->result.project = req.project;
        j->result.has_preview = req.has_preview;
        j->result.preview = req.preview;
        j->result.session = req.session;
        j->result.revision = req.revision;
        snprintf(j->result.error, sizeof(j->result.error), "%s", error);
        SDL_UnlockMutex(j->lock);
    }

    return 0;
}

static ScheduleJob *ensure_job(DmAudio *a) {
    if (a->schedule_job) {
        return a->schedule_job;
    }

    ScheduleJob *j = calloc(1, sizeof(*j));

    if (!j) {
        return NULL;
    }

    j->lock = SDL_CreateMutex();
    j->cond = SDL_CreateCondition();

    if (!j->lock || !j->cond) {
        if (j->cond) {
            SDL_DestroyCondition(j->cond);
        }

        if (j->lock) {
            SDL_DestroyMutex(j->lock);
        }

        free(j);
        return NULL;
    }

    j->thread = SDL_CreateThread(schedule_worker, "dm_schedule", j);

    if (!j->thread) {
        SDL_DestroyCondition(j->cond);
        SDL_DestroyMutex(j->lock);
        free(j);
        return NULL;
    }

    a->schedule_job = j;
    return j;
}

void dm_audio_schedule_free(DmAudio *a) {
    if (!a || !a->schedule_job) {
        return;
    }

    ScheduleJob *j = a->schedule_job;
    SDL_LockMutex(j->lock);
    j->quit = true;
    j->cancelled_seq = ++j->latest_seq;
    clear_request(&j->pending);
    clear_result(&j->result);
    SDL_SignalCondition(j->cond);
    SDL_UnlockMutex(j->lock);

    SDL_WaitThread(j->thread, NULL);
    SDL_DestroyCondition(j->cond);
    SDL_DestroyMutex(j->lock);
    free(j);
    a->schedule_job = NULL;
}

bool dm_audio_update_async(DmAudio *a, const DmProject *p, const DmPatternPreview *preview,
                           uint64_t session, uint64_t revision) {
    if (!a || !p) {
        return false;
    }

    DmProject copy = {0};

    if (!dm_project_copy(&copy, p)) {
        return false;
    }

    ScheduleJob *j = ensure_job(a);

    if (!j) {
        dm_project_free(&copy);
        return false;
    }

    SDL_LockMutex(j->lock);
    uint64_t seq = ++j->latest_seq;
    clear_request(&j->pending);
    j->pending.valid = true;
    j->pending.project = copy;
    j->pending.has_preview = preview != NULL;

    if (preview) {
        j->pending.preview = *preview;
    }

    j->pending.session = session;
    j->pending.revision = revision;
    j->pending.seq = seq;
    SDL_SignalCondition(j->cond);
    SDL_UnlockMutex(j->lock);
    return true;
}

int dm_audio_update_poll(DmAudio *a, uint64_t session, uint64_t revision,
                         DmAudioPublished *published, char *error, size_t cap) {
    if (!a || !a->schedule_job) {
        return 0;
    }

    ScheduleJob *j = a->schedule_job;
    SDL_LockMutex(j->lock);

    if (!j->result.valid) {
        SDL_UnlockMutex(j->lock);
        return 0;
    }

    ScheduleResult res = j->result;
    j->result = (ScheduleResult){0};
    SDL_UnlockMutex(j->lock);

    /* Builds finish in request order, so a result is newer than anything published before it. */
    if (res.session != session || res.revision > revision) {
        clear_result(&res);
        return 0;
    }

    if (!res.ok) {
        if (error && cap) {
            snprintf(error, cap, "%s", res.error);
        }

        clear_result(&res);
        return -1;
    }

    double seconds = dm_audio_position(a);
    bool playing = dm_audio_playing(a);
    bool ok = dm_audio_publish_schedule(a, &res.project, res.has_preview ? &res.preview : NULL,
                                        res.schedule, seconds, playing, true, error, cap);
    if (ok && published) {
        *published = (DmAudioPublished){res.revision, res.project};
    } else {
        dm_project_free(&res.project);
    }

    return ok ? 1 : -1;
}

void dm_audio_update_cancel(DmAudio *a) {
    if (!a || !a->schedule_job) {
        return;
    }

    ScheduleJob *j = a->schedule_job;
    SDL_LockMutex(j->lock);
    j->cancelled_seq = ++j->latest_seq;
    clear_request(&j->pending);
    clear_result(&j->result);
    SDL_UnlockMutex(j->lock);
}
