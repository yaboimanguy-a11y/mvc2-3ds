/*
 * mvc2 on the Nintendo 3DS: the game's own code, drawn with citro3d.
 *
 * The game runs on the top screen (port/render/3ds, the fl render library
 * bound by port/host/fe/fe_render.c, fe_char.c and fe_mesh.c as in the
 * browser app); the bottom screen is a status console that is also written
 * to mvc2_log.txt next to the app.
 *
 * Controls (PS2 pad):  D-pad / Circle Pad = directions
 *   Y = Square   X = Triangle   B = Cross   A = Circle
 *   L = L1   R = R1   ZL = L2   ZR = R2   START   SELECT
 * Bottom screen: the move list of player 1's point character
 * (movelist_3ds.c; touch: top row = team, middle = next page, bottom row =
 * frame skip / 4:3 or stretched).
 * L + R + START + SELECT: quit (the HOME button works too).
 * Held while the app starts (diagnostics): L = no 3D meshes, R = the FPU
 * stays in round-to-nearest (no EE float mode).
 *
 * disc image: a .iso, .cue or .bin in the app's own folder (sys_3ds.c).
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../host/host_loop.h"
#include "../../render/render.h"
#include "../../render/3ds/r_c3d.h"
#include "../pad.h"
#include "sys_3ds.h"
#include "movelist_3ds.h"

/* The game was built for the PS2's large stacks (libctru default: 32 KiB). */
u32 __stacksize__ = 1024 * 1024;
/* GPU-visible memory: vertex rings, textures, the character tile cache */
u32 __ctru_linear_heap_size = 40u << 20;

void fe_render_init(void);
int fe_audio_init(int open_device);
void fe_audio_frame(void);
void fe_audio_shutdown(void);
int audio_3ds_core(void);
int r3ds_char_misses(void);

#define STATUS_EVERY 60

static PadRaw read_pad(u32 held)
{
    PadRaw p;
    memset(&p, 0, sizeof p);
    p.lx = p.ly = p.rx = p.ry = 0x80;
    p.connected = 1;
    static const struct { u32 key; uint16_t ps2; } map[] = {
        { KEY_DUP, PS2_UP }, { KEY_DDOWN, PS2_DOWN }, { KEY_DLEFT, PS2_LEFT }, { KEY_DRIGHT, PS2_RIGHT },
        { KEY_CPAD_UP, PS2_UP }, { KEY_CPAD_DOWN, PS2_DOWN }, { KEY_CPAD_LEFT, PS2_LEFT }, { KEY_CPAD_RIGHT, PS2_RIGHT },
        { KEY_Y, PS2_SQUARE }, { KEY_X, PS2_TRIANGLE }, { KEY_B, PS2_CROSS }, { KEY_A, PS2_CIRCLE },
        { KEY_L, PS2_L1 }, { KEY_R, PS2_R1 }, { KEY_ZL, PS2_L2 }, { KEY_ZR, PS2_R2 },
        { KEY_START, PS2_START }, { KEY_SELECT, PS2_SELECT },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (held & map[i].key) p.buttons |= map[i].ps2;
    /* opposite directions cancel (a worn d-pad or pad + stick) */
    if ((p.buttons & (PS2_UP | PS2_DOWN)) == (PS2_UP | PS2_DOWN)) p.buttons &= (uint16_t)~(PS2_UP | PS2_DOWN);
    if ((p.buttons & (PS2_LEFT | PS2_RIGHT)) == (PS2_LEFT | PS2_RIGHT)) p.buttons &= (uint16_t)~(PS2_LEFT | PS2_RIGHT);
    return p;
}

int main(int argc, char **argv)
{
    static char dir[256];
    bool n3ds = false;
    int logging;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    consoleDebugInit(debugDevice_CONSOLE); /* the host layer logs on stderr */
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(true);              /* New 3DS: 804 MHz + L2 cache */
    sys3ds_app_dir(argc, argv, dir, sizeof dir);
    logging = sys3ds_log_open(dir) == 0;
    printf("MvC2 decomp for 3DS (%s)\n", n3ds ? "New 3DS" : "Old 3DS");
    printf("log: %s\n", logging ? "mvc2_log.txt" : "cannot write mvc2_log.txt");
    hidScanInput();
    u32 start_held = hidKeysHeld();
    if (start_held & KEY_R) printf("R held: FPU left in round-to-nearest\n");
    else sys3ds_fpu_ps2_mode();

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4) || r_init() != 0) {
        printf("\nGPU init failed (citro3d / renderer).\n");
        sys3ds_wait_for_start();
        sys3ds_log_close();
        gfxExit();
        return 1;
    }
    fe_render_init();
    if (start_held & KEY_L) {
        r3ds_set_mesh_enabled(0);
        printf("L held: 3D meshes off\n");
    }
    printf("linear memory free: %lu KB\n", (unsigned long)(linearSpaceFree() >> 10));

    if (sys3ds_open_disc(argc, argv, dir) != 0) {
        sys3ds_wait_for_start();
        sys3ds_log_close();
        C3D_Fini();
        gfxExit();
        return 1;
    }
    /* sound: the port's engine (TSD effects / voices + ADX music) on NDSP,
     * mixed on its own thread (port/audio/audio.c, the 3DS section) */
    fe_audio_init(1);
    if (audio_3ds_core() >= -1)
        printf("sound: on (mixer thread on core %d)\n", audio_3ds_core());
    else
        printf("sound: off (no DSP firmware: run the DSP1 homebrew once)\n");
    sys3ds_boot_timed();
    printf("Running. L+R+START+SELECT: quit.\n");
    sys3ds_log_flush();
    /* the bottom screen is the move list from here; the log keeps everything */
    int have_list = movelist_load(dir) == 0;
    printf("move list: %s\n", have_list ? "movelist.txt" : "no movelist.txt (bottom screen shows help)");
    sys3ds_log_console(0);
    if (movelist_ui_init() != 0)
        printf("bottom screen: citro2d init failed\n");
    int frame_skip = 1;
    movelist_status("", frame_skip, 0);
    movelist_update();

    u64 t_window = osGetTime();
    double logic_ms_sum = 0, frame_ms_sum = 0, logic_ms_max = 0, gpu_ms_sum = 0;
    int nwin = 0, steps_sum = 0, skipped_sum = 0;
    PadRaw idle;
    memset(&idle, 0, sizeof idle);
    idle.lx = idle.ly = idle.rx = idle.ry = 0x80;
    idle.connected = 1;

    /* Game speed: the logic runs one step per 1/60 s of real time.  When a
     * frame took longer, the next one runs the steps that are due (up to 3)
     * and draws only the last (the others are r_set_discard steps, as the
     * browser build's frame skip): the game keeps its speed and the picture
     * rate drops instead.  Frame skip off: one step per frame, and a slow
     * frame slows the game down (the PS2's behaviour). */
    const double step_ticks = SYSCLOCK_ARM11 / 60.0;
    double acc = 0;
    u64 last_tick = svcGetSystemTick();

    while (aptMainLoop()) {
        hidScanInput();
        u32 held = hidKeysHeld(), down = hidKeysDown();
        const u32 quit = KEY_L | KEY_R | KEY_START | KEY_SELECT;
        if ((held & quit) == quit)
            break;
        if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            int act = movelist_touch(tp.px, tp.py);
            if (act == MOVELIST_TOGGLE_ASPECT)
                r3ds_set_present_mode(r3ds_present_mode() == R3DS_PRESENT_43 ? R3DS_PRESENT_STRETCH : R3DS_PRESENT_43);
            else if (act == MOVELIST_TOGGLE_SKIP)
                frame_skip = !frame_skip;
            if (act)
                movelist_status(NULL, frame_skip, r3ds_present_mode() == R3DS_PRESENT_STRETCH);
        }
        PadRaw p0 = read_pad(held);

        u64 now_tick = svcGetSystemTick();
        acc += (double)(now_tick - last_tick);
        last_tick = now_tick;
        int steps = 1;
        if (frame_skip) {
            steps = (int)((acc + step_ticks / 2) / step_ticks);
            if (steps < 1) steps = 1;
            if (steps > 3) steps = 3;
            acc -= steps * step_ticks;
            if (acc > 2 * step_ticks) acc = 2 * step_ticks; /* further behind: the game slows down */
            if (acc < -step_ticks) acc = -step_ticks;
        } else {
            acc = 0;
        }

        u64 f0 = osGetTime();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        r3ds_frame_start();
        u64 l0 = osGetTime();
        for (int st = 0; st < steps; st++) {
            host_pad_set(0, &p0);
            host_pad_set(1, &idle);
            r_set_discard(st < steps - 1);
            mvc2_frame();
            fe_audio_frame(); /* renders only without a device (keeps the music status moving) */
        }
        r_set_discard(0);
        double logic_ms = (double)(osGetTime() - l0);
        r3ds_present();
        r3ds_frame_finish();
        movelist_render();  /* the bottom screen, only when it changed */
        C3D_FrameEnd(0);
        double frame_ms = (double)(osGetTime() - f0);

        logic_ms_sum += logic_ms;
        frame_ms_sum += frame_ms;
        gpu_ms_sum += C3D_GetDrawingTime();
        steps_sum += steps;
        skipped_sum += steps - 1;
        if (logic_ms > logic_ms_max) logic_ms_max = logic_ms;
        if (mvc2_host_frame < 300)
            sys3ds_log_flush(); /* early frames: a freeze still leaves the log up to it */
        if ((nwin + 1) % 15 == 0)
            movelist_update();
        if (++nwin == STATUS_EVERY) {
            u64 now = osGetTime();
            double secs = (double)(now - t_window) / 1000.0;
            int draws, verts, meshes;
            size_t exp;
            r3ds_stats(&draws, &verts, &meshes, &exp);
            printf("f%6ld st %06X %4.1ffps (game %4.1f/s, %d skipped) logic %4.1f/%4.1fms frame %4.1fms gpu %4.1fms "
                   "dr %d v %d mesh %d tex %uK tiles %d lin %luK rej %d\n",
                   mvc2_host_frame, (unsigned)mvc2_game_state(), nwin / secs, steps_sum / secs, skipped_sum,
                   logic_ms_sum / nwin, logic_ms_max, frame_ms_sum / nwin, gpu_ms_sum / nwin,
                   draws, verts, meshes, (unsigned)(exp >> 10), r3ds_char_misses(),
                   (unsigned long)(linearSpaceFree() >> 10), r3ds_mesh_rejects());
            char st[48];
            snprintf(st, sizeof st, "%.0f fps  cpu %.0f  gpu %.0f ms", nwin / secs, logic_ms_sum / nwin, gpu_ms_sum / nwin);
            movelist_status(st, frame_skip, r3ds_present_mode() == R3DS_PRESENT_STRETCH);
            sys3ds_log_flush();
            nwin = 0;
            steps_sum = skipped_sum = 0;
            logic_ms_sum = frame_ms_sum = logic_ms_max = gpu_ms_sum = 0;
            t_window = now;
        }
    }

    printf("\nquit at frame %ld\n", mvc2_host_frame);
    fe_audio_shutdown();
    {
        extern void audio_shutdown(void);
        audio_shutdown();
    }
    sys3ds_log_close();
    r_shutdown();
    C3D_Fini();
    gfxExit();
    return 0;
}
