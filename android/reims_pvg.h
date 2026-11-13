/*
 * The paravirtual GPU on reims-vgpu (see reims_pvg.c).
 *
 * The shape `devices/vmapple.c` used for the Metal device model, with the arguments it actually
 * has: the two MMIO windows, the two interrupt lines, and the guest's RAM together with the file
 * behind it.
 */
#ifndef HENGE_REIMS_PVG_H
#define HENGE_REIMS_PVG_H

#include <stdint.h>

typedef struct HvReimsPvg HvReimsPvg;

/*
 * Creates the device and registers its MMIO windows.
 *
 * `gfx_pa` and `iosfc_pa` are the two windows the guest finds the GPU and its IOSurface mapper
 * at; `gfx_irq` and `iosfc_irq` are their interrupt lines, as the GIC's INTIDs.
 *
 * `ram` with `ram_size` at `ram_pa` is the guest's RAM. `ram_fd` is the file behind it, which
 * reims-vgpu maps to build its views of guest pages — pass -1 and the GPU cannot reach guest
 * memory at all. On Android that file is a memfd (`devices/vmapple.c`).
 *
 * Returns NULL when reims-vgpu refuses the device.
 */
HvReimsPvg *hv_reims_pvg_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa,
                                int gfx_irq, int iosfc_irq, uint64_t ram_pa, uint8_t *ram,
                                uint64_t ram_size, int ram_fd);

/*
 * Hands the host's `ANativeWindow` to the display. Call it when the activity's surface exists, and
 * again whenever it comes back. The window must outlive `hv_reims_pvg_destroy`.
 */
void hv_reims_pvg_attach_window(HvReimsPvg *p, void *native_window);

/* Whether the device got its display and its IOSurface mapper. For bring-up: a device whose
 * display was refused still runs, it just has nowhere to show a frame. */
int hv_reims_pvg_have_display(HvReimsPvg *p);
int hv_reims_pvg_have_mapper(HvReimsPvg *p);

/* The engine's device tick: presents a frame when the guest has presented one. */
void hv_reims_pvg_poll(HvReimsPvg *p);

void hv_reims_pvg_destroy(HvReimsPvg *p);

#endif /* HENGE_REIMS_PVG_H */
