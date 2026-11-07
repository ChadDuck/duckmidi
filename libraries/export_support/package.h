#ifndef DES_PACKAGE_H
#define DES_PACKAGE_H

#include "export_support.h"

/* Pure source package: the probe and target inputs are read-only; no ledger is opened. */
DES_API bool es_package_plan(EsRepository *probe, const EsModifications *, const DmeHost *, char *,
                             size_t);

#endif
