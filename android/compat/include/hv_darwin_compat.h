/*
 * Android/Linux compatibility, force-included into every engine translation unit.
 *
 * Apple's <pthread.h> carries a few Darwin extensions the engine calls that
 * bionic does not declare. They cannot be added by shadowing <pthread.h> (the
 * sources take bionic's for everything else), so the CMakeLists passes this
 * header with -include.
 */
#pragma once

#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * core/dispatch.c:486 waits for the next wake-up with a *relative* timeout.
 *
 * The translation matters: pthread_cond_timedwait() takes an absolute time on
 * the condvar's clock, and by default that clock is CLOCK_REALTIME, which NTP
 * can step. A guest waiting 50 us must not be woken early or late because the
 * wall clock moved, so this waits on CLOCK_MONOTONIC instead.
 *
 * pthread_cond_clockwait() is the portable way to pick the clock (bionic from
 * API 30); before that the deadline is converted to CLOCK_REALTIME, which is
 * correct except across a clock step.
 */
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 30
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                                     const struct timespec *rel) {
    struct timespec abs;
    clock_gettime(CLOCK_MONOTONIC, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    while (abs.tv_nsec >= 1000000000L) {
        abs.tv_nsec -= 1000000000L;
        abs.tv_sec++;
    }
    return pthread_cond_clockwait(cond, mutex, CLOCK_MONOTONIC, &abs);
}
#else
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                                     const struct timespec *rel) {
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    while (abs.tv_nsec >= 1000000000L) {
        abs.tv_nsec -= 1000000000L;
        abs.tv_sec++;
    }
    return pthread_cond_timedwait(cond, mutex, &abs);
}
#endif

#ifdef __cplusplus
}
#endif
