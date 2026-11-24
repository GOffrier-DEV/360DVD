#ifndef DVD360_PTHREAD_H
#define DVD360_PTHREAD_H

#include <stdatomic.h>

typedef struct { atomic_flag held; } pthread_mutex_t;
typedef int pthread_mutexattr_t;

static inline int pthread_mutex_init(pthread_mutex_t *mutex,
                                     const pthread_mutexattr_t *attributes)
{
    (void)attributes;
    atomic_flag_clear(&mutex->held);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    while (atomic_flag_test_and_set(&mutex->held)) { }
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    atomic_flag_clear(&mutex->held);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

#endif
