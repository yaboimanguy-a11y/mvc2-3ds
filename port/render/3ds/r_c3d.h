/* 3DS-only entry points of the citro3d renderer (r_c3d.c). */
#ifndef PORT_R_C3D_H
#define PORT_R_C3D_H
#include <stddef.h>
#include <stdint.h>
#include <citro3d.h>

/* A frame, immediate (default: the self-test):
 *   C3D_FrameBegin, r3ds_frame_start, draws, r3ds_present, r3ds_frame_finish, C3D_FrameEnd
 * deferred (the app): the draws are recorded while the logic runs, and
 * replayed once the GPU is free, so the logic overlaps the GPU's last frame:
 *   r3ds_frame_start, draws, r3ds_present, C3D_FrameBegin, r3ds_frame_submit,
 *   (other citro3d work), C3D_FrameEnd */
void r3ds_set_deferred(int on);
void r3ds_frame_start(void);   /* frees the garbage of frames done, starts recording (immediate: clears the top screen) */
void r3ds_present(void);       /* the canvas (fl_get_display part) -> top screen */
void r3ds_frame_finish(void);  /* immediate, before C3D_FrameEnd: flushes batches and the vertex cache */
/* deferred, after C3D_FrameBegin: flushes, then replays the frame into
 * citro3d, once per eye when slider > 0 (stereoscopic 3D, osGet3DSliderState) */
void r3ds_frame_submit(float slider);
uint32_t r3ds_frame_number(void);
int r3ds_frame_records(void);  /* draw records of the frame being built */
/* stereo: the smoothed depth range of the stage (0 when none) and the eyes drawn last frame */
void r3ds_stereo_info(float *zfar, float *znear, int *eyes);

enum { R3DS_PRESENT_43 = 0, R3DS_PRESENT_STRETCH = 1 };
void r3ds_set_present_mode(int mode);
int r3ds_present_mode(void);

/* last frame: draw calls, vertices, mesh draws; bytes of expanded indexed textures */
void r3ds_stats(int *draws, int *verts, int *mesh_draws, size_t *exp_bytes);

/* meshes: off draws none (diagnostics); refused draws so far (bad buffers / indices) */
void r3ds_set_mesh_enabled(int on);
/* how meshes are drawn (r_c3d.c): CPU vertex processing through the 2D
 * shader (default), or the GPU mesh shader (vertex lists / without its fog
 * output / indexed) for the self-test */
enum { R3DS_MESH_CPU = 0, R3DS_MESH_GPU = 1, R3DS_MESH_GPU_NOFOG = 2, R3DS_MESH_GPU_INDEXED = 3 };
void r3ds_set_mesh_mode(int mode);
int r3ds_mesh_rejects(void);

/* textures owned by others (character tile cache pages): an id for r_use(id, RM_RGBA) */
unsigned int r3ds_ext_register(C3D_Tex *t);
void r3ds_ext_unregister(unsigned int id);
/* a palette row as PICA RGBA8 words, and its version (bumped on every write) */
const uint32_t *r3ds_pal_row(int row, uint32_t *ver);
/* RGBA8 pixels (stride in pixels) into a tiled RGBA8 texture at (x0, y0) */
void r3ds_tex_put_rgba8(C3D_Tex *t, int x0, int y0, int w, int h, const uint8_t *rgba, int stride);
/* the GPU texture for a renderer texture id (+ palette row for indexed ones) */
C3D_Tex *r3ds_resolve(unsigned int id, int row, float *su, float *sv);
#endif
