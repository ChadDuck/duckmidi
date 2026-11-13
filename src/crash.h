#ifndef DM_CRASH_H
#define DM_CRASH_H
#include <stdbool.h>

bool dm_crash_install(const char *settings_folder);
/* UI-thread only. Records a bounded, timestamped status message for the next crash report. */
void dm_crash_note(const char *text);
#endif
