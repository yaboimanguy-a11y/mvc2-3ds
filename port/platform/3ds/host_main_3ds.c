/*
 * mvc2_host on the Nintendo 3DS: the headless boot test (port/stubs/host_main.c
 * does the same on desktop and node).
 *
 * Boots the game's own code from the user's disc image on the SD card and
 * runs its 60 Hz logic loop with no picture or sound, printing progress on
 * the top screen and into mvc2_log.txt.  It measures how fast the game's
 * logic runs on the console (the app, main_3ds.c, draws it).
 *
 *   disc image: a .iso, .cue or .bin in the app's own folder (any name), e.g.
 *               sdmc:/3ds/mvc2/Marvel vs. Capcom 2 (USA).bin
 *               (or the path given as argv[1], e.g. from 3dslink)
 *   START: stop   (after the run: START again to exit)
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mvc2_stubs.h"
#include "../../host/host_loop.h"
#include "sys_3ds.h"

/* The game was built for the PS2's large stacks; libctru's default main
 * thread stack is 32 KiB.  The browser build gives it 1 MiB too. */
u32 __stacksize__ = 1024 * 1024;

#define REPORT_EVERY 60

/* ---- run summary -------------------------------------------------------- */
#define MAX_STATES 64
static struct { long frame; unsigned state; u64 ms; } states[MAX_STATES];
static int nstates, states_dropped;

static void note_state(unsigned st, u64 ms)
{
    if (nstates && states[nstates - 1].state == st)
        return;
    if (nstates == MAX_STATES) {
        states_dropped++;
        return;
    }
    states[nstates].frame = mvc2_host_frame;
    states[nstates].state = st;
    states[nstates].ms = ms;
    nstates++;
}

int main(int argc, char **argv)
{
    static char dir[256];
    int i, logging;
    bool n3ds = false;
    u64 t_boot, t_last;
    long last_frame = 0;
    double fps_min = 0, fps_max = 0;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    consoleDebugInit(debugDevice_CONSOLE); /* the host layer logs on stderr */
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(true);              /* New 3DS: 804 MHz + L2 cache */
    sys3ds_app_dir(argc, argv, dir, sizeof dir);
    logging = sys3ds_log_open(dir) == 0;

    printf("MvC2 decomp: 3DS boot test\n");
    printf("%s, %u faked symbols in the link\n", n3ds ? "New 3DS" : "Old 3DS", mvc2_fake_count);
    printf("log: %s\n", logging ? "mvc2_log.txt (next to the app)" : "cannot write mvc2_log.txt");
    sys3ds_fpu_ps2_mode();

    if (sys3ds_open_disc(argc, argv, dir) != 0) {
        sys3ds_wait_for_start();
        sys3ds_log_close();
        gfxExit();
        return 1;
    }

    sys3ds_boot_timed();
    t_boot = osGetTime();
    printf("Running the game loop (START stops)\n");

    t_last = t_boot;
    sys3ds_log_flush();
    note_state((unsigned)mvc2_game_state(), 0);
    while (aptMainLoop()) {
        if (sys3ds_start_pressed())
            break;
        mvc2_frame();
        note_state((unsigned)mvc2_game_state(), osGetTime() - t_boot);
        if (mvc2_host_frame - last_frame >= REPORT_EVERY) {
            u64 now = osGetTime();
            double ms = (double)(now - t_last), fps = ms > 0 ? (mvc2_host_frame - last_frame) * 1000.0 / ms : 0.0;
            printf("frame %6ld  state %06X  %5.1f logic fps\n", mvc2_host_frame,
                   (unsigned)mvc2_game_state(), fps);
            if (fps > 0 && (fps < fps_min || fps_min == 0))
                fps_min = fps;
            if (fps > fps_max)
                fps_max = fps;
            last_frame = mvc2_host_frame;
            t_last = now;
            sys3ds_log_flush(); /* a crash later still leaves the log up to here */
        }
    }

    {
        u64 run_ms = osGetTime() - t_boot;
        printf("\n==== summary ====\n");
        printf("stopped at frame %ld after %.1f s\n", mvc2_host_frame, run_ms / 1000.0);
        printf("logic speed: avg %.1f fps (min %.1f, max %.1f per %d frames)\n",
               run_ms ? mvc2_host_frame * 1000.0 / run_ms : 0.0, fps_min, fps_max, REPORT_EVERY);
        printf("game states (g[0..2] as hex), in order:\n");
        for (i = 0; i < nstates; i++)
            printf("  frame %6ld  %7.2f s  state %06X\n", states[i].frame, states[i].ms / 1000.0,
                   states[i].state);
        if (states_dropped)
            printf("  (+%d later changes not listed)\n", states_dropped);
        sys3ds_print_read_stats("in total");
    }
    mvc2_host_step = "done";
    mvc2_stub_report();
    sys3ds_log_flush();
    printf("\nLog saved: %s/mvc2_log.txt\n", dir);
    sys3ds_wait_for_start();
    sys3ds_log_close();
    gfxExit();
    return 0;
}
