#ifndef DM_EDITOR_PREFERENCES_FILE_H
#define DM_EDITOR_PREFERENCES_FILE_H
/* The preferences file (duckmidi-settings.ini): reading, writing and shortcut names. The
   Preferences window is editor/preferences.c. */
#include "editor/editor.h"
bool dm_binding_parse(const char *text, Binding *binding);
bool dm_binding_format(Binding binding, char *out, size_t cap);
bool dm_preferences_load(Editor *e);
bool dm_preferences_save(Editor *e);
#endif
