/* deadstore.h -- dead pseudo-register store removal (816-tcc fork). CC0. */
#ifndef DEADSTORE_H
#define DEADSTORE_H

#include "helpers.h"

/* removes stores to tcc__rN / tcc__fN that are never read; returns how many */
size_t deadStores(dynArray *file);

#endif
