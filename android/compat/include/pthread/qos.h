/*
 * Android/Linux compatibility: <pthread/qos.h>
 *
 * devices/vmapple.c asks for QOS_CLASS_UTILITY on its prefault thread, so that
 * the background pass that touches guest pages before the guest does yields to
 * the vCPUs. Apple's QoS classes are a scheduler hint; Linux/Android's
 * equivalent is the thread's nice value, which needs no privilege to raise
 * (lowering priority is always allowed, raising it is not).
 *
 * A no-op would be acceptable — the prefault thread only warms pages — but
 * demoting it is free and is what the call was asking for.
 */
#pragma once

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int pthread_qos_class_t;

#define QOS_CLASS_USER_INTERACTIVE 0x21
#define QOS_CLASS_USER_INITIATED 0x19
#define QOS_CLASS_DEFAULT 0x15
#define QOS_CLASS_UTILITY 0x11
#define QOS_CLASS_BACKGROUND 0x09
#define QOS_CLASS_UNSPECIFIED 0x00

/*
 * QOS_CLASS_UTILITY is Apple's "work the user is not waiting on". nice 10 is
 * the conventional Linux stand-in. prio is a relative offset Apple applies
 * within the class; it is accepted and ignored, because Linux's nice value is
 * the only knob available without privileges.
 */
static inline int pthread_set_qos_class_self_np(pthread_qos_class_t qos_class, int prio) {
    (void)prio;
    int nice_value;
    switch (qos_class) {
        case QOS_CLASS_USER_INTERACTIVE:
        case QOS_CLASS_USER_INITIATED:
        case QOS_CLASS_DEFAULT: nice_value = 0; break;
        case QOS_CLASS_UTILITY: nice_value = 10; break;
        case QOS_CLASS_BACKGROUND: nice_value = 19; break;
        default: return 0; /* UNSPECIFIED: leave the scheduler alone */
    }
#if defined(__ANDROID__)
    setpriority(PRIO_PROCESS, (id_t)gettid(), nice_value);
#else
    setpriority(PRIO_PROCESS, 0, nice_value);
#endif
    return 0;
}

#ifdef __cplusplus
}
#endif
