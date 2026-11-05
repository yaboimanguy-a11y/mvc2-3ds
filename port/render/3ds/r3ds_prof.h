/* Frame-time zones of the 3DS app: system ticks (SYSCLOCK_ARM11 per second)
 * added up by main_3ds.c, the renderer and the sound mixer, printed and
 * cleared with each status line (mvc2_log.txt).  Zones may nest (the mesh,
 * character and texture zones are inside the logic zone). */
#ifndef PORT_R3DS_PROF_H
#define PORT_R3DS_PROF_H
#include <stdint.h>
#include <3ds/types.h>
#include <3ds/svc.h>

enum {
    RP_VBLANK,   /* main: waiting for the display refresh (ahead of time) */
    RP_LOGIC,    /* main: the game's steps (and the draws they record) */
    RP_GPUWAIT,  /* main: C3D_FrameBegin, waiting for the GPU to finish the last frame */
    RP_SUBMIT,   /* main: r3ds_frame_submit, the records into citro3d (all eyes) */
    RP_UI,       /* main: the bottom screen */
    RP_END,      /* main: C3D_FrameEnd */
    RP_MESH,     /* renderer: 3D meshes on the CPU (in RP_LOGIC) */
    RP_CHAR,     /* renderer: character sprites, tile cache (in RP_LOGIC) */
    RP_TEX,      /* renderer: indexed texture expansions (in RP_LOGIC) */
    RP_ALOCK,    /* main thread waiting for the sound mixer's lock */
    RP_MESHWAIT, /* main thread waiting for the mesh transform worker (core 2) */
    RP_N
};
extern uint64_t r3ds_prof[RP_N];
#define RP_T() svcGetSystemTick()
#define RP_ADD(z, t0) (r3ds_prof[(z)] += svcGetSystemTick() - (t0))
#endif
