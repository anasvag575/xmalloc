#ifndef _ALLOCATOR_LIST_H
#define _ALLOCATOR_LIST_H

/* Global header for all allocator sub-headers */
#include "allocator_internal.h"

/* Some info about how the counting list works
 *
 * A pointer stored in these lists will be 64-bits wide with
 * the following layout:
 *
 * | (N bits) 1..1 | ((52-N) bits) useful address | (12 bits) 0..0 |
 *
 * The 12 LSB bits since we only store pages (and pages are aligned at
 * multiples of 4KB), we can safely use those to store additional info.
 *
 * The top N bits, which are most of the times unused parts of the
 * whole virtual address space can be also be used for something
 * else.
 *
 * Some research has shown the following (WHICH BY NO MEANS IS FULLY PORTABLE):
 * - Virtual adresses are usually about 48-52 bits valid, so the MSBs are
 * sign-extended (usually 1).
 * - So our effective pointer can be packed even more tightly to a conservative 36-40 bits,
 * leavings about 24-28 bits to be used for counters and state (ABA problem).
 *
 * Our safest bet is to consider the worst case of 52 which through page alignment
 * becomes 40 effective bits. Hence 24 bits left for state and count.
 *
 * So a dq_count_node will have the 40 bits for the pointer, 12 for the state
 * and 12 for the counter. In that way 64-bit compare and swaps will work
 * fine.
 * */

/* Virtual addresses info, base modifier is the effective bits count
 * This should be a multiple of 2. */
#define VIRTUAL_EFFECTIVE_BITS  (52)
#define VIRTUAL_UNUSED_BITS     (64 - VIRTUAL_EFFECTIVE_BITS)

/* Queue count, state and pointer manipulation */
#define PTR_SHIFT  (PAGE_BITS)
#define PTR_BITS   (64 - PAGE_BITS - VIRTUAL_UNUSED_BITS)
#define PTR_MASK   (((1UL) << (64 - VIRTUAL_UNUSED_BITS)) - 1)

/* Counter and state get the leftover split in half */
#define COUNT_BITS ((64 - PTR_BITS) >> 1)
#define STATE_BITS ((64 - PTR_BITS) >> 1)

/* Counter max value */
#define COUNT_MAX  ((1UL << COUNT_BITS) - 1)

/* Counting stack node */
typedef struct dq_count_node
{
    unsigned long int nxt:PTR_BITS;
    unsigned long int count:COUNT_BITS;
    unsigned long int state:STATE_BITS;
}dq_ct_node;

/* Union representation for compare and swap */
typedef union dq_c_node_both
{
    dq_ct_node top;
    unsigned long int both;
}both_count_dq;

/* Setter and getter */
#define SET_PTR(x)          ((((uintptr_t)(x)) >> PTR_SHIFT) & PTR_MASK)
#define GET_PTR(x)          ((dq_ct_node *) ((((uintptr_t)(x))|(~PTR_MASK)) << PTR_SHIFT))

/************* COUNTING SINGLY LINKED LISTS COMMON *************/

/* Check if stack is empty */
static inline int stack_is_empty(dq_ct_node *stack_top)
{
    return !stack_top->count;
}

/* Check if stack is full */
static inline int stack_is_full(dq_ct_node *stack_top)
{
    return stack_top->count == COUNT_MAX;
}

/************* NON-ATOMIC COUNTING SINGLY LINKED LISTS *************/

/* Insertion in stack */
static inline int stack_insert(dq_ct_node *stack_top, void *page)
{
    /* Stack is full */
    if(stack_top->count == COUNT_MAX) return 0;

    /* new_node->next = head */
    *((dq_ct_node *) page) = *stack_top;

    /* head = new_node */
    stack_top->count++;
    stack_top->nxt = SET_PTR(page);

    return 1;
}

/* Removal from stack */
static inline page_t *stack_remove(dq_ct_node *stack_top)
{
    /* Empty */
    if(!stack_top->count) return NULL;

    /* ret_node = head */
    dq_ct_node *page = GET_PTR(stack_top->nxt);

    /* head = head->nxt */
    stack_top->nxt = page->nxt;
    stack_top->count--;

    /* Return page */
    return (page_t *)page;
}

/************* ATOMIC COUNTING SINGLY LINKED LISTS *************/

/* Atomic insertion in stack - Returns 0 in case of failure */
static inline int stack_insert_atomic(dq_ct_node *stack_top, void *page)
{
    /* This is only to avoid aliasing errors by the compiler (union) */
    both_count_dq new_head;

    do
    {
        /* Get old_head */
        dq_ct_node old_head = *stack_top;

        /* Maxed out the queue - Queue counter will overflow */
        if(old_head.count == COUNT_MAX) return 0;

        /* new_node->next = head */
        *((dq_ct_node *) page) = old_head;

        /* head = new_node */
        new_head.top.count = old_head.count + 1;
        new_head.top.state = old_head.state + 1;
        new_head.top.nxt = SET_PTR(page);
    }
    while(!ATOMIC_CAS((unsigned long int *) stack_top, &new_head.both, (unsigned long int *) page));

    return 1;
}

/* Atomic removal from stack - Returns NULL in case of failure */
static inline page_t *stack_remove_atomic(dq_ct_node *stack_top)
{
    both_count_dq old_head;
    both_count_dq next;

    /* This is only to avoid aliasing errors by the compiler (use different pointer type) */
    both_count_dq *ref_top = (both_count_dq *) stack_top;

    while(1)
    {
        old_head.top = *stack_top;

        /* Empty head */
        if(!old_head.top.nxt) return NULL;

        /* Next entry */
        next.top.nxt = (GET_PTR(old_head.top.nxt))->nxt;
        next.top.count = old_head.top.count - 1;
        next.top.state = old_head.top.state + 1;

        if(ATOMIC_CAS(&ref_top->both, &next.both, &old_head.both))
            return (page_t *)GET_PTR(old_head.top.nxt);
    }
}

/************* NON-ATOMIC DOUBLY LINKED LISTS *************/

/* Inserts node in the front of the list */
static inline void insert_front_dq(heap_t *c, page_t *page)
{
    if (!c->head)
    {
        c->tail = c->head = page;
        page->next = page->prev = NULL;
    }
    else
    {
        page->next = c->head;
        page->next->prev = page;
        c->head = page;
    }
}

/* Inserts node in the tail of the list */
static inline void insert_tail_dq(heap_t *c, page_t *page)
{
    if (!c->head)
    {
        c->tail = c->head = page;
        page->next = page->prev = NULL;
    }
    else
    {
        c->tail->next = page;
        page->prev = c->tail;
        c->tail = page;
    }
}

/* Removed node from the front of the list */
static inline page_t *remove_front_dq(heap_t *c)
{
    page_t *curr = c->head;

    if (c->head == c->tail)
    {
        c->head = NULL;
        c->tail = NULL;
    }
    else
    {
        c->head = curr->next;
        c->head->prev = NULL;
        curr->next = NULL;
    }

    return curr;
}

/* Removed node from the tail of the list */
static inline page_t *remove_tail_dq(heap_t *c)
{
    page_t *curr = c->tail;

    if (c->tail == c->head)
    {
        c->tail = NULL;
        c->head = NULL;
    }
    else
    {
        c->tail = curr->prev;
        c->tail->next = NULL;
        curr->prev = NULL;
    }

    return curr;
}

/* Removed node from anywhere in the list, assuming the page is not the head */
static inline void remove_node_dq(heap_t *c, page_t *page)
{
    if (page == c->tail)
    {
        c->tail = page->prev;
        page->prev->next = NULL;
    }
    else
    {
        page->prev->next = page->next;
        page->next->prev = page->prev;
    }

    page->next = page->prev = NULL;
}

/*********** MACROS FOR OBJECT LOCAL STACKS **************/
#define STACK_PUSH_OBJECT(page, obj, obj_offset)                         \
    do                                                                   \
    {                                                                    \
        *(obj) = (page)->freed;             /* cur->next =  head */      \
        (page)->freed = (obj_offset);       /* head = cur */             \
        (page)->allocated_objects--;                                     \
    }while(0)

#define STACK_POP_OBJECT(page, obj)                                         \
    do                                                                      \
    {                                                                       \
        (obj) = ((char *)(page)) + page->freed;    /* ret = head */         \
        (page)->freed = *((unsigned int *) (obj)); /* head = head->next */  \
        (page)->allocated_objects++;                                        \
    }while(0)

#endif
