#ifndef DM_PREFERENCES_H
#define DM_PREFERENCES_H
#include "editor.h"
bool dm_binding_parse(const char *text,Binding *binding);
bool dm_binding_format(Binding binding,char *out,size_t cap);
bool dm_preferences_load(Editor *e, uint32_t shortcut_schema);
bool dm_preferences_save(Editor *e, uint32_t shortcut_schema, const char *const *names);
#endif
