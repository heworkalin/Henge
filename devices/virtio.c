#include "virtio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmu.h"   /* hv_mmu_walk: reading the guest stack needs the guest's own page tables */

/* Layout of the one BAR (BAR 4, 16 KB), as QEMU's virtio-pci lays it out. */
#define BAR 4
#define COMMON 0x0000
#define ISR 0x1000
#define DEVICE 0x2000
#define NOTIFY 0x3000
#define NOTIFY_MULT 4

#define DESC_F_NEXT 1
#define DESC_F_WRITE 2
#define DESC_F_INDIRECT 4

static void add_cap(HvVirtio *v, uint8_t at, uint8_t next, uint8_t type, uint32_t off, uint32_t len) {
    uint8_t *c = v->pci.config + at;
    c[0] = 0x09;                    /* vendor-specific */
    c[1] = next;
    c[2] = type == 2 ? 20 : 16;
    c[3] = type;
    c[4] = BAR;
    memcpy(c + 8, &off, 4);
    memcpy(c + 12, &len, 4);
    if (type == 2) {
        uint32_t mult = NOTIFY_MULT;
        memcpy(c + 16, &mult, 4);
    }
}

static void reset(HvVirtio *v) {
    v->driver_features = 0;
    v->dfsel = v->gfsel = 0;
    v->status = 0;
    v->isr = 0;
    v->qsel = 0;
    for (int i = 0; i < v->nq; i++) v->q[i] = (HvVirtq){.size = HV_VIRTQ_MAX_SIZE};
    hv_pci_set_intx(&v->pci, 0);
}

/* HV_VIRTIO_CFG_LOG=1 traces what a driver asks the virtio transport, narrowly: only the USB
   device (PCI 1af4:1052) and only its common configuration. The restore flow's handover is
   exactly "did the kernel's AppleVirtIOUSBDeviceController get past prepareDefaults", so the
   order of these accesses (device features -> status -> queue setup -> DRIVER_OK) says which
   step it stopped at. */
static int vtrace_usb(HvVirtio *v) {
    static int on = -1;
    if (on < 0) on = getenv("HV_VIRTIO_CFG_LOG") != NULL;
    /* 106b:1a01 -- the USB device port. (1af4:1052 is virtio-input; tracing that one instead
       shows the keyboard's driver, which resets for its own reasons and says nothing about the
       restore handover.) */
    return on && v->pci.config[0] == 0x6b && v->pci.config[1] == 0x10 &&
           v->pci.config[2] == 0x01 && v->pci.config[3] == 0x1a;
}

/* Debug: the guest's real state at the moment it touches the USB port's status word.  The MMIO
   callback runs while the guest is stopped on that access, so the general purpose registers are
   still the guest's -- unlike cpu->pc, which is the start of the translated block and therefore
   points somewhere else entirely.  Walking the stack for kernel return addresses turns "status
   changed at some pc" into a call chain, which is what says who is driving the negotiation. */
static void vusb_state_dump(HvVirtio *v, const char *what) {
    HvCpu *c = v->m->cpu;
    if (!c) return;
    fprintf(stderr, "vusb@ %s: el %d sp %llx lr %llx pc(block) %llx\n", what, (int)c->el,
            (unsigned long long)c->sp, (unsigned long long)c->x[30], (unsigned long long)c->pc);
    for (int i = 0; i < 8; i++) fprintf(stderr, "vusb@   x%d %llx\n", i, (unsigned long long)c->x[i]);
    uint64_t base = c->sp & ~7ull;
    int shown = 0;
    for (uint64_t at = base; at < base + 0x1000 && shown < 24; at += 8) {
        HvWalk w;
        uint64_t val = 0;
        hv_mmu_walk(c, at, HV_ACC_READ, 1, &w);
        if (w.fsc || hv_phys_read(v->m, w.pa, &val, 8)) continue;
        /* Kernel code addresses only: 0xfffffe.. and halfword aligned. */
        if ((val >> 40) == 0xfffffe && (val & 3) == 0) {
            fprintf(stderr, "vusb@   stack+%#llx = %llx\n", (unsigned long long)(at - base),
                    (unsigned long long)val);
            shown++;
        }
    }
}

static uint64_t common_read(HvVirtio *v, uint64_t off) {
    HvVirtq *q = v->qsel < v->nq ? &v->q[v->qsel] : NULL;
    if (vtrace_usb(v)) fprintf(stderr, "vusb-cfg: read +%#llx\n", (unsigned long long) off);
    if (off == 0x14 && vtrace_usb(v)) vusb_state_dump(v, "read status");
    switch (off) {
    case 0x00: return v->dfsel;
    case 0x04: return v->dfsel < 2 ? (uint32_t)(v->features >> (32 * v->dfsel)) : 0;
    case 0x08: return v->gfsel;
    case 0x0c: return v->gfsel < 2 ? (uint32_t)(v->driver_features >> (32 * v->gfsel)) : 0;
    case 0x10: return 0xffff;                  /* config_msix_vector: no MSI-X */
    case 0x12: return v->nq;
    case 0x14: return v->status;
    case 0x15: return v->config_gen;
    case 0x16: return v->qsel;
    case 0x18: return q ? q->size : 0;
    case 0x1a: return 0xffff;
    case 0x1c: return q ? q->enabled : 0;
    case 0x1e: return v->qsel;                 /* queue_notify_off = queue index */
    case 0x20: return q ? (uint32_t)q->desc : 0;
    case 0x24: return q ? q->desc >> 32 : 0;
    case 0x28: return q ? (uint32_t)q->avail : 0;
    case 0x2c: return q ? q->avail >> 32 : 0;
    case 0x30: return q ? (uint32_t)q->used : 0;
    case 0x34: return q ? q->used >> 32 : 0;
    default: return 0;
    }
}

static void set64(uint64_t *f, uint64_t off, uint64_t v, unsigned size) {
    if (size == 8) *f = v;
    else if (off & 4) *f = (*f & 0xffffffffull) | v << 32;
    else *f = (*f & ~0xffffffffull) | (uint32_t)v;
}

static void common_write(HvVirtio *v, uint64_t off, uint64_t val, unsigned size) {
    HvVirtq *q = v->qsel < v->nq ? &v->q[v->qsel] : NULL;
    switch (off) {
    case 0x00: v->dfsel = (uint32_t)val; break;
    case 0x08: v->gfsel = (uint32_t)val; break;
    case 0x0c:
        if (v->gfsel < 2) {
            uint64_t mask = 0xffffffffull << (32 * v->gfsel);
            v->driver_features = (v->driver_features & ~mask) | (((uint64_t)(uint32_t)val << (32 * v->gfsel)) & v->features);
        }
        break;
    case 0x14:
        if (vtrace_usb(v)) {
            /* Who wrote it: the guest's own state at the access, so the writer can be looked up
               in the kernelcache.  The ACKNOWLEDGE write is where the USB device controller
               stops, and knowing the function that made it turns "somewhere in a kext" into a
               symbol. */
            vusb_state_dump(v, (uint8_t)val ? "write status (nonzero)" : "write status (reset)");
            fprintf(stderr, "vusb-cfg: status = %#llx\n", (unsigned long long) val);
        }
        /* The USB device controller raises its interrupt line on a state change, as the hardware
           does when a device is attached: AppleVirtIOUSBDeviceController writes ACKNOWLEDGE and
           then leaves its start routine waiting for that event before it reads device features.
           Without it the negotiation never proceeds past 0x1, the guest never enables a queue,
           and the host has nothing to talk to (experiment, HV_VUSB_INTX=0 disables).

           The line is left asserted: the guest clears it by reading the ISR window (see bar_read),
           which is what the real device does.  A one-shot pulse was tried first and the guest
           never saw it -- the assertion and the deassertion landed in the same JIT block. */
        if (vtrace_usb(v) && (val & 1) && !getenv("HV_VUSB_INTX"))
            hv_pci_set_intx(&v->pci, 1);
        if ((uint8_t)val == 0) reset(v);
        else v->status = (uint8_t)val;
        break;
    case 0x16: v->qsel = (uint16_t)val; break;
    case 0x18:
        if (vtrace_usb(v)) fprintf(stderr, "vusb-cfg: queue %u size = %llu\n", v->qsel, (unsigned long long) val);
        if (q && val && val <= HV_VIRTQ_MAX_SIZE && !(val & (val - 1))) q->size = (uint16_t)val;
        else if (q) /* the driver would index its rings by a size we do not use: say so */
            fprintf(stderr, "virtio %04x: queue %u size %llu refused (max %d)\n", v->pci.config[2] | v->pci.config[3] << 8,
                    v->qsel, (unsigned long long)val, HV_VIRTQ_MAX_SIZE);
        if (getenv("HV_VIRTIO_CFG_LOG"))
            fprintf(stderr, "virtio %04x: queue %u size %llu\n", v->pci.config[2] | v->pci.config[3] << 8, v->qsel, (unsigned long long)val);
        break;
    case 0x1c:
        if (vtrace_usb(v)) fprintf(stderr, "vusb-cfg: queue %u enable = %llu\n", v->qsel, (unsigned long long) val);
        if (q) q->enabled = val & 1;
        break;
    case 0x20: case 0x24: if (q) set64(&q->desc, off, val, size); break;
    case 0x28: case 0x2c: if (q) set64(&q->avail, off, val, size); break;
    case 0x30: case 0x34: if (q) set64(&q->used, off, val, size); break;
    default: break;
    }
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvVirtio *v = d->opaque;
    if (bar != BAR) return ~0ull;
    if (off < ISR) return common_read(v, off);
    if (off < DEVICE) { /* ISR: reading clears it and drops the line */
        uint8_t isr = v->isr;
        v->isr = 0;
        hv_pci_set_intx(&v->pci, 0);
        /* The only evidence that the guest actually took the interrupt we raised: the spec
           requires reading the ISR window to clear it, so a run with no "isr read" line means
           the line never reached this driver, not that the driver ignored it. */
        if (vtrace_usb(v)) fprintf(stderr, "vusb-cfg: isr read = %#x\n", isr);
        return isr;
    }
    if (off < NOTIFY) {
        uint64_t r = 0;
        uint32_t o = (uint32_t)(off - DEVICE);
        if (o < v->config_len) memcpy(&r, v->config + o, size > v->config_len - o ? v->config_len - o : size);
        static int cfg_log = -1;
        if (cfg_log < 0) cfg_log = getenv("HV_VIRTIO_CFG_LOG") != NULL;
        if (cfg_log) /* debugging: which device config fields the driver reads */
            fprintf(stderr, "virtio %04x:%04x config read +%u (%u) = %llx\n", v->pci.config[0] | v->pci.config[1] << 8,
                    v->pci.config[2] | v->pci.config[3] << 8, o, size, (unsigned long long)r);
        return r;
    }
    return 0;
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t val, unsigned size) {
    HvVirtio *v = d->opaque;
    if (bar != BAR) return;
    if (off < ISR) {
        common_write(v, off, val, size);
    } else if (off >= DEVICE && off < NOTIFY) {
        if (v->config_write) v->config_write(v, (uint32_t)(off - DEVICE), val, size);
    } else if (off >= NOTIFY) {
        int q = (int)((off - NOTIFY) / NOTIFY_MULT);
        if (q < v->nq && v->q[q].enabled && (v->status & 4) && v->notify) v->notify(v, q); /* DRIVER_OK */
    }
}

void hv_virtio_init(HvVirtio *v, HvMachine *m, int device_id, uint32_t class_code, int nq, uint64_t features) {
    memset(v, 0, sizeof *v);
    v->m = m;
    v->nq = (uint16_t)nq;
    v->features = features | HV_VIRTIO_F_VERSION_1;
    hv_pci_init(&v->pci, 0x1af4, (uint16_t)(0x1040 + device_id), class_code << 8 | 1, 0x1af4, (uint16_t)device_id);
    v->pci.opaque = v;
    v->pci.bar_read = bar_read;
    v->pci.bar_write = bar_write;
    hv_pci_add_bar(&v->pci, BAR, 0x4000, 1);
    v->pci.config[0x06] |= 0x10;                 /* capability list */
    v->pci.config[0x34] = 0x40;
    add_cap(v, 0x40, 0x50, 1, COMMON, 0x38);
    add_cap(v, 0x50, 0x60, 3, ISR, 1);
    add_cap(v, 0x60, 0x70, 4, DEVICE, 0x100);
    add_cap(v, 0x70, 0x00, 2, NOTIFY, NOTIFY_MULT * HV_VIRTQ_MAX);
    reset(v);
}

/* The USB device port presents itself as an Apple device (106b:1a01), and that transport
   requires MSI: IOPCIBridge::resolveMSIInterrupts asks
   IOPCIMessagedInterruptController::getDeviceMSILimit, which reads this capability's MMC
   field. With no capability the limit is 0, AppleVirtIOPCITransport::findNextMSIInterruptIndex
   fails, assignInterruptAction never puts an interrupt source into the transport's OSArray,
   and setInterruptsEnabled aborts device initialisation immediately after ACKNOWLEDGE --
   which is exactly where this device used to stop (status 0x1, then reset, forever).
   Standard virtio devices (1af4:xxxx) take the multi-vector branch and do not need it, so
   this is only called for 106b:1a01. HV_NO_MSI=1 turns it back off for comparison.

   This has to run *after* the caller has rewritten the vendor/device ids: hv_virtio_init()
   creates the device as 1af4:1040 and virtio_usb.c patches it to 106b:1a01 afterwards. */
void hv_virtio_add_msi(HvVirtio *v) {
    if (getenv("HV_NO_MSI")) return;
    uint8_t at = 0x80;
    uint8_t *c = v->pci.config + at;
    memset(c, 0, 16);
    c[0] = 0x05;                 /* MSI capability */
    c[1] = 0x00;                 /* end of the capability list */
    c[2] = 0x02;                 /* Message Control: MMC=1 -> 2 vectors; MSI off until enabled */
    v->pci.config[0x71] = at;    /* link it after the notify capability at 0x70 */
    for (int i = 0; i < 16; i++) v->pci.wmask[at + i] = 0xff; /* the driver writes address/data here */
    v->msi_cap = at;
}

/* Raise an interrupt the way the device is configured to: an MSI is a memory write carrying the
   guest-programmed data word to the guest-programmed address (which lands in the gic-msi frame),
   otherwise the legacy INTx line. */
static void virtio_raise(HvVirtio *v) {
    if (v->msi_cap) {
        uint8_t *c = v->pci.config + v->msi_cap;
        if (c[2] & 1) {                        /* MSI enable */
            uint32_t addr;
            uint16_t data;
            memcpy(&addr, c + 4, 4);
            memcpy(&data, c + 8, 2);
            HvMmio *mm = addr ? hv_mmio_lookup(v->m, addr) : NULL;
            if (mm && addr - mm->pa < mm->size && mm->write) {
                mm->write(mm->opaque, addr - mm->pa, data, 4);
                return;
            }
        }
    }
    hv_pci_set_intx(&v->pci, 1);
}

void hv_virtio_interrupt(HvVirtio *v) {
    v->isr |= 1;
    virtio_raise(v);
}

void hv_virtio_config_changed(HvVirtio *v) {
    v->config_gen++;
    v->isr |= 2;
    virtio_raise(v);
}

int hv_virtq_pop(HvVirtio *v, int qi, HvVirtqElem *e) {
    HvVirtq *q = &v->q[qi];
    uint16_t *avail = hv_phys_ptr(v->m, q->avail, 4 + 2u * q->size);
    if (!avail) return 0; /* not set up: nothing taken (-1 means an element was taken but is broken) */
    uint16_t idx = __atomic_load_n(&avail[1], __ATOMIC_ACQUIRE);
    if (q->last_avail == idx) return 0;
    /* The guest writes the ring entry (and the descriptors) before the index,
       from another core: the acquire above keeps these reads after it, or a
       stale entry (the driver fills unused ones with 0xffff) comes back. */
    uint16_t head = __atomic_load_n(&avail[2 + q->last_avail % q->size], __ATOMIC_RELAXED);
    if (head >= q->size) {
        /* Not written yet as far as this core can see (the driver keeps unused
           entries at 0xffff). Taking it would complete a request that does not
           exist (the guest panics "Adding invalid index 65535") or lose the
           real one: leave it for the next notify or poll. */
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x: avail entry %u not ready (head %u, idx %u); retrying later\n",
                    v->pci.config[2] | v->pci.config[3] << 8, q->last_avail, head, idx);
        return 0;
    }
    q->last_avail++;
    if (q->out[head]) {
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x q%d: head %u taken again while still outstanding\n",
                    v->pci.config[2] | v->pci.config[3] << 8, qi, head);
    }
    q->out[head] = 1;
    e->head = head;
    e->nseg = 0;
    uint64_t table = q->desc;
    uint32_t count = q->size;
    uint16_t i = head;
    for (uint32_t steps = 0; steps < HV_VIRTQ_MAX_SEGS; steps++) {
        if (i >= count) return -1;
        uint8_t *d = hv_phys_ptr(v->m, table + 16u * i, 16);
        if (!d) return -1;
        uint64_t addr;
        uint32_t len;
        uint16_t flags, next;
        memcpy(&addr, d, 8);
        memcpy(&len, d + 8, 4);
        memcpy(&flags, d + 12, 2);
        memcpy(&next, d + 14, 2);
        if (flags & DESC_F_INDIRECT) { /* not offered, but harmless to follow */
            table = addr;
            count = len / 16;
            i = 0;
            continue;
        }
        if (e->nseg == HV_VIRTQ_MAX_SEGS) return -1;
        e->seg[e->nseg++] = (HvVirtqSeg){addr, len, (flags & DESC_F_WRITE) != 0};
        if (!(flags & DESC_F_NEXT)) return 1;
        i = next;
    }
    return -1;
}

void hv_virtq_push(HvVirtio *v, int qi, const HvVirtqElem *e, uint32_t written) {
    HvVirtq *q = &v->q[qi];
    if (e->head >= q->size || !q->out[e->head]) {
        /* Not something the driver gave us (or given back already): pushing it
           would corrupt its free list. */
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x q%d: completion of head %u that is not outstanding (dropped)\n",
                    v->pci.config[2] | v->pci.config[3] << 8, qi, e->head);
        return;
    }
    q->out[e->head] = 0;
    uint8_t *used = hv_phys_ptr(v->m, q->used, 4 + 8u * q->size);
    if (!used) return;
    uint16_t idx;
    memcpy(&idx, used + 2, 2);
    uint32_t id = e->head;
    memcpy(used + 4 + 8u * (idx % q->size), &id, 4);
    memcpy(used + 8 + 8u * (idx % q->size), &written, 4);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    idx++;
    memcpy(used + 2, &idx, 2);
}

static uint32_t copy(HvVirtio *v, const HvVirtqElem *e, int writable, uint32_t off, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    for (int s = 0; s < e->nseg && done < len; s++) {
        const HvVirtqSeg *g = &e->seg[s];
        if (g->write != writable) continue;
        if (off >= g->len) {
            off -= g->len;
            continue;
        }
        uint32_t n = g->len - off;
        if (n > len - done) n = len - done;
        uint8_t *p = hv_phys_ptr(v->m, g->addr + off, n);
        if (!p) break;
        if (writable) memcpy(p, buf + done, n);
        else memcpy(buf + done, p, n);
        done += n;
        off = 0;
    }
    return done;
}

uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t off, void *buf, uint32_t len) {
    return copy(v, e, 0, off, buf, len);
}

uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t off, const void *buf, uint32_t len) {
    return copy(v, e, 1, off, (uint8_t *)buf, len);
}

uint32_t hv_virtq_writable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (e->seg[s].write) n += e->seg[s].len;
    return n;
}
