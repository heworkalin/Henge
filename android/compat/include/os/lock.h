/*
 * Android/Linux compatibility: <os/lock.h>
 *
 * os_unfair_lock is a non-recursive mutex. Reached from devices/pvg_lite.c and
 * host/vm.c for the display-layer handoff, and those two use it only as a
 * short critical section — pthread_mutex_t has the same contract, and Android's
 * bionic implements it with futexes, so the cost is the same shape.
 *
 * os_unfair_lock_lock() on a lock already owned by the caller is a documented
 * no-op (a recursive lock request on an unfair lock does not deadlock), but
 * neither caller re-enters, so the plain mutex behaviour is what they expect.
 */
#pragma once

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef pthread_mutex_t os_unfair_lock;
typedef pthread_mutex_t *os_unfair_lock_t;

/*
 * Apple's OS_UNFAIR_LOCK_INIT is a compound literal,
 *
 *     #define OS_UNFAIR_LOCK_INIT ((os_unfair_lock){0})
 *
 * not a bare brace initialiser, which is why callers can *assign* it to an
 * existing lock (devices/pvg_lite.c: `p->layer_lock = OS_UNFAIR_LOCK_INIT;`).
 * The same shape built from bionic's PTHREAD_MUTEX_INITIALIZER keeps that
 * assignment valid, both in a declaration and at file scope.
 */
#define OS_UNFAIR_LOCK_INIT ((os_unfair_lock)PTHREAD_MUTEX_INITIALIZER)

static inline void os_unfair_lock_lock(os_unfair_lock_t lock) { pthread_mutex_lock(lock); }
static inline void os_unfair_lock_unlock(os_unfair_lock_t lock) { pthread_mutex_unlock(lock); }
static inline int os_unfair_lock_trylock(os_unfair_lock_t lock) { return pthread_mutex_trylock(lock) == 0; }

#ifdef __cplusplus
}
#endif
