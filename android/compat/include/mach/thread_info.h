/*
 * Android/Linux compatibility: <mach/thread_info.h>, folded into <mach/mach.h>.
 *
 * core/dispatch.c and core/machine.c ask the kernel whether a CPU thread is
 * running and how much CPU time it has burned, to tell "the host took the CPU
 * away" from "the engine is spinning". That is diagnostics, not behaviour, but
 * the numbers should be real rather than invented, so this maps onto Linux's
 * per-thread clocks and /proc.
 *
 * mach_port_t here is an opaque slot number, because the Mach port namespace
 * does not exist. mach_thread_self() hands out one slot per thread and
 * thread_info() maps it back, so a handle captured earlier keeps naming the
 * same thread — which is what core/machine.c's sync probe relies on.
 */
#pragma once

#include <mach/mach.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t seconds;
    int32_t microseconds;
} time_value_t;

#define TH_STATE_RUNNING 1
#define TH_STATE_STOPPED 2
#define TH_STATE_WAITING 3
#define TH_STATE_UNINTERRUPTIBLE 4
#define TH_STATE_HALTED 5

typedef struct {
    time_value_t user_time;   /* CPU time burned in user mode */
    time_value_t system_time; /* CPU time burned in kernel mode */
    int32_t run_state;        /* TH_STATE_RUNNING when the thread is on a CPU */
    int32_t flags;
    int32_t suspend_count;
    int32_t sleep_time; /* seconds spent sleeping, -1 when not applicable */
} thread_basic_info_data_t;

typedef thread_basic_info_data_t *thread_basic_info_t;
typedef int *thread_info_t;

#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT                                                                \
    ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(natural_t)))

kern_return_t thread_info(mach_port_t thread, int flavor, thread_info_t info, mach_msg_type_number_t *count);

#ifdef __cplusplus
}
#endif
