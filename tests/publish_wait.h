/* Waits until the schedule of the latest edit queued on the background worker is published, as
   the editor's next frames would; an earlier edit's build may publish first. Needs
   editor/internal.h. */
#ifndef DUCKMIDI_TEST_PUBLISH_WAIT_H
#define DUCKMIDI_TEST_PUBLISH_WAIT_H
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static inline void test_publish_wait(Editor *e) {
    if (!e || !e->active || !e->audio) {
        fprintf(stderr, "FAIL background schedule publish: no active editor with audio\n");
        exit(1);
    }

    uint64_t start = SDL_GetTicks();

    while (e->published_revision != e->publish_revision) {
        int status = ed_publish_poll(e);

        if (status < 0) {
            fprintf(stderr, "FAIL background schedule publish failed: %s\n", e->status);
            exit(1);
        }

        if (SDL_GetTicks() - start >= 5000) {
            fprintf(stderr, "FAIL background schedule publish timed out\n");
            exit(1);
        }

        SDL_Delay(1);
    }
}
#endif
