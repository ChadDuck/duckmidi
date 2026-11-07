#ifndef OOT_REPOSITORY_H
#define OOT_REPOSITORY_H

#include "oot_plugin.h"
#include "../../export_support/export_support.h"
#include "../../export_support/recipe.h"

extern const DmeHistoryAdapter oot_history_adapter;
/* Public-signature adapter callbacks over this module. */
int32_t DME_CALL oot_history_detect(const DmeHistoryApi *, DmeHistoryRoot *, DmeHistoryTarget *,
                                    char *, uint64_t);
int32_t DME_CALL oot_history_inventory(const DmeHistoryApi *, DmeHistory *, DmeHistoryList *,
                                       char *, uint64_t);
int32_t DME_CALL oot_history_allow(const char *);
int32_t DME_CALL oot_history_render(const DmeHistoryApi *, DmeHistory *, const DmeHistoryIntent *,
                                    uint32_t, DmeHistoryPlan *, char *, uint64_t);
int32_t DME_CALL oot_history_targets(const DmeHistoryApi *, DmeHistory *, DmeHistoryTargets *,
                                     char *, uint64_t);
bool oot_repository_targets(EsRepository *, EsTargets *, char *, size_t);
bool oot_repository_sample_targets(OotPluginJob *, EsRepository *, const DmeContext *,
                                   const DmeHost *, char *, size_t);
bool oot_repository_apply(OotPluginJob *, EsRepository *, const OotResult *, char *, size_t);
bool oot_repository_package(OotPluginJob *, EsRepository *, const OotResult *, const DmeHost *,
                            char *, size_t);

#endif
