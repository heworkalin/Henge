/*
 * The PVG Metal replay, absent on Android.
 *
 * devices/pvg_lite.c is the PVG device model: it parses the guest's FIFO and
 * MMIO. devices/pvg_metal.m is the other half — it takes what the parser hands it
 * and replays it onto a real Metal device, which is what actually draws.
 *
 * Android has no Metal. The replacement is reims-vgpu (plan.md §0.1), which takes
 * the same guest command stream and runs it on Vulkan — so on Android the Metal
 * replay is not ported, it is *replaced*, and until Stage 2 wires reims-vgpu in
 * there is no replay to call.
 *
 * These definitions exist so the device model links and behaves honestly in the
 * meantime: the GPU's MMIO and FIFO are still emulated (so the guest's driver
 * talks to a real device and does not fault), the FIFO is still consumed, and
 * nothing is drawn. A frame request says so instead of handing back a buffer of
 * garbage that would look like a rendering bug.
 *
 * hv_pvg_metal_create() returning a non-NULL token is deliberate: pvg_lite.c
 * treats NULL as "no replay" and skips the paths these stubs answer, which would
 * hide where the frames are being dropped. Keeping a token makes the drop
 * visible in one place.
 */

#include "pvg_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HV_NO_REPLAY ((HvPvgMetal *)&hv_no_replay_token)

static char hv_no_replay_token;
static int hv_reported;

static void report_once(void) {
    if (hv_reported) return;
    hv_reported = 1;
    fprintf(stderr,
            "hengevm: PVG: no Metal replay on this build — the guest's GPU commands are "
            "parsed and discarded; the display will stay blank until reims-vgpu is wired in "
            "(plan.md Stage 2)\n");
}

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) {
    (void)lite;
    report_once();
    return HV_NO_REPLAY;
}

void hv_pvg_metal_destroy(HvPvgMetal *m) { (void)m; }

void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t channel, uint32_t *command, uint32_t length) {
    (void)m;
    (void)lite;
    (void)channel;
    (void)command;
    (void)length;
}

void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) {
    (void)m;
    (void)layer;
}

uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) {
    (void)m;
    return 0;
}

void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; }

void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) {
    (void)m;
    (void)mapping;
}

/* No frame exists, so there is nothing to copy and no pixels to point at. */
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t width, uint32_t height) {
    (void)m;
    (void)bgra;
    (void)width;
    (void)height;
    return -1;
}

int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **pixels, uint32_t *width, uint32_t *height) {
    (void)m;
    if (pixels) *pixels = NULL;
    if (width) *width = 0;
    if (height) *height = 0;
    return -1;
}

void hv_pvg_metal_quiesce(HvPvgMetal *m) { (void)m; }

/* Nothing to save or restore: there is no replay state to preserve across a
 * snapshot of the machine. Reporting zero bytes keeps a caller from believing it
 * captured something. */
size_t hv_pvg_metal_display_state(HvPvgMetal *m, void *out, size_t cap) {
    (void)m;
    (void)out;
    (void)cap;
    return 0;
}

void hv_pvg_metal_set_display_state(HvPvgMetal *m, const void *in, size_t len) {
    (void)m;
    (void)in;
    (void)len;
}
