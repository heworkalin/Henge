/*
 * Android/Linux compatibility: <mach/mach.h>
 *
 * Only what the engine actually reaches for is declared here:
 *   mach_task_self()        — identity, this process
 *   mach_thread_self()      — identity, this thread (host/main.c profiling only)
 *   mach_port_deallocate()  — port lifetimes do not exist here, so a no-op
 *   mach_error_string()     — for the diagnostic prints
 * plus the scalar types and KERN_SUCCESS those calls are spelled with.
 *
 * The guest RAM helper in devices/vmapple.c is an iOS-only trick: it asks the
 * kernel to keep pages out of the app's jetsam footprint, which is how an iOS
 * app is allowed to hold 4-8 GB. Android has no footprint ledger to escape, so
 * vm_allocate() is a plain anonymous mmap and the memory-entry calls become
 * successes that change nothing (see <mach/vm_map.h>).
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int kern_return_t;
typedef int mach_error_t;
typedef unsigned int mach_port_t;
typedef unsigned int mach_msg_type_number_t;
typedef unsigned int natural_t;
typedef mach_port_t task_t;
typedef mach_port_t thread_t;

#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_PROTECTION_FAILURE 2
#define KERN_NO_SPACE 3
#define KERN_INVALID_ARGUMENT 4
#define KERN_FAILURE 5

#define MACH_PORT_NULL 0
#define MACH_PORT_VALID(p) ((p) != MACH_PORT_NULL)
#define TASK_NULL 0

/* Apple spells these in <mach/boolean.h>; devices/vmapple.c passes FALSE to
 * vm_map(). bionic does not define them. */
typedef int boolean_t;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

mach_port_t mach_task_self(void);
mach_port_t mach_thread_self(void);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
char *mach_error_string(kern_return_t kr);

#ifdef __cplusplus
}
#endif

/*
 * core/dispatch.c and core/machine.c use thread_basic_info_data_t while
 * including only <mach/mach.h>, as Apple's header provides it. Included at the
 * bottom so <mach/thread_info.h> can include this one without recursing (the
 * #pragma once above is already in force by the time this line runs).
 */
#include <mach/thread_info.h>
