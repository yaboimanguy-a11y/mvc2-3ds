/*
 * Function sampling profiler of the profiling build (mvc2_prof.3dsx, built
 * with MVC2_PROF and the game, host and renderer code compiled with
 * -finstrument-functions-after-inlining).
 *
 * The 3DS kernel does not let a program read another thread's registers, so
 * the instrumented functions keep a shadow call stack (__cyg_profile_func_*
 * below, main thread only) and a sampler thread on another core reads it
 * about 2000 times a second: the function on top gets a "self" sample, every
 * function on the stack an "inclusive" one.  prof_fn_report() prints the
 * largest of both to the log as addresses; port/tools/prof_symbolize.py
 * turns them into names with the build's mvc2_prof.elf.
 *
 * The hooks only count; everything else runs on the sampler thread.  The
 * hooks cost some time per call, so small functions called very often look
 * a little more expensive than they are.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "prof_fn_3ds.h"

#define NOINS __attribute__((no_instrument_function))
#define PSTK 256           /* shadow stack entries (deeper calls are counted, not recorded) */
#define HSLOTS 8192        /* functions seen (open addressing) */
#define INCL_DEPTH 64      /* stack entries the inclusive counts look at */

static void *volatile main_tls;
static volatile uint32_t sh_sp;
static void *volatile sh_stk[PSTK];
static volatile int phase;  /* PROF_PHASE_* of the main thread */

typedef struct Slot { uint32_t fn, self, incl; } Slot;
static Slot slots[HSLOTS];
static uint32_t phase_samples[PROF_PHASE_N], samples, lost;
static Thread sampler;
static volatile int quit;
static LightLock table_lock; /* the sampler's table against the report */

NOINS void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)site;
    if (getThreadLocalStorage() != main_tls) return;
    uint32_t sp = sh_sp;
    if (sp < PSTK) sh_stk[sp] = fn;
    sh_sp = sp + 1;
}

NOINS void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn;
    (void)site;
    if (getThreadLocalStorage() != main_tls) return;
    uint32_t sp = sh_sp;
    if (sp) sh_sp = sp - 1;
}

NOINS void prof_fn_phase(int p) { phase = p; }

NOINS static Slot *slot_of(uint32_t fn)
{
    uint32_t h = (fn >> 2) * 2654435761u;
    for (uint32_t k = 0; k < HSLOTS; k++) {
        Slot *s = &slots[(h + k) & (HSLOTS - 1)];
        if (s->fn == fn) return s;
        if (!s->fn) { s->fn = fn; return s; }
    }
    return NULL;
}

NOINS static void sample(void)
{
    int ph = phase;
    if (ph < 0 || ph >= PROF_PHASE_N) ph = PROF_PHASE_OTHER;
    LightLock_Lock(&table_lock);
    samples++;
    phase_samples[ph]++;
    if (ph == PROF_PHASE_LOGIC) {
        uint32_t sp = sh_sp, n = sp < PSTK ? sp : PSTK;
        uint32_t stk[INCL_DEPTH];
        if (n > INCL_DEPTH) n = INCL_DEPTH;
        for (uint32_t i = 0; i < n; i++) stk[i] = (uint32_t)sh_stk[i];
        if (sp == 0 || sp > PSTK) {
            lost++; /* outside instrumented code, or deeper than the shadow stack */
        } else {
            Slot *s = slot_of((uint32_t)sh_stk[sp - 1]);
            if (s) s->self++;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t j;
            for (j = 0; j < i && stk[j] != stk[i]; j++) {}
            if (j < i) continue; /* recursion: once per sample */
            Slot *s = slot_of(stk[i]);
            if (s) s->incl++;
        }
    }
    LightLock_Unlock(&table_lock);
}

NOINS static void sampler_main(void *arg)
{
    (void)arg;
    while (!quit) {
        svcSleepThread(500000); /* 0.5 ms */
        sample();
    }
}

NOINS int prof_fn_start(void)
{
    LightLock_Init(&table_lock);
    main_tls = getThreadLocalStorage();
    sh_sp = 0;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio = prio > 0x19 ? prio - 8 : 0x18; /* above the game and the mixer: wakes on time */
    int core = 2;
    sampler = threadCreate(sampler_main, NULL, 16 * 1024, prio, core, false);
    if (!sampler) {
        core = 1;
        APT_SetAppCpuTimeLimit(30);
        sampler = threadCreate(sampler_main, NULL, 16 * 1024, prio, core, false);
    }
    printf("prof: function sampler %s (core %d)\n", sampler ? "running" : "NOT started", sampler ? core : -1);
    return sampler ? 0 : -1;
}

#define NTOP 40
NOINS static void print_top(const char *what, int self, uint32_t total)
{
    static uint32_t tfn[NTOP], tv[NTOP];
    int ntop = 0;
    for (int i = 0; i < HSLOTS; i++) {
        const Slot *s = &slots[i];
        uint32_t v = self ? s->self : s->incl;
        if (!s->fn || !v) continue;
        int k;
        if (ntop < NTOP) k = ntop++;
        else if (tv[NTOP - 1] < v) k = NTOP - 1;
        else continue;
        tfn[k] = s->fn;
        tv[k] = v;
        while (k > 0 && tv[k - 1] < tv[k]) { /* keep them sorted, largest first */
            uint32_t f = tfn[k - 1], c = tv[k - 1];
            tfn[k - 1] = tfn[k]; tv[k - 1] = tv[k];
            tfn[k] = f; tv[k] = c;
            k--;
        }
    }
    printf("prof %s (%% of the logic samples):\n", what);
    for (int i = 0; i < ntop; i++)
        printf("  %5.1f%%  0x%08lX\n", total ? 100.0 * tv[i] / total : 0.0, (unsigned long)tfn[i]);
}

NOINS void prof_fn_report(int reset)
{
    if (!sampler) return;
    LightLock_Lock(&table_lock);
    uint32_t logic = phase_samples[PROF_PHASE_LOGIC];
    double n = samples ? (double)samples : 1.0;
    printf("prof: %lu samples: logic %.1f%% (outside instrumented code %.1f%%), gpu wait %.1f%%, "
           "vblank wait %.1f%%, submit %.1f%%, other %.1f%%\n",
           (unsigned long)samples, 100.0 * logic / n, logic ? 100.0 * lost / logic : 0.0,
           100.0 * phase_samples[PROF_PHASE_GPUWAIT] / n, 100.0 * phase_samples[PROF_PHASE_VBLANK] / n,
           100.0 * phase_samples[PROF_PHASE_SUBMIT] / n, 100.0 * phase_samples[PROF_PHASE_OTHER] / n);
    print_top("self", 1, logic);
    print_top("inclusive", 0, logic);
    if (reset) {
        memset(slots, 0, sizeof slots);
        memset(phase_samples, 0, sizeof phase_samples);
        samples = lost = 0;
    }
    LightLock_Unlock(&table_lock);
}

NOINS void prof_fn_stop(void)
{
    if (!sampler) return;
    quit = 1;
    threadJoin(sampler, U64_MAX);
    threadFree(sampler);
    sampler = NULL;
}
