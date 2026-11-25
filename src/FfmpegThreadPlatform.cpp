#include <xtl.h>
#include <stdlib.h>
#include <stdint.h>

namespace
{
    struct ThreadStart
    {
        unsigned (*run)(void*);
        void* arg;
    };

    DWORD WINAPI RunThread(void* opaque)
    {
        ThreadStart start = *static_cast<ThreadStart*>(opaque);
        free(opaque);
        return start.run(start.arg);
    }
}

extern "C" {
void* dvd360_mutex_create()
{
    CRITICAL_SECTION* mutex = static_cast<CRITICAL_SECTION*>(malloc(sizeof(CRITICAL_SECTION)));
    if (mutex)
        InitializeCriticalSection(mutex);
    return mutex;
}
void dvd360_mutex_destroy(void* mutex)
{
    DeleteCriticalSection(static_cast<CRITICAL_SECTION*>(mutex));
    free(mutex);
}
void dvd360_mutex_lock(void* mutex) { EnterCriticalSection(static_cast<CRITICAL_SECTION*>(mutex)); }
int dvd360_mutex_trylock(void* mutex) { return TryEnterCriticalSection(static_cast<CRITICAL_SECTION*>(mutex)); }
void dvd360_mutex_unlock(void* mutex) { LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(mutex)); }
void* dvd360_event_create() { return CreateEvent(NULL, FALSE, FALSE, NULL); }
void dvd360_event_set(void* event) { SetEvent(event); }
int dvd360_wait(void* handle, unsigned timeout)
{
    const DWORD status = WaitForSingleObject(handle, timeout);
    return status == WAIT_OBJECT_0 ? 0 : status == WAIT_TIMEOUT ? 1 : -1;
}
void dvd360_handle_close(void* handle) { CloseHandle(handle); }
void dvd360_yield() { Sleep(0); }
int32_t dvd360_compare_exchange(volatile int32_t* value, int32_t next, int32_t expected)
{
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(value), next, expected);
}
void dvd360_exchange(volatile int32_t* value, int32_t next)
{
    InterlockedExchange(reinterpret_cast<volatile LONG*>(value), next);
}
uint32_t dvd360_fetch_add(volatile uint32_t* value, uint32_t add)
{
    return InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(value), add);
}
void* dvd360_thread_create(unsigned (*run)(void*), void* arg)
{
    ThreadStart* start = static_cast<ThreadStart*>(malloc(sizeof(ThreadStart)));
    if (!start)
        return NULL;
    start->run = run;
    start->arg = arg;
    HANDLE thread = CreateThread(NULL, 0, RunThread, start, CREATE_SUSPENDED, NULL);
    if (!thread)
    {
        free(start);
        return NULL;
    }
    XSetThreadProcessor(thread, 4);
    ResumeThread(thread);
    return thread;
}
}
