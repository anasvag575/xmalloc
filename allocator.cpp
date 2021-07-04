/* Header related */
#include "allocator_header.h"

/* Lists manipulation */
#include "allocator_list.h"

/* External header file */
#include "allocator.h"

/* For new and delete */
#include <new>

/* Activates debug mode */
//#define DEBUG

/* Static assertion used for debugging */
#define CTC(x) ({ extern int __attribute__((error("assertion failure: '" #x "' not true"))) compile_time_check(); ((x)?0:compile_time_check()),0; })

/* Message printer in case of error - Local variable only for compiler warnings */
#define PANIC_ERR(x) do{int _locret_ = write(2, (x), strlen((x))); _locret_++; abort();} while(false)

/* Page block management */
static void *mmap_wrap(size_t page_num);
static void munmap_wrap(const void *block, const size_t page_num);
static void *get_pageblock(size_t page_num);
static void ret_pageblock(const void *block, const size_t page_num);

/* Class sizes decoders */
static int class_size_decode(const size_t size, int *pageblock_size);
static int object_type_decode(const void *obj, int *page_offset);

/* Header related */
static void header_write_small(const page_t *page, char *obj);
static void header_write_large(char *obj, size_t sz);

/* Pageblock internal operations */
static page_t *page_internal_init(const void *alloc, const int object_class_idx, const int page_num, const unsigned int thread_id);
static void *page_internal_alloc(page_t *page);
static void page_internal_free(heap_t *local_heap, page_t *page, char *obj, const unsigned int thread_id);

/* Large objects allocations manipulation */
static void *large_alloc(const size_t size);
static void large_free(const void *obj);

/********************************* GLOBAL VARS ***************************/

/* These are the class sizes including the needed header for each object */
const static int class_sizes[] =
{
    /* 1st set of classes - Offset 16 bytes */
    16, 32, 48, 64, 80, 96, 112, 128,
    144, 160, 176, 192, 208, 224, 240, 256,
    272, 288, 304, 320, 336, 352, 368, 384,
    400, 416, 432, 448, 464, 480, 496, 512,

    /* 2nd set of classes - Offset 32 bytes */
    544, 576, 608, 640, 672, 704, 736, 768,
    800, 832, 864, 896, 928, 960, 992, 1024,

    /* 3rd set of classes - Offset 64 bytes */
    1088, 1152, 1216, 1280, 1344, 1408, 1472, 1536,
    1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048
};

/* Global pageblock freelists */
static dq_ct_node global_freeheap[CLASS_PAGES_NUM] = {0};

/* Thread ID counter - Sequentially given to each new thread created */
static unsigned int global_thread_id = 0;

/********************************* THREAD LOCAL VARS ***************************/

/* Private structure for each thread */
typedef struct thread_data_struct
{
    unsigned int thread_id;                        /* The thread ID */
    heap_t private_heap[CLASS_NUM];                /* The local heap with all the classes of small objects */
    dq_ct_node top[CLASS_PAGES_NUM];               /* Local pageblock free lists - Local caching */

    /* Default Constructor - Called when thread spawns */
    thread_data_struct()
    {
        /* Set pointers all pointers to NULL and get a unique ID */
        memset(this->private_heap, 0, CLASS_NUM * sizeof(heap_t));
        memset(this->top, 0, CLASS_PAGES_NUM * sizeof(dq_ct_node));
        this->thread_id = ATOMIC_ADD(&global_thread_id, 1);
    }

    /* Default Destructor - Called when thread terminates (during cleanup phase) */
    ~thread_data_struct()
    {
        for(int i = 0; i < CLASS_NUM; i++) /* Traverse array of classes */
        {
            heap_t *bin = &this->private_heap[i];
            page_t *next = NULL;

            for(page_t *cur = bin->head; cur; cur = next) /* Traverse each class list of pageblocks */
            {
                next = cur->next;

                /* There are still objects in the pageblock */
                if(cur->allocated_objects && cur->sync.shared.count != cur->allocated_objects)
                {
                    rfid_un old_head, new_head;

                    do
                    {
                        /* Fix next */
                        old_head.both = cur->sync.both;

                        /* Means block is completely free now */
                        if(old_head.shared.count == cur->allocated_objects)
                            goto block_empty;

                        /* New head should have orphan ID */
                        new_head = old_head;
                        new_head.shared.thread_id = ORPHAN_ID;
                    }
                    while(!ATOMIC_CAS(&cur->sync.both, &new_head.both, &old_head.both));

                    /* Block was orphaned - Next one */
                    continue;
                }

                block_empty: /* JUMPTAG */

                /* Release back to global freelist or to the OS */
                if(!stack_insert_atomic(&global_freeheap[IDX_BY_PAGE_SZ(cur->page_num)], cur))
                    munmap_wrap(cur, cur->page_num);
            }
        }

        for(int i = 0; i < CLASS_PAGES_NUM; i++) /* Traverse array of cached page classes */
        {
            while(!stack_is_empty(&this->top[i])) /* Remove from page class list every pageblock */
            {
                page_t *cur = stack_remove(&this->top[i]);

                /* Release back to global freelist or to the OS */
                if(!stack_insert_atomic(&global_freeheap[i], cur))
                    munmap_wrap(cur, PAGE_SZ_BY_IDX(i));
            }
        }
    }

}thread_private_t;

/* Private thread data */
static thread_local thread_private_t thread_data;

/********************************* DEBUG ONLY VARS AND MACROS ***************************/
#ifdef DEBUG
    #define DEBUG_COUNT_FUNCTION_CALLS      /* Exteme slowdown - Only for heavy debug */
    #define DEBUG_COUNT_KERNEL_CALLS
    #define DEBUG_PROBE_MEMORY_USAGE

#ifdef DEBUG_COUNT_FUNCTION_CALLS
    static long unsigned int total_malloc_ops = 0, total_realloc_ops = 0, total_free_ops = 0;

    #define DEBUG_COUNT_MALLOCS()       (ATOMIC_ADD(&total_malloc_ops, 1))
    #define DEBUG_COUNT_REALLOCS()      (ATOMIC_ADD(&total_realloc_ops, 1))
    #define DEBUG_COUNT_FREES()         (ATOMIC_ADD(&total_free_ops, 1))
#else
    #define DEBUG_COUNT_MALLOCS()
    #define DEBUG_COUNT_REALLOCS()
    #define DEBUG_COUNT_FREES()
#endif

#ifdef DEBUG_COUNT_KERNEL_CALLS
    static long unsigned int total_mmap = 0, total_munmap = 0;

    #define DEBUG_COUNT_MMAP()          (ATOMIC_ADD(&total_mmap, 1))
    #define DEBUG_COUNT_MUNMAP()        (ATOMIC_ADD(&total_munmap, 1))
#else
    #define DEBUG_COUNT_MMAP()
    #define DEBUG_COUNT_MUNMAP()
#endif

#ifdef DEBUG_PROBE_MEMORY_USAGE
    static long unsigned int total_alloc_mem = 0, total_dealloc_mem = 0, peak_mem = 0, total_real_alloc_mem = 0;
    static long unsigned int total_page_steals = 0;

    #define DEBUG_TOTAL_STEALS()        (ATOMIC_ADD(&total_page_steals, 1))
    #define DEBUG_TOTAL_ALLOC(x)        (ATOMIC_ADD(&total_alloc_mem, (x)))
    #define DEBUG_TOTAL_DEALLOC(x)      (ATOMIC_ADD(&total_dealloc_mem, (x)))
    #define DEBUG_REAL_TOTAL_ALLOC(x)   (ATOMIC_ADD(&total_real_alloc_mem, (x)))
    #define DEBUG_PEAK_MEM()            do{     \
                                            long unsigned int __loc_var__ = total_alloc_mem - total_dealloc_mem; \
                                            if(__loc_var__ > peak_mem) ATOMIC_STORE(&peak_mem, &__loc_var__);    \
                                        }while(0)
#else
    #define DEBUG_TOTAL_STEALS()
    #define DEBUG_TOTAL_ALLOC(x)
    #define DEBUG_TOTAL_DEALLOC(x)
    #define DEBUG_REAL_TOTAL_ALLOC(x)
    #define DEBUG_PEAK_MEM()
#endif

#else
    /* Empty calls */
    #define DEBUG_TOTAL_STEALS()
    #define DEBUG_COUNT_MALLOCS()
    #define DEBUG_COUNT_REALLOCS()
    #define DEBUG_COUNT_FREES()
    #define DEBUG_COUNT_MMAP()
    #define DEBUG_COUNT_MUNMAP()
    #define DEBUG_TOTAL_ALLOC(x)
    #define DEBUG_TOTAL_DEALLOC(x)
    #define DEBUG_REAL_TOTAL_ALLOC(x)
    #define DEBUG_PEAK_MEM()
#endif

/* Wrapper for mmap system call */
static void *mmap_wrap(size_t page_num)
{
    void *block = mmap(NULL, page_num * PAGE_SZ, MMAP_PROT_ARGS, MMAP_FLAGS_ARGS, -1, 0);

    /* MAP_FAILED is -1 - A small trick here is to do a branchless conditional set */
    if(block == MAP_FAILED) block = NULL; //    block += (block == MAP_FAILED);

    DEBUG_COUNT_MMAP();
    DEBUG_TOTAL_ALLOC(page_num * PAGE_SZ);
    DEBUG_PEAK_MEM();

    return block;
}

/* Wrapper for munmap system call */
static void munmap_wrap(const void *block, const size_t page_num)
{
    DEBUG_COUNT_MUNMAP();
    DEBUG_TOTAL_DEALLOC(page_num * PAGE_SZ);

    munmap((void*)block, page_num * PAGE_SZ);
}

/* Gets pageblock from the page allocator */
static void *get_pageblock(const size_t page_num)
{
    const unsigned int page_class_idx = IDX_BY_PAGE_SZ(page_num);

    /* 1st level - Local thread cache */
     void *block = stack_remove(&thread_data.top[page_class_idx]);

    if(!block)
    {
        /* 2nd level - Global cache */
        block = stack_remove_atomic(&global_freeheap[page_class_idx]);

        /* 3rd level - Request from the OS */
        if(!block)  block = mmap_wrap(page_num);
    }

    return block;
}

/* Returns pageblock to the page allocator */
static void ret_pageblock(const void *block, const size_t page_num)
{
    const unsigned int page_class_idx = IDX_BY_PAGE_SZ(page_num);

    /* 1st level of caching - Local thread cache */
    if(stack_insert(&thread_data.top[page_class_idx], (page_t *) block))
        return;

    /* 2nd level of caching - Global cache */
    if(!stack_insert_atomic(&global_freeheap[page_class_idx], (page_t *) block))
        munmap_wrap(block, page_num);
}

/* Forms the header for a small allocation */
static void header_write_small(const page_t *page, char *obj)
{
    /* Form the header */
    header_t header = HEADER_SMALL | HEADER_VALID;

    /* How many pages separate us from the start of the page */
    const int page_offset = ((uintptr_t) ((obj - (char *)page))) >> PAGE_BITS;

    /* Set the page offset value */
    header |= HEADER_PAGE_OFFSET_SET(page_offset);

    /* Write the header */
    WRITE_HEADER(obj - sizeof(header_t), &header);
}

/* Forms the header for a large allocation. Assumes large allocations are page aligned. */
static void header_write_large(char *obj, const size_t sz)
{
    /* Form the header */
    const header_t header = HEADER_LARGE | HEADER_VALID;

    /* Write the large alloc size and the common header */
    WRITE_LARGE_HEADER_SZ((size_t *)obj, &sz);
    WRITE_HEADER(obj + LARGE_HEADER_SIZE - sizeof(header_t), &header);
}

/* Decodes the type of object and checks if any corruption happened */
static int object_type_decode(const void *obj, int *page_offset)
{
    const header_t header = GET_HEADER(obj);

    /* Write out the page offset - In case of large allocation this is ignored */
    *page_offset = HEADER_PAGE_OFFSET_GET(header);

    /* Check validity */
    return HEADER_IS_BLOCK_VALID(header) ? HEADER_PAGE_GET_TYPE(header) : -1;
}

/* Finds the real class size and returns index to it */
static int class_size_decode(const size_t size, int *pageblock_size)
{
    /* Constants - Compiler will optimize away */
    const unsigned int range_shift = 8 , range_mult = 512, base_shift = 4;
    const unsigned int range_offset[] = {0, 32, 48};
    unsigned int range_idx, subrange_idx;

    /* Some notes since there is some hackery going on here.
     *
     * @Class layout
     *
     * There are 64 classes in total which are split into 2 levels and to find the final
     * classes, 2 indexes are needed.
     *
     * @1st level:
     * The first level is split in the total range we have, which is [1, 2047] and we split
     * this in a non-uniform fashion with the following layout:
     *      | [1-511] = Class0   | [512-1023] = Class1  | [1024-2047] = Class2  |
     *      | 16-byte offsets    |   32-byte offsets    |    64-byte offsets    |
     *
     * Each class is then split into sub-classes where sizes are uniformly distributed with each
     * sub-class having the class offset as a difference between each other.
     *
     * @2nd level:
     * To understand it easier, for the second level here is an example:
     *  - Assume an allocation request of 24 bytes.
     *  - We go to the first class, Class0, so our first index is 0.
     *  - Inside class 0 we have the following layout:
     *      | [1 - 15] bytes | [16 - 31] bytes | [32 - 47] bytes | ..(more classes).. | [240 - 255] bytes |
     *      |  Sub-class0    |   Sub-class1    |   Sub-class2    |     .......        |    Sub-class15    |
     *
     *  - Our request fits inside Sub-class1, so we go there.
     *
     * @Finalize:
     * When we have both indexes we can calculate the base of the first class (where it starts in the
     * array of bins of each thread) and add to it the sub-range, to get the final index.
     * */

    /* Get the first level class index */
    range_idx = LOG2((size >> range_shift) | 1);                                    /* log2(size / 256 | 1) (OR 1 is used since log(0) is undefined) */

    /* Get the second level class index */
    subrange_idx = (size - range_mult * range_idx) >> (base_shift + range_idx);     /* (Size - Range_min) / (16 + Range_divider) */

    /* Calculate the page class we are dealing with - Page multipliers */
    *pageblock_size = 1 << (range_idx + PAGE_MULTIPLIER);                           /* Pageblock size = 2^(range_idx + PAGE_MULTIPLIER) */

    /* Finalize */
    return range_offset[range_idx] + subrange_idx;
}

/* Performs an allocation for a large object */
static void *large_alloc(const size_t sz)
{
    /* Find how many pages are needed - Header is included */
    const size_t pages_num = GET_PAGE_NUM(sz + LARGE_HEADER_SIZE);

    DEBUG_REAL_TOTAL_ALLOC(pages_num * PAGE_SZ);

    /* Get needed pages - Large allocations directly to the kernel */
    char *ret = (char *)mmap_wrap(pages_num);

    /* Success - Write the header and move to the payload */
    if(ret)
    {
        header_write_large(ret, pages_num);
        ret += LARGE_HEADER_SIZE;
    }

    return ret;
}

/* Performs a free for a large object */
static void large_free(const void *obj)
{
    /* Return the memory to the kernel */
    munmap_wrap(GET_LARGER_ALLOC_START(obj), GET_LARGER_ALLOC_SZ(obj));
}

/* Initializes a pageblock for the local heap */
static page_t *page_internal_init(const void *alloc, const int object_class_idx, const int page_num, const unsigned int thread_id)
{
    /* Header starts from the initial mapped area - Common  */
    page_t *page = (page_t *)alloc;
    page->object_size = class_sizes[object_class_idx];
    page->page_num = page_num;
    page->allocated_objects = 0;
    page->freed = 0;
    page->sync.shared.thread_id = thread_id;
    page->sync.shared.remotely_freed = 0;
    page->sync.shared.count = 0;
    page->next = page->prev = NULL;

    /* The start of allocation space is after the header */
    page->unallocated_off = sizeof(page_t);

    /* Final step is to align the unallocated area at -1 8-byte aligned addresses */
    uintptr_t align_rq = ALIGN_MASK(DEFAULT_ALLIGN);
    align_rq -= ((uintptr_t) (((char *) page) + page->unallocated_off)) & align_rq;

//    /* TODO Opportunity for more alignment - Powers of 2 can be aligned even more */
//    if(object_class_idx && IS_POWER_OF_TWO(class_sizes[object_class_idx - 1]))
//    {
//        /* The minimum of this class is the alignment request */
//        align_rq = ALIGN_MASK(class_sizes[object_class_idx - 1]);
//        alignment = ((uintptr_t) (((char *) page) + page->unallocated_off)) & align_rq;
//        align_rq -= alignment;
//    }

    /* Now we fix that by the amount of alignment */
    page->unallocated_off += align_rq;

    return page;
}

/* Tries to allocate an object from the page */
static void *page_internal_alloc(page_t *page)
{
    /* This function has 3 main paths of allocation:
     * 1) A recently freed object, from the local LIFO (Fast path)
     * 2) A never-allocated object from the unallocated area of the pageblock (Fast path)
     * 3) A remotely freed object, from the remote LIFO (Slow path).
     */

    char *ret = NULL;
    char *page_ptr = (char *)page;

    /* Collect any remotely freed objects */
    if(page->sync.shared.remotely_freed)
    {
        rfid_un old_head, new_head;

        do
        {
            /* Get old head */
            old_head.both = page->sync.both;

            /* Zero out */
            new_head.shared.thread_id = old_head.shared.thread_id;
            new_head.shared.count = 0;
            new_head.shared.remotely_freed = 0;
        }
        while(!ATOMIC_CAS(&page->sync.both, &new_head.both, &old_head.both));

        /* Insert in free LIFO */
        while(old_head.shared.remotely_freed)
        {
            /* Get object and keep next */
            unsigned int *obj = (unsigned int *)(page_ptr + old_head.shared.remotely_freed);
            unsigned int next = ((rfid_un *)obj)->shared.remotely_freed;

            /* Push in local LIFO */
            STACK_PUSH_OBJECT(page, obj, old_head.shared.remotely_freed);

            /* Update cur */
            old_head.shared.remotely_freed = next;
        }
    }

    /* First try - Check freed LIFO for available objects */
    if(page->freed)
    {
        STACK_POP_OBJECT(page, ret);
        return (void *)ret;
    }

    /* Second try - Check unallocated area for availability */

    /* Find the base for the unallocated objects and the page limit */
    char *base_alloc = page_ptr + page->unallocated_off;
    const char *page_limit = page_ptr + (((unsigned int)page->page_num) * PAGE_SZ);

    /* Check that we do not exceed allocation bounds */
    if((base_alloc + page->object_size) < page_limit)
    {
        /* Hold return value */
        ret = base_alloc + sizeof(header_t);
        
        /* Form header */
        header_write_small(page, ret);

        /* Move the unallocated offset to the next object */
        page->unallocated_off += page->object_size;
        page->allocated_objects++;
    }

    return (void *)ret;
}

/* De-allocate an object inside a pageblock */
static void page_internal_free(heap_t *local_heap, page_t *page, char *obj, const unsigned int thread_id)
{
    /* This function has 3 main paths:
     * 1) Local free, then we simply insert in the local LIFO (simplest case).
     * 2) Remote free where we insert in the remote LIFO, there are 2 paths:
     *    => 2a) During the CAS operation we detect that the pageblock was orphaned,
     *           we try to steal it and insert the pageblock in our pageblocks list (arry of classes).
     *    => 2b) Else, leave it with the old value, of the other thread that owns the block.
    */

    /* Mainly to eliminate any typecasts below */
    const unsigned int obj_offset = (unsigned int)((uintptr_t) (obj - ((char *) page)));

    if(page->sync.shared.thread_id == thread_id) /* Local case */
    {
        /* Push in local LIFO */
        STACK_PUSH_OBJECT(page, (unsigned int *)obj, obj_offset);

        /* Check if the pageblock can be released back */
        if(!page->allocated_objects && local_heap->head != page)
        {
            remove_node_dq(local_heap, page);
            ret_pageblock((void *)page, page->page_num);
        }
    }
    else /* Remote free, we do not own it */
    {
        rfid_un new_head;
        rfid_un *obj_ptr = (rfid_un *) obj;
        bool maybe_stolen;

        do
        {
            /* Old head values init */
            obj_ptr->both = page->sync.both;
            new_head.both = obj_ptr->both;
            maybe_stolen = false;

            /* Steal case - Opportunistically try to also steal the pageblock in one go */
            if(obj_ptr->shared.thread_id == ORPHAN_ID)
            {
                new_head.shared.thread_id = thread_id;
                maybe_stolen = true;
            }

            /* Else update for insertion */
            new_head.shared.remotely_freed = obj_offset;
            new_head.shared.count += 1;
        }
        while(!ATOMIC_CAS(&page->sync.both, &new_head.both, &obj_ptr->both));

        /* Successful steal means insertion in our list */
        if(maybe_stolen && page->sync.shared.thread_id == thread_id)
        {
            DEBUG_TOTAL_STEALS();
            insert_front_dq(local_heap, page);
        }
    }
}

/* Performs compile time checks - Asserts compiler error in case of failure */
void compile_check_dummy(void)
{
    /* CTC - Compile Time Check
     * Mostly checks some assumptions that are more or less pretty common, but
     * in any case we should have a safeguard. */

    /* Counting atomic list node - Compare and exchange for 64bits only */
    CTC((sizeof(dq_ct_node) == 8));
    CTC((sizeof(rfid) == 8));

    /* Headers are 1-byte, else everything is doomed */
    CTC((sizeof(header_t) == 1));

    /* Minimum class is 7 effective bytes - So int offsets is all we have */
    CTC((sizeof(int) == 4));

    /* Sometimes long int is used as an alias for pointers and compare and exchange */
    CTC((sizeof(long int) == 8));
    CTC((sizeof(void *) == 8));
    CTC((sizeof(size_t) == 8));
}

void *malloc(size_t sz)
{
    /* Check input - 0 byte allocations not supported */
    if(!sz) return NULL;

    DEBUG_COUNT_MALLOCS();

    /* Small allocations go to the front-end */
    if(sz < SMALL_ALLOCATION_LIMIT)
    {
        /* Get the class information */
        int page_num;
        int class_idx = class_size_decode(sz, &page_num);
        heap_t *bin = &thread_data.private_heap[class_idx];

        DEBUG_REAL_TOTAL_ALLOC(class_sizes[class_idx]);

        /* Traverse the pageblock list to find candidate for an allocation */
        for(page_t *cur = bin->head; cur; cur = cur->next)
        {
            void *ret = page_internal_alloc(cur);
            if(ret) return ret;
        }

        /* Allocate and initialize a pageblock */
        void *alloc = get_pageblock(page_num);

        /* Failure */
        if(!alloc) return NULL;

        /* Initialize page and link to list */
        page_t *page = page_internal_init(alloc, class_idx, page_num, thread_data.thread_id);
        insert_front_dq(bin, page);

        /* Allocate from the page and return */
        return page_internal_alloc(page);
    }

    /* Perform large allocation */
    return large_alloc(sz);
}

void *calloc(size_t nmemb, size_t sz)
{
    const size_t total_alloc = nmemb * sz;

    /* Overflow found */
    if (nmemb != 0 && total_alloc / nmemb != sz) return NULL;

    /* Call malloc and set to 0*/
    void *ptr = malloc(total_alloc);
    if(ptr) memset(ptr, 0, total_alloc);

    return ptr;
}

void *realloc(void *obj, size_t sz)
{
    int page_offset;
    size_t old_sz;
    void *ret = obj;

    /* 0 size is not supported */
    if(!obj) return malloc(sz);

    DEBUG_COUNT_REALLOCS();

    /* Find the type of object */
    switch(object_type_decode(obj, &page_offset))
    {
        case CLASS_SMALL:
        {
            /* We also need to account for the header size */
            old_sz = (GET_PAGE_START(obj, page_offset)->object_size) - sizeof(header_t);
            break;
        }
        case CLASS_LARGE:
        {
            old_sz = GET_LARGER_ALLOC_SZ(obj);
            break;
        }
        default: PANIC_ERR("Broken object, aborting [realloc]..\n");
    }

    /* If size is smaller simple return the same */
    if(old_sz > sz) return ret;

    /* Malloc - Copy - Free */
    ret = malloc(sz);

    /* Move the allocation to the new block */
    if(ret)
    {
        memcpy(ret, obj, old_sz);
        free(obj);
    }

    return ret;
}

void free(void *obj)
{
    int page_offset = 0;                            /* Page offset to reach start of page */
    thread_private_t *local_data = &thread_data;    /* Thread local storage reference */
    heap_t *local_heap;                             /* Local heap for classes */

    /* Empty */
    if(!obj) return;

    DEBUG_COUNT_FREES();

    /* Find the type of object */
    switch(object_type_decode(obj, &page_offset))
    {
        case CLASS_SMALL: break; /* Handle below */
        case CLASS_LARGE: large_free(obj); return;
        default: PANIC_ERR("Broken object, aborting..[free]\n");
    }

    /* Get the start of the pageblock */
    page_t *page = GET_PAGE_START(obj, page_offset);

    /* Get the local heap by the class size */
    local_heap = &local_data->private_heap[class_size_decode(page->object_size - 1, &page_offset)];

    /* Free object */
    page_internal_free(local_heap, page, (char *)obj, local_data->thread_id);
}

void *operator new(std::size_t count)
{
    return malloc(count);
}

void *operator new[](std::size_t count)
{
    return malloc(count);
}

void *operator new(std::size_t count, const std::nothrow_t &tag)
{
    return malloc(count);
}

void *operator new[](std::size_t count, const std::nothrow_t &tag)
{
    return malloc(count);
}

void operator delete(void *ptr)
{
    free(ptr);
}

void operator delete[](void *ptr)
{
    free(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &tag)
{
    free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t &tag)
{
    free(ptr);
}

void operator delete(void *ptr, std::size_t sz)
{
    free(ptr);
}

void operator delete[](void *ptr, std::size_t sz)
{
    free(ptr);
}

void malloc_debug_stats(void)
{
#ifdef DEBUG
    thread_private_t *local_data = &thread_data;
    page_t *cur = NULL;

    printf("\n**********************************************\n");
    printf("**************** MALLOC STATS ****************\n");
    printf("**********************************************\n");
    printf(" This thread ID: %d\n", thread_data.thread_id);
    printf(" Total threads created: %d\n", global_thread_id);

#ifdef DEBUG_COUNT_FUNCTION_CALLS
    printf(" Total_malloc_ops: %ld\n Total_realloc_ops: %ld\n Total_free_ops: %ld\n", total_malloc_ops, total_realloc_ops, total_free_ops);
#endif

#ifdef DEBUG_COUNT_KERNEL_CALLS
    printf(" Total_mmap: %ld\n Total_munmap: %ld\n",total_mmap, total_munmap);
#endif

#ifdef DEBUG_PROBE_MEMORY_USAGE
    printf(" Total page steals: %ld\n", total_page_steals);
    printf(" Total_alloc mem(kb): %ld\n Total_dealloc mem(kb): %ld\n Effective allocated mem(kb): %ld\n Peak allocated mem(kb): %ld\n", total_alloc_mem >> 10 ,
                                                                                                                                      total_dealloc_mem >> 10,
                                                                                                                                      total_real_alloc_mem >> 10,
                                                                                                                                      peak_mem >> 10);
#endif

    for(int i = 0; i < CLASS_NUM; i++)
    {
        heap_t *bin = &local_data->private_heap[i];

        if(!bin->head)
        {
            // printf(" (NULL)\n");
            continue;
        }

        int counter = 0, total_objects = 0;

        /* Traverse the pageblock list to find candidate for an allocation */
        for(cur = bin->head; cur; cur = cur->next)
        {
            counter++;
            total_objects += cur->allocated_objects;
        }

        printf("object size: %d:: Blocks %d - Total objects %d\n", class_sizes[i], counter, total_objects);
//        fflush(stdout);
    }

    printf(" Header (Mask-Shift): [Header type: 0x%02x-%d] [Page_off: 0x%02x-%d] [Security: 0x%02x-%d]\n", HEADER_TYPE_MASK, HEADER_TYPE_SHIFT,
                                                                                                           HEADER_PAGE_OFF_MASK, HEADER_PAGE_OFF_SHIFT,
                                                                                                           HEADER_VALID_MASK, 0);

    printf(" Header Form: [ Type: %d bit | Page_off: %d bits | Security: %d bits ]\n", 1, HEADER_PAGE_OFF_BITS, HEADER_SECURITY_BITS);
    printf(" Counting stacks: [Ptr mask: %016lx] [InvPtr mask: %016lx] [Count max: %ld] [Count bits %d] [Ptr bits %d]\n",  PTR_MASK, ~PTR_MASK,
                                                                                                                           COUNT_MAX, COUNT_BITS,
                                                                                                                           PTR_BITS);
    printf(" Counting node: [ Unused bits: %d bit | Real ptr: %d bits | Page offset: %d bits ]<=>", VIRTUAL_UNUSED_BITS,
                                                                                                    PTR_BITS, PAGE_BITS);

    printf("[ Real ptr: %d bits | State: %d bits | Count: %d bits ]\n", PTR_BITS, STATE_BITS, COUNT_BITS);
    #endif
}
