/*
 * Android/Linux compatibility: <TargetConditionals.h>
 *
 * host/jit_region.c and host/vm.c include this to branch on the Apple platform
 * with TARGET_OS_OSX / TARGET_OS_IPHONE. On Android every Apple platform macro
 * is 0, so the Darwin-only paths compile away and the #else branches (which
 * already describe a non-Apple host) are what run.
 *
 * Note that this is exactly backwards from Apple, where TARGET_OS_MAC is 1 and
 * TARGET_OS_IPHONE is 1 on iOS. Both are 0 here, and neither is "the other
 * platform": the callers check them as "am I on macOS", not "am I on Apple".
 */
#pragma once

#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_TV 0
#define TARGET_OS_WATCH 0
#define TARGET_OS_VISION 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_EMBEDDED 0
