/*
 * The game back to its power-on state, in this process, and fingerprints of
 * its state (online play: port/platform/3ds/net_3ds.c).
 *
 * Online, two consoles run the same game steps with the same pad inputs and
 * must stay identical.  The game is deterministic (one logic step per frame,
 * pads read at its start, file reads complete at once, the RTC fixed by
 * host_clock_fixed), so it is enough that both start from the same memory:
 * each console restarts the game to the state it had when the app started,
 * whatever was played before, and boots it again (mvc2_boot).
 *
 * The game's memory on the host:
 *   - the EE data image (mvc2_hole_00424200, EE 0x00424200..0x004C2580, the
 *     credited data units and the holes filled from the disc): copied at the
 *     first boot, before anything ran, and copied back;
 *   - the EE BSS (mvc2_ee_bss) and the scratchpad: zero at power-on;
 *   - the EE heap (boot.c): emptied, same address;
 * and the host code that answers the game's library calls keeps a little
 * state of its own (open card files, ADXF handles, semaphores, the sound
 * client's queue, ...): each module's reset puts it back as at start.
 * The frontend (renderer caches, sound engine, character banks) is reset by
 * the app (fe_restart in port/host/fe), as it differs per platform.
 *
 * Fingerprints: mvc2_state_hash_frame hashes one slice of the state per
 * frame (1/32 of the data image, the BSS and the scratchpad, plus a 64 KB
 * piece of the heap in use), so two consoles comparing the hashes of the
 * same frame check every byte every few seconds at a small cost per frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "host_loop.h"

extern unsigned char mvc2_hole_00424200[];
extern unsigned char mvc2_ee_scratchpad[];
#define IMAGE_EE 0x00424200u
#define IMAGE_SIZE ((size_t)(EE_BSS_START - IMAGE_EE))
#define BSS_SIZE ((size_t)(EE_BSS_END - EE_BSS_START + 16))
#define SCRATCH_SIZE 0x4000u
#define SLICES 32
#define HEAP_CHUNK 0x10000u

u8 *host_ee_heap_base(void);
size_t host_ee_heap_used(void);
void host_boot_reset(void);
void host_kernel_reset(void);
void host_iop_reset(void);
void host_mc_reset(void);
void host_cri_reset(void);
void host_game_fix_reset(void);
void tsdc_restart(void);
/* port/stubs/ps2_kernel.c: not in every link */
__attribute__((weak)) void ps2_kernel_reset(void);

static u8 *pristine;

void mvc2_pristine_capture(void)
{
    if (pristine)
        return;
    pristine = (u8 *)malloc(IMAGE_SIZE);
    if (pristine)
        memcpy(pristine, mvc2_hole_00424200, IMAGE_SIZE);
    else
        host_log("restart: no memory for the data image copy (online play off)");
}

int mvc2_can_restart(void)
{
    return pristine != NULL;
}

int mvc2_restart(void)
{
    if (!pristine)
        return -1;
    host_cri_reset();      /* stops the music first (the mixer reads the heap) */
    memcpy(mvc2_hole_00424200, pristine, IMAGE_SIZE);
    memset(mvc2_ee_bss, 0, BSS_SIZE);
    memset(mvc2_ee_scratchpad, 0, SCRATCH_SIZE);
    host_boot_reset();
    host_kernel_reset();
    host_iop_reset();
    host_mc_reset();
    host_game_fix_reset();
    tsdc_restart();
    if (ps2_kernel_reset)
        ps2_kernel_reset();
    mvc2_host_frame = -1;
    return 0;
}

/* ---- fingerprints ---- */

/* four independent lanes (no multiply chain), then mixed */
static u32 hash_words(const u8 *p, size_t n, u32 seed)
{
    const u32 *w = (const u32 *)p;
    size_t nw = n / 4, i = 0;
    u32 a = seed ^ 0x9E3779B9u, b = seed ^ 0x85EBCA6Bu, c = seed ^ 0xC2B2AE35u, d = seed ^ 0x27D4EB2Fu;
    for (; i + 4 <= nw; i += 4) {
        a = (a ^ w[i]) * 0x01000193u;
        b = (b ^ w[i + 1]) * 0x01000193u;
        c = (c ^ w[i + 2]) * 0x01000193u;
        d = (d ^ w[i + 3]) * 0x01000193u;
    }
    for (; i < nw; i++)
        a = (a ^ w[i]) * 0x01000193u;
    for (i = nw * 4; i < n; i++)
        b = (b ^ p[i]) * 0x01000193u;
    a ^= (b << 7 | b >> 25) ^ (c << 13 | c >> 19) ^ (d << 21 | d >> 11);
    a ^= a >> 16;
    a *= 0x85EBCA6Bu;
    a ^= a >> 13;
    return a;
}

static void slice_of(size_t len, int k, size_t *lo, size_t *hi)
{
    *lo = (len * (size_t)k / SLICES) & ~(size_t)3;
    *hi = k == SLICES - 1 ? len : (len * (size_t)(k + 1) / SLICES) & ~(size_t)3;
}

static const char *region_name[] = { "data", "bss", "scratchpad", "heap" };

u32 mvc2_state_hash_frame(long frame, MvcHashSlice *where)
{
    int k = (int)(frame % SLICES);
    size_t lo, hi, used = host_ee_heap_used(), nchunk;
    u32 h;
    slice_of(IMAGE_SIZE, k, &lo, &hi);
    h = hash_words(mvc2_hole_00424200 + lo, hi - lo, (u32)frame);
    slice_of(BSS_SIZE, k, &lo, &hi);
    h = hash_words(mvc2_ee_bss + lo, hi - lo, h);
    slice_of(SCRATCH_SIZE, k, &lo, &hi);
    h = hash_words(mvc2_ee_scratchpad + lo, hi - lo, h);
    nchunk = (used + HEAP_CHUNK - 1) / HEAP_CHUNK;
    if (where) {
        where->slice = k;
        where->heap_lo = where->heap_hi = 0;
    }
    if (nchunk) {
        size_t c = (size_t)frame % nchunk, a = c * HEAP_CHUNK, b = a + HEAP_CHUNK;
        if (b > used)
            b = used;
        h = hash_words(host_ee_heap_base() + a, b - a, h);
        if (where) {
            where->heap_lo = (u32)a;
            where->heap_hi = (u32)b;
        }
    }
    return h;
}

/* each region's hash (data, bss, scratchpad, heap in use) */
void mvc2_state_hash_regions(u32 out[4])
{
    out[0] = hash_words(mvc2_hole_00424200, IMAGE_SIZE, 1);
    out[1] = hash_words(mvc2_ee_bss, BSS_SIZE, 2);
    out[2] = hash_words(mvc2_ee_scratchpad, SCRATCH_SIZE, 3);
    out[3] = host_ee_heap_base() ? hash_words(host_ee_heap_base(), host_ee_heap_used(), 4) : 0;
}

const char *mvc2_state_region_name(int r)
{
    return r >= 0 && r < 4 ? region_name[r] : "?";
}

/* A dump for comparing two consoles (or two runs) offline: "MV2S", frame,
 * then each region as (name[12], EE or heap offset, size, bytes).  The heap
 * goes in only from heap_lo to heap_hi (it is 28 MB). */
int mvc2_state_dump(const char *path, long frame, u32 heap_lo, u32 heap_hi)
{
    FILE *f = fopen(path, "wb");
    size_t used = host_ee_heap_used();
    struct { char name[12]; u32 at, size; } r[4];
    const u8 *src[4];
    int i, ok = 1;
    u32 hdr[2];
    if (!f)
        return -1;
    if (heap_hi > used)
        heap_hi = (u32)used;
    if (heap_lo > heap_hi)
        heap_lo = heap_hi;
    memset(r, 0, sizeof r);
    strcpy(r[0].name, "data");       r[0].at = IMAGE_EE;      r[0].size = (u32)IMAGE_SIZE;  src[0] = mvc2_hole_00424200;
    strcpy(r[1].name, "bss");        r[1].at = EE_BSS_START;  r[1].size = (u32)BSS_SIZE;    src[1] = mvc2_ee_bss;
    strcpy(r[2].name, "scratchpad"); r[2].at = 0x70000000u;   r[2].size = SCRATCH_SIZE;     src[2] = mvc2_ee_scratchpad;
    strcpy(r[3].name, "heap");       r[3].at = heap_lo;       r[3].size = heap_hi - heap_lo;
    src[3] = host_ee_heap_base() ? host_ee_heap_base() + heap_lo : mvc2_ee_scratchpad;
    if (!host_ee_heap_base())
        r[3].size = 0;
    memcpy(hdr, "MV2S", 4);
    hdr[1] = (u32)frame;
    ok &= fwrite(hdr, sizeof hdr, 1, f) == 1;
    for (i = 0; i < 4; i++) {
        ok &= fwrite(&r[i], sizeof r[i], 1, f) == 1;
        if (r[i].size)
            ok &= fwrite(src[i], r[i].size, 1, f) == 1;
    }
    ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}
