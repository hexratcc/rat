#ifndef _RATCC_MALLOC_H
#define _RATCC_MALLOC_H

#include <stdlib.h>

void *memalign(size_t alignment, size_t size);
void *valloc(size_t size);
size_t malloc_usable_size(void *ptr);

#endif
