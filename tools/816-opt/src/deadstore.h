/* deadstore.h -- dead pseudo-register store removal (816-tcc fork). CC0. */
#ifndef DEADSTORE_H
#define DEADSTORE_H

#include "helpers.h"

/* removes stores to tcc__rN / tcc__fN that are never read; returns how many */
size_t deadStores(dynArray *file);
/* a value parked in a pseudo-register for one use, forwarded to it;
   returns how many */
size_t forwardOperands(dynArray *file);
/* a load of what A, X or Y already holds, dropped; returns how many */
size_t knownRegs(dynArray *file);
/* a computation repeated from the same sources, dropped for the first's
   register; returns how many */
size_t reuseBlocks(dynArray *file);

#endif
