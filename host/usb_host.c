#include "usb_host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int wait_ep(HvVirtioUsb *d, uint8_t ep, void *buf, uint32_t max, int timeout_ms) {
    HvVusbMsg m;
    int r = hv_vusb_recv(d, ep, &m, timeout_ms);
    if (r == 0) return HV_USB_ETIMEOUT;
    if (r < 0) return HV_USB_EGONE;
    int rc;
    switch (m.type) {
    case HV_VUSB_RSP_DATA: {
        uint32_t n = m.len < max ? m.len : max;
        if (n && buf) memcpy(buf, m.data, n);
        rc = (int)n;
        break;
    }
    case HV_VUSB_RSP_STALL: rc = HV_USB_ESTALL; break;
    case HV_VUSB_RSP_RESET: rc = HV_USB_EIO; break; /* a reset answer where data was expected */
    default: rc = HV_USB_EIO; break;
    }
    free(m.data);
    return rc;
}

int64_t hv_usb_wait_online(HvVirtioUsb *d, int timeout_ms) {
    for (int t = 0; t <= timeout_ms; t += 10) {
        uint32_t s;
        if (hv_vusb_online(d, &s)) return s;
        usleep(10000);
    }
    return -1;
}

int hv_usb_reset(HvVirtioUsb *d, int timeout_ms) {
    if (hv_vusb_send(d, HV_VUSB_REQ_RESET, 0, NULL, 0) != 0) return HV_USB_EGONE;
    HvVusbMsg m;
    int r = hv_vusb_recv(d, 0xff, &m, timeout_ms);
    if (r == 0) return HV_USB_ETIMEOUT;
    if (r < 0) return HV_USB_EGONE;
    int ok = m.type == HV_VUSB_RSP_RESET;
    free(m.data);
    return ok ? 0 : HV_USB_EIO;
}

/*
 * How many bytes one request to the guest may carry.
 *
 * The guest posts receive buffers of a fixed size (usbmuxd's 16 KiB), and a request larger than the
 * buffer is silently cut to what fits — the caller still sees a completed transfer. Bulk OUT is
 * split to this size for exactly that reason; control OUT was not, and a control transfer is what
 * a DFU upload uses, so an oversized one would corrupt the image with nothing in the log to show
 * for it.
 */
static int control_chunked(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                           uint16_t wIndex, void *data, uint16_t wLength, uint32_t room,
                           int timeout_ms);

static uint32_t guest_receive_bytes(void) {
    static uint32_t bytes;
    if (!bytes) bytes = getenv("HV_USB_OUT_CHUNK") ? (uint32_t)atoi(getenv("HV_USB_OUT_CHUNK")) : 16384u;
    return bytes;
}

int hv_usb_control(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t wLength, int timeout_ms) {
    int in = (bmRequestType & 0x80) != 0;
    uint32_t room = guest_receive_bytes();
    uint32_t len = 8 + (in ? 0 : wLength);
    if (!in && len > room) {
        if (!getenv("HV_USB_CTRL_CHUNK")) {
            /* Said out loud, and delivered the old way: splitting changes what the guest sees in
               the middle of a restore, which is not something to change on a hunch. */
            fprintf(stderr, "hengevm: usb: control OUT of %u bytes is larger than the guest's %u-byte "
                            "receive buffer; it will be cut (set HV_USB_CTRL_CHUNK=1 to split it)\n",
                    (unsigned) len, (unsigned) room);
        } else {
            return control_chunked(d, bmRequestType, bRequest, wValue, wIndex, data, wLength, room,
                                   timeout_ms);
        }
    }
    uint8_t *req = malloc(len);
    if (!req) return HV_USB_EIO;
    req[0] = bmRequestType, req[1] = bRequest;
    memcpy(req + 2, &wValue, 2);
    memcpy(req + 4, &wIndex, 2);
    memcpy(req + 6, &wLength, 2);
    if (!in && wLength) memcpy(req + 8, data, wLength);
    int r = hv_vusb_send(d, HV_VUSB_REQ_DATA, 0, req, len);
    free(req);
    if (r != 0) return HV_USB_EGONE;
    r = wait_ep(d, 0, in ? data : NULL, in ? wLength : 0, timeout_ms);
    if (r < 0) return r;
    return in ? r : wLength;
}

/*
 * A control OUT whose payload is larger than one request: the setup and the first slice go in the
 * first request, the rest follow as further data requests on the same endpoint — the shape a real
 * bus produces, and the shape the bulk path already uses.
 */
static int control_chunked(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                           uint16_t wIndex, void *data, uint16_t wLength, uint32_t room,
                           int timeout_ms) {
    const uint8_t *in = data;
    uint32_t first = room > 8 ? room - 8 : 0;
    uint32_t off = 0;
    for (;;) {
        uint32_t take = wLength - off < first ? wLength - off : first;
        uint32_t len = 8 + take;
        uint8_t *req = malloc(len);
        if (!req) return HV_USB_EIO;
        req[0] = bmRequestType, req[1] = bRequest;
        memcpy(req + 2, &wValue, 2);
        memcpy(req + 4, &wIndex, 2);
        memcpy(req + 6, &wLength, 2);
        if (take) memcpy(req + 8, in + off, take);
        int r = hv_vusb_send(d, HV_VUSB_REQ_DATA, 0, req, len);
        free(req);
        if (r != 0) return HV_USB_EGONE;
        off += take;
        if (off >= wLength) break;
        first = room;   /* every later request is all data */
    }
    return wLength;
}

int hv_usb_bulk(HvVirtioUsb *d, uint8_t ep, void *data, uint32_t len, int timeout_ms) {
    int in = (ep & 0x80) != 0;
    int r = in ? hv_vusb_send_in(d, ep, len) : hv_vusb_send(d, HV_VUSB_REQ_DATA, ep, data, len);
    if (r != 0) return HV_USB_EGONE;
    r = wait_ep(d, ep, in ? data : NULL, in ? len : 0, timeout_ms);
    if (r < 0) return r;
    return in ? r : (int)len;
}

static void print_string(HvVirtioUsb *d, uint8_t idx) {
    if (!idx) return;
    uint8_t buf[256];
    int n = hv_usb_control(d, 0x80, 6, (uint16_t)(0x300 | idx), 0x409, buf, 255, 2000);
    if (n < 2) {
        fprintf(stderr, "usb probe: string %u -> %d\n", idx, n);
        return;
    }
    char s[128];
    int k = 0;
    for (int i = 2; i + 1 < n && k < 127; i += 2) s[k++] = buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '?';
    s[k] = 0;
    fprintf(stderr, "usb probe: string %u = \"%s\"\n", idx, s);
}

static void *probe(void *arg) {
    HvVirtioUsb *d = arg;
    int64_t s = hv_usb_wait_online(d, 120000);
    fprintf(stderr, "usb probe: online (session %lld)\n", (long long)s);
    if (s < 0) return NULL;
    usleep(200000);
    fprintf(stderr, "usb probe: reset -> %d\n", hv_usb_reset(d, 2000));
    uint8_t dd[18] = {0};
    int n = hv_usb_control(d, 0x80, 6, 0x100, 0, dd, 18, 2000);
    fprintf(stderr, "usb probe: device descriptor -> %d:", n);
    for (int i = 0; i < (n > 0 ? n : 0); i++) fprintf(stderr, " %02x", dd[i]);
    fprintf(stderr, "\n");
    if (n == 18) {
        print_string(d, dd[14]);
        print_string(d, dd[15]);
        print_string(d, dd[16]);
    }
    return NULL;
}

void hv_usb_probe_start(HvVirtioUsb *d) {
    pthread_t t;
    pthread_create(&t, NULL, probe, d);
    pthread_detach(t);
}
