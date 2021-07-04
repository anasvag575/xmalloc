#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <stddef.h>

/* Guards for compilation with C++ */
#ifdef __cplusplus
    #define _BEGIN_DECLS_ extern "C" {
    #define _END_DECLS_         }
#else
    #define _BEGIN_DECLS_
    #define _END_DECLS_
#endif

_BEGIN_DECLS_

/* External routines */
void *malloc(size_t sz);
void *calloc(size_t nmemb, size_t sz);
void *realloc(void *obj, size_t sz);
void free(void *obj);
void malloc_debug_stats(void);

_END_DECLS_

#endif
