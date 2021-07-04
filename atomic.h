#ifndef _ATOMIC_H
#define _ATOMIC_H

/* TODO - check memory order values, maybe relaxed can help performance */

/* Atomic types */
typedef volatile int spin_t;

/* Generalized atomic store */
#define ATOMIC_STORE(ptr, val) do{__atomic_store((volatile typeof(ptr)) (ptr), (volatile typeof(val)) (val), __ATOMIC_SEQ_CST);} while(0)

/* Generalized atomic compare and swap operation */
#define ATOMIC_CAS(ptr, new_val, old_val) (__atomic_compare_exchange((volatile typeof(ptr))     (ptr),                 \
                                                                     (volatile typeof(old_val)) (old_val),             \
                                                                     (volatile typeof(new_val)) (new_val),             \
                                                                                                        0,             \
                                                                                         __ATOMIC_SEQ_CST,             \
                                                                                        __ATOMIC_RELAXED))

/* Generalized atomic add - Returns the previous value in ptr <Add then Load> */
#define ATOMIC_ADD(ptr, val) (__atomic_add_fetch((volatile typeof(ptr)) (ptr), (volatile typeof(val)) (val), __ATOMIC_SEQ_CST))

/* Initializes spin lock */
static inline void spin_lock_init(spin_t *lock)
{
    *lock = 0;
}

/* Acquires spin lock */
static inline void spin_lock(spin_t *lock)
{
    do
    {
        while(*lock);
    } while(__sync_lock_test_and_set(lock, 1));
}

/* Releases spin lock */
static inline void spin_unlock(spin_t *lock)
{
    __sync_lock_release(lock);
}

#endif
