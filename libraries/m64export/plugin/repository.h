#ifndef M64_REPOSITORY_H
#define M64_REPOSITORY_H

#include "m64_plugin.h"
#include "../../export_support/export_support.h"
#include "../../export_support/recipe.h"

extern const DmeHistoryAdapter m64_history_adapter;
/* Public-signature adapter callbacks over this module. */
int32_t DME_CALL m64_history_detect(const DmeHistoryApi *, DmeHistoryRoot *, DmeHistoryTarget *,
                                    char *, uint64_t);
int32_t DME_CALL m64_history_inventory(const DmeHistoryApi *, DmeHistory *, DmeHistoryList *,
                                       char *, uint64_t);
int32_t DME_CALL m64_history_allow(const char *);
int32_t DME_CALL m64_history_render(const DmeHistoryApi *, DmeHistory *, const DmeHistoryIntent *,
                                    uint32_t, DmeHistoryPlan *, char *, uint64_t);
int32_t DME_CALL m64_history_targets(const DmeHistoryApi *, DmeHistory *, DmeHistoryTargets *,
                                     char *, uint64_t);
bool m64_repository_targets(EsRepository *, EsTargets *, char *, size_t);
bool m64_repository_sequence(M64PluginJob *, EsRepository *, char *, size_t);
bool m64_repository_bank(M64PluginJob *, EsRepository *, char *, size_t);
bool m64_repository_sample_targets(M64PluginJob *, EsRepository *, const DmeContext *,
                                   const DmeHost *, char *, size_t);
bool m64_repository_apply(M64PluginJob *, EsRepository *, const M64Result *, char *, size_t);
bool m64_repository_package(M64PluginJob *, EsRepository *, const M64Result *, const DmeHost *,
                            char *, size_t);

#endif
