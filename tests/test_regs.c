/* Guest registers across every way a block can leave.

   A register that comes back changed after a helper call, a chained branch, an indirect branch or
   a return is not a subtle bug: the guest reads it back and uses it as an address. The programs
   here are instruction words rather than assembler, because tests/*.S use Darwin assembler syntax
   and these have to build on any aarch64 toolchain.

   Guest code sits at 0x10000 in a 16 KB RAM block, so blocks on the first page and blocks on the
   second one (0x11000) are both reachable, which is what makes a cross-page branch testable. */
#include "dispatch.h"
#include "exception.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HvMachine machine;
static HvCpu cpu;
static int failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL %-34s got %llx want %llx\n", what, (unsigned long long) got,
               (unsigned long long) want);
        failures++;
    } else {
        printf("ok   %-34s %llx\n", what, (unsigned long long) got);
    }
}

/* Load `image` (16 KB, mapped at 0x10000), start at 0x10000 and run until stop_pc. */
static int run_image(const char *name, const uint32_t *image, uint64_t stop, int chain, int log) {
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code) exit(1);
    memcpy(code, image, 16384);
    if (hv_machine_init(&machine, 4 << 20) != 0) exit(1);
    hv_machine_add_ram(&machine, 0x10000, 16384, code, 1);
    hv_cpu_reset_index(&cpu, &machine, 0);
    cpu.pc = 0x10000;
    machine.stop_pc = stop;
    machine.chain = chain;
    machine.log = log;
    printf("-- %s: running\n", name);
    fflush(stdout);
    int r = hv_run(&cpu);
    printf("-- %s: hv_run %d pc %llx\n", name, r, (unsigned long long) cpu.pc);
    return r;
}

/* A 16 KB image with a head of `nh` words at 0x10000 and `tail` at 0x11010. */
static uint32_t *image_of(const uint32_t *head, size_t nh, const uint32_t *tail, size_t nt) {
    uint32_t *w = calloc(16384, 1);
    if (!w) exit(1);
    memcpy(w, head, nh * 4);
    if (tail) memcpy((uint8_t *) w + 0x1010, tail, nt * 4);
    return w;
}

/* movz x16,#0x1234; movz x17,#0x5678: the sentinels every program starts with. */
#define SENTINELS 0xd2824690u, 0xd28acf11u
/* add x3,x16,xzr; add x4,x17,xzr; b . (the copy-and-stop tail). */
#define TAIL_COPY 0x8b1f0203u, 0x8b1f0224u, 0x14000000u
/* Where each tail ends: TAIL_COPY is three words, the cross-page tail is four (the msr first). */
#define STOP_COPY 0x11018u
#define STOP_MSR 0x1101cu

static void check_sentinels(const char *when) {
    char what[96];
    snprintf(what, sizeof what, "x16 %s", when), check(what, cpu.x[16], 0x1234);
    snprintf(what, sizeof what, "x17 %s", when), check(what, cpu.x[17], 0x5678);
    snprintf(what, sizeof what, "x3 %s", when), check(what, cpu.x[3], 0x1234);
    snprintf(what, sizeof what, "x4 %s", when), check(what, cpu.x[4], 0x5678);
}

int main(void) {
    {   /* A system register write goes to the helper; a load behind it takes the memory slow path. */
        static const uint32_t head[] = {
            SENTINELS,
            0xd518c00a,             /* msr vbar_el1, x10 */
            0xd2802001, 0xf2a00021, /* x1 = 0x10100 */
            0xf9400022,             /* ldr x2, [x1] */
            0xf9000422,             /* str x2, [x1, #8] */
            0x8b1f0203, 0x8b1f0224, /* the copies */
            0x14000000,             /* b . */
        };
        uint32_t *img = image_of(head, sizeof head / 4, NULL, 0);
        run_image("helper and memory", img, 0x10024, 0, 0);
        free(img);
        check_sentinels("after helper and memory");
    }

    {   /* A back edge that gets chained: after the first iteration the dispatcher is not involved. */
        static const uint32_t head[] = {
            SENTINELS,
            0xd2800065,             /* mov x5, #3 */
            0xd2802001, 0xf2a00021, /* x1 = 0x10100 */
            0xd10004a5,             /* sub x5, x5, #1 */
            0xf9400022,             /* ldr x2, [x1] */
            0xb5ffffc5,             /* cbnz x5, back */
            0x8b1f0203, 0x8b1f0224, /* the copies */
            0x14000000,             /* b . */
        };
        uint32_t *img = image_of(head, sizeof head / 4, NULL, 0);
        run_image("chained loop", img, 0x10028, 1, 0);
        free(img);
        check_sentinels("after a chained loop");
    }

    {   /* A conditional branch to the next page, whose target then makes a helper call: the shape
           the firmware dies in (cmp, b.eq far, far: msr vbar_el1). */
        static const uint32_t head[] = {
            SENTINELS,
            0xd2800020, 0xd2800021, /* x0 = x1 = 1 */
            0xeb00003f,             /* cmp x1, x0 */
            0x54007fe0,             /* b.eq 0x11010 */
            0x14000000,             /* b . */
        };
        static const uint32_t tail[] = {
            0xd518c00a,             /* msr vbar_el1, x10 */
            0x8b1f0203, 0x8b1f0224, /* the copies */
            0x14000000,             /* b . */
        };
        uint32_t *img = image_of(head, sizeof head / 4, tail, sizeof tail / 4);
        run_image("cross-page branch, then helper", img, STOP_MSR, 1, 0);
        free(img);
        check_sentinels("after a cross-page branch");
    }

    {   /* br x9: the indirect-branch cache, whose miss is a thunk of its own. */
        static const uint32_t head[] = {
            SENTINELS,
            0xd2820209, 0xf2a00029, /* x9 = 0x11010 */
            0xd61f0120,             /* br x9 */
            0x14000000,
        };
        static const uint32_t tail[] = {TAIL_COPY};
        uint32_t *img = image_of(head, sizeof head / 4, tail, sizeof tail / 4);
        run_image("br x9", img, STOP_COPY, 1, 0);
        free(img);
        check_sentinels("after br x9");
    }

    {   /* ret: the same cache, behind the return-address table. */
        static const uint32_t head[] = {
            SENTINELS,
            0x1000805e,             /* adr x30, 0x11010 */
            0xd65f03c0,             /* ret */
        };
        static const uint32_t tail[] = {TAIL_COPY};
        uint32_t *img = image_of(head, sizeof head / 4, tail, sizeof tail / 4);
        run_image("ret", img, STOP_COPY, 1, 0);
        free(img);
        check_sentinels("after ret");
    }

    {   /* The same as the cross-page case with the diagnostics on, which is how the app runs: it
           makes every block record the ring prologue that reads these registers. */
        static const uint32_t head[] = {
            SENTINELS,
            0xd2800020, 0xd2800021,
            0xeb00003f,
            0x54007fe0,
            0x14000000,
        };
        static const uint32_t tail[] = {
            0xd518c00a,
            0x8b1f0203, 0x8b1f0224,
            0x14000000,
        };
        uint32_t *img = image_of(head, sizeof head / 4, tail, sizeof tail / 4);
        run_image("cross-page branch with log=2", img, STOP_MSR, 1, 2);
        free(img);
        check_sentinels("with the block trace on");
    }

    {   /* An exception: the guest writes VBAR_EL1 and takes an SVC, so the vector runs with the
           registers the engine rebuilt for it. This is the path a fault also takes. */
        uint32_t *w = calloc(16384, 1);
        static const uint32_t head[] = {
            SENTINELS,
            0xd281000a, 0xf2a0002a, /* x10 = 0x10800: VBAR */
            0xd518c00a,             /* msr vbar_el1, x10 */
            0xd4000001,             /* svc #0 */
            0x14000000,             /* b . */
        };
        static const uint32_t vec[] = {TAIL_COPY};
        memcpy(w, head, sizeof head);
        /* the synchronous vector for an exception from EL1 is VBAR + 0x200 */
        memcpy((uint8_t *) w + 0x0a00, vec, sizeof vec);
        run_image("svc with a vector", w, 0x10a08, 0, 0);
        free(w);
        check_sentinels("after an svc to the vector");
    }

    {   /* A data abort: a load from an address nothing maps. The vector runs with the registers the
           engine rebuilt, which is the state the firmware panicked with on the device. */
        uint32_t *w = calloc(16384, 1);
        static const uint32_t head[] = {
            SENTINELS,
            0xd281000a, 0xf2a0002a, /* x10 = 0x10800: VBAR */
            0xd518c00a,             /* msr vbar_el1, x10 */
            0xd2800001, 0xf2aa0001, /* x1 = 0x50000000: nothing maps there */
            0xf9400022,             /* ldr x2, [x1] */
            0x14000000,
        };
        static const uint32_t vec[] = {TAIL_COPY};
        memcpy(w, head, sizeof head);
        memcpy((uint8_t *) w + 0x0a00, vec, sizeof vec);
        run_image("abort to the vector", w, 0x10a08, 0, 0);
        free(w);
        check_sentinels("after a data abort");
    }

    {   /* Register pressure: more live guest registers than host ones, so the translator has to
           spill, and the access that follows is the one a helper emulates. The base register it
           reads has to be the guest's, whichever place the block kept it in. */
        uint32_t *w = calloc(16384, 1);
        static const uint32_t head[] = {
            SENTINELS,
            0xd281000a, 0xf2a0002a,     /* x10 = 0x10800 */
            0xd518c00a,                 /* msr vbar_el1, x10 */
            0xd2800001, 0xf2aa0001,     /* x1 = 0x50000000 */
            0xd2800220, 0xd2800442, 0xd2800663, 0xd2800884, 0xd2800aa5, /* x0, x2..x5 live */
            0xd2800cc6, 0xd2800ee7, 0xd2801108, 0xd2801329, 0xd280222b, /* x6..x9, x11 */
            0xd280444c, 0xd280666d, 0xd280888e, 0xd280aab3, 0xd280ccd4, /* x12..x14, x19, x20 */
            0xd280eef5, 0xd2811116, 0xd2813337, 0xd2815558, 0xd2817779, /* x21..x25 */
            0xd281999a, 0xd281bbbb,                                     /* x26, x27 */
            0x8b02003c,                 /* add x28, x1, x2 */
            0xf940002f,                 /* ldr x15, [x1]: must abort at 0x50000000 */
            0x14000000,
        };
        static const uint32_t vec[] = {
            0xd538600f,                 /* mrs x15, far_el1 */
            0x8b1f021d,                 /* add x29, x16, xzr */
            0x14000000,
        };
        memcpy(w, head, sizeof head);
        memcpy((uint8_t *) w + 0x0a00, vec, sizeof vec);
        run_image("abort with register pressure", w, 0x10a08, 0, 0);
        free(w);
        check("FAR_EL1 is the address the guest used", cpu.x[15], 0x50000000);
        check("x16 after the abort", cpu.x[16], 0x1234);
        check("x29 (copy of x16 in the vector)", cpu.x[29], 0x1234);
    }

    printf("%s\n", failures ? "FAILED" : "all register checks passed");
    return failures ? 1 : 0;
}
