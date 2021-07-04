/* Compile with gcc -Wall -g my_prog.c -lmy_lib -L. -o my_prog -lpthread 
 * Make sure you have appended the location of the .so to the environment
 * variable LD_LIBRARY_PATH */

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>

#include "allocator.h"

/* Atomics */
#include "atomic.h"

/* Atomic lists */
#include "allocator_list.h"

/* Dummy argument for thread functions */
typedef struct arguments
{
   int low, high, ret;
   void *buf;
   double res;
}arg_t;

#define COUNTING_LIFO_SZ   4095

/* Local allocs/frees test parameters */
#define LARGE_ALLOC_LOC_SZ  80050
#define MAX_THREAD_BUF_SZ   200000      /* The largest thread stack can hold about 2 * 200k buffers */

/* For remote tests */
#define ALLOC_BUF_REM_SIZE      500000       /* Usually the largest stack allocated buffer about 1M */
#define ALLOC_BUF_ADOPT_SIZE    500000       /* Usually the largest stack allocated buffer about 1M */
#define ALIGN_RQ        0x0f                 /* 16 byte alignment */
#define CLASS_NUM       64

/* Sync flag for tests */
volatile int pass = 0;

/***************************** SUPPORTING OPERATIONS **************************/

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

/* Extremely fast implementation of log2 for integers */
#define LOG2(x) ((unsigned int) (8 * sizeof(unsigned int) - __builtin_clz((x)) - 1))

/* Decodes classes when accessing local heap for request */
static int class_size_decode(size_t size, int *page_class)
{
    /* Constants - Compiler will optimize away */
    const unsigned int range_shift = 8;
    const unsigned int base_shift = 4;                      /* Sub-range base divider */
    const unsigned int range_offset[] = {0, 32, 48};        /* Range class base for each range_idx */
    const unsigned int range_min[] = {0, 512, 1024};        /* Range minimum for each sub-class */

    /* Indexers */
    unsigned int range_idx, subrange_idx;

    /* Get the first level class index */
    range_idx = LOG2((size >> range_shift) | 1);         /* log2(size / 256 | 1) (OR 1 is used since log(0) is undefined) */

    /* Get the second level class index */
    subrange_idx = (size - range_min[range_idx]) >> (base_shift + range_idx);   /* (Size - Range_min) / (16 + Range_divider) */

    /* Calculate the page class we are dealing with - Page multipliers */
    if(page_class)
    {
        *page_class = 1 << (range_idx);                       /* Page_class = pow(2, range_idx) */
    }

    /* Finalize */
    return class_sizes[range_offset[range_idx] + subrange_idx];
}

/* Shuffler to generate random indexes */
static void shuffle_func(int *array, size_t n)
{
    if (n > 1)
    {
        size_t i;
        for (i = 0; i < n - 1; i++)
        {
          size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
          int t = array[j];
          array[j] = array[i];
          array[i] = t;
        }
    }
}

/***************************** THREAD FUNCTIONS  **************************/

void *thread_atomic_LIFO_func(void *arg)
{
    arg_t *in_args = (arg_t *) arg;
    dq_ct_node *atomic_lifo = (dq_ct_node *) in_args->buf;
    int trials = (rand() % in_args->ret) >> 1;
    void *buf[trials];
    in_args->ret = 1;

    while(!pass); /* Wait for sync flag */

    /* Initialize */
    memset(buf, 0, trials * sizeof(void *));

    for(int i = 0; i < trials; i++)
    {
        buf[i] = stack_remove_atomic(atomic_lifo);
    }

    for(int i = 0; i < trials; i++)
    {
        if(buf[i])
        {
            if(!stack_insert_atomic(atomic_lifo, buf[i])) /* Fail - Return */
            {
                in_args->ret = 0;
                return NULL;
            }
        }
    }

    return NULL;
}

void *thread_remote_func(void *arg)
{
    arg_t *limits = (arg_t *) arg;
    int **buf_ref = (int **)(limits->buf);

    /* Wait for main thread to start everything */
    while (!pass);

    /* Start freeing in the range given */
    for (int i = limits->low; i < limits->high; i++) free(buf_ref[i]);

    /* Return Success */
    limits->ret = 1;

    /* Verification print */
//    printf("TID: %lu - Finished with limits [%d - %d]\n", pthread_self(), limits->low, limits->high);
//    fflush(stdout);

    return NULL;
}

void *thread_local_func(void *arg)
{
    /* Argument extraction */
    arg_t *ret = (arg_t *)arg;
    int alloc_sz = (ret->high > MAX_THREAD_BUF_SZ) ? MAX_THREAD_BUF_SZ : ret->high;

    int *local_buffer_int[alloc_sz];
    double *local_buffer_double[alloc_sz];

    /* Initialize buffers */
    memset(local_buffer_int, 0, alloc_sz * sizeof(int *));
    memset(local_buffer_int, 0, alloc_sz * sizeof(double *));

    /* Start the mallocs - Also initialize with something */
    for(int i = 0; i < alloc_sz; i++)
    {
        local_buffer_int[i] = malloc(sizeof(int));
        local_buffer_double[i] = malloc(sizeof(double));
        *local_buffer_int[i] = i;
        *local_buffer_double[i] = i * 3.14;
    }

    /* A large malloc */
    char *large_alloc = malloc(alloc_sz);
    char val_large = large_alloc[rand()%alloc_sz];

    /* Trick to disable optimizer */
    int val_int = *local_buffer_int[rand()%alloc_sz];
    double val_double = *local_buffer_double[rand()%alloc_sz];

    /* Now the frees */
    for(int i = 0; i < alloc_sz; i++)
    {
        free(local_buffer_int[i]);
        free(local_buffer_double[i]);
    }

    free(large_alloc);

    /* Trick to disable optimizer */
    ret->res = val_double + val_int + val_large;
    ret->ret = 1;

    /* In case we need any prints */
//    printf("TID: %lu - Val int %d - Val double %lf - Val large %c\n", pthread_self(), val_int, val_double, val_large);
//    fflush(stdout);

    return NULL;
}

void *thread_shuffle_func(void *args)
{
    arg_t *args_loc = (arg_t *) args;

    const int alloc_num = (args_loc->high < MAX_THREAD_BUF_SZ) ? args_loc->high: MAX_THREAD_BUF_SZ;
    int *local_buffer_int[alloc_num];
    int idx_arr[alloc_num];
    int j = 0;

    /* Init allocation buffer and index buffer */
    for(int i = 0; i < alloc_num; i++)
    {
        local_buffer_int[i] = NULL;
        idx_arr[i] = i;
    }

    /* Run twice */
    while (j < args_loc->low)
    {
        /* Start the mallocs - Also initialize with something */
        for(int i = 0; i < alloc_num; i++)
        {
            local_buffer_int[i] = calloc(1, sizeof(int));
            *local_buffer_int[i] = i;
        }

        /* Shuffle order of frees */
        shuffle_func(idx_arr, alloc_num);

        /* Now the frees */
        for(int i = 0; i < alloc_num; i++) free(local_buffer_int[idx_arr[i]]);

        j++;
    }

    /* Return */
    args_loc->ret = 1;

    return NULL;
}

void *thread_stress_shuffle_func(void *args)
{
    arg_t *args_loc = (arg_t *) args;

    const int alloc_num = (args_loc->high < MAX_THREAD_BUF_SZ) ? args_loc->high: MAX_THREAD_BUF_SZ;
    void *ptr[alloc_num];
    int idx_arr[alloc_num];

    /* Init allocation buffer and index buffer */
    for (int j = 0; j < alloc_num; j++)
    {
        ptr[j] = NULL;
        idx_arr[j] = j;
    }

    /* Allocate from all the classes */
    for (int i = 0; i < CLASS_NUM; i++)
    {
        int req_sz = class_sizes[i] - 1; /* Minus header */

        for(int j = 0; j < alloc_num; j++)
        {
            ptr[j] = malloc(req_sz);
            memset(ptr[j], 0, req_sz);
        }

//        malloc_debug_stats();

        /* Reshuffle idx array */
        shuffle_func(idx_arr, alloc_num);

        /* Random frees */
        for (int j = 0; j < alloc_num; j++)
        {
            free(ptr[idx_arr[j]]);
        }

        /* Reset local array */
        memset(ptr, 0, alloc_num * sizeof(void *));
    }

    /* Return success */
    args_loc->ret = 1;

    return NULL;
}

void *thread_alloc_no_free_func(void * args)
{
    arg_t *inf = (arg_t *) args;
    int **array = (int **)inf->buf;

    for (int i = 0; i < inf->ret; i++)
    {
        array[i] = malloc(sizeof(int));
        *array[i] = i * 10;
    }

    inf->ret = 1;

    return NULL;
}

void *thread_adoption_func(void * args)
{
    arg_t *inf = (arg_t *) args;
    int *array[inf->ret];
    int **temp = (int **) inf->buf;

    for (int i = inf->low; i < inf->high; i++) {
        free(temp[i]);
    }
//    malloc_debug_stats();

    for(int i = 0; i < inf->ret; i++)
    {
        array[i] = malloc(sizeof(int));
        *array[i] = i;
    }

    for (int i = 0; i < inf->ret; i++)
    {
        free(array[i]);
    }

    inf->ret = 1;

    return NULL;
}

/***************************** TESTS **************************/

/* Base test for the counting atomic LIFOs data structure */
int test_atomic_counting_queues(int threads_num, int allocs)
{
    /* Setup arguments */
    allocs = (allocs > COUNTING_LIFO_SZ) ? COUNTING_LIFO_SZ : allocs;

    /* Atomic LIFO */
    dq_ct_node atomic_lifo = {.nxt = 0, .count = 0, .state = 0};
    void *buf[allocs];

    /* Sync all threads */
    int new = 1, old = 0, ret = 1;

    /* Threads */
    arg_t dummy_res[threads_num];
    pthread_t tid[threads_num];

    memset(buf, 0, allocs * sizeof(void *));

    /* Setup mmaps */
    for(int i = 0; i < allocs; i++)
    {
        void *ret = mmap(NULL, PAGE_SZ, MMAP_PROT_ARGS, MMAP_FLAGS_ARGS, -1, 0);

        if(ret == MAP_FAILED) return 0;
        if(!stack_insert_atomic(&atomic_lifo, ret)) return 0;
        buf[i] = ret;
    }

    /* Create threads */
    for (int i = 0; i < threads_num; i++)
    {
        dummy_res[i].buf = &atomic_lifo;
        dummy_res[i].ret = allocs;
        pthread_create(&tid[i], NULL, thread_atomic_LIFO_func, &dummy_res[i]);
    }

    /* Threads are free to run now */
    ATOMIC_CAS(&pass, &new, &old);

    for (int i = 0; i < threads_num; i++)
    {
        pthread_join(tid[i], NULL);
    }

    /* Setup mmaps */
    for(int i = 0; i < allocs; i++)
    {
        int goto_next = 0;
        void *ret = stack_remove_atomic(&atomic_lifo);
        for(int j = 0; j < allocs; j++)
        {
            if(buf[j] == ret) /* Confirm */
            {
                munmap(ret, PAGE_SZ);
                goto_next = 1;
            }
        }

        if(!goto_next) return 0;
    }

    /* Accumulate the return values */
    ret = 1;

    for(int i = 0; i < threads_num; i++) ret &= dummy_res[i].ret;

    return ret;
}

/* Base test for malloc and free */
int test_class_integrity_malloc(int allocs)
{
    const int alloc_num = (allocs < MAX_THREAD_BUF_SZ) ? allocs: MAX_THREAD_BUF_SZ;     /* Benchmark allocation tests */
    const int max_allocation_sz = 2047;                                                 /* Maximum request size */
    const int min_allocation_sz = 1;                                                    /* Minimum request size */
    uintptr_t align_rq = ALIGN_RQ;                                                      /* Allignment at least 16bytes */

    /* The test array */
    void *ret[alloc_num];

    /* Initialize */
    memset(ret, 0, alloc_num * sizeof(void *));

    for(int i = min_allocation_sz; i <= max_allocation_sz; i++)
    {
        for(int j = 0; j < alloc_num; j++)
        {
            int class_sz_max = class_size_decode(i, NULL);
            ret[j] = malloc(i);

            if(!ret[j])
            {
                printf("Alloc failed for [%d] size and iter [%d]\n", i, j);
                return 0;
            }

            /* Validation for sizes */
            if(i >= class_sz_max)
            {
                printf("Class size fail [%p] - [%d]\n", &ret[j], class_sz_max);
                return 0;
            }

            /* Validation for alignment */
            if(((uintptr_t) ret[j]) & align_rq)
            {
                printf("Class align fail [%p] - [%d]\n", ret[j], class_sz_max);
                return 0;
            }

            /* Validation for headers */
            memset(ret[j], 0, i);
        }

        for(int j = 0; j < alloc_num; j++)
        {
            free(ret[j]);
        }
    }

    return 1;
}

/* Base test for realloc and free */
int test_class_integrity_realloc(int allocs)
{
    const int alloc_num = (allocs < MAX_THREAD_BUF_SZ) ? allocs: MAX_THREAD_BUF_SZ;       /* Benchmark allocation tests */
    const int max_allocation_sz = 2047;                                                   /* Maximum request size */
    const int min_allocation_sz = 1;                                                      /* Minimum request size */
    uintptr_t align_rq = ALIGN_RQ;                                                        /* Allignment at least 16bytes */

    /* The test array */
    void *ret[alloc_num];

    /* Initialize */
    memset(ret, 0, alloc_num * sizeof(void *));

    for(int j = 0; j < alloc_num; j++)
    {
        for(int i = min_allocation_sz; i <= max_allocation_sz; i++)
        {
            int class_sz_max = class_size_decode(i, NULL);

            ret[j] = realloc(ret[j], i);

            if(!ret[j])
            {
                printf("Alloc failed for [%d] size and iter [%d]\n", i, j);
                return 0;
            }

            /* Validation for sizes */
            if(i >= class_sz_max)
            {
                printf("Class size fail [%p] - [%d]\n", &ret[j], class_sz_max);
                return 0;
            }

            /* Validation for alignment */
            if(((uintptr_t) ret[j]) & align_rq)
            {
                printf("Class align fail [%p] - [%d]\n", ret[j], class_sz_max);
                return 0;
            }

            /* Validation for headers */
            memset(ret[j], 0, i);
        }
    }

    for(int j = 0; j < alloc_num; j++)
    {
            free(ret[j]);
    }

    return 1;
}

/* Test mainly for local frees and local mallocs only and caching */
int test_local_threads(int threads_num, int alloc_count, int print_flag)
{
    pthread_t tid[threads_num];
    arg_t dummy_res[threads_num];
    int ret = 1;

    for (int i = 0; i < threads_num; i++)
    {
        dummy_res[i].high = alloc_count;
        dummy_res[i].ret = 0;
        pthread_create(&tid[i], NULL, thread_local_func, &dummy_res[i]);
    }

    for (int i = 0; i < threads_num; i++)
    {
        pthread_join(tid[i], NULL);
    }

    /* 2nd round */
    for (int i = 0; i < threads_num; i++)
    {
        dummy_res[i].high = alloc_count;
        dummy_res[i].ret = 0;
        pthread_create(&tid[i], NULL, thread_local_func, &dummy_res[i]);
    }

    for (int i = 0; i < threads_num; i++)
    {
        pthread_join(tid[i], NULL);
        if(print_flag) printf("[T%d]=%f |", i, dummy_res[i].res);
    }

    /* Flush output */
    if(print_flag) printf("\n");

    for(int i = 0; i < threads_num; i++)
    {
        ret &= dummy_res[i].ret;
    }

    return ret;
}

/* Tests remote frees only */
int test_remote_threads(int threads_num, int alloc_count)
{
    /* Verify */
    if((alloc_count % threads_num) || alloc_count > ALLOC_BUF_REM_SIZE)
    {
        printf("Alloc_count %% thread_num: has to be zero %d\n", alloc_count % threads_num);
        printf("Alloc_count has to be less than %d: %d\n", ALLOC_BUF_REM_SIZE, alloc_count);
        return 0;
    }

    int *buffer[alloc_count];
    pthread_t tid[threads_num];
    arg_t tid_arg[threads_num];
    int new = 1, old = 0, ret = 1;

    /* Set to zero */
    memset(buffer, 0, alloc_count * sizeof(void *));

    for (int i = 0; i < alloc_count; i++)
    {
        buffer[i] = malloc(sizeof(int));
        *buffer[i] = i;
    }

    int offset = alloc_count / threads_num;

    for (int i = 0; i < threads_num; i++)
    {
        tid_arg[i].low = i * offset;
        tid_arg[i].high = (i + 1) * offset;
        tid_arg[i].buf = buffer;
        tid_arg[i].ret = 0;
        pthread_create(&tid[i], NULL, thread_remote_func, &tid_arg[i]);
    }

    /* Threads are free to run now */
    ATOMIC_CAS(&pass, &new, &old);

    for (int i = 0; i < threads_num; i++)
    {
        pthread_join(tid[i], NULL);
    }

    /* Do a local only round of allocations/frees */
    for (int i = 0; i < alloc_count; i++)
    {
        buffer[i] = malloc(sizeof(int));
        *buffer[i] = i;
    }

    for (int i = 0; i < alloc_count; i++) free(buffer[i]);

    /* Return status code */
    for (int i = 0; i < threads_num; i++) ret &= tid_arg[i].ret;

    return ret;
}

/* Test local random mallocs/frees */
int test_shuffle_threads(int threads_num, int alloc_count, int reps, int class_stress)
{
    pthread_t tid[threads_num];
    arg_t dummy_res[threads_num];
    int ret = 1;

    for (int i = 0; i < threads_num; i++)
    {
        dummy_res[i].high = alloc_count;
        dummy_res[i].low = reps;
        dummy_res[i].ret = 0;

        if(!class_stress)
        {
            pthread_create(&tid[i], NULL, thread_shuffle_func, &dummy_res[i]);
        }
        else
        {
            pthread_create(&tid[i], NULL, thread_stress_shuffle_func, &dummy_res[i]);
        }
    }

    for (int i = 0; i < threads_num; i++)
    {
        pthread_join(tid[i], NULL);
    }

    for(int i = 0; i < threads_num; i++)
    {
        ret &= dummy_res[i].ret;
    }

    return ret;
}

/* Tests adoption policy */
int test_adoption_policy(int threads_num, int alloc_count, int reps)
{
    /* Verify */
    if((alloc_count % (threads_num - 1)) || (threads_num < 1)|| alloc_count > ALLOC_BUF_ADOPT_SIZE)
    {
        printf("Alloc_count %% thread_num: has to be zero %d\n", alloc_count % threads_num);
        printf("Alloc_count has to be less than %d: %d\n", ALLOC_BUF_ADOPT_SIZE, alloc_count);
        return 0;
    }

    pthread_t tid[threads_num];
    arg_t dummy_res[threads_num];
    void *alloc_buf[alloc_count];
    int j = 0, ret = 1;

    while (j < reps)
    {
        memset(alloc_buf, 0, alloc_count * sizeof(void *));

        dummy_res[0].buf = alloc_buf;
        dummy_res[0].high = dummy_res[0].low = 0;
        dummy_res[0].ret = alloc_count;

        /* Producer - Malloc with leaks for this thread */
        pthread_create(&tid[0], NULL, thread_alloc_no_free_func, &dummy_res[0]);
        pthread_join(tid[0], NULL);

        int offset = alloc_count / (threads_num - 1);

        /* Consumers - Remote free / Pagesteals */
        for (int i = 1; i < threads_num; i++)
        {
            dummy_res[i].buf = alloc_buf;
            dummy_res[i].high = i * offset;
            dummy_res[i].low = (i - 1) * offset;
            dummy_res[i].ret = alloc_count / 2;
            pthread_create(&tid[i], NULL, thread_adoption_func, &dummy_res[i]);
        }

        for (int i = 1; i < threads_num; i++) pthread_join(tid[i], NULL);
        for (int i = 1; i < threads_num; i++) ret &= dummy_res[i].ret;
        if (!ret) return 0;

        j++;
    }


    return ret;
}

/* Test main just for compilation */
int main(int argc, char **argv)
{
    int ret;

    const int testcases_num = 9;
    const char *test_names[] =
    {
        "counting-atomic-LIFO",
        "class-integrity-malloc",
        "class-integrity-realloc",
        "local-only-threads",
        "remote-only-threads",
        "shuffle-local-threads",
        "shuffle-complex-local-threads",
        "adoption-policy-stress",
        "run-all-tests"
    };

    /* Get the testcase ID */
    int testcase_id = (argc != 2) ? 10000: atoi(argv[1]);
    int break_flag = 1;

    if(testcase_id == 8) /* If we want to run the full testsuite */
    {
        break_flag = 0;
        testcase_id = 0;
        printf("\n***********Running FULL testsuite************\n");
    }

    switch(testcase_id)
    {
        case 0:
        {
            ret = test_atomic_counting_queues(5, 20000);
            printf("Atomic counting queues test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 1:
        {
            ret = test_class_integrity_malloc(1000);
            printf("Simple malloc integrity test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 2:
        {
            ret = test_class_integrity_realloc(1000);
            printf("Simple realloc integrity test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 3:
        {
            ret = test_local_threads(6, 100000, 0);
            printf("Local thread test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 4:
        {
            ret = test_remote_threads(20, 400000);
            printf("Remote thread test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 5:
        {
            ret = test_shuffle_threads(6, 1000, 6, 0);
            printf("Shuffle thread test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 6:
        {
            ret = test_shuffle_threads(10, 1000, 6, 1);
            printf("Shuffle thread stress test: [PASSED] = %s\n", ret ? "YES":"NO");
            if(break_flag) break;
        }
        case 7:
        {
            ret = test_adoption_policy(11, 500000, 10);
            printf("Adoption thread test: [PASSED] = %s\n", ret ? "YES":"NO");
            break;
        }
        default:
        {
            printf("Wrong arguments!! => ./test_alloc <Testcase_num> \n");
            printf("Testcases:\n");
            for(int i = 0; i < testcases_num; i++) printf("\t Name:[%s] - ID:[%d]\n", test_names[i], i);
            exit(-1);
        }
    }

    /* Debug info */
    malloc_debug_stats();

    return ret;
}

