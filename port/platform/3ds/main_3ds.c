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
 * (movelist_3ds.c; touch: top row = team, middle = next page, the cog at
 * the bottom left = settings: frame skip, 4:3 or widescreen, check for
 * updates from the GitHub releases, update_3ds.c).
 * L + R + START + SELECT: quit (the HOME button works too).
 * Held while the app starts: L = no 3D meshes (diagnostics), R = start the
 * profiling build instead (the "MvC2 profiler" CIA, when installed).
 * 3D slider up: stereoscopic 3D (the stage behind the fighters; r_c3d.c).
 * VS on the main menu: online VS against another 3DS (online_3ds.c): the
 * lobby on the bottom screen, then a match in lockstep over Wi-Fi.
 * Every second the log gets the frame rate and where the frame time went
 * (r3ds_prof.h zones, and the game's own steps).
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
#include "../../render/3ds/r3ds_prof.h"
#include "../pad.h"
#include "sys_3ds.h"
#include "movelist_3ds.h"
#include "update_3ds.h"
#include "settings_3ds.h"
#include "online_3ds.h"
#include <unistd.h>
#include <sys/stat.h>
#ifdef MVC2_PROF
/* the profiling build (mvc2_prof.3dsx): function sampling, prof_fn_3ds.c */
#include "prof_fn_3ds.h"
#define PROF_PHASE(p) prof_fn_phase(p)
#else
#define PROF_PHASE(p) ((void)0)
#endif

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

/* ---- the game's steps (game_loop.c STEP names), timed through mvc2_step_hook */
#define MAX_PSTEPS 24
static struct { const char *name; u64 ticks; } psteps[MAX_PSTEPS];
static int npsteps;
static u64 prof_step_t0;

static void prof_step_hook(const char *name)
{
    u64 t = svcGetSystemTick();
    int i;
    for (i = 0; i < npsteps && psteps[i].name != name; i++) {}
    if (i == npsteps) {
        if (npsteps == MAX_PSTEPS) { prof_step_t0 = t; return; }
        psteps[npsteps].name = name;
        psteps[npsteps].ticks = 0;
        npsteps++;
    }
    psteps[i].ticks += t - prof_step_t0;
    prof_step_t0 = t;
}

static void prof_steps_reset(void)
{
    for (int i = 0; i < npsteps; i++) psteps[i].ticks = 0;
}

/* the steps over 0.05 ms per logic step, as "name ms" (per logic step) */
static void prof_steps_print(int nsteps)
{
    char line[400];
    int n = snprintf(line, sizeof line, "  steps:");
    for (int i = 0; i < npsteps && n < (int)sizeof line - 40; i++) {
        double ms = (double)psteps[i].ticks * 1000.0 / SYSCLOCK_ARM11 / (nsteps > 0 ? nsteps : 1);
        if (ms < 0.05) continue;
        const char *nm = psteps[i].name;
        const char *sp = strchr(nm, ' '); /* "func_XXXXXXXX what": the words after the address */
        n += snprintf(line + n, sizeof line - n, " %.18s %.2f", sp ? sp + 1 : nm, ms);
    }
    printf("%s\n", line);
}

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

/* This program's id for online play: both consoles must run the same code
 * (the version, when this file was built, and where the link put the end of
 * the program, which moves with almost any change to any part of it). */
extern char __end__[];
static uint32_t build_id(void)
{
    char s[96];
    uint32_t h = 2166136261u;
    snprintf(s, sizeof s, "%s %s %s %lx %lx", update_version(), __DATE__, __TIME__, (unsigned long)(uintptr_t)__end__,
             (unsigned long)(uintptr_t)&mvc2_frame);
    for (const char *c = s; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return h;
}

#ifndef MVC2_PROF
/* R held at the start: the profiling build instead, when its CIA ("MvC2
 * profiler", title 0x000400000F3D3300, mvc2_prof.cia) is installed.  An
 * application jump: it starts when this program ends.  1 = jumping. */
#define PROF_TITLE_ID 0x000400000F3D3300ULL
static int jump_to_profiler(void)
{
    u64 tid = PROF_TITLE_ID;
    AM_TitleEntry info;
    int installed = 0;
    if (R_SUCCEEDED(amInit())) {
        installed = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &info));
        amExit();
    }
    if (!installed) {
        printf("R held: the profiler is not installed.\nInstall mvc2_prof.cia with FBI to use it.\n"
               "Starting the game normally...\n");
        svcSleepThread(3000000000LL);
        return 0;
    }
    printf("R held: starting the profiler...\n");
    static u8 param[0x300], hmac[0x20];
    if (R_FAILED(APT_PrepareToDoApplicationJump(0, PROF_TITLE_ID, MEDIATYPE_SD)) ||
        R_FAILED(APT_DoApplicationJump(param, sizeof param, hmac))) {
        printf("Could not start the profiler; starting the game normally.\n");
        svcSleepThread(2000000000LL);
        return 0;
    }
    return 1;
}
#endif

int main(int argc, char **argv)
{
    static char dir[256];
    bool n3ds = false;
    int logging;

    gfxInitDefault();
    gfxSet3D(true); /* right-eye framebuffers; a frame drawn for one eye shows in 2D */
    consoleInit(GFX_BOTTOM, NULL);
    consoleDebugInit(debugDevice_CONSOLE); /* the host layer logs on stderr */
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(true);              /* New 3DS: 804 MHz + L2 cache */
    sys3ds_app_dir(argc, argv, dir, sizeof dir);
    /* the app's folder is the working directory (the memory card folder
     * mvc2_memcard/ is relative), also when started as a CIA */
    chdir(dir);
    static Settings3DS cfg;
    settings_load(dir, &cfg);
    /* Each character mode keeps its own memory card: everything unlocked
     * (mvc2_memcard_unlocked/) or the game's own progression, bought in the
     * shop (mvc2_memcard/).  A save holds the unlocks, so one card for both
     * would carry "everything" into progression.  Earlier builds always
     * unlocked everything: their card becomes the unlocked one. */
    if (cfg.version < 2) {
        char a[300], b[300];
        struct stat sa, sb;
        snprintf(a, sizeof a, "%s/mvc2_memcard", dir);
        snprintf(b, sizeof b, "%s/mvc2_memcard_unlocked", dir);
        if (stat(a, &sa) == 0 && stat(b, &sb) != 0) rename(a, b);
        settings_save(dir, &cfg);
    }
    setenv("MVC2_MEMCARD", cfg.unlock_all ? "mvc2_memcard_unlocked" : "mvc2_memcard", 1);
    const int unlock_active = cfg.unlock_all; /* the mode this run plays (a change waits for a restart) */
    sys3ds_log_tee();
#ifdef MVC2_PROF
    cfg.logging = 1; /* the profiler's report goes to the log */
#endif
    logging = cfg.logging && sys3ds_log_file(dir, 1) == 0;
    printf("MvC2 decomp for 3DS v%s, built %s %s (%s, %s)\n", update_version(), __DATE__, __TIME__,
           n3ds ? "New 3DS" : "Old 3DS", envIsHomebrew() ? "3dsx" : "CIA");
    printf("log: %s\n", logging ? "mvc2_log.txt" : cfg.logging ? "cannot write mvc2_log.txt" : "off (settings)");
    hidScanInput();
    u32 start_held = hidKeysHeld();
#ifndef MVC2_PROF
    if ((start_held & KEY_R) && jump_to_profiler()) {
        /* the profiler starts when this program has ended */
        sys3ds_log_close();
        gfxExit();
        return 0;
    }
#endif
    sys3ds_fpu_ps2_mode();

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4) || r_init() != 0) {
        printf("\nGPU init failed (citro3d / renderer).\n");
        sys3ds_wait_for_start();
        sys3ds_log_close();
        gfxExit();
        return 1;
    }
    fe_render_init();
    r3ds_set_deferred(1); /* record during the logic, replay after C3D_FrameBegin */
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
    int frame_skip = cfg.frame_skip;
    r3ds_set_present_mode(cfg.stretch ? R3DS_PRESENT_STRETCH : R3DS_PRESENT_43);
    mvc2_unlock_all = cfg.unlock_all;
    mvc2_trace_state = logging;
    movelist_settings(cfg.logging, cfg.unlock_all, unlock_active);
    movelist_status("", frame_skip, cfg.stretch);
    movelist_update();
    online_init(dir, build_id(), cfg.unlock_all ? "mvc2_memcard_unlocked" : "mvc2_memcard", unlock_active);
    printf("online: build %08X%s\n", (unsigned)build_id(), mvc2_can_restart() ? "" : " (off: no memory for the restart copy)");

    PadRaw idle;
    memset(&idle, 0, sizeof idle);
    idle.lx = idle.ly = idle.rx = idle.ry = 0x80;
    idle.connected = 1;

    /* Pacing: one logic step per refresh of the top screen (59.83 Hz), in
     * phase with it: when ahead, the loop waits for the refresh; when behind
     * (a frame took longer), the next frame runs the steps that are due (up
     * to 3) and draws only the last (r_set_discard steps, the browser build's
     * frame skip): the game keeps its speed and the picture rate drops
     * instead.  Frame skip off: one step per frame, and a slow frame slows
     * the game down (the PS2's behaviour).
     * Each frame: the logic records its draws (r_c3d.c, deferred), then
     * C3D_FrameBegin waits for the GPU to finish the last frame, which it
     * drew while this frame's logic ran, and the records go to citro3d. */
    const double vb_ticks = SYSCLOCK_ARM11 / 59.8261;
    u64 next_tick = svcGetSystemTick();
    u64 t_window = svcGetSystemTick();
    double logic_ms_max = 0, gpu_ms_sum = 0;
    int nwin = 0, steps_sum = 0, skipped_sum = 0, eyes_sum = 0, nstatus = 0;
    double frame_ms_max = 0;
    u64 frame_t0 = svcGetSystemTick();
    char shown_status[48] = "";
    memset(r3ds_prof, 0, sizeof r3ds_prof);
    prof_steps_reset();
    mvc2_step_hook = logging ? prof_step_hook : NULL; /* the step times only go to the log */
#ifdef MVC2_PROF
    prof_fn_start();
#endif

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
            if (act >= MOVELIST_ONLINE) {
                online_touch(act);
                act = 0;
            } else if ((act == MOVELIST_UPDATE_CHECK || act == MOVELIST_UPDATE_INSTALL) && online_lobby_open())
                act = 0; /* not during online play */
            if (act == MOVELIST_TOGGLE_ASPECT)
                r3ds_set_present_mode(r3ds_present_mode() == R3DS_PRESENT_43 ? R3DS_PRESENT_STRETCH : R3DS_PRESENT_43);
            else if (act == MOVELIST_TOGGLE_SKIP)
                frame_skip = !frame_skip;
            else if (act == MOVELIST_UPDATE_CHECK)
                update_check(dir);
            else if (act == MOVELIST_UPDATE_INSTALL)
                update_install();
            else if (act == MOVELIST_TOGGLE_LOG) {
                cfg.logging = !cfg.logging;
                logging = cfg.logging && sys3ds_log_file(dir, 1) == 0;
                if (!cfg.logging) sys3ds_log_file(dir, 0);
                mvc2_step_hook = logging ? prof_step_hook : NULL;
                mvc2_trace_state = logging;
                if (logging) printf("log on (settings), v%s built %s %s (%s), frame %ld\n", update_version(), __DATE__,
                                    __TIME__, envIsHomebrew() ? "3dsx" : "CIA", mvc2_host_frame);
            } else if (act == MOVELIST_TOGGLE_UNLOCK) {
                /* from the next start, with that mode's memory card (above):
                 * switching now would write this run's unlocks into the
                 * other mode's save */
                cfg.unlock_all = !cfg.unlock_all;
            }
            if (act) {
                cfg.frame_skip = frame_skip;
                cfg.stretch = r3ds_present_mode() == R3DS_PRESENT_STRETCH;
                if (act != MOVELIST_UPDATE_CHECK && act != MOVELIST_UPDATE_INSTALL) settings_save(dir, &cfg);
                movelist_settings(cfg.logging, cfg.unlock_all, unlock_active);
                movelist_status(NULL, frame_skip, cfg.stretch);
            }
        }
        {
            /* the updater (background thread) -> the settings screen */
            char ul[96];
            int changed, us = update_state(ul, sizeof ul, &changed);
            if (changed) {
                movelist_update_status(us, ul);
                printf("update: %s\n", ul);
            }
        }
        /* online play: the network, the lobby, a restart when a match starts */
        online_refresh();
        PadRaw p0 = read_pad(held);
        const uint16_t mine = p0.buttons;
        const int online = online_drives_game();
        /* VS on the main menu opens the online lobby; the lobby keeps the pad */
        if (!online && (online_take_vs(p0.buttons) || online_blocks_pad())) p0 = idle;
        float slider = osGet3DSliderState(); /* 0: 2D (one eye) */

        /* pacing (above) */
        u64 now = svcGetSystemTick();
        if ((s64)(next_tick - now) > (s64)(vb_ticks * 0.25)) {
            u64 t0 = RP_T();
            PROF_PHASE(PROF_PHASE_VBLANK);
            gspWaitForVBlank();
            PROF_PHASE(PROF_PHASE_OTHER);
            RP_ADD(RP_VBLANK, t0);
            now = svcGetSystemTick();
            next_tick = now; /* due now: in phase with the refresh */
        }
        int steps = 1;
        if (frame_skip) {
            s64 late = (s64)(now - next_tick);
            if (late > 0) steps = 1 + (int)((double)late / vb_ticks);
            if (steps > 3) steps = 3;
            next_tick += (u64)(steps * vb_ticks);
            if ((s64)(now - next_tick) > (s64)vb_ticks)
                next_tick = now; /* still behind after 3 steps: the game slows down */
        } else {
            if ((s64)(now - next_tick) > 0) next_tick = now;
            next_tick += (u64)vb_ticks;
        }

        if (online) {
            /* a match (lockstep: only frames whose both pads are known) or
             * the sync test / the boot into VS (many steps, the last drawn) */
            steps = online_plan(steps, mine);
            if (steps == 0) {
                /* nothing new: the last picture stays; the bottom screen
                 * when it changed; the next refresh */
                if (movelist_dirty()) {
                    C3D_FrameBegin(0);
                    movelist_render();
                    C3D_FrameEnd(0);
                } else {
                    gspWaitForVBlank();
                }
                next_tick = svcGetSystemTick();
                continue;
            }
        }
        r3ds_frame_start();
        u64 l0 = RP_T();
        PROF_PHASE(PROF_PHASE_LOGIC);
        for (int st = 0; st < steps; st++) {
            PadRaw p1 = idle;
            if (online) online_pads(&p0, &p1);
            host_pad_set(0, &p0);
            host_pad_set(1, &p1);
            r_set_discard(st < steps - 1);
            prof_step_t0 = RP_T();
            mvc2_frame();
            fe_audio_frame(); /* renders only without a device (keeps the music status moving) */
            if (online && !online_after_step()) {
                steps = st + 1; /* (its picture was not drawn: the last one stays) */
                break;
            }
        }
        r_set_discard(0);
        r3ds_present();
        PROF_PHASE(PROF_PHASE_OTHER);
        u64 l1 = RP_T();
        r3ds_prof[RP_LOGIC] += l1 - l0;
        double logic_ms = (double)(l1 - l0) * 1000.0 / SYSCLOCK_ARM11;

        u64 t0 = RP_T();
        PROF_PHASE(PROF_PHASE_GPUWAIT);
        C3D_FrameBegin(0);
        RP_ADD(RP_GPUWAIT, t0);
        PROF_PHASE(PROF_PHASE_SUBMIT);
        t0 = RP_T();
        r3ds_frame_submit(slider);
        RP_ADD(RP_SUBMIT, t0);
        PROF_PHASE(PROF_PHASE_OTHER);
        t0 = RP_T();
        int ui_drew = movelist_render(); /* the bottom screen, only when it changed */
        RP_ADD(RP_UI, t0);
        t0 = RP_T();
        /* GX_CMDLIST_FLUSH: flush only the command list; the renderer flushed
         * what it wrote (vertex ring, textures, tile pages).  The bottom
         * screen's vertices need the whole-heap flush (0), only when it drew. */
        C3D_FrameEnd(ui_drew ? 0 : GX_CMDLIST_FLUSH);
        RP_ADD(RP_END, t0);

        gpu_ms_sum += C3D_GetDrawingTime();
        steps_sum += steps;
        skipped_sum += steps - 1;
        {
            int eyes;
            r3ds_stereo_info(NULL, NULL, &eyes);
            eyes_sum += eyes;
        }
        if (logic_ms > logic_ms_max) logic_ms_max = logic_ms;
        {
            u64 tf = svcGetSystemTick();
            double fms = (double)(tf - frame_t0) * 1000.0 / SYSCLOCK_ARM11;
            if (fms > frame_ms_max) frame_ms_max = fms;
            frame_t0 = tf;
        }
        if (logging && mvc2_host_frame < 300)
            sys3ds_log_flush(); /* early frames: a freeze still leaves the log up to it */
        if ((nwin + 1) % 15 == 0)
            movelist_update();
        if (++nwin == STATUS_EVERY) {
            u64 tnow = svcGetSystemTick();
            double secs = (double)(tnow - t_window) / SYSCLOCK_ARM11;
            double ms[RP_N];
            for (int z = 0; z < RP_N; z++) ms[z] = (double)r3ds_prof[z] * 1000.0 / SYSCLOCK_ARM11 / nwin;
            int draws, verts, meshes;
            size_t exp;
            float zf, zn;
            r3ds_stats(&draws, &verts, &meshes, &exp);
            r3ds_stereo_info(&zf, &zn, NULL);
            if (logging) {
            printf("f%6ld st %06X %4.1ffps (game %4.1f/s, %d skipped) logic %4.1f/%4.1fms gpu %4.1fms "
                   "dr %d v %d mesh %d tex %uK tiles %d lin %luK rej %d 3d %.1f\n",
                   mvc2_host_frame, (unsigned)mvc2_game_state(), nwin / secs, steps_sum / secs, skipped_sum,
                   ms[RP_LOGIC], logic_ms_max, gpu_ms_sum / nwin,
                   draws, verts, meshes, (unsigned)(exp >> 10), r3ds_char_misses(),
                   (unsigned long)(linearSpaceFree() >> 10), r3ds_mesh_rejects(), (double)eyes_sum / nwin);
            /* where each frame's time went (ms per frame, averaged) */
            printf("  time: vblank %.1f logic %.1f (mesh %.1f char %.1f tex %.1f sndlock %.2f) meshwait %.2f gpuwait %.1f "
                   "submit %.1f ui %.1f end %.1f, longest frame %.1f | records %d stage z %.3f..%.3f%s\n",
                   ms[RP_VBLANK], ms[RP_LOGIC], ms[RP_MESH], ms[RP_CHAR], ms[RP_TEX], ms[RP_ALOCK], ms[RP_MESHWAIT],
                   ms[RP_GPUWAIT], ms[RP_SUBMIT], ms[RP_UI], ms[RP_END], frame_ms_max, r3ds_frame_records(),
                   (double)zf, (double)zn, "");
            prof_steps_print(steps_sum);
            }
            /* the bottom screen's status row: redrawn only when its text
             * changes (a redraw costs CPU time and a whole-heap cache flush) */
            char st[64];
            if (logging) /* with the log on: what limits the frame rate, CPU (game core) or GPU */
                snprintf(st, sizeof st, "%.0f fps  cpu %.1f  gpu %.1f%s", nwin / secs + 0.3,
                         ms[RP_LOGIC] + ms[RP_SUBMIT] + ms[RP_MESHWAIT], gpu_ms_sum / nwin, eyes_sum > nwin ? " 3D" : "");
            else
                snprintf(st, sizeof st, "%.0f fps%s", nwin / secs + 0.3, eyes_sum > nwin ? "  3D" : "");
            {
                char ol[32];
                if (online_status(ol, sizeof ol))
                    snprintf(st + strlen(st), sizeof st - strlen(st), "  %s", ol);
            }
            if (strcmp(st, shown_status)) {
                strcpy(shown_status, st);
                movelist_status(st, frame_skip, r3ds_present_mode() == R3DS_PRESENT_STRETCH);
            }
#ifdef MVC2_PROF
            if ((nstatus + 1) % 30 == 0)
                prof_fn_report(1); /* every 30 s, then counted afresh */
#endif
            /* the SD card write every 10 s (a write can take milliseconds) */
            if (++nstatus % 10 == 0 && logging)
                sys3ds_log_flush();
            nwin = 0;
            steps_sum = skipped_sum = eyes_sum = 0;
            logic_ms_max = gpu_ms_sum = frame_ms_max = 0;
            memset(r3ds_prof, 0, sizeof r3ds_prof);
            prof_steps_reset();
            t_window = tnow;
        }
    }
    mvc2_step_hook = NULL;
    online_shutdown();
    update_shutdown();
#ifdef MVC2_PROF
    prof_fn_report(1);
    prof_fn_stop();
#endif

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
