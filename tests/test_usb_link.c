/*
 * Exercises host/libusb_shim.c against a scripted guest: no virtio, no JIT, no device, no
 * Android. What it pins down is the session dance the restore flow depends on, which used to
 * cost three minutes per iteration on the phone:
 *
 *   1. a device that is on the bus enumerates and reports ARRIVED;
 *   2. the restore kernel's device end (AppleVirtIOUSBDeviceController) answers the descriptor
 *      requests but NOT our vendor reset -- enumeration must still succeed, because requiring
 *      that reply is what made "restore mode" never appear;
 *   3. a session bump (hv_vusb_force_rescan) reports LEFT and then ARRIVED again;
 *   4. with the device off the bus the device list is empty.
 *
 * It also measures how long libusb_handle_events() spends inside enumeration, because that time
 * is taken while the completion queue is locked, so a slow/blocking enum delays every transfer
 * completion the restore tools are waiting for.
 *
 *   cc -o build/test-usb-link tests/test_usb_link.c host/libusb_shim.c \
 *      -Ithird_party/libusb-1.0 -Idevices -Ihost -lpthread
 */
#include <libusb-1.0/libusb.h>

#include "devices/virtio_usb.h"
#include "host/usb_host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* ---- the scripted guest -------------------------------------------------------------- */

static int g_online = 1;
static uint32_t g_session = 1;
static int g_reset_replies = 1;      /* iBEC answers; the restore kernel's end does not */
static int g_reset_ms;               /* how long the unanswered reset takes to give up */
static int g_reset_calls, g_ctrl_calls, g_send_calls;

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,          /* USB 2.0, EP0 64 */
    0xac, 0x05, 0x81, 0x12,                  /* Apple 0x1281 (the PID the guest reports) */
    0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[18] = {
    9, 2, 18, 0, 1, 1, 0, 0xa0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
};

int hv_vusb_online(HvVirtioUsb *d, uint32_t *session) {
    (void) d;
    if (session) *session = g_session;
    return g_online;
}

int hv_vusb_send(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len) {
    (void) d, (void) type, (void) ep, (void) data, (void) len;
    g_send_calls++;
    return 0;
}

int hv_vusb_send_in(HvVirtioUsb *d, uint8_t ep, uint32_t len) {
    (void) d, (void) ep, (void) len;
    g_send_calls++;
    return 0;
}

int hv_vusb_recv(HvVirtioUsb *d, uint8_t ep, HvVusbMsg *m, int timeout_ms) {
    (void) d, (void) ep, (void) m, (void) timeout_ms;
    return 0; /* nothing ever comes back on this path */
}

int hv_usb_reset(HvVirtioUsb *d, int timeout_ms) {
    (void) d, (void) timeout_ms;
    g_reset_calls++;
    if (g_reset_replies) return 0;
    if (g_reset_ms) usleep((useconds_t) g_reset_ms * 1000);
    return HV_USB_ETIMEOUT;
}

int hv_usb_control(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                   uint16_t wIndex, void *data, uint16_t wLength, int timeout_ms) {
    (void) d, (void) wIndex, (void) timeout_ms;
    g_ctrl_calls++;
    if (bmRequestType != 0x80 || bRequest != 6) return 0;
    if ((wValue >> 8) == 1) {                       /* device descriptor */
        uint16_t n = wLength < sizeof dev_desc ? wLength : (uint16_t) sizeof dev_desc;
        memcpy(data, dev_desc, n);
        return n;
    }
    if ((wValue >> 8) == 2) {                       /* configuration descriptor */
        uint16_t n = wLength < sizeof cfg_desc ? wLength : (uint16_t) sizeof cfg_desc;
        memcpy(data, cfg_desc, n);
        return n;
    }
    return 0;
}

/* ---- hotplug accounting -------------------------------------------------------------- */

static int g_arrivals, g_departures;

static int on_hotplug(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event ev, void *user) {
    (void) ctx, (void) dev, (void) user;
    if (ev == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) g_arrivals++;
    else if (ev == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) g_departures++;
    return 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static void pump(void) {
    struct timeval tv = {0, 5000};
    libusb_handle_events_timeout(NULL, &tv);
}

static int device_count(void) {
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(NULL, &list);
    if (n > 0) libusb_free_device_list(list, 1);
    return (int) (n > 0 ? n : 0);
}

static int differs, checks;

static void check(const char *what, int got, int want) {
    checks++;
    int bad = got != want;
    if (bad) differs++;
    printf("%-58s %s (got %d, want %d)\n", what, bad ? "DIFFERS" : "ok", got, want);
}

int main(void) {
    /* The shim keeps the pointer only; it never dereferences it. */
    hv_libusb_shim_attach((HvVirtioUsb *) (uintptr_t) 0x1000);

    if (libusb_init(NULL) != 0) {
        printf("libusb_init failed\n");
        return 1;
    }
    libusb_hotplug_callback_handle h;
    if (libusb_hotplug_register_callback(NULL,
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0,
            0, 0, 0, on_hotplug, NULL, &h) != 0) {
        printf("hotplug register failed\n");
        return 1;
    }

    /* 1. device present: one arrival, one device listed. */
    double t0 = now_ms();
    pump();
    double first_ms = now_ms() - t0;
    check("first enumeration reports one arrival", g_arrivals, 1);
    check("device list has the device", device_count(), 1);
    check("enumeration asked the guest for descriptors", g_ctrl_calls > 0, 1);

    /* 2. the restore kernel's end: no reply to the vendor reset. */
    g_reset_replies = 0;
    g_session++;                    /* hv_vusb_force_rescan() */
    t0 = now_ms();
    pump();
    double noreset_ms = now_ms() - t0;
    check("departure reported for the old session", g_departures, 1);
    check("device still enumerates without the reset reply", g_arrivals, 2);
    check("device list has it again", device_count(), 1);

    /* 3. another bump (the userland one) behaves the same way. */
    g_session++;
    pump();
    check("second bump reports another departure", g_departures, 2);
    check("second bump re-enumerates", g_arrivals, 3);

    /* 4. device off the bus: gone from the list. */
    g_online = 0;
    pump();
    check("going offline reports a departure", g_departures, 3);
    check("device list is empty while offline", device_count(), 0);

    /* 5. how long enumeration holds the shim's event path. A real unanswered reset costs its
          full timeout inside libusb_handle_events(), which delays every transfer completion. */
    g_online = 1;
    g_reset_ms = 200;               /* pretend the reset takes 200 ms to give up */
    setenv("HV_RESET_ENUM", "1", 1); /* the old behaviour, for the timing measurement */
    g_session++;
    t0 = now_ms();
    pump();
    double slow_ms = now_ms() - t0;
    printf("\ntiming: first enum %.2f ms, no-reset enum %.2f ms, 200 ms reset enum %.2f ms\n",
           first_ms, noreset_ms, slow_ms);
    check("a slow reset blocks the event path for ~its timeout", slow_ms >= 150.0, 1);

    printf("\n%d of %d differ\n", differs, checks);
    return differs ? 1 : 0;
}
