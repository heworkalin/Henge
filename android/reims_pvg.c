/*
 * The paravirtual GPU, on reims-vgpu instead of on Metal.
 *
 * `devices/pvg_lite.c` is Henge's own PVG device model: it parses the FIFO the guest's GPU
 * driver writes and replays the commands through `pvg_metal.m` onto Metal.framework. Android has
 * no Metal, so this file replaces the whole of that: the same two MMIO windows are registered at
 * the same addresses and the same interrupt lines are driven, but every register access and every
 * command goes to `reims-vgpu`, which reimplements the device properly and replays it onto
 * Vulkan.
 *
 * What the engine sees is deliberately the same shape as before: `devices/vmapple.c` created the
 * old device with `hv_pvg_lite_create()` and polled it with `hv_pvg_lite_poll()`, and this file
 * offers `hv_reims_pvg_create()` / `hv_reims_pvg_poll()` / `hv_reims_pvg_destroy()` with the
 * arguments it actually has. Nothing else in the engine changes.
 *
 * Three things are worth knowing before reading on.
 *
 * **Guest RAM must be a file.** reims-vgpu builds its views of guest pages by mapping the file
 * behind them (`RvgMemoryRangeFile`); a range with no file gets no view, and a guest use that
 * needs one fails. That is why `hv_vmapple_create()` puts guest RAM in a memfd on Android
 * (`devices/vmapple.c`), and why the descriptor is passed in here rather than reached through the
 * machine.
 *
 * **The window belongs to the host.** An Android app's display surface is the activity's
 * `SurfaceView` surface, not something this library opens. The display is created with a window
 * config so that `reims-vgpu` accepts frames for it, and the `ANativeWindow` arrives later
 * through `hv_reims_pvg_attach_window()`, once the activity has a surface.
 *
 * **Interrupts come back as callbacks.** `raise_interrupt` runs on reims-vgpu's own thread with
 * the device's signal lock held, so it does the one thing that is safe there: pend the line in the
 * GIC. It must not wait for a host thread that is inside a call on this device.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <reims_vgpu.h>

#include "gicv3.h"
#include "machine.h"
#include "reims_pvg.h"

/* The MMIO windows the guest's GPU driver and its IOSurface mapper find the device at. They are
 * the same two the Metal device model used (`VMAPPLE_GFX`, `VMAPPLE_IOSFC`), and the same size:
 * one 64 KiB page each, which is what the guest's device tree says. */
#define REIMS_PVG_MMIO_SIZE 0x10000

struct HvReimsPvg {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq;
    int iosfc_irq;

    RvgDevice *device;
    RvgDisplay *display;
    RvgIOSurfaceDevice *iosurface;

    /* The guest's RAM, as reims-vgpu was given it: one range at `VMAPPLE_MEM`. The map and its
     * file table are copied by `rvg_device_create()`, so these stay as they are for as long as
     * the device lives. */
    RvgMemoryRange range;
    RvgMemoryMap map;
    RvgMemoryRangeFile file;

    /* The host callbacks' context, which is this device. */
    RvgHostCallbacks host;
    RvgIOSurfaceCallbacks iosurface_callbacks;

    /* Frames the host asked for and that have not been presented: `hv_reims_pvg_poll` encodes
     * one and commits it. */
    int frame_pending;
    /* Whether the host's window is attached. Until it is, the display is off-screen: the guest
     * can still set a mode and present, and the frames can be encoded and read back. */
    int window_attached;
    /* Off-screen frames reported so far, so the first one and every sixtieth answer "is the
     * guest drawing" without filling the log. */
    uint64_t offscreen_frames;
    /* Whether each device's first raise has been named in the log (see on_raise_interrupt). */
    int raises_logged, iosfc_raises_logged;
    /* Whether the guest has set a mode, and to what: until it has, there is nothing to present
     * and the window stays as it is. */
    int have_mode;
    unsigned long long gfx_writes, iosfc_writes, gfx_reads;
    unsigned long long new_frames;
    uint16_t mode_width, mode_height;
    uint32_t mode_format;
    int log;
};

/* reims-vgpu's log, into logcat: without this a refusal inside the library is silent, and the
 * first sign of trouble is an abort in a thread with no message. The strings are borrowed for the
 * call and not NUL-terminated, hence the "%.*s". */
static void on_log(void *ctx, RvgLogLevel level, const char *target, size_t target_length,
                   const char *message, size_t message_length) {
    (void)ctx;
    /*
     * The fifo thread polls the command ring and reports an empty one as "command 0 of length 0,
     * outside 12..=49152; skipped" -- every poll, several times a millisecond. That is the ring
     * being empty, which is the normal state before the guest submits anything, and it filled a
     * 182 MB log while saying nothing. Once every five seconds is enough to see it is happening.
     */
    static const char *const noise = "command 0 of length 0";
    static time_t last;
    if (message_length >= 22 && memmem(message, message_length, noise, 22)) {
        time_t now = time(NULL);
        if (now - last < 5) return;
        last = now;
    }
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case RVG_LOG_ERROR: priority = ANDROID_LOG_ERROR; break;
    case RVG_LOG_WARN: priority = ANDROID_LOG_WARN; break;
    case RVG_LOG_DEBUG: priority = ANDROID_LOG_DEBUG; break;
    case RVG_LOG_TRACE: priority = ANDROID_LOG_VERBOSE; break;
    default: priority = ANDROID_LOG_INFO; break;
    }
    /* logcat *and* stderr: a command-line run has no logcat reader, and a warning from inside the
     * library (a refused command, a fifo fault) was invisible there -- the one place it matters
     * most, because the CLI is where the guest is brought up. */
    /* Into the log file too, not only logcat: a warning from the library is the only thing that
     * explains a black window, and the app's log file is the one place a run can be read after the
     * fact. Everything with HV_REIMS_LOG; the warnings and errors always. */
    if (getenv("HV_REIMS_LOG") || level == RVG_LOG_ERROR || level == RVG_LOG_WARN) {
        fprintf(stderr, "reims[%.*s]: %.*s\n", (int)target_length, target, (int)message_length,
                message);
    }
    __android_log_print(priority, "HENGEREIMS", "%.*s: %.*s", (int)target_length, target,
                        (int)message_length, message);
}

/* The display callbacks. All of them run on reims-vgpu's display thread, one at a time. */
static void on_mode_change(void *ctx, uint16_t width, uint16_t height, uint32_t pixel_format) {
    HvReimsPvg *p = ctx;
    p->have_mode = 1;
    p->mode_width = width;
    p->mode_height = height;
    p->mode_format = pixel_format;
    /* Once per mode change: the moment there is something to present at all. */
    fprintf(stderr, "reims: guest mode %ux%u, IOSurface format 0x%08x\n", width, height,
            pixel_format);
}

static void on_new_frame(void *ctx) {
    HvReimsPvg *p = ctx;
    p->frame_pending = 1;
    p->new_frames++;
}

/* The guest's cursor. Henge draws the guest's cursor itself, and an Android window has no cursor
 * to place, so these only need to exist: giving `cursor_glyph` at all is what lets the guest read
 * a glyph size limit of 256 instead of 0, which is a limit it can work with. The pixels are
 * borrowed for the call and pointedly not copied here. */
static void on_cursor_glyph(void *ctx, const RvgCursorGlyph *glyph) {
    (void)ctx;
    if (glyph) {
        // Touch nothing: the glyph is borrowed, and this port has no cursor surface yet.
    }
}

static void on_cursor_show(void *ctx, uint8_t show) { (void)ctx, (void)show; }

/* The position is read from the display when it is drawn (`rvg_display_cursor_position`), so the
 * callback has nothing to carry. */
static void on_cursor_move(void *ctx) { (void)ctx; }

/*
 * A raised guest interrupt. The device's signal lock is held here: pend and return.
 *
 * `vector` is the library's own numbering, not the guest's INTID: both devices raise 0
 * ("raiseInterrupt(0)"), one line each, and the guest's line is the one the device tree gives the
 * device. Pending the vector itself therefore pended INTID 0, which the GIC drops silently
 * (`hv_gicv3_pend_body` ignores anything below 32): the guest's GPU driver waited for an
 * interrupt that could never arrive, while the engine's report said "no SPI was ever pended".
 */
static void on_raise_interrupt(void *ctx, uint32_t vector) {
    HvReimsPvg *p = ctx;
    (void)vector;
    if (p->raises_logged < 1) {
        p->raises_logged++;
        fprintf(stderr, "reims: raise (vector %u) -> SPI %d\n", vector, p->gfx_irq);
    }
    hv_gicv3_pend(p->gic, p->gfx_irq);
}

/* The IOSurface device raises one: the guest reads its mapper's page for the rest. */
static void on_iosurface_interrupt(void *ctx, uint32_t vector) {
    HvReimsPvg *p = ctx;
    (void)vector;
    if (p->iosfc_raises_logged < 1) {
        p->iosfc_raises_logged++;
        fprintf(stderr, "reims: iosfc raise (vector %u) -> SPI %d\n", vector, p->iosfc_irq);
    }
    hv_gicv3_pend(p->gic, p->iosfc_irq);
}

/* The two MMIO windows. The engine hands a byte offset into the window and a width; reims-vgpu's
 * register file is 32-bit, and its windows are the same offsets the guest uses. */
/*
 * The guest reads these back to learn what the device made of its writes (a status word, a ring's
 * slot count). That half of the conversation had no logging at all, so a guest polling a register
 * for an answer that never came looked exactly like a guest that had stopped asking.
 */
static uint64_t gfx_read(void *opaque, uint64_t off, unsigned size) {
    HvReimsPvg *p = opaque;
    (void)size;
    uint64_t v = rvg_device_mmio_read(p->device, off);
    p->gfx_reads++;
    if (p->log) {
        static unsigned long n;
        if (n < 300) {
            n++;
            fprintf(stderr, "reims: gfx read  +%#llx = %#llx\n", (unsigned long long) off,
                    (unsigned long long) v);
        } else if (n == 300) {
            n++;
            fprintf(stderr, "reims: gfx read  (300 shown, further reads not logged)\n");
        }
    }
    return v;
}

static void gfx_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvReimsPvg *p = opaque;
    (void)size;
    p->gfx_writes++;
    /* Bring-up: which register the guest programs on the GPU device. */
    if (p->log >= 1) fprintf(stderr, "reims: gfx write +%#llx = %#llx\n",
                             (unsigned long long) off, (unsigned long long) val);
    /* The EFI display's own registers (start page, length, depth, mode, stride, and the
     * stride-alignment read at 0x1224): the firmware's progress bar reaches the host as presents
     * this display makes, so whether the guest programs it decides whether a black GPU frame is
     * "the guest has not drawn yet" or "the firmware's bar never got a display". Bounded: the
     * guest programs these a handful of times. */
    if (off >= 0x1200 && off <= 0x1300) {
        static unsigned long efi_writes;
        if (efi_writes < 120) {
            efi_writes++;
            fprintf(stderr, "reims: efi reg +%#llx = %#llx\n", (unsigned long long) off,
                    (unsigned long long) val);
        } else if (efi_writes == 120) {
            efi_writes++;
            fprintf(stderr, "reims: efi reg (120 writes shown, further ones not logged)\n");
        }
    }
    rvg_device_mmio_write(p->device, off, (uint32_t)val);
}

static uint64_t iosfc_read(void *opaque, uint64_t off, unsigned size) {
    HvReimsPvg *p = opaque;
    (void)size;
    if (!p->iosurface) return 0;
    return rvg_iosurface_device_mmio_read(p->iosurface, off);
}

static void iosfc_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvReimsPvg *p = opaque;
    (void)size;
    p->iosfc_writes++;
    if (!p->iosurface) return;
    /* Bring-up: the guest's writes to the mapper's registers, so a refusal from inside reims can
       be tied to the value that caused it. The register meanings are reims's (REG_RING_SIZE at
       0x1008 is a count of 16-byte commands, and ring_size*16 must be a whole 16 KiB page). */
    if (p->log >= 1 && off <= 0x1020)
        fprintf(stderr, "reims: iosfc write +%#llx = %#llx\n", (unsigned long long) off,
                (unsigned long long) val);
    rvg_iosurface_device_mmio_write(p->iosurface, off, val);
}

/* One error line, which is all a bring-up needs: `rvg_status_message` names the refusal. */
static void fail(const char *what, RvgStatus status) {
    fprintf(stderr, "reims: %s failed: %s\n", what, rvg_status_message(status));
}

HvReimsPvg *hv_reims_pvg_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa,
                                int gfx_irq, int iosfc_irq, uint64_t ram_pa, uint8_t *ram,
                                uint64_t ram_size, int ram_fd) {
    HvReimsPvg *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->m = m;
    p->gic = gic;
    p->gfx_irq = gfx_irq;
    p->iosfc_irq = iosfc_irq;
    p->log = getenv("HV_REIMS_LOG") != NULL;
    /* Every line at DEBUG, or TRACE with HV_REIMS_LOG. */
    rvg_log_set_handler(on_log, NULL, p->log ? RVG_LOG_TRACE : RVG_LOG_DEBUG);

    /* The guest's RAM, and the file behind it. `fd < 0` is legal and means "no views", which
     * leaves the GPU unable to reach guest memory: worth saying out loud rather than failing
     * cryptically later. */
    if (ram && ram_size) {
        p->range.physical_address = ram_pa;
        p->range.physical_length = ram_size;
        p->range.host_address = ram;
        p->map.ranges = &p->range;
        p->map.range_count = 1;
        p->file.fd = ram_fd;
        p->file.flags = 0;
        p->file.offset = 0;
        if (ram_fd < 0) {
            fprintf(stderr,
                    "reims: guest RAM has no file, so the GPU gets no views of it "
                    "(see devices/vmapple.c: RAM must be a memfd on Android)\n");
        }
    }

    p->host.struct_size = sizeof p->host;
    p->host.ctx = p;
    p->host.raise_interrupt = on_raise_interrupt;

    RvgDeviceConfig config;
    memset(&config, 0, sizeof config);
    config.struct_size = sizeof config;
    config.display_port_count = 1;
    /* The guest's textures and buffers name the IOSurface mapping ids it made through the mapper
     * MMIO device below (`rvg_iosurface_device_create`). Without this flag the device looks them
     * up through no mapper service at all, and every such resource raised "no IOSurface for
     * mapping N" (measured: surfaces 1..5 mapped, then eight exec faults). "The process's latest
     * RvgIOSurfaceDevice" is resolved at each lookup, so the device made after this one is the
     * one it finds. */
    config.use_iosurface_mapper = true;
    /* The guest asks for a writable cursor while it decodes an indirect command buffer; a local
     * task answers that by ending the process ("tasks.unsupported.writable_cursor", measured).
     * Isolated tasks are served by the task service on threads of this process, which implements
     * it, so user tasks (1 and up; task 0 stays local) run isolated. */
    config.process_isolation = true;
    config.memory_map = (ram && ram_size) ? &p->map : NULL;
    config.memory_files = (ram && ram_size) ? &p->file : NULL;
    config.callbacks = &p->host;

    RvgStatus st = rvg_device_create(&config, &p->device);
    if (st != RVG_OK) {
        fail("rvg_device_create", st);
        free(p);
        return NULL;
    }

    /* The IOSurface mapper, which the guest's GPU driver finds at its own window. */
    p->iosurface_callbacks.struct_size = sizeof p->iosurface_callbacks;
    p->iosurface_callbacks.ctx = p;
    p->iosurface_callbacks.raise_interrupt = on_iosurface_interrupt;

    RvgIOSurfaceDeviceConfig iosfc_config;
    memset(&iosfc_config, 0, sizeof iosfc_config);
    iosfc_config.struct_size = sizeof iosfc_config;
    iosfc_config.memory_map = &p->map;
    iosfc_config.callbacks = &p->iosurface_callbacks;
    st = rvg_iosurface_device_create(&iosfc_config, &p->iosurface);
    if (st != RVG_OK) {
        /* Not fatal: a guest that never touches the mapper does not need it, and losing the
         * display with it would be worse than losing the mapper. */
        fail("rvg_iosurface_device_create", st);
        p->iosurface = NULL;
    }

    /* The display, with a window: frames have nowhere to go without one, and the host's
     * `ANativeWindow` arrives once its activity has a surface
     * (`hv_reims_pvg_attach_window`). */
    RvgDisplayCallbacks display_callbacks;
    memset(&display_callbacks, 0, sizeof display_callbacks);
    display_callbacks.struct_size = sizeof display_callbacks;
    display_callbacks.ctx = p;
    display_callbacks.mode_change = on_mode_change;
    display_callbacks.new_frame = on_new_frame;
    display_callbacks.cursor_glyph = on_cursor_glyph;
    display_callbacks.cursor_show = on_cursor_show;
    display_callbacks.cursor_move = on_cursor_move;

    /* Whether a window exists is decided at creation, not at attach: the library's display keeps
     * a window link only where the config names one, and `rvg_display_attach_android_window`
     * answers RVG_ERR_UNSUPPORTED (measured in the app: "window refused, display stays offscreen")
     * for a display made without one. On Android the host's ANativeWindow arrives later and is
     * installed into that link, so the app asks for the link up front (`HV_REIMS_WINDOW=1`, set
     * by android/henge_vm.c) while the command-line tool, which never has a window, does not.
     *
     * The config carries no flags and no callbacks: "NULL is a table with every callback absent",
     * and without RVG_WINDOW_MATCH_HOST_SCREEN the display offers 1920x1080 at 1x until the guest
     * sets its own mode. */
    int want_window = 0;
    {
        const char *w = getenv("HV_REIMS_WINDOW");
        want_window = w && *w && *w != '0';
    }
    RvgWindowConfig window_config;
    memset(&window_config, 0, sizeof window_config);
    window_config.struct_size = sizeof window_config;
    RvgDisplayConfig display_config;
    memset(&display_config, 0, sizeof display_config);
    display_config.struct_size = sizeof display_config;
    display_config.port = 0;
    display_config.serial_number = 1;
    display_config.callbacks = &display_callbacks;
    display_config.window = want_window ? &window_config : NULL;

    st = rvg_display_create(p->device, &display_config, &p->display);
    if (st != RVG_OK) {
        fail("rvg_display_create", st);
        p->display = NULL;
    }
    /* The display starts off-screen, because a window arrives late (`hv_reims_pvg_attach_window`)
     * and a command-line process never has one. Said once, so a run's log states the display
     * decision instead of leaving it to be inferred from whether a picture appeared. */
    /* Once, unconditionally: whether the host has a window (the app) or not (the command line) is
     * the first thing a black screen has to be told apart by, and the app's log is the only one
     * available there. */
    {
        static int said;
        if (!said) {
            said = 1;
            if (!p->display) {
                fprintf(stderr, "reims: display kind = NONE (rvg_display_create refused it)\n");
            } else if (want_window) {
                fprintf(stderr, "reims: display kind = window (created with a window link; the host "
                                "installs its ANativeWindow when the surface exists)\n");
            } else {
                fprintf(stderr, "reims: display kind = offscreen (no window: frames can be encoded "
                                "and read back)\n");
            }
        }
    }
    if (p->log) {
        if (!p->display) {
            fprintf(stderr, "reims: display kind = NONE (rvg_display_create refused it)\n");
        } else if (want_window) {
            fprintf(stderr, "reims: display kind = window (created with a window link; the host "
                            "installs its ANativeWindow when the surface exists)\n");
        } else {
            fprintf(stderr, "reims: display kind = offscreen (no window: frames can be encoded "
                            "and read back)\n");
        }
    }

    /* The modes the guest is offered. rvg_display_set_modes is what connects a display to the
     * guest at all: "The guest's display page for the port takes the first 128: the first list it
     * takes connects the display to the guest" -- until then the guest has nothing to set and the
     * display never reports a mode. The app passes the screen's own modes; a command-line run has
     * no screen, so it offers the sizes a Mac reports by default. */
    if (p->display) {
        RvgDisplayMode modes[3];
        memset(modes, 0, sizeof modes);
        modes[0].width = 1920, modes[0].height = 1080, modes[0].refresh_rate_hz = 60.0;
        modes[1].width = 2560, modes[1].height = 1440, modes[1].refresh_rate_hz = 60.0;
        modes[2].width = 1280, modes[2].height = 800,  modes[2].refresh_rate_hz = 60.0;
        rvg_display_set_modes(p->display, modes, 3);
        if (p->log) fprintf(stderr, "reims: offered 3 display modes (1920x1080, 2560x1440, 1280x800)\n");
    }

    hv_machine_add_mmio(m, gfx_pa, REIMS_PVG_MMIO_SIZE, p, gfx_read, gfx_write, "reims-gfx");
    hv_machine_add_mmio(m, iosfc_pa, REIMS_PVG_MMIO_SIZE, p, iosfc_read, iosfc_write,
                        "reims-iosurface");

    if (p->log) {
        fprintf(stderr, "reims: device up, display %s, mapper %s\n", p->display ? "yes" : "no",
                p->iosurface ? "yes" : "no");
    }
    return p;
}

void hv_reims_pvg_attach_window(HvReimsPvg *p, void *native_window) {
    if (!p || !p->display || !native_window) {
        if (p && p->display && !native_window)
            fprintf(stderr, "reims: display stays offscreen (no window given)\n");
        return;
    }
    RvgStatus st = rvg_display_attach_android_window(p->display, native_window);
    if (st != RVG_OK) {
        fail("rvg_display_attach_android_window", st);
        /* The refusal is not fatal to the machine: the display keeps working off-screen, and a
         * grab can still say whether frames exist. */
        fprintf(stderr, "reims: window refused, display stays offscreen\n");
        return;
    }
    p->window_attached = 1;
    /* Always, and with the window's size: this line is the answer to "did the GPU ever get a
       window", and without it a black screen and a working-but-empty screen look identical in the
       log. The guest's mode is reported separately when it sets one. */
    fprintf(stderr, "reims: display attached to the host's window\n");
}

/** What the display is doing: {has a mode, guest presents, host presents}. For the app's status. */
void hv_reims_pvg_stats(HvReimsPvg *p, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!p || !p->display) return;
    uint64_t guest = 0, host = 0;
    rvg_display_present_counts(p->display, &guest, &host);
    out[0] = p->have_mode ? 1 : 0;
    out[1] = guest > 0x7fffffff ? 0x7fffffff : (int)guest;
    out[2] = host > 0x7fffffff ? 0x7fffffff : (int)host;
}

int hv_reims_pvg_have_display(HvReimsPvg *p) { return p && p->display != NULL; }

HvReimsDisplayKind hv_reims_pvg_display_kind(HvReimsPvg *p) {
    if (!p || !p->display) return HV_REIMS_DISPLAY_NONE;
    return p->window_attached ? HV_REIMS_DISPLAY_WINDOW : HV_REIMS_DISPLAY_OFFSCREEN;
}

const char *hv_reims_pvg_display_name(HvReimsPvg *p) {
    switch (hv_reims_pvg_display_kind(p)) {
    case HV_REIMS_DISPLAY_WINDOW: return "window";
    case HV_REIMS_DISPLAY_OFFSCREEN: return "offscreen";
    default: return "none";
    }
}

int hv_reims_pvg_have_mapper(HvReimsPvg *p) { return p && p->iosurface != NULL; }

/** True once the guest has a mode and has presented: the point where the GPU owns the screen. */
int hv_reims_pvg_ready(HvReimsPvg *p) {
    if (!p || !p->display) return 0;
    uint64_t guest = 0, host = 0;
    rvg_display_present_counts(p->display, &guest, &host);
    return p->have_mode && guest > 0;
}

void hv_reims_pvg_poll(HvReimsPvg *p) {
    if (!p || !p->display) return;
    /* Every 20 s, what the display is doing, in one line: a black window has three very different
     * causes and only these numbers tell them apart -- no mode (the guest never set one), a mode
     * and the guest never presents, or the guest presenting with the host never showing it. */
    {
        /* Once a second, what the guest asked of the GPU device since the last line: the guest
         * talks to the device by doorbells (writes at +0x1020) and reads back its answers, so a
         * boot that has handed the screen to the GPU shows both moving, and one that has not
         * shows them still. Cheap enough to leave on: the counters are increments. */
        static time_t last_bus;
        static unsigned long long gfx0, iosfc0, reads0, frames0;
        time_t now = time(NULL);
        if (now != last_bus) {
            last_bus = now;
            fprintf(stderr, "reims: bus | gfx writes=%llu (+%llu/s) reads=%llu (+%llu/s) iosfc "
                            "writes=%llu (+%llu/s) new frames=%llu (+%llu/s)\n",
                    p->gfx_writes, p->gfx_writes - gfx0, p->gfx_reads, p->gfx_reads - reads0,
                    p->iosfc_writes, p->iosfc_writes - iosfc0, p->new_frames,
                    p->new_frames - frames0);
            gfx0 = p->gfx_writes;
            reads0 = p->gfx_reads;
            iosfc0 = p->iosfc_writes;
            frames0 = p->new_frames;
        }
    }
    {
        static time_t last;
        time_t now = time(NULL);
        if (now - last >= 15) {
            last = now;
            uint64_t guest = 0, host = 0;
            rvg_display_present_counts(p->display, &guest, &host);
            /* Unconditional on purpose: this line is the answer to "why is the window black", and
             * a run whose log inside the app is the only one available does not set HV_REIMS_LOG. */
            fprintf(stderr,
                    "reims: status | window=%d mode=%s%ux%u | new frames=%llu | guest presents=%llu "
                    "| host presents=%llu\n",
                    p->window_attached, p->have_mode ? "" : "(none) ",
                    (unsigned)(p->have_mode ? p->mode_width : 0),
                    (unsigned)(p->have_mode ? p->mode_height : 0), p->new_frames,
                    (unsigned long long)guest, (unsigned long long)host);
        }
    }
    /* Nothing to do here on purpose: a window's frames are presented by reims-vgpu's own render
     * thread through the swapchain over the attached VkSurfaceKHR (see reims-vgpu-vulkan's
     * swapchain.rs), not by the host. `frame_pending` only feeds the off-screen grab used by the
     * command-line tool and by `hv_reims_pvg_grab`. An earlier comment here claimed this branch
     * encoded and committed a frame; it never did, and reading it as "the host presents" led to a
     * wrong conclusion that the guest was not drawing at all. */
    if (p->frame_pending && p->have_mode) {
        p->frame_pending = 0;
        /* Off-screen, a frame is not shown anywhere until someone encodes it; one line here is
         * what distinguishes "the guest draws and nobody looks" from "the guest never sets a
         * mode", which is the question the stall raised. */
        if (!p->window_attached) {
            if (p->offscreen_frames == 0 || p->offscreen_frames % 60 == 0)
                fprintf(stderr, "reims: offscreen frame %llu (%ux%u)%s\n",
                        (unsigned long long)p->offscreen_frames + 1,
                        p->mode_width, p->mode_height,
                        p->offscreen_frames == 0 ? " — grab it with HV_GRAB_FRAME/HV_GRAB_EVERY" : "");
            p->offscreen_frames++;
        }
        if (p->log) fprintf(stderr, "reims: frame %ux%u\n", p->mode_width, p->mode_height);
    }
}

/*
 * Encodes the guest's current frame into a CPU-readable target and writes it to `path` as a BMP.
 *
 * This is the off-screen half of the display, and the Android-side counterpart of what the app
 * does through a Surface: rvg_frame_target_create makes a texture "the library can draw into and
 * the CPU can read", rvg_display_encode_frame copies the display's current frame into it, and
 * rvg_frame_target_read_pixels brings it back. No window is involved, which is what makes a
 * command-line bring-up able to see what the guest drew.
 *
 * Returns 0 when a file was written. Refuses before the guest has set a mode: until then there
 * is no frame to encode.
 */
int hv_reims_pvg_grab(HvReimsPvg *p, const char *path) {
    if (!p || !p->display || !p->device || !p->have_mode) return -1;

    RvgFrameTargetConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.width = p->mode_width;
    cfg.height = p->mode_height;
    cfg.format = RVG_PIXEL_FORMAT_BGRA8_UNORM;   /* the format the display presents in */

    RvgFrameTarget *target = NULL;
    if (rvg_frame_target_create(p->device, &cfg, &target) != RVG_OK) return -1;

    RvgFrame *frame = NULL;
    int ok = 0;
    if (rvg_display_encode_frame(p->display, target, NULL, &frame) == RVG_OK) {
        if (rvg_frame_commit(frame) == RVG_OK && rvg_frame_wait(frame) == RVG_OK) ok = 1;
        rvg_frame_release(frame);
    }
    if (!ok) {
        rvg_frame_target_release(target);
        return -1;
    }

    size_t row = (size_t)p->mode_width * 4, size = row * p->mode_height;
    uint8_t *px = malloc(size);
    if (!px) {
        rvg_frame_target_release(target);
        return -1;
    }
    ok = rvg_frame_target_read_pixels(target, px, row, size) == RVG_OK;
    rvg_frame_target_release(target);
    if (!ok) {
        free(px);
        return -1;
    }

    /* BMP, 32-bit BGRA, rows bottom-up: the buffer is already BGRA, so only the row order has to
       be flipped. */
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(px);
        return -1;
    }
    uint8_t h[54];
    memset(h, 0, sizeof h);
    h[0] = 'B', h[1] = 'M';
    uint32_t file_size = (uint32_t)(54 + size), data_off = 54, hdr = 40;
    int32_t w = p->mode_width, ht = p->mode_height;
    uint16_t planes = 1, bpp = 32;
    memcpy(h + 2, &file_size, 4);
    memcpy(h + 10, &data_off, 4);
    memcpy(h + 14, &hdr, 4);
    memcpy(h + 18, &w, 4);
    memcpy(h + 22, &ht, 4);
    memcpy(h + 26, &planes, 2);
    memcpy(h + 28, &bpp, 2);
    fwrite(h, 1, sizeof h, f);
    for (int y = p->mode_height - 1; y >= 0; y--) fwrite(px + (size_t)y * row, 1, row, f);
    fclose(f);
    free(px);
    if (p->log) fprintf(stderr, "reims: wrote %s (%ux%u)\n", path, p->mode_width, p->mode_height);
    return 0;
}

void hv_reims_pvg_destroy(HvReimsPvg *p) {
    if (!p) return;
    if (p->display) rvg_display_release(p->display);
    if (p->iosurface) rvg_iosurface_device_release(p->iosurface);
    if (p->device) rvg_device_release(p->device);
    free(p);
}
