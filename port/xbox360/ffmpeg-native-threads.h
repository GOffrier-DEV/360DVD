/* RXDK implementation of the subset used by FFmpeg's w32threads backend. */
#ifndef COMPAT_W32PTHREADS_H
#define COMPAT_W32PTHREADS_H

#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

/* Keep XDK and picolibc headers in separate translation units. */
void *dvd360_mutex_create(void);
void dvd360_mutex_destroy(void *);
void dvd360_mutex_lock(void *);
int dvd360_mutex_trylock(void *);
void dvd360_mutex_unlock(void *);
void *dvd360_event_create(void);
void dvd360_event_set(void *);
int dvd360_wait(void *, uint32_t);
void dvd360_handle_close(void *);
void dvd360_yield(void);
void *dvd360_thread_create(unsigned (*)(void *), void *);
int32_t dvd360_compare_exchange(volatile int32_t *, int32_t, int32_t);
void dvd360_exchange(volatile int32_t *, int32_t);
uint32_t dvd360_fetch_add(volatile uint32_t *, uint32_t);

typedef volatile int32_t pthread_once_t;
#define PTHREAD_ONCE_INIT 0

static inline int pthread_once(pthread_once_t *once, void (*init)(void))
{
    if (dvd360_compare_exchange(once, 1, 0) == 0) {
        init();
        dvd360_exchange(once, 2);
    } else {
        while (dvd360_compare_exchange(once, 2, 2) != 2)
            dvd360_yield();
    }
    return 0;
}

typedef struct {
    void *cs;
    pthread_once_t state;
} pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }

static inline void rxdk_mutex_ready(pthread_mutex_t *m)
{
    const int32_t state = dvd360_compare_exchange(&m->state, 1, 0);
    if (state == 2)
        return;
    if (state == 0) {
        m->cs = dvd360_mutex_create();
        if (!m->cs)
            abort();
        dvd360_exchange(&m->state, 2);
    } else {
        while (dvd360_compare_exchange(&m->state, 2, 2) != 2)
            dvd360_yield();
    }
}

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    m->state = 0;
    rxdk_mutex_ready(m);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (m->state == 2)
        dvd360_mutex_destroy(m->cs);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    rxdk_mutex_ready(m);
    dvd360_mutex_lock(m->cs);
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *m)
{
    rxdk_mutex_ready(m);
    return dvd360_mutex_trylock(m->cs) ? 0 : EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    dvd360_mutex_unlock(m->cs);
    return 0;
}

typedef struct RxdkWaiter {
    struct RxdkWaiter *next;
    void *event;
    int signaled;
} RxdkWaiter;

typedef struct {
    pthread_mutex_t lock;
    RxdkWaiter *head;
} pthread_cond_t;
#define PTHREAD_COND_INITIALIZER { 0 }

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr)
{
    c->head = NULL;
    return pthread_mutex_init(&c->lock, NULL);
}

static inline int pthread_cond_destroy(pthread_cond_t *c)
{
    return pthread_mutex_destroy(&c->lock);
}

static inline int rxdk_cond_wait(pthread_cond_t *c, pthread_mutex_t *m, uint32_t timeout)
{
    RxdkWaiter waiter = { NULL, dvd360_event_create(), 0 };
    if (!waiter.event)
        abort();
    pthread_mutex_lock(&c->lock);
    waiter.next = c->head;
    c->head = &waiter;
    pthread_mutex_unlock(&c->lock);
    pthread_mutex_unlock(m);
    int result = dvd360_wait(waiter.event, timeout);
    pthread_mutex_lock(&c->lock);
    RxdkWaiter **link = &c->head;
    while (*link != &waiter)
        link = &(*link)->next;
    *link = waiter.next;
    pthread_mutex_unlock(&c->lock);
    dvd360_handle_close(waiter.event);
    pthread_mutex_lock(m);
    return waiter.signaled || result == 0 ? 0 : result == 1 ? ETIMEDOUT : EINVAL;
}

static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return rxdk_cond_wait(c, m, UINT32_MAX);
}

static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         const struct timespec *at)
{
    int64_t ms = (int64_t)at->tv_sec * 1000 + at->tv_nsec / 1000000 - av_gettime() / 1000;
    return rxdk_cond_wait(c, m, ms <= 0 ? 0 : ms >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)ms);
}

static inline int rxdk_cond_wake(pthread_cond_t *c, int all)
{
    pthread_mutex_lock(&c->lock);
    for (RxdkWaiter *w = c->head; w; w = w->next) {
        if (!w->signaled) {
            w->signaled = 1;
            dvd360_event_set(w->event);
            if (!all)
                break;
        }
    }
    return pthread_mutex_unlock(&c->lock);
}

static inline int pthread_cond_signal(pthread_cond_t *c) { return rxdk_cond_wake(c, 0); }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { return rxdk_cond_wake(c, 1); }

typedef struct RxdkThread {
    void *handle;
    void *(*run)(void *);
    void *arg, *result;
} *pthread_t;

static unsigned rxdk_thread_start(void *opaque)
{
    pthread_t t = opaque;
    t->result = t->run(t->arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*run)(void *), void *arg)
{
    pthread_t t = av_mallocz(sizeof(*t));
    if (!t)
        return EAGAIN;
    t->run = run;
    t->arg = arg;
    t->handle = dvd360_thread_create(rxdk_thread_start, t);
    if (!t->handle) {
        av_free(t);
        return EAGAIN;
    }
    *thread = t;
    return 0;
}

static inline int pthread_join(pthread_t t, void **result)
{
    if (dvd360_wait(t->handle, UINT32_MAX) != 0)
        return EINVAL;
    if (result)
        *result = t->result;
    dvd360_handle_close(t->handle);
    av_free(t);
    return 0;
}

static inline int win32_thread_setname(const char *name) { return AVERROR(ENOSYS); }

#endif
