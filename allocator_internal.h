#ifndef _ALLOCATOR_INTERNAL_H
#define _ALLOCATOR_INTERNAL_H

/* Standard library inclusions */
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

/* Kernel mmap/munmap */
#include <sys/mman.h>

/* Atomic operations */
#include "atomic.h"

/* Class info */
#define CLASS_SMALL         0
#define CLASS_LARGE         1
#define CLASS_NUM           64

/* Page information */
#define CLASS_PAGES_NUM         3
#define PAGE_BITS               12
#define PAGE_SZ                 (1 << PAGE_BITS)        /* Default page size 4KB */
#define PAGE_MULTIPLIER         3                       /* 2^Multiplier * [1, 2, 4] * PAGE_SZ */
#define SMALL_ALLOCATION_LIMIT  (PAGE_SZ/2)             /* Small allocation limit is half a page below */

/* Minimum alignment requirement */
#define DEFAULT_ALLIGN     0x10

/* Mmap system call flags */
#define MMAP_PROT_ARGS  (PROT_READ | PROT_WRITE)
#define MMAP_FLAGS_ARGS  (MAP_ANONYMOUS | MAP_PRIVATE)

/* MACROS for pageblock manipulation */
#define ALIGN_MASK(x)           ((x) - 1)
#define GET_PAGE_BOUNDARY(x)    ((char *)((uintptr_t)(x) & (~ALIGN_MASK(PAGE_SZ))))
#define GET_PAGE_START(x, off)  ((page_t *)(GET_PAGE_BOUNDARY((x)) - (off) * PAGE_SZ))
#define GET_PAGE_NUM(x)         (((x) >> PAGE_BITS) + (!!((x) & ALIGN_MASK(PAGE_SZ))))

/* Manipulating page classes by index and size */
#define PAGE_SZ_BY_IDX(x)       (1 << ((x) + PAGE_MULTIPLIER))
#define IDX_BY_PAGE_SZ(x)       (LOG2((unsigned int)((x) >> PAGE_MULTIPLIER)))

/* Fast implementation of log2 for integers */
#define LOG2(x) ((unsigned int) (8 * sizeof(unsigned int) - __builtin_clz((x)) - 1))

/* Tests if an integer is a power of 2 */
#define IS_POWER_OF_TWO(x) (((x) & ((x) - 1)) == 0)

/* rfid struct parameters - Check below */
#define REMOTELY_FREED_OFFSET_BITS       24
#define REMOTELY_FREED_COUNT_BITS        16
#define THREAD_ID_BITS                   24

/* The thread "ID" of an orphaned pageblock */
#define ORPHAN_ID           ((1 << THREAD_ID_BITS) - 1)

/* These numbers are for the rfid struct and depend mainly on the page multiplier and the header size.
 * So the page sizes(in bytes) go as follows:
 *
 * Given the fact that the maximum multiplier is limited by the header size, the largest
 * multiplier we can have is 5.
 *
 * For the remotely_freed field:
 * => 2^Multiplier * {1, 2, 4} * Page_sz  => so our largest size is 2^(Multiplier + 14) = 2^19
 * So, the field that holds the offset has to have more than 19 bits.
 *
 * For the count:
 * => It is trickier but one has to find the most objects a pageblock (always legal by the policy
 * we are using) can hold. Count has to be just larger than that.
 * => In our case, the most populous pageblock is the first page class (with a maximum size of 2^17). The smallest object
 * is the first object sub-class of 16-bytes.
 * So, the division yields 2^17/2^4 = 2^13 maximum objects (this is an upper bound so we are covered)
 *
 * For the thread_id:
 * We use whatever is left and try to have at least 1000000 < IDs.
 */

/* A character is (1 byte), the size of our header */
typedef char header_t;

/* Heap that holds an object class - A doubly linked list with head and tail */
typedef struct private_heap_struct
{
    struct pageblock_struct *head, *tail;
}heap_t;

/* Remotely freed list and thread ID */
typedef struct shared_rfid_struct
{
    unsigned long int count: REMOTELY_FREED_COUNT_BITS;            /* Count is the most objects we can have in a pageblock */
    unsigned long int remotely_freed: REMOTELY_FREED_OFFSET_BITS;  /* Offset maximum is 2^(Multiplier + 14) */
    unsigned long int thread_id: THREAD_ID_BITS;                   /* Thread ID is limited */
}rfid;

/* For cmp & swap union representation */
typedef union rfid_union
{
    rfid shared;                           /* Structure representation */
    unsigned long int both;                /* Merged representation - For cmp & swap */
}rfid_un;

/* Pageblock for the object classes */
typedef struct pageblock_struct
{
    /* Management info */
    struct pageblock_struct *next, *prev;      /* Links for other blocks */
    unsigned short int page_num;               /* How many pages this pageblock has */
    unsigned short int object_size;            /* The object size of this page */
    unsigned int allocated_objects;            /* Number of active allocated objects */

    /* Local memory requests */
    unsigned int unallocated_off;              /* Unallocated objects offset start */
    unsigned int freed;                        /* Local frees - Owning thread */

    /* ID and Rf list */
    volatile rfid_un sync;                     /* Collective data that are in sync via cmp & swap */
}page_t;

#endif
