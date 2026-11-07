#ifndef TEST_REPOSITORY_INSTALL_H
#define TEST_REPOSITORY_INSTALL_H

#include "../include/duckmidi_repository_plugin.h"
#include <stddef.h>

/* Export to Repository through the history runtime, as the editor runs it: preflight, then the
   durable install, then finish on the exporter's settings instance. */
int32_t repository_install(const DmeExporter *exporter, void *settings, const DmeContext *,
                           const DmeHost *, const char *plugins, const char *checkout, char *error,
                           size_t capacity);

/* Reverts every recorded change of the checkout's history scope. */
int32_t repository_revert_all(const DmeExporter *exporter, void *settings, const DmeHost *,
                              const char *plugins, const char *checkout, char *error,
                              size_t capacity);

/* A checkout file's content with a terminator, or NULL when it is absent. Free with SDL_free. */
char *checkout_read(const char *checkout, const char *path);
void checkout_write(const char *checkout, const char *path, const char *text);

/* A new empty folder under agents/reports for one scenario. */
void scenario_folder(char *out, size_t capacity, const char *group, const char *name);

#endif
