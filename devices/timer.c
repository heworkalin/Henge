#include <stdio.h>

#include "timer.h"

#include "sysreg.h"

static uint64_t one(HvGic *g, int c, int ppi, uint64_t ctl, uint64_t cval, uint64_t now, uint64_t offset) {
    int enabled = (ctl & 1) && !(ctl & 2);
    int fired = enabled && now - offset >= cval;
    {   /* When a timer line turns on: whether the guest's timer is programmed at all, and whether
           the GIC ever asserts it, are different questions and the answer was neither, so far. */
        static int was[2][HV_MAX_CPUS];
        static int seen;
        int kind = ppi == HV_PPI_VTIMER ? 1 : 0;
        if (fired != was[kind][c] && seen++ < 20)
            fprintf(stderr, "hengevm: timer %s cpu %d fired %d ctl %llx cval %llx now-off %llx\n",
                    kind ? "virtual" : "physical", c, fired, (unsigned long long) ctl,
                    (unsigned long long) cval, (unsigned long long) (now - offset));
        was[kind][c] = fired;
    }
    hv_gicv3_set_ppi(g, c, ppi, fired);
    /* Deadline in host counter units; nothing to wait for if off or already firing. */
    return enabled && !fired ? cval + offset : 0;
}

uint64_t hv_timer_update(HvGic *g, HvCpu *cpu) {
    uint64_t now = hv_counter();
    {
        static int seen;
        if (seen++ < 5)
            fprintf(stderr, "hengevm: timer update vctl %llx cval %llx now-off %llx\n",
                    (unsigned long long) cpu->cntv_ctl, (unsigned long long) cpu->cntv_cval,
                    (unsigned long long) (now - cpu->cntvoff));
    }
    uint64_t v = one(g, (int)cpu->index, HV_PPI_VTIMER, cpu->cntv_ctl, cpu->cntv_cval, now, cpu->cntvoff);
    uint64_t p = one(g, (int)cpu->index, HV_PPI_PTIMER, cpu->cntp_ctl, cpu->cntp_cval, now, 0);
    if (!v) return p;
    if (!p) return v;
    return v < p ? v : p;
}
