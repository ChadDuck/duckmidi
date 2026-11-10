#ifndef DM_SAMPLE_IDS_H
#define DM_SAMPLE_IDS_H

#include <stdbool.h>

enum { DM_SAMPLE_ID_CAP = 49 };

bool dm_sample_id_valid(const char *text);

#endif
