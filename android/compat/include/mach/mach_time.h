/*
 * Android/Linux compatibility: <mach/mach_time.h>
 *
 * The Henge engine counts time in Apple's 24 MHz tick everywhere it matters:
 *   core/sysreg.c     CNTFRQ = 24000000, counter() feeds the guest's CNTPCT
 *   core/dispatch.c   24 * 50 == 50 us, 24000 ticks == 1 ms, * 24000000ull for seconds
 *   core/machine.c    dt < 240 / 2400 / 24000 buckets
 *   devices/pvg_lite.c 24000000ull / refresh rate
 *
 * So mach_absolute_time() must keep returning 24 MHz ticks on Android. The
 * implementation derives them from CLOCK_MONOTONIC_RAW (no NTP slew), which
 * keeps the guest's counter monotonic and free of step adjustments.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HV_TICK_HZ 24000000ull /* = core/sysreg.h CNTFRQ */

uint64_t mach_absolute_time(void);

#ifdef __cplusplus
}
#endif
