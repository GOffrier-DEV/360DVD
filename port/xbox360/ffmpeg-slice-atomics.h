/* The RXDK SDK interlocked functions emit Xenon-safe reservation sequences. */
#include <stdint.h>
#include <stdatomic.h>

uint32_t dvd360_fetch_add(volatile uint32_t *, uint32_t);
int32_t dvd360_compare_exchange(volatile int32_t *, int32_t, int32_t);

static inline int dvd360_slice_compare_exchange(atomic_int *value, int *expected, int next)
{
    int prior = dvd360_compare_exchange((volatile int32_t *)value, next, *expected);
    if (prior == *expected)
        return 1;
    *expected = prior;
    return 0;
}

#undef atomic_fetch_add_explicit
#define atomic_fetch_add_explicit(value, add, order) \
    dvd360_fetch_add((volatile uint32_t *)(value), (add))
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(value, expected, next, success, failure) \
    dvd360_slice_compare_exchange((value), (expected), (next))
