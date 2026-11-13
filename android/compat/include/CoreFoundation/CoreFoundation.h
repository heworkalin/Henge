/*
 * Android/Linux compatibility: <CoreFoundation/CoreFoundation.h>
 *
 * The engine takes exactly two CoreFoundation calls: CFRetain and CFRelease, and
 * only on the display layer handle — devices/pvg_lite.c keeps it so the Metal
 * replay can be handed a CAMetalLayer lazily, and host/vm.c does the same for the
 * iOS host.
 *
 * On Android there is no Metal and no CAMetalLayer: the PVG device is replaced by
 * reims-vgpu (plan.md §0.1), so no Android caller ever sets a layer and the
 * handle is never dereferenced. It is a token, and it must still be retained and
 * released with balanced counts because pvg_lite.c checks nothing else.
 *
 * So this is a reference *counter* over a fixed set of tokens, not an object
 * system: CFRetain adds or increments an entry, CFRelease decrements and drops it
 * at zero. Nothing is allocated or freed — the object belongs to whoever made it,
 * which is what CoreFoundation means too. Retaining a token we have never seen is
 * valid; releasing one to zero simply forgets it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  /* getenv: devices/pvg_lite.c takes it from this chain on Apple */
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const void *CFTypeRef;
typedef const void *CFStringRef;
typedef const void *CFDictionaryRef;
typedef unsigned char Boolean;

CFTypeRef CFRetain(CFTypeRef cf);
void CFRelease(CFTypeRef cf);

#ifdef __cplusplus
}
#endif
