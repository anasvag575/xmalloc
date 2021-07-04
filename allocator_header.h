#ifndef _ALLOCATOR_HEADER_H
#define _ALLOCATOR_HEADER_H

/* Global header for all allocator sub-headers */
#include "allocator_internal.h"

/* Layout of a header
 *
 * |S/L (1)| PageOff (N) | Security (7-N)| <=> Total 1 byte
 *
 * - S/L: Small or large allocation (1 bit).
 * - PageOff: Distance in pages from the start of the pageblock (N bits),
 *            this can be (1^N - 1) pages at most and is only valid for small
 *            allocations.
 * - Security: Verifications bits, left for double frees or corruption (7-N bits).
 * */

/* Sum of these HAS to be 8 bits (or 1 byte) */
#define HEADER_TOTAL_BITS       8
#define HEADER_TYPE_BITS        1
#define HEADER_PAGE_OFF_BITS    (2 + PAGE_MULTIPLIER)
#define HEADER_SECURITY_BITS    (HEADER_TOTAL_BITS - HEADER_TYPE_BITS - HEADER_PAGE_OFF_BITS)
#define GEN_MASK(x)             ((1 << (x)) - 1)

/* Activates strict header validation or atomic stores for the headers */
//#define HEADER_VALIDATION_ACTIVE
//#define HEADER_ATOMIC_STORE_ACTIVE

/* Verification in case of changes */
#if !((HEADER_TOTAL_BITS) > (HEADER_PAGE_OFF_BITS))
    #error Header is not 1 byte!!!
#endif

/* These won't change and stay the same */
#define HEADER_SMALL            0x00
#define HEADER_LARGE            0x80
#define HEADER_TYPE_MASK        0x80
#define HEADER_TYPE_SHIFT       7
#define SECURITY_OPCODE         0xFF

/* Create the parametric shifts/masks for security and page offsets */
#define HEADER_PAGE_OFF_SHIFT   (HEADER_SECURITY_BITS)
#define HEADER_PAGE_OFF_MASK    (GEN_MASK(HEADER_PAGE_OFF_BITS) << HEADER_PAGE_OFF_SHIFT)
#define HEADER_VALID_MASK       (GEN_MASK(HEADER_SECURITY_BITS))
#define HEADER_VALID            (SECURITY_OPCODE & HEADER_VALID_MASK)

/* Large allocation header manipulation */
#define LARGE_HEADER_SIZE               16
#define GET_LARGER_ALLOC_START(obj)     ((void *)(((char *) obj) - LARGE_HEADER_SIZE))
#define GET_LARGER_ALLOC_SZ(obj)        (*((size_t *)(((char *) obj) - LARGE_HEADER_SIZE)))

/* Macros for small header manipulation */
#define GET_HEADER(ptr)             (*(((char *) (ptr)) - 1))
#define HEADER_PAGE_OFFSET_SET(x)	(((char)(x)) << HEADER_PAGE_OFF_SHIFT)
#define HEADER_PAGE_OFFSET_GET(x)   ((((char)(x)) & HEADER_PAGE_OFF_MASK) >> HEADER_PAGE_OFF_SHIFT)
#define HEADER_PAGE_GET_TYPE(x)     ((((char)(x)) & HEADER_TYPE_MASK) >> HEADER_TYPE_SHIFT)
#define HEADER_IS_BLOCK_VALID(x)    ((((char)(x)) & HEADER_VALID_MASK) == HEADER_VALID)

#ifdef HEADER_ATOMIC_STORE_ACTIVE
    /* Atomically write header */
    #define WRITE_HEADER(ptr, header)            \
            do{                                         \
                ATOMIC_STORE((ptr), (header));          \
            }while(0)

    #define WRITE_LARGE_HEADER_SZ(ptr, sz)       \
            do{                                         \
                ATOMIC_STORE((ptr), (sz));              \
            }while(0)
#else
    /* Non-atomic writes */
    #define WRITE_LARGE_HEADER_SZ(ptr, sz) (*(ptr) = *(sz))
    #define WRITE_HEADER(ptr, header) (*(ptr) = *(header))
#endif

#ifdef HEADER_VALIDATION_ACTIVE
    /* Set/reset the validity of an object */
    #define HEADER_INVALIDATE(ptr)  \
            do{                             \
             header_t __local_header = ((GET_HEADER((ptr))) & ~HEADER_VALID_MASK) | (HEADER_VALID_MASK & ~HEADER_VALID);    \
             WRITE_HEADER(((char *)(ptr)) - 1, &__local_header);                                                     \
            }while(0)

    #define HEADER_VALIDATE(ptr)  \
            do{                             \
             header_t __local_header = ((GET_HEADER((ptr))) & ~HEADER_VALID_MASK) | (HEADER_VALID_MASK & HEADER_VALID);     \
             WRITE_HEADER(((char *)(ptr)) - 1, &__local_header);                                                     \
            }while(0)
#else
    /* Empty calls - Used for debugging purposes */
    #define HEADER_INVALIDATE(ptr)
    #define HEADER_VALIDATE(ptr)
#endif


#endif
