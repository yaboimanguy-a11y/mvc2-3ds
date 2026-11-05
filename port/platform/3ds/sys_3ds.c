/* Shared pieces of the 3DS programs (see sys_3ds.h). */
#include <3ds.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include "sys_3ds.h"
#include "../../host/host_loop.h"
#include "../fs.h"

void sys3ds_app_dir(int argc, char **argv, char *out, size_t n)
{
    const char *slash;
    if (argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6) &&
        (slash = strrchr(argv[0], '/')) != NULL && slash - argv[0] < (long)n) {
        memcpy(out, argv[0], (size_t)(slash - argv[0]));
        out[slash - argv[0]] = 0;
        return;
    }
    snprintf(out, n, "%s", SYS3DS_DEFAULT_DIR);
}

/* preference of a disc image file by extension (any case): 1 .iso, 2 .cue,
 * 3 .bin; 0 = not a disc image.  A .bin works alone (the layout is detected
 * from the data, port/platform/fs.c), so its .cue is optional. */
static int disc_rank(const char *name)
{
    static const char *ext[] = { ".iso", ".cue", ".bin" };
    size_t l = strlen(name), i, j;
    for (i = 0; i < 3; i++) {
        if (l <= 4)
            break;
        for (j = 0; j < 4 && tolower((unsigned char)name[l - 4 + j]) == ext[i][j]; j++)
            ;
        if (j == 4)
            return (int)i + 1;
    }
    return 0;
}

#define MAX_DISCS 8
typedef struct { int rank; char path[512]; } DiscFile;

/* the disc image files in dir, preferred first (skipping hidden and macOS
 * "._" files); returns how many */
static int find_discs(const char *dir, DiscFile *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0, r, i, j;
    if (!d)
        return 0;
    while (n < MAX_DISCS && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || (r = disc_rank(e->d_name)) == 0)
            continue;
        if (snprintf(out[n].path, sizeof out[n].path, "%s/%s", dir, e->d_name) >= (int)sizeof out[n].path)
            continue;
        out[n++].rank = r;
    }
    closedir(d);
    for (i = 1; i < n; i++)  /* insertion sort by preference */
        for (j = i; j > 0 && out[j].rank < out[j - 1].rank; j--) {
            DiscFile t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

void sys3ds_print_read_stats(const char *what)
{
    uint64_t calls, bytes;
    fs_read_stats(&calls, &bytes);
    printf("disc reads %s: %llu calls, %.2f MB\n", what, (unsigned long long)calls, bytes / 1048576.0);
}

int sys3ds_open_disc(int argc, char **argv, const char *dir)
{
    static DiscFile discs[MAX_DISCS];
    int ndiscs, i, opened = 0;
    u64 t0;
    if (argc > 1 && argv[1] && argv[1][0]) {
        snprintf(discs[0].path, sizeof discs[0].path, "%s", argv[1]);
        ndiscs = 1;
    } else {
        ndiscs = find_discs(dir, discs);
    }
    if (ndiscs == 0) {
        printf("\nNo disc image in %s/\n", dir);
        printf("Put your own dump of SLUS-20486 in that\nfolder: a .iso, or a .bin (+ .cue).\n");
        return -1;
    }
    /* the first file that opens as the game disc (a .cue whose track file
     * was renamed, a stray .bin, ... are skipped) */
    t0 = osGetTime();
    for (i = 0; i < ndiscs && !opened; i++)
        opened = host_disc_open(discs[i].path) == 0;
    if (!opened) {
        printf("\nCannot read the disc image:\n");
        for (i = 0; i < ndiscs; i++)
            printf("  %s\n", discs[i].path);
        printf("It must be a .iso, .bin or .cue of the\nPS2 disc SLUS-20486.\n");
        return -1;
    }
    printf("disc: %s (opened in %llu ms)\n", discs[i - 1].path, (unsigned long long)(osGetTime() - t0));
    sys3ds_print_read_stats("to open");
    return 0;
}

/* ---- log file ----------------------------------------------------------
 * Everything printed goes to the console and to mvc2_log.txt next to the
 * app, so a run can be read back on a computer.  The console's own devoptab
 * does the screen part. */
static FILE *log_file;
static const devoptab_t *console_out;
static devoptab_t tee_out;
static int echo = 1;   /* stdout / stderr also on the console */

static ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    if (log_file)
        fwrite(ptr, 1, len, log_file);
    if (echo)
        return console_out->write_r(r, fd, ptr, len);
    return (ssize_t)len;
}

void sys3ds_log_console(int on)
{
    fflush(stdout);
    fflush(stderr);
    echo = on; /* off: the bottom screen is graphics (the move list); the log file keeps everything */
}

void sys3ds_con_print(const char *s)
{
    if (console_out) {
        console_out->write_r(_REENT, NULL, s, strlen(s));
    } else {
        fputs(s, stdout);
        fflush(stdout);
    }
}

/* stdout / stderr through the tee (console + the log file when open) */
void sys3ds_log_tee(void)
{
    if (console_out) return;
    console_out = devoptab_list[STD_OUT];
    tee_out = *console_out;
    tee_out.write_r = tee_write;
    devoptab_list[STD_OUT] = &tee_out;
    devoptab_list[STD_ERR] = &tee_out;
}

/* the log file on (a new mvc2_log.txt) or off; 0 = done */
int sys3ds_log_file(const char *dir, int on)
{
    fflush(stdout);
    fflush(stderr);
    if (!on) {
        if (log_file) { fclose(log_file); log_file = NULL; }
        return 0;
    }
    if (log_file) return 0;
    char path[300];
    snprintf(path, sizeof path, "%s/mvc2_log.txt", dir);
    log_file = fopen(path, "w");
    if (!log_file)
        return -1;
    setvbuf(log_file, NULL, _IOFBF, 16 * 1024);
    return 0;
}

int sys3ds_log_open(const char *dir)
{
    sys3ds_log_tee();
    return sys3ds_log_file(dir, 1);
}

void sys3ds_log_flush(void)
{
    fflush(stdout);
    fflush(stderr);
    if (log_file)
        fflush(log_file);
}

void sys3ds_log_close(void)
{
    sys3ds_log_flush();
    if (console_out) {
        devoptab_list[STD_OUT] = console_out;
        devoptab_list[STD_ERR] = console_out;
        console_out = NULL;
    }
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

/* ---- boot step timing ---------------------------------------------------
 * mvc2_step_hook runs after each step of mvc2_boot: its time and the disc
 * reads it made (calls into the image file, MB) are printed. */
static u64 step_t;
static uint64_t step_calls, step_bytes;

static void step_reset(void)
{
    step_t = osGetTime();
    fs_read_stats(&step_calls, &step_bytes);
}

static void step_done(const char *name)
{
    u64 now = osGetTime();
    uint64_t calls, bytes;
    fs_read_stats(&calls, &bytes);
    printf("  %6llu ms  %6llu reads %6.2f MB  %s\n", (unsigned long long)(now - step_t),
           (unsigned long long)(calls - step_calls), (bytes - step_bytes) / 1048576.0, name);
    step_reset();
}

unsigned long sys3ds_boot_timed(void)
{
    u64 t0 = osGetTime();
    mvc2_host_step = "boot";
    printf("boot steps:\n");
    step_reset();
    mvc2_step_hook = step_done;
    mvc2_boot(1);
    mvc2_step_hook = NULL;
    unsigned long ms = (unsigned long)(osGetTime() - t0);
    printf("boot: %lu ms\n", ms);
    sys3ds_print_read_stats("after boot");
    return ms;
}

int sys3ds_start_pressed(void)
{
    hidScanInput();
    return (hidKeysDown() & KEY_START) != 0;
}

void sys3ds_wait_for_start(void)
{
    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        if (sys3ds_start_pressed())
            break;
        gspWaitForVBlank();
    }
}

/* ---- FPU: the EE's rounding --------------------------------------------
 * The EE's COP1 rounds every single-precision result toward zero and has no
 * denormals (port/host/ps2_float.c); the VFP11 does exactly that with
 * FPSCR.RMode = RZ (bits 22-23) and FZ (bit 24).  DN (bit 25) keeps the
 * VFP in RunFast mode (no support-code traps).  libctru starts threads with
 * 0x03000000 (FZ + DN, round to nearest). */
static inline uint32_t get_fpscr(void) { uint32_t v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v; }
static inline void set_fpscr(uint32_t v) { __asm__ volatile("vmsr fpscr, %0" : : "r"(v)); }

void sys3ds_fpu_ps2_mode(void)
{
    uint32_t before = get_fpscr();
    set_fpscr((before & ~0x03C00000u) | 0x03C00000u);
    printf("fpu: FPSCR %08lX -> %08lX (round toward zero, flush to zero)\n",
           (unsigned long)before, (unsigned long)get_fpscr());
}
