#ifndef ODZ_THREAD_H
#define ODZ_THREAD_H

/*
 * Thin threading wrapper: pthreads on POSIX, Win32 threads on Windows.
 * Only provides create + join -- that's all the compressor needs.
 */

#ifdef _WIN32

#include <stdlib.h>
#include <windows.h>
#include <process.h>

typedef HANDLE odz_thread_t;
#define ODZ_THREAD_NULL ((odz_thread_t)0)

typedef struct { void *(*fn)(void *); void *arg; } odz_tramp_t_;

static unsigned __stdcall odz_thread_fn_(void *p) {
    odz_tramp_t_ t = *((odz_tramp_t_ *)p);
    free(p);
    t.fn(t.arg);
    return 0;
}

static inline int odz_thread_create(odz_thread_t *t, void *(*fn)(void *), void *arg) {
    odz_tramp_t_ *tr = (odz_tramp_t_ *)malloc(sizeof(*tr));
    if (!tr) return -1;
    tr->fn = fn; tr->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, odz_thread_fn_, tr, 0, NULL);
    if (!*t) { free(tr); return -1; }
    return 0;
}

static inline void odz_thread_join(odz_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else /* POSIX */

#include <pthread.h>

typedef pthread_t odz_thread_t;
#define ODZ_THREAD_NULL ((odz_thread_t)0)

static inline int odz_thread_create(odz_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static inline void odz_thread_join(odz_thread_t t) {
    pthread_join(t, NULL);
}

#endif
#endif /* ODZ_THREAD_H */
