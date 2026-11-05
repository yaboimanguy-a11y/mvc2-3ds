/* Renderer core for the Nintendo 3DS (citro3d / PICA200): the r_gl.c
 * interface (render state, palettes, texture handle tables, batching, 2D
 * primitives, canvas + present, overlay) on the 3DS GPU.
 *
 * Differences from the GL renderer, all forced by the hardware:
 * - No paletted textures and no dependent reads: an indexed texture is
 *   expanded to RGBA8 on the CPU per palette row, cached by (texture
 *   version, palette version).  A batch of indexed triangles is split into
 *   runs of one palette row at flush time (the row is per vertex).
 *   Character sprites have their own tile cache (r_char_c3d.c).
 * - Textures are power-of-two and tiled; other sizes are padded and the UVs
 *   scaled (uniform uvscale).
 * - The GPU runs a frame's commands after the frame: a texture that was
 *   drawn with in this frame is never overwritten (copy on write / new
 *   expansion) and freed one frame later.
 * - Clears are quads, so they keep their place among the draws and respect
 *   the scissor like glClear does.
 * - Present: the 640x480 canvas (a 1024x512 render-target texture) is
 *   scaled to the 400x240 top screen with the bilinear filter.  The CRT /
 *   sharp shaders of the GL renderer are not available.
 */
#include "../r_internal.h"
#include "../decode.h"
#include "r_c3d.h"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const u8 r2d_shbin[];
extern const u32 r2d_shbin_size;
extern const u8 mesh_shbin[];
extern const u32 mesh_shbin_size;
extern const u8 mesh_nofog_shbin[];
extern const u32 mesh_nofog_shbin_size;

#define MAX_TEX 4096
#define MAX_PAL 4096
#define MAX_VERTS 32768
#define VRING_BYTES (4u << 20)          /* per frame, two of them (2D + expanded meshes) */
#define EXP_PER_TEX 4
#define EXP_BUDGET (20u << 20)          /* expanded indexed textures, bytes */
#define MAX_DEAD 2048
#define MAX_EXT 64

/* texture ids (the "GLuint" of r_internal.h) */
#define ID_WHITE 0x10000u
#define ID_PAL 0x10001u
#define ID_EXT0 0x20000u                /* r3ds_ext_register: + index */

typedef struct Exp {
    C3D_Tex *t;
    int row;
    uint32_t pal_ver, tex_ver, last;
} Exp;

typedef struct TexRec {
    int used, w, h, fmt, pw, ph;
    uint8_t *idx;          /* indexed: one index per pixel, w*h */
    C3D_Tex *rgba;         /* direct colour: the GPU texture */
    uint32_t ver, used_frame;
    Exp exp[EXP_PER_TEX];
} TexRec;
typedef struct PalRec { int row, ncolors, used; } PalRec;

/* uniform locations of mesh.v.pica */
typedef struct MeshLoc {
    int mvp, vp, misc, cscale, modc, matsel, sel, fsel, uvrow0, uvrow1, uvscale, ldir, lcol, lamb, cnorm;
} MeshLoc;

enum { TGT_NONE = 0, TGT_CANVAS, TGT_TOP };

static struct {
    shaderProgram_s prog, mprog[2];     /* mesh.v.pica, mesh_nofog.v.pica */
    DVLB_s *dvlb, *mdvlb[2];
    MeshLoc mloc[2];
    C3D_Tex fog_ramp;       /* 256x8, alpha = s: the fog factor as a TEV source */
    int u_scale, u_rotx, u_roty, u_uvscale;
    C3D_Tex canvas;
    C3D_RenderTarget *canvas_rt, *top_rt;
    C3D_Tex white;
    uint8_t *vring[2];
    size_t vused;
    int vring_full_logged;
    TexRec tex[MAX_TEX];
    PalRec pal[MAX_PAL];
    uint32_t pal_rgba[R_PAL_ROWS][256];  /* PICA RGBA8 words (R<<24|G<<16|B<<8|A) */
    uint32_t pal_ver[R_PAL_ROWS];
    int pal_next_row;
    size_t exp_bytes;
    struct { C3D_Tex *t; uint32_t frame; } dead[MAX_DEAD];
    int ndead;
    struct { C3D_Tex *t; } ext[MAX_EXT];
    RVtx verts[MAX_VERTS];
    int nverts;
    GLuint cur_tex; int cur_mode;
    int target;             /* current C3D draw target */
    int view_w, view_h, tilt;
    uint32_t frame;
    /* fl state (func_003DF850), as r_gl.c */
    uint32_t state_ad0, state_ad4, cur_texture, tex_slots[4];
    uint32_t material_color, fog_color, global_mod, clear_color, state_5f;
    float fog_a, fog_b;
    float matrices[0x60][16];
    uint32_t matrix_dirty;
    uint8_t materials[32][76];
    uint8_t lights[3][100];
    int vp_i[4];
    float vp_f[2];
    int ztest, zwrite;
    int blend_ok; GPU_BLENDEQUATION bl_eq; GPU_BLENDFACTOR bl_src, bl_dst;
    int scissor[4];
    int origin_x, origin_y;
    int in_overlay, discard, sprite_filter;
    int draws, last_draws, last_verts, frame_verts, mesh_draws, last_mesh_draws;
    int canvas_bound, flip_clear, clear_pending, drawn;
    int disp_w, disp_h;
    int present_mode;       /* R3DS_PRESENT_* */
    int in_frame;           /* between r3ds_frame_start and r3ds_frame_finish */
    int unit1_bound;        /* the fog ramp on texture unit 1 (fogged meshes) */
} R;

void r3ds_char_frame_finish(void); /* r_char_c3d.c: flush the tile pages written this frame */

/* ---------------- textures: tiled PICA layout ---------------- */
static uint8_t morton[64];

static void init_morton(void)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            morton[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                          ((x & 4) << 2) | ((y & 4) << 3));
}

/* Tiled PICA layout of texel (x, y), y counted in memory order.  The GPU
 * stores textures bottom row first: texture coordinate t samples memory row
 * (height - 1 - t * height), as render targets are written (window y = 0 is
 * the last row).  The renderer's v = 0 is the top of a picture, so picture
 * row r goes to memory row (height - 1 - r): see img_row(). */
static inline uint32_t tile_index(uint32_t x, uint32_t y, uint32_t pw)
{
    return (((y >> 3) * (pw >> 3) + (x >> 3)) << 6) | morton[((y & 7) << 3) | (x & 7)];
}

/* memory row of picture row r (top down) in a texture `ph` rows high */
static inline uint32_t img_row(int r, int ph) { return (uint32_t)(ph - 1 - r); }

static int pow2(int v)
{
    int p = 8;
    while (p < v && p < 1024) p <<= 1;
    return p;
}

static inline uint32_t pica_rgba8(const uint8_t *c) { return (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3]; }

static C3D_Tex *tex_new(int pw, int ph, GPU_TEXCOLOR fmt)
{
    C3D_Tex *t = (C3D_Tex *)malloc(sizeof *t);
    if (!t) return NULL;
    if (!C3D_TexInit(t, (u16)pw, (u16)ph, fmt)) { free(t); return NULL; }
    memset(t->data, 0, t->size);
    return t;
}

/* retire: freed when the GPU is done with the frame that may still use it */
static void tex_retire(C3D_Tex *t)
{
    if (!t) return;
    if (R.ndead == MAX_DEAD) { /* should not happen: keep it rather than free it early */
        return;
    }
    R.dead[R.ndead].t = t;
    R.dead[R.ndead].frame = R.frame;
    R.ndead++;
}

static void free_dead(void)
{
    int k = 0;
    for (int i = 0; i < R.ndead; i++) {
        if (R.dead[i].frame < R.frame) {
            C3D_TexDelete(R.dead[i].t);
            free(R.dead[i].t);
        } else {
            R.dead[k++] = R.dead[i];
        }
    }
    R.ndead = k;
}

void r3ds_tex_put_rgba8(C3D_Tex *t, int x0, int y0, int w, int h, const uint8_t *rgba, int stride)
{
    uint32_t *d = (uint32_t *)t->data;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * stride * 4;
        uint32_t my = img_row(y0 + y, t->height);
        for (int x = 0; x < w; x++, s += 4) d[tile_index((uint32_t)(x0 + x), my, t->width)] = pica_rgba8(s);
    }
}

/* indexed pixels through a palette of PICA RGBA8 words */
static void put_indexed(C3D_Tex *t, int x0, int y0, int w, int h, const uint8_t *idx, int stride, const uint32_t *pal)
{
    uint32_t *d = (uint32_t *)t->data;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = idx + (size_t)y * stride;
        uint32_t my = img_row(y0 + y, t->height);
        for (int x = 0; x < w; x++) d[tile_index((uint32_t)(x0 + x), my, t->width)] = pal[s[x]];
    }
}

/* ---------------- shader / pipeline ---------------- */
static void bind_2d_pipeline(void)
{
    C3D_BindProgram(&R.prog);
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);          /* x y z w */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);          /* u v */
    AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);  /* rgba */
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 1);          /* pal (unused by the shader) */
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_CullFace(GPU_CULL_NONE);
}

/* ---------------- init ---------------- */
int r_init(void)
{
    memset(&R, 0, sizeof R);
    init_morton();
    R.sprite_filter = -1;
    R.present_mode = R3DS_PRESENT_43;

    R.dvlb = DVLB_ParseFile((u32 *)r2d_shbin, r2d_shbin_size);
    if (!R.dvlb) return -1;
    shaderProgramInit(&R.prog);
    shaderProgramSetVsh(&R.prog, &R.dvlb->DVLE[0]);
    R.u_scale = shaderInstanceGetUniformLocation(R.prog.vertexShader, "scale");
    R.u_rotx = shaderInstanceGetUniformLocation(R.prog.vertexShader, "rotx");
    R.u_roty = shaderInstanceGetUniformLocation(R.prog.vertexShader, "roty");
    R.u_uvscale = shaderInstanceGetUniformLocation(R.prog.vertexShader, "uvscale");

    for (int k = 0; k < 2; k++) {
        R.mdvlb[k] = k == 0 ? DVLB_ParseFile((u32 *)mesh_shbin, mesh_shbin_size)
                            : DVLB_ParseFile((u32 *)mesh_nofog_shbin, mesh_nofog_shbin_size);
        if (!R.mdvlb[k]) return -1;
        shaderProgramInit(&R.mprog[k]);
        shaderProgramSetVsh(&R.mprog[k], &R.mdvlb[k]->DVLE[0]);
        shaderInstance_s *vs = R.mprog[k].vertexShader;
#define ML(n) R.mloc[k].n = shaderInstanceGetUniformLocation(vs, #n)
        ML(mvp); ML(vp); ML(misc); ML(cscale); ML(modc); ML(matsel); ML(sel); ML(fsel);
        ML(uvrow0); ML(uvrow1); ML(uvscale); ML(ldir); ML(lcol); ML(lamb); ML(cnorm);
#undef ML
    }

    /* the 640x480 canvas lives in the bottom-left of a 1024x512 texture */
    if (!C3D_TexInitVRAM(&R.canvas, 1024, 512, GPU_RGBA8)) return -1;
    /* C3D_DEPTHTYPE is a transparent union of int and the enum, which needs
     * int-sized enums; with devkitARM's short enums it is built by hand
     * (through __i, so C3D_DEPTHTYPE_OK's __i >= 0 holds) */
    C3D_DEPTHTYPE depth24 = { .__i = GPU_RB_DEPTH24_STENCIL8 };
    R.canvas_rt = C3D_RenderTargetCreateFromTex(&R.canvas, GPU_TEXFACE_2D, 0, depth24);
    if (!R.canvas_rt) return -1;
    R.top_rt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, depth24);
    if (!R.top_rt) return -1;
    C3D_RenderTargetSetOutput(R.top_rt, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    if (!C3D_TexInit(&R.fog_ramp, 256, 8, GPU_RGBA8)) return -1;
    {
        static uint8_t ramp[256 * 8 * 4];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 256; x++) {
                uint8_t *q = ramp + (y * 256 + x) * 4;
                q[0] = q[1] = q[2] = 255;
                q[3] = (uint8_t)x;
            }
        r3ds_tex_put_rgba8(&R.fog_ramp, 0, 0, 256, 8, ramp, 256);
        C3D_TexFlush(&R.fog_ramp);
        C3D_TexSetFilter(&R.fog_ramp, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&R.fog_ramp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    if (!C3D_TexInit(&R.white, 8, 8, GPU_RGBA8)) return -1;
    memset(R.white.data, 0xFF, R.white.size);
    C3D_TexFlush(&R.white);

    R.vring[0] = (uint8_t *)linearAlloc(VRING_BYTES);
    R.vring[1] = (uint8_t *)linearAlloc(VRING_BYTES);
    if (!R.vring[0] || !R.vring[1]) return -1;

    R.pal_next_row = 1; /* row 0 stays transparent black */
    R.cur_tex = ID_WHITE;
    R.vp_i[2] = FL_SCREEN_W; R.vp_i[3] = FL_SCREEN_H; R.vp_f[1] = 1.f;
    for (int i = 0x16; i < 0x39; i++)
        for (int k = 0; k < 16; k++) R.matrices[i][k] = (k % 5) == 0 ? 1.f : 0.f;
    R.global_mod = 0xFFFFFFFFu;
    { float big = 10000000.f; R.fog_a = big; R.fog_b = big; }
    fl_default_state();
    if (r_mesh_init()) return -1;
    return 0;
}

void r_shutdown(void)
{
    r_mesh_shutdown();
    for (int i = 0; i < MAX_TEX; i++)
        if (R.tex[i].used) fl_tex_release((uint32_t)(i + 1));
    R.frame += 2;
    free_dead();
}

/* ---------------- frame (the 3DS app) ---------------- */
void r3ds_frame_start(void)
{
    R.in_frame = 1;
    R.frame++;
    free_dead();
    gls_frame_start();
    R.vused = 0;
    R.target = TGT_NONE;
    R.canvas_bound = 0;
    C3D_RenderTargetClear(R.top_rt, C3D_CLEAR_ALL, 0x000000FF, 0);
}

void r3ds_frame_finish(void)
{
    r_flush();
    r3ds_char_frame_finish();
    if (R.vused) GSPGPU_FlushDataCache(R.vring[R.frame & 1], R.vused);
    R.in_frame = 0;
}

uint32_t r3ds_frame_number(void) { return R.frame; }
C3D_Tex *r3ds_canvas_tex(void) { return &R.canvas; } /* the self-test reads it back */
void r3ds_set_present_mode(int mode) { R.present_mode = mode; }
int r3ds_present_mode(void) { return R.present_mode; }
void r3ds_stats(int *draws, int *verts, int *mesh_draws, size_t *exp_bytes)
{
    if (draws) *draws = R.last_draws;
    if (verts) *verts = R.last_verts;
    if (mesh_draws) *mesh_draws = R.last_mesh_draws;
    if (exp_bytes) *exp_bytes = R.exp_bytes;
}

/* ---------------- draw targets ---------------- */
static void set_view(int w, int h, int tilt)
{
    R.view_w = w;
    R.view_h = h;
    R.tilt = tilt;
}

static void target_canvas(void)
{
    if (R.target != TGT_CANVAS) {
        C3D_FrameDrawOn(R.canvas_rt);
        C3D_SetViewport(0, 0, FL_SCREEN_W, FL_SCREEN_H);
        R.target = TGT_CANVAS;
    }
    set_view(FL_SCREEN_W, FL_SCREEN_H, 0);
}

static void target_top(void)
{
    if (R.target != TGT_TOP && R.in_frame) {
        C3D_FrameDrawOn(R.top_rt);
        R.target = TGT_TOP;
    }
    set_view(400, 240, 1);
}

/* the scissor in canvas pixels (y down) -> framebuffer (y up) */
static void apply_scissor(const int s[4])
{
    if (R.target != TGT_CANVAS || !s) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        return;
    }
    int x0 = s[0], x1 = s[0] + s[2];
    int y0 = FL_SCREEN_H - (s[1] + s[3]), y1 = FL_SCREEN_H - s[1];
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)x0, (u32)y0, (u32)x1, (u32)y1);
}

/* ---------------- emitting draws ---------------- */
typedef struct DrawMode {
    C3D_Tex *tex;          /* NULL = untextured */
    float su, sv;          /* uv scale */
    int linear, clamp;
    int blend;             /* 1 = the fl blend state, 0 = off, 2 = overlay alpha blend */
    int depth;             /* 1 = the fl depth state, 0 = off, 2 = always + write all, 3 = always + depth only */
    int alpha_test;
    int scissor;           /* 1 = the fl scissor */
    int scale_rgb2;        /* TEV x2 on rgb: the PS2's 128 = 1.0 (CPU-path meshes) */
    int scale_a2;          /* TEV x2 on alpha */
    int alpha_ref;         /* alpha test: alpha > ref */
} DrawMode;

static void emit_state(const DrawMode *m, const void *dst, int n);
static void apply_state(const DrawMode *m, int alpha_ref);

static void emit(const RVtx *v, int n, const DrawMode *m)
{
    size_t bytes = sizeof(RVtx) * (size_t)n;
    if (n <= 0 || !R.in_frame) return; /* e.g. the game draws during boot: no GPU frame is open */
    if (R.vused + bytes > VRING_BYTES) {
        if (!R.vring_full_logged) {
            fprintf(stderr, "[r3ds] vertex ring full (%u KB per frame): draws dropped\n", VRING_BYTES >> 10);
            R.vring_full_logged = 1;
        }
        return;
    }
    uint8_t *dst = R.vring[R.frame & 1] + R.vused;
    memcpy(dst, v, bytes);
    R.vused += (bytes + 15) & ~(size_t)15;
    emit_state(m, dst, n);
}

/* blend / depth / alpha test / scissor of a draw (2D and meshes) */
static void apply_state(const DrawMode *m, int alpha_ref)
{
    if (m->blend == 1 && R.blend_ok)
        C3D_AlphaBlend(R.bl_eq, R.bl_eq, R.bl_src, R.bl_dst, R.bl_src, R.bl_dst);
    else if (m->blend == 2)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    if (m->depth == 1) {
        GPU_TESTFUNC f;
        switch (R.ztest) {
        case 0: f = GPU_NEVER; break;
        case 3: f = GPU_GREATER; break;
        case 7: f = GPU_ALWAYS; break;
        default: f = GPU_GEQUAL; break;
        }
        C3D_DepthTest(true, f, R.zwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    } else if (m->depth == 2) {
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
    } else if (m->depth == 3) {
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
    } else {
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    }
    C3D_AlphaTest(m->alpha_test ? true : false, GPU_GREATER, alpha_ref);
    apply_scissor(m->scissor ? R.scissor : NULL);
}

/* per-frame vertex ring: n bytes (16-aligned) the GPU reads this frame */
static void *ring_alloc(size_t bytes)
{
    if (R.vused + bytes > VRING_BYTES) return NULL;
    void *p = R.vring[R.frame & 1] + R.vused;
    R.vused += (bytes + 15) & ~(size_t)15;
    return p;
}

static void emit_state(const DrawMode *m, const void *dst, int n)
{
    bind_2d_pipeline();
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, dst, sizeof(RVtx), 4, 0x3210);

    C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_scale, 2.f / (float)R.view_w, -2.f / (float)R.view_h, -1.f, 1.f);
    if (R.tilt) {
        /* Mtx_OrthoTilt: clip x = ndc y, clip y = -ndc x */
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_rotx, 0.f, 1.f, 0.f, 0.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_roty, -1.f, 0.f, 0.f, 0.f);
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_rotx, 1.f, 0.f, 0.f, 0.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_roty, 0.f, 1.f, 0.f, 0.f);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, R.u_uvscale, m->su, m->sv, 0.f, 0.f);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (m->tex) {
        GPU_TEXTURE_FILTER_PARAM f = m->linear ? GPU_LINEAR : GPU_NEAREST;
        GPU_TEXTURE_WRAP_PARAM w = m->clamp ? GPU_CLAMP_TO_EDGE : GPU_REPEAT;
        C3D_TexSetFilter(m->tex, f, f);
        C3D_TexSetWrap(m->tex, w, w);
        C3D_TexBind(0, m->tex);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
    if (m->scale_rgb2) C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
    if (m->scale_a2) C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    if (R.unit1_bound) { C3D_TexBind(1, &R.white) /* not NULL: citro3d dereferences it */; R.unit1_bound = 0; } /* the 2D shader feeds unit 0 only */

    apply_state(m, m->alpha_ref);

    C3D_DrawArrays(GPU_TRIANGLES, 0, n);
    R.draws++;
    R.frame_verts += n;
}

/* ---------------- palettes ---------------- */
int r_pal_alloc_rows(int n)
{
    if (R.pal_next_row + n > R_PAL_ROWS) return -1;
    int r = R.pal_next_row;
    R.pal_next_row += n;
    return r;
}

void r_pal_write(int row, int first, int n, const uint8_t *rgba)
{
    if (row < 0 || row >= R_PAL_ROWS || first < 0 || first >= 256) return;
    if (first + n > 256) n = 256 - first;
    r_flush(); /* batched draws keep the colours they were given */
    for (int k = 0; k < n; k++) R.pal_rgba[row][first + k] = pica_rgba8(rgba + 4 * k);
    R.pal_ver[row]++;
}

const uint32_t *r3ds_pal_row(int row, uint32_t *ver)
{
    if (row < 0 || row >= R_PAL_ROWS) row = 0;
    if (ver) *ver = R.pal_ver[row];
    return R.pal_rgba[row];
}

uint32_t fl_pal_create(int ncolors, const uint8_t *rgba)
{
    for (int i = 0; i < MAX_PAL; i++) {
        if (R.pal[i].used) continue;
        int row = r_pal_alloc_rows(1);
        if (row < 0) return 0;
        R.pal[i].used = 1;
        R.pal[i].row = row;
        R.pal[i].ncolors = ncolors;
        if (rgba) r_pal_write(row, 0, ncolors > 256 ? 256 : ncolors, rgba);
        return (uint32_t)(i + 1) << 16;
    }
    return 0;
}

void fl_pal_update(uint32_t handle, int first, int n, const uint8_t *rgba)
{
    int i = (int)(handle >> 16) - 1;
    if (i < 0 || i >= MAX_PAL || !R.pal[i].used) return;
    r_pal_write(R.pal[i].row, first, n, rgba);
}

int fl_pal_define(uint32_t handle, int ncolors, const uint8_t *rgba)
{
    int i = (int)(handle >> 16) - 1;
    if (i < 0 || i >= MAX_PAL) return -1;
    PalRec *pr = &R.pal[i];
    if (!pr->used) {
        int row = r_pal_alloc_rows(1);
        if (row < 0) return -1;
        pr->used = 1;
        pr->row = row;
    }
    pr->ncolors = ncolors;
    if (rgba) r_pal_write(pr->row, 0, ncolors > 256 ? 256 : ncolors, rgba);
    return 0;
}

/* ---------------- textures ---------------- */
static int is_indexed(int fmt) { return fmt == FL_TEX_IDX4 || fmt == FL_TEX_IDX8; }

static void exp_drop(Exp *e, int retire)
{
    if (!e->t) return;
    R.exp_bytes -= e->t->size;
    if (retire) tex_retire(e->t);
    else { C3D_TexDelete(e->t); free(e->t); }
    memset(e, 0, sizeof *e);
}

static void tex_clear(TexRec *t)
{
    for (int k = 0; k < EXP_PER_TEX; k++) exp_drop(&t->exp[k], 1);
    if (t->rgba) tex_retire(t->rgba);
    free(t->idx);
    memset(t, 0, sizeof *t);
}

static int tex_setup(TexRec *t, int w, int h, int fmt, const void *pixels)
{
    if (w > 1024) w = 1024;
    if (h > 1024) h = 1024;
    t->used = 1;
    t->w = w;
    t->h = h;
    t->fmt = fmt;
    t->pw = pow2(w);
    t->ph = pow2(h);
    t->ver = 1;
    if (is_indexed(fmt)) {
        t->idx = (uint8_t *)calloc((size_t)w * h, 1);
        if (!t->idx) return -1;
        if (pixels) memcpy(t->idx, pixels, (size_t)w * h);
    } else {
        t->rgba = tex_new(t->pw, t->ph, GPU_RGBA8);
        if (!t->rgba) return -1;
        if (pixels) r3ds_tex_put_rgba8(t->rgba, 0, 0, w, h, (const uint8_t *)pixels, w);
        C3D_TexFlush(t->rgba);
    }
    return 0;
}

uint32_t fl_tex_create(int w, int h, int fmt, const void *pixels)
{
    for (int i = 0; i < MAX_TEX; i++) {
        if (R.tex[i].used) continue;
        r_flush();
        if (tex_setup(&R.tex[i], w, h, fmt, pixels)) { tex_clear(&R.tex[i]); return 0; }
        return (uint32_t)(i + 1);
    }
    return 0;
}

int fl_tex_define(uint32_t handle, int w, int h, int fmt, const void *pixels)
{
    int i = (int)(handle & 0xFFFF) - 1;
    if (i < 0 || i >= MAX_TEX || w <= 0 || h <= 0) return -1;
    TexRec *t = &R.tex[i];
    r_flush();
    if (t->used && t->w == (w > 1024 ? 1024 : w) && t->h == (h > 1024 ? 1024 : h) && t->fmt == fmt) {
        fl_tex_update(handle, 0, 0, w, h, pixels);
        return 0;
    }
    if (R.cur_tex == (GLuint)(i + 1)) R.cur_tex = ID_WHITE;
    tex_clear(t);
    return tex_setup(t, w, h, fmt, pixels);
}

int fl_tex_defined(uint32_t handle)
{
    int i = (int)(handle & 0xFFFF) - 1;
    return i >= 0 && i < MAX_TEX && R.tex[i].used;
}

void fl_tex_update(uint32_t handle, int x, int y, int w, int h, const void *pixels)
{
    int i = (int)(handle & 0xFFFF) - 1;
    if (i < 0 || i >= MAX_TEX || !R.tex[i].used || !pixels) return;
    TexRec *t = &R.tex[i];
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h || w <= 0 || h <= 0) return;
    r_flush();
    if (is_indexed(t->fmt)) {
        for (int r = 0; r < h; r++)
            memcpy(t->idx + (size_t)(y + r) * t->w + x, (const uint8_t *)pixels + (size_t)r * w, (size_t)w);
        t->ver++; /* expansions are rebuilt on their next use */
        return;
    }
    if (t->used_frame == R.frame) {
        /* drawn with in this frame: write a copy, free the old one later */
        C3D_Tex *n = tex_new(t->pw, t->ph, GPU_RGBA8);
        if (!n) return;
        memcpy(n->data, t->rgba->data, n->size);
        tex_retire(t->rgba);
        t->rgba = n;
    }
    r3ds_tex_put_rgba8(t->rgba, x, y, w, h, (const uint8_t *)pixels, w);
    C3D_TexFlush(t->rgba);
    t->ver++;
}

void fl_tex_release(uint32_t handle)
{
    int i = (int)(handle & 0xFFFF) - 1, p = (int)(handle >> 16) - 1;
    r_flush();
    if (i >= 0 && i < MAX_TEX && R.tex[i].used) {
        if (R.cur_tex == (GLuint)(i + 1)) R.cur_tex = ID_WHITE;
        tex_clear(&R.tex[i]);
    }
    if (p >= 0 && p < MAX_PAL) R.pal[p].used = 0; /* the row is not reclaimed, as in r_gl.c */
}

int fl_tex_info(uint32_t handle, int *w, int *h, int *fmt)
{
    int i = (int)(handle & 0xFFFF) - 1;
    if (i < 0 || i >= MAX_TEX || !R.tex[i].used) return -1;
    if (w) *w = R.tex[i].w;
    if (h) *h = R.tex[i].h;
    if (fmt) *fmt = R.tex[i].fmt;
    return 0;
}

GLuint r_tex_gl(uint32_t handle, int *mode, int *pal_row)
{
    int i = (int)(handle & 0xFFFF) - 1, p = (int)(handle >> 16) - 1;
    if (i < 0 || i >= MAX_TEX || !R.tex[i].used) return 0;
    *mode = is_indexed(R.tex[i].fmt) ? RM_INDEX : RM_RGBA;
    *pal_row = (p >= 0 && p < MAX_PAL && R.pal[p].used) ? R.pal[p].row : 0;
    return (GLuint)(i + 1);
}

GLuint r_pal_texture(void) { return ID_PAL; }
GLuint r_white_texture(void) { return ID_WHITE; }

GLuint r3ds_ext_register(C3D_Tex *t)
{
    for (int i = 0; i < MAX_EXT; i++)
        if (!R.ext[i].t) { R.ext[i].t = t; return ID_EXT0 + (GLuint)i; }
    return 0;
}

void r3ds_ext_unregister(GLuint id)
{
    if (id >= ID_EXT0 && id < ID_EXT0 + MAX_EXT) R.ext[id - ID_EXT0].t = NULL;
}

/* keep expansions within EXP_BUDGET: drop the least recently used ones that
 * no draw of this frame uses */
static void exp_trim(size_t need)
{
    while (R.exp_bytes + need > EXP_BUDGET) {
        Exp *old = NULL;
        for (int i = 0; i < MAX_TEX; i++) {
            if (!R.tex[i].used || !R.tex[i].idx) continue;
            for (int k = 0; k < EXP_PER_TEX; k++) {
                Exp *e = &R.tex[i].exp[k];
                if (e->t && e->last != R.frame && (!old || e->last < old->last)) old = e;
            }
        }
        if (!old) return;
        exp_drop(old, 1);
    }
}

/* the RGBA8 expansion of indexed texture t with palette row `row` */
static C3D_Tex *expansion(TexRec *t, int row)
{
    uint32_t pv;
    const uint32_t *pal = r3ds_pal_row(row, &pv);
    Exp *hit = NULL, *slot = NULL;
    for (int k = 0; k < EXP_PER_TEX; k++) {
        Exp *e = &t->exp[k];
        if (e->t && e->row == row) { hit = e; break; }
    }
    if (hit && hit->pal_ver == pv && hit->tex_ver == t->ver) {
        hit->last = R.frame;
        return hit->t;
    }
    if (hit) {
        if (hit->last != R.frame) {
            /* not drawn this frame: rebuild in place */
            put_indexed(hit->t, 0, 0, t->w, t->h, t->idx, t->w, pal);
            C3D_TexFlush(hit->t);
            hit->pal_ver = pv;
            hit->tex_ver = t->ver;
            hit->last = R.frame;
            return hit->t;
        }
        exp_drop(hit, 1); /* this frame's draws keep the old one */
        slot = hit;
    }
    if (!slot) {
        for (int k = 0; k < EXP_PER_TEX && !slot; k++)
            if (!t->exp[k].t) slot = &t->exp[k];
    }
    if (!slot) {
        for (int k = 0; k < EXP_PER_TEX; k++) {
            Exp *e = &t->exp[k];
            if (e->last != R.frame && (!slot || e->last < slot->last)) slot = e;
        }
        if (!slot) return NULL; /* all rows of this texture drawn this frame */
        exp_drop(slot, 1);
    }
    exp_trim((size_t)t->pw * t->ph * 4);
    C3D_Tex *n = tex_new(t->pw, t->ph, GPU_RGBA8);
    if (!n) return NULL;
    put_indexed(n, 0, 0, t->w, t->h, t->idx, t->w, pal);
    C3D_TexFlush(n);
    slot->t = n;
    slot->row = row;
    slot->pal_ver = pv;
    slot->tex_ver = t->ver;
    slot->last = R.frame;
    R.exp_bytes += n->size;
    return n;
}

/* the C3D texture for an id (direct colour) or an indexed id + row */
static C3D_Tex *resolve(GLuint id, int row, float *su, float *sv)
{
    *su = *sv = 1.f;
    if (id == ID_WHITE || id == 0) return &R.white;
    if (id >= ID_EXT0 && id < ID_EXT0 + MAX_EXT) return R.ext[id - ID_EXT0].t;
    if (id < 1 || id > MAX_TEX) return NULL;
    TexRec *t = &R.tex[id - 1];
    if (!t->used) return NULL;
    t->used_frame = R.frame;
    *su = (float)t->w / (float)t->pw;
    *sv = (float)t->h / (float)t->ph;
    if (t->idx) return expansion(t, row);
    return t->rgba;
}

/* for r_mesh draws (phase B) */
C3D_Tex *r3ds_resolve(GLuint id, int row, float *su, float *sv) { return resolve(id, row, su, sv); }

/* ---------------- batching ---------------- */
int r_texture_filter_linear(void)
{
    if (R.sprite_filter >= 0 && !R.in_overlay) return R.sprite_filter;
    return (R.state_ad0 & 0x10000) ? 0 : 1;
}

void r_canvas_bind(void)
{
    if (R.in_overlay || !R.in_frame) return;
    target_canvas();
    if (R.clear_pending) {
        uint32_t c = R.clear_color;
        RVtx q[6];
        memset(q, 0, sizeof q);
        static const float xs[6] = { 0, FL_SCREEN_W, FL_SCREEN_W, 0, FL_SCREEN_W, 0 };
        static const float ys[6] = { 0, 0, FL_SCREEN_H, 0, FL_SCREEN_H, FL_SCREEN_H };
        for (int k = 0; k < 6; k++) {
            q[k].x = xs[k]; q[k].y = ys[k]; q[k].z = -1.f; q[k].w = 1.f; /* window depth 0 */
            q[k].c[0] = (uint8_t)(c >> 16); q[k].c[1] = (uint8_t)(c >> 8); q[k].c[2] = (uint8_t)c; q[k].c[3] = 255;
        }
        DrawMode m = { NULL, 1.f, 1.f, 0, 0, 0, 2, 0, 0, 0, 0, 0 };
        emit(q, 6, &m);
        R.clear_pending = 0;
        R.drawn = 0;
    }
    R.canvas_bound = 1;
    R.drawn = 1;
}

static void draw_batch(const RVtx *v, int n, GLuint tex, int mode)
{
    int linear = r_texture_filter_linear();
    int clamp = (R.state_ad0 & 0x20000) ? 1 : 0;
    DrawMode m = { NULL, 1.f, 1.f, linear, clamp, R.in_overlay ? 2 : 1, R.in_overlay ? 0 : 1, !R.in_overlay, !R.in_overlay, 0, 0, 0 };
    if (mode == RM_FLAT) {
        emit(v, n, &m);
        return;
    }
    if (mode == RM_INDEX) {
        /* runs of one palette row */
        int s = 0;
        while (s < n) {
            int e = s + 3;
            while (e + 3 <= n && v[e].pal == v[s].pal) e += 3;
            m.tex = resolve(tex, (int)v[s].pal, &m.su, &m.sv);
            if (m.tex) emit(v + s, e - s, &m);
            s = e;
        }
        return;
    }
    m.tex = resolve(tex, 0, &m.su, &m.sv);
    if (m.tex) emit(v, n, &m);
}

void r_flush(void)
{
    if (!R.nverts) return;
    if (R.discard && !R.in_overlay) { R.nverts = 0; return; }
    if (!R.in_overlay) r_canvas_bind();
    draw_batch(R.verts, R.nverts - R.nverts % 3, R.cur_tex, R.cur_mode);
    R.nverts = 0;
}

void r_use(GLuint tex, int mode)
{
    if (!tex) tex = ID_WHITE;
    if (tex != R.cur_tex || mode != R.cur_mode) {
        r_flush();
        R.cur_tex = tex;
        R.cur_mode = mode;
    }
}

RVtx *r_verts(int n)
{
    if (R.nverts + n > MAX_VERTS) r_flush();
    RVtx *v = &R.verts[R.nverts];
    R.nverts += n;
    memset(v, 0, sizeof(RVtx) * n);
    return v;
}

void fl_set_flip_clear(int on) { R.flip_clear = on != 0; }
void fl_frame_flip(void) { r_flush(); R.clear_pending = 1; }
void r_set_discard(int on) { r_flush(); R.discard = on != 0; }
int r_discarding(void) { return R.discard && !R.in_overlay; }
void r_set_sprite_filter(int f) { R.sprite_filter = f; }

static uint8_t gs_to_unorm(unsigned g) { unsigned v = (g * 255 + 64) / 128; return (uint8_t)(v > 255 ? 255 : v); }
static unsigned halve(unsigned t) { return t == 0xFF ? 0x80 : t ? ((t >> 1) ? (t >> 1) : 1) : 0; }

void r_color_tex(uint32_t c, uint8_t o[4])
{
    o[0] = gs_to_unorm(halve((c >> 16) & 0xFF));
    o[1] = gs_to_unorm(halve((c >> 8) & 0xFF));
    o[2] = gs_to_unorm(halve(c & 0xFF));
    o[3] = gs_to_unorm(halve(c >> 24));
}

void r_color_flat(uint32_t c, uint8_t o[4])
{
    o[0] = (uint8_t)(c >> 16);
    o[1] = (uint8_t)(c >> 8);
    o[2] = (uint8_t)c;
    o[3] = gs_to_unorm(halve(c >> 24));
}

float r_depth_of(float z)
{
    float d = -z;
    return d < -1.f ? -1.f : d > 1.f ? 1.f : d;
}
float r_snap_x(float x) { return floorf(x - (float)R.origin_x); }
float r_snap_y(float y) { return floorf(y - (float)R.origin_y); }

/* ---------------- blend: func_003E06D0 (as r_gl.c) ---------------- */
static void set_blend_from_state(void)
{
    uint32_t f = R.state_ad0;
    int src = f & 0xF, dst = (f >> 4) & 0xF;
    GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
    GPU_BLENDFACTOR sf = GPU_ONE, df = GPU_ZERO;
    int ok = 1;
#define B(e, s, d) do { eq = (e); sf = (s); df = (d); } while (0)
    switch (src) {
    case 1:
        switch (dst) {
        case 1:
            if ((f & 0xC00) == 0x400) B(GPU_BLEND_SUBTRACT, GPU_ONE, GPU_ONE);
            else B(GPU_BLEND_ADD, GPU_ONE, GPU_ONE);
            break;
        case 0: B(GPU_BLEND_ADD, GPU_ONE, GPU_ZERO); break;
        case 2: B(GPU_BLEND_ADD, GPU_ONE, GPU_SRC_ALPHA); break;
        case 3: B(GPU_BLEND_SUBTRACT, GPU_ONE, GPU_SRC_ALPHA); break;
        case 4: B(GPU_BLEND_ADD, GPU_ONE, GPU_DST_ALPHA); break;
        case 5: B(GPU_BLEND_SUBTRACT, GPU_ONE, GPU_DST_ALPHA); break;
        default: ok = 0;
        }
        break;
    case 0:
        switch (dst) {
        case 1: B(GPU_BLEND_ADD, GPU_ZERO, GPU_ONE); break;
        case 0: B(GPU_BLEND_ADD, GPU_ZERO, GPU_ZERO); break;
        case 2: B(GPU_BLEND_ADD, GPU_ZERO, GPU_SRC_ALPHA); break;
        case 3: B(GPU_BLEND_ADD, GPU_ZERO, GPU_ONE_MINUS_SRC_ALPHA); break;
        case 4: B(GPU_BLEND_ADD, GPU_ZERO, GPU_DST_ALPHA); break;
        case 5: B(GPU_BLEND_ADD, GPU_ZERO, GPU_ONE_MINUS_DST_ALPHA); break;
        default: ok = 0;
        }
        break;
    case 2:
        switch (dst) {
        case 1: B(GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE); break;
        case 0: B(GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ZERO); break;
        case 3: B(GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); break;
        default: ok = 0;
        }
        break;
    case 3:
        switch (dst) {
        case 0: B(GPU_BLEND_ADD, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO); break;
        case 2: B(GPU_BLEND_ADD, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA); break;
        default: ok = 0;
        }
        break;
    case 4:
        switch (dst) {
        case 1: B(GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ONE); break;
        case 0: B(GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ZERO); break;
        case 5: B(GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA); break;
        default: ok = 0;
        }
        break;
    case 5:
        switch (dst) {
        case 0: B(GPU_BLEND_ADD, GPU_ONE_MINUS_DST_ALPHA, GPU_ZERO); break;
        case 4: B(GPU_BLEND_ADD, GPU_ONE_MINUS_DST_ALPHA, GPU_DST_ALPHA); break;
        default: ok = 0;
        }
        break;
    default: ok = 0;
    }
#undef B
    if (!ok) return;
    if (R.blend_ok && eq == R.bl_eq && sf == R.bl_src && df == R.bl_dst) return;
    r_flush();
    R.blend_ok = 1;
    R.bl_eq = eq;
    R.bl_src = sf;
    R.bl_dst = df;
}

/* ---------------- func_003DF850 (as r_gl.c) ---------------- */
int fl_set_render_state(int id, uintptr_t val)
{
    uint32_t v = (uint32_t)val;
    if (id >= 0x19 && id < 0x39) {
        if (val) {
            memcpy(R.matrices[id], (const void *)val, 64);
            R.matrix_dirty |= 1u << (id - 0x19);
        } else {
            R.matrix_dirty &= ~(1u << (id - 0x19));
        }
        return 1;
    }
    if (id >= 0x39 && id < 0x59) {
        if (val) memcpy(R.materials[id - 0x39], (const void *)val, 76);
        return 1;
    }
    if (id >= 0x59 && id < 0x5C) {
        uint8_t *l = R.lights[id - 0x59];
        float d[3];
        if (val) memcpy(l, (const void *)val, 100);
        memcpy(d, l + 0x34, 12);
        float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (n > 0.f) { d[0] /= n; d[1] /= n; d[2] /= n; }
        memcpy(l + 0x34, d, 12);
        return 1;
    }
    switch (id) {
    case 0xE: R.material_color = v; break;
    case 0xF: R.fog_color = v; break;
    case 0x10: memcpy(&R.fog_a, &v, 4); break;
    case 0x11: memcpy(&R.fog_b, &v, 4); break;
    case 0x12: R.state_ad4 = (R.state_ad4 & 0xFFFE7FFF) | (v ? 0x8000 : 0); break;
    case 1:
        R.state_ad4 &= 0xFFFFFC7F;
        if (v == 1) R.state_ad4 |= 0x100;
        else if (v == 2) R.state_ad4 |= 0x80;
        break;
    case 2:
        R.state_ad4 &= 0xFFFFFFF3;
        if (v == 1) R.state_ad4 |= 4;
        else if (v == 2) R.state_ad4 |= 8;
        break;
    case 0xD:
        if ((R.state_ad0 & 0xC00) != v) {
            R.state_ad0 = (R.state_ad0 & 0xFFFFF3FF) | v;
            set_blend_from_state();
        }
        break;
    case 0x5D:
        if ((R.state_ad0 & 0xFF) != v) {
            R.state_ad0 = (R.state_ad0 & 0xFFFFFF00) | v;
            set_blend_from_state();
        }
        break;
    case 4: case 5: case 6: case 7:
        if (id == 4) R.cur_texture = v;
        R.tex_slots[id - 4] = v;
        break;
    case 0x14: fl_set_clear_color(v); break;
    case 0x16: memcpy(R.matrices[0x16], (const void *)val, 64); R.matrix_dirty = 0xFFFFFFFFu; break;
    case 0x17: memcpy(R.matrices[0x17], (const void *)val, 64); R.matrix_dirty = 0xFFFFFFFFu; break;
    case 0x18:
        if (val) {
            const int32_t *q = (const int32_t *)val;
            memcpy(R.vp_i, q, 16);
            memcpy(R.vp_f, q + 4, 8);
            fl_set_scissor(R.vp_i[0], R.vp_i[1], R.vp_i[2], R.vp_i[3]);
        }
        R.matrix_dirty = 0xFFFFFFFFu;
        break;
    case 0x15:
        R.state_ad4 &= 0xFFFFFFFC;
        if (v == 2) R.state_ad4 |= 2;
        break;
    case 0x5C: R.state_ad4 = (R.state_ad4 & 0xFFFFFF) | v; break;
    case 0x5F: R.state_5f = v; break;
    case 0x61: R.state_ad4 = (R.state_ad4 & 0xFFFFBFFF) | (v ? 0x4000 : 0); break;
    case 0x62:
        if (((R.state_ad0 & 0x10000) != 0) != (v == 0x10000)) r_flush();
        R.state_ad0 = (R.state_ad0 & 0xFFFEFFFF) | (v == 0x10000 ? 0x10000 : 0);
        break;
    case 0x63:
        if (((R.state_ad0 & 0x20000) != 0) != (v == 0x20000)) r_flush();
        R.state_ad0 = (R.state_ad0 & 0xFFF9FFFF) | (v == 0x20000 ? 0x20000 : 0);
        break;
    case 0x65: R.state_ad4 = (R.state_ad4 & 0xFFFDFFFF) | (v ? 0x20000 : 0); break;
    case 0x66: R.global_mod = v; break;
    case 0xC: R.state_ad4 = (R.state_ad4 & 0xFFFFFFBF) | (v == 1 ? 0x40 : 0); break;
    case 0x6C:
        if ((v == 0 || v == 1 || v == 3 || v == 7) && R.ztest != (int)v) {
            r_flush();
            R.ztest = (int)v;
        }
        /* fall through: the original switch has no break here */
    case 0x6B:
        if (R.zwrite != (v != 0)) r_flush();
        R.zwrite = v != 0;
        break;
    default: break;
    }
    return 1;
}

uintptr_t fl_get_render_state(int id)
{
    switch (id) {
    case 1:
        switch (R.state_ad4 & 0x380) { case 0x100: return 1; case 0x80: return 2; default: return 0; }
    case 0x15:
        switch (R.state_ad4 & 3) { case 0: return 1; case 2: return 2; default: return 0; }
    case 4: case 5: case 6: case 7: return R.tex_slots[id - 4];
    case 0x14: return R.clear_color;
    default:
        if (id >= 0x16 && id < 0x39) return (uintptr_t)R.matrices[id];
        if (id >= 0x39 && id < 0x59) return (uintptr_t)R.materials[id - 0x39];
        return 0;
    }
}

const float *r_matrix(int id) { return R.matrices[id]; }
void fl_sync_mode_words(uint32_t ad0, uint32_t ad4)
{
    if (ad0 == R.state_ad0 && ad4 == R.state_ad4) return;
    r_flush();
    R.state_ad0 = ad0;
    R.state_ad4 = ad4;
    set_blend_from_state();
}
uint32_t r_state_ad0(void) { return R.state_ad0; }
uint32_t r_state_ad4(void) { return R.state_ad4; }
uint32_t r_global_mod(void) { return R.global_mod; }
uint32_t r_material_color(void) { return R.material_color; }
uint32_t r_fog_color(void) { return R.fog_color; }
void r_fog_params(float *near_10, float *far_11) { *near_10 = R.fog_a; *far_11 = R.fog_b; }
void r_viewport(int vi[4], float vf[2]) { memcpy(vi, R.vp_i, 16); memcpy(vf, R.vp_f, 8); }
const uint8_t *r_material_slot(int i) { return R.materials[i]; }
const uint8_t *r_light(int i) { return R.lights[i]; }
void r_count_draw(int verts) { R.draws++; R.frame_verts += verts; }
void r_apply_state(void) { }  /* the 3DS emit() applies blend/depth per draw */

/* ---------------- meshes: r_mesh.c's glDrawElements ----------------
 * The uniforms r_mesh.c set (gls_uniform) drive mesh.v.pica; the fragment
 * part of its GL shader is the TEV:
 *   stage 0: texture * colour (x2: the PS2's 128 = 1.0), or the colour
 *            (rgb x1 from 255 units, alpha x2 from 128 units)
 *   stage 1: fog = mix(fog colour, colour, f), f from the fog ramp texture */
static int Ui(const char *name);
static const float *U(const char *name)
{
    static const float zero[16];
    const GlsUniform *u = gls_uniform(name);
    return u ? u->f : zero;
}
static int Ui(const char *name)
{
    const GlsUniform *u = gls_uniform(name);
    return u ? u->i : 0;
}

/* 3DS app switches and diagnostics */
static int mesh_off, mesh_logged, mesh_rejects, mesh_mode;
void r3ds_set_mesh_enabled(int on) { mesh_off = !on; }
/* R3DS_MESH_CPU (default): vertices transformed, lit and coloured on the CPU
 * and drawn through the 2D shader and pipeline.  The GPU paths (mesh.v.pica)
 * hung the console on the first mesh on hardware; the self-test bisects them. */
void r3ds_set_mesh_mode(int mode) { mesh_mode = mode; }
int r3ds_mesh_rejects(void) { return mesh_rejects; }

static void mesh_reject(const char *why, const GlsDraw *d, unsigned v)
{
    if (mesh_rejects++ < 20)
        fprintf(stderr, "[r3ds] mesh draw refused: %s (count %d, vbuf %p %u bytes, index type %x, value %u)\n",
                why, (int)d->count, (const void *)d->vbuf, (unsigned)d->vsize, (unsigned)d->itype, v);
}

/* The mesh shader done on the CPU (the same maths as mesh.v.pica / r_mesh.c's
 * k_mvs), drawn through the 2D shader: x/y in canvas pixels with w, so the
 * 2D shader's clip = (pixels -> NDC) * w is the mesh's own clip position and
 * the GPU clips and interpolates perspective-correctly as with the shader.
 * Colour in GS units goes out as bytes with the TEV x2 (128 = 1.0).  Fog is
 * applied for untextured meshes (to the colour); textured ones are drawn
 * without it for now (it needs a second colour the 2D shader does not have). */
typedef struct MeshCtx {
    const float *m, *vp, *cs, *md, *mc, *u0, *u1, *u3, *fp, *fc, *ld, *lc, *la;
    float vpz, lclamp;
    int lit, mat, uvx, fog_col;
} MeshCtx;

/* one vertex (pos 0..2, nrm 3..5, col 6..9, uv 10..11) -> RVtx */
static inline void mesh_xform(const MeshCtx *x, const float *V, RVtx *q)
{
    const float *m = x->m;
    float cx = V[0] * m[0] + V[1] * m[4] + V[2] * m[8] + m[12];
    float cy = V[0] * m[1] + V[1] * m[5] + V[2] * m[9] + m[13];
    float cz = V[0] * m[2] + V[1] * m[6] + V[2] * m[10] + m[14];
    float cw = V[0] * m[3] + V[1] * m[7] + V[2] * m[11] + m[15];
    float nx = cx * x->vp[0] + cw * x->vp[1], ny = cy * x->vp[2] + cw * x->vp[3], nz = cz * x->vpz;
    float w = cw;
    if (w < 1e-6f && w > -1e-6f) w = w < 0 ? -1e-6f : 1e-6f;
    float iw = 1.f / w;
    q->x = (nx * iw + 1.f) * (FL_SCREEN_W / 2.f);
    q->y = (1.f - ny * iw) * (FL_SCREEN_H / 2.f);
    q->z = nz * iw;
    q->w = w;
    float c[4];
    if (x->lit) {
        const float *ld = x->ld, *lc = x->lc, *la = x->la;
        float d0 = V[3] * ld[0] + V[4] * ld[1] + V[5] * ld[2];
        float d1 = V[3] * ld[4] + V[4] * ld[5] + V[5] * ld[6];
        float d2 = V[3] * ld[8] + V[4] * ld[9] + V[5] * ld[10];
        d0 = d0 > 0.f ? d0 : 0.f; d1 = d1 > 0.f ? d1 : 0.f; d2 = d2 > 0.f ? d2 : 0.f;
        for (int ch = 0; ch < 3; ch++) {
            float v = lc[ch] * d0 + lc[4 + ch] * d1 + lc[8 + ch] * d2 + la[ch];
            c[ch] = floorf((v < x->lclamp ? v : x->lclamp) * x->md[ch]);
        }
        c[3] = floorf(la[3] * x->md[3]);
    } else if (x->mat) {
        for (int ch = 0; ch < 4; ch++) c[ch] = x->mc[ch];
    } else {
        for (int ch = 0; ch < 4; ch++) c[ch] = floorf(V[6 + ch] * x->cs[ch] * x->md[ch]);
    }
    if (x->fog_col) {
        float f = (x->fp[0] - cw) * x->fp[1];
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        for (int ch = 0; ch < 3; ch++) c[ch] = x->fc[ch] * 255.f + (c[ch] - x->fc[ch] * 255.f) * f;
    }
    for (int ch = 0; ch < 4; ch++) {
        float v = c[ch];
        q->c[ch] = (uint8_t)(v <= 0.f ? 0 : v >= 255.f ? 255 : (int)v);
    }
    if (x->uvx) {
        q->u = x->u0[0] * V[10] + x->u1[0] * V[11] + x->u3[0];
        q->v = x->u0[1] * V[10] + x->u1[1] * V[11] + x->u3[1];
    } else {
        q->u = V[10];
        q->v = V[11];
    }
    q->pal = 0.f;
}

/* The mesh shader done on the CPU (the same maths as mesh.v.pica / r_mesh.c's
 * k_mvs), drawn through the 2D shader: x/y in canvas pixels with w, so the
 * 2D shader's clip = (pixels -> NDC) * w is the mesh's own clip position and
 * the GPU clips and interpolates perspective-correctly as with the shader.
 * Colour in GS units goes out as bytes with the TEV x2 (128 = 1.0).  Fog is
 * applied for untextured meshes (to the colour); textured ones are drawn
 * without it for now (it needs a second colour the 2D shader does not have).
 * Each vertex the indices use is transformed once (into `xv`), then the
 * triangle list is gathered from those. */
static RVtx *xv;
static size_t xv_cap;

static void mesh_draw_cpu(const GlsDraw *d, size_t isz, C3D_Tex *tex, float su, float sv,
                          int lit, int mat, int uvx, int fog, int textured)
{
    RVtx *o = (RVtx *)ring_alloc(sizeof(RVtx) * (size_t)d->count);
    if (!o) { mesh_reject("vertex ring full", d, 0); return; }
    const GlsUniform *um = gls_uniform("u_mvp");
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    MeshCtx x;
    x.m = um ? um->m : ident;
    x.vp = U("u_vp"); x.cs = U("u_cscale"); x.md = U("u_mod"); x.mc = U("u_matcol");
    x.u0 = U("u_uv0"); x.u1 = U("u_uv1"); x.u3 = U("u_uv3"); x.fp = U("u_fog"); x.fc = U("u_fogcol");
    x.ld = U("u_ldir"); x.lc = U("u_lcol"); x.la = U("u_lamb");
    x.vpz = U("u_vpz")[0];
    x.lclamp = U("u_lclamp")[0];
    x.lit = lit; x.mat = mat; x.uvx = uvx; x.fog_col = fog && !textured;

#define IDX(k) (isz == 4 ? ((const uint32_t *)d->indices)[k] : isz == 2 ? ((const uint16_t *)d->indices)[k] \
                                                                    : ((const uint8_t *)d->indices)[k])
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (int k = 0; k < d->count; k++) {
        uint32_t ix = IDX(k);
        if (ix < mn) mn = ix;
        if (ix > mx) mx = ix;
    }
    size_t nu = (size_t)(mx - mn + 1);
    if (nu < (size_t)d->count) {
        if (nu > xv_cap) {
            RVtx *n = (RVtx *)realloc(xv, nu * sizeof *n);
            if (!n) { mesh_reject("out of memory", d, (unsigned)nu); return; }
            xv = n;
            xv_cap = nu;
        }
        for (size_t i = 0; i < nu; i++) mesh_xform(&x, (const float *)(d->vbuf + (mn + i) * 48), &xv[i]);
        for (int k = 0; k < d->count; k++) o[k] = xv[IDX(k) - mn];
    } else {
        for (int k = 0; k < d->count; k++) mesh_xform(&x, (const float *)(d->vbuf + (size_t)IDX(k) * 48), &o[k]);
    }
#undef IDX
    int linear = Ui("u_linear"), clamp = Ui("u_clamp");
    /* textured: tex * c * 2 (c in GS units, 128 = 1.0); untextured: rgb c / 255, alpha c * 2 / 255 */
    DrawMode dm = { tex, su, sv, linear, clamp, 1, 1, 1, 1, textured, 1, 1 };
    emit_state(&dm, o, d->count);
}

void r3ds_mesh_draw(const GlsDraw *d)
{
    R.mesh_draws++;
    if (!R.in_frame || R.discard || d->count <= 0 || mesh_off) return;
    const GlsAttrib *a = d->attr;
    if (!a[0].enabled || a[0].stride != 48 || a[0].offset != 0) { mesh_reject("not an MVtx buffer", d, (unsigned)a[0].stride); return; }
    if (!d->vbuf || d->vsize < 48) { mesh_reject("no vertex buffer", d, 0); return; }
    /* every index inside the vertex buffer: a GPU read past it can hang the console */
    {
        size_t isz = d->itype == GL_UNSIGNED_INT ? 4 : d->itype == GL_UNSIGNED_SHORT ? 2 : 1;
        if ((size_t)d->count * isz > d->isize) { mesh_reject("index range past its buffer", d, (unsigned)d->isize); return; }
        uint32_t nv = (uint32_t)(d->vsize / 48), mx = 0;
        for (int k = 0; k < d->count; k++) {
            uint32_t ix = isz == 4 ? ((const uint32_t *)d->indices)[k] : isz == 2 ? ((const uint16_t *)d->indices)[k]
                                                                                   : ((const uint8_t *)d->indices)[k];
            if (ix > mx) mx = ix;
        }
        if (mx >= nv) { mesh_reject("index past the vertices", d, mx); return; }
        if (mesh_logged < 8) {
            mesh_logged++;
            fprintf(stderr, "[r3ds] mesh draw %d: %d indices (max %u) over %u vertices, vbuf %p, ib %p, flags %d\n",
                    mesh_logged, (int)d->count, (unsigned)mx, (unsigned)nv, (const void *)d->vbuf, d->indices, Ui("u_flags"));
        }
    }
    r_flush();
    r_canvas_bind();

    int flags = Ui("u_flags");
    int lit = (flags & 4) != 0, mat = !lit && (flags & 2) != 0, vtx = !lit && !mat;
    int uvx = (flags & 8) != 0, fog = (flags & 16) != 0, textured = (flags & 32) != 0;

    /* texture (palette row for indexed ones) */
    float su = 1.f, sv = 1.f;
    C3D_Tex *tex = NULL;
    if (textured) {
        int mode = Ui("u_mode"), row = (int)U("u_palrow")[0];
        tex = resolve(d->tex[0], mode == RM_INDEX ? row : 0, &su, &sv);
        if (!tex) tex = &R.white;
    }

    size_t isz = d->itype == GL_UNSIGNED_INT ? 4 : d->itype == GL_UNSIGNED_SHORT ? 2 : 1;
    if (mesh_mode == R3DS_MESH_CPU) {
        mesh_draw_cpu(d, isz, tex, su, sv, lit, mat, uvx, fog, textured);
        return;
    }
    int prog = mesh_mode == R3DS_MESH_GPU_NOFOG ? 1 : 0;
    if (prog) fog = 0;
    const MeshLoc *ml = &R.mloc[prog];

    /* vertices: expanded through the indices into the frame's ring (plain
     * triangle list), or the buffer itself with 8 / 16 bit indices */
    const void *idx = d->indices, *vsrc = d->vbuf;
    int itype = C3D_UNSIGNED_SHORT;
    if (mesh_mode != R3DS_MESH_GPU_INDEXED) {
        uint8_t *dst = (uint8_t *)ring_alloc((size_t)d->count * 48);
        if (!dst) { mesh_reject("vertex ring full", d, 0); return; }
        for (int k = 0; k < d->count; k++) {
            uint32_t ix = isz == 4 ? ((const uint32_t *)d->indices)[k] : isz == 2 ? ((const uint16_t *)d->indices)[k]
                                                                                   : ((const uint8_t *)d->indices)[k];
            memcpy(dst + (size_t)k * 48, d->vbuf + (size_t)ix * 48, 48);
        }
        vsrc = dst;
    } else if (d->itype == GL_UNSIGNED_BYTE) {
        itype = C3D_UNSIGNED_BYTE;
    } else if (d->itype == GL_UNSIGNED_INT) {
        uint16_t *s16 = (uint16_t *)ring_alloc(sizeof(uint16_t) * (size_t)d->count);
        if (!s16) return;
        const uint32_t *s32 = (const uint32_t *)d->indices;
        for (int k = 0; k < d->count; k++) {
            if (s32[k] > 0xFFFF) return; /* > 65536 vertices: not drawn */
            s16[k] = (uint16_t)s32[k];
        }
        idx = s16;
    }

    C3D_BindProgram(&R.mprog[prog]);
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);  /* position */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);  /* normal */
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 4);  /* colour (byte units) */
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);  /* uv */
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vsrc, 48, 4, 0x3210);
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_CullFace(GPU_CULL_NONE);

    const GlsUniform *um = gls_uniform("u_mvp");
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float *m = um ? um->m : ident;
    for (int j = 0; j < 4; j++) /* GLSL u_mvp * p with the row-major array: clip.j = sum_i p_i m[4i + j] */
        C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->mvp + j, m[j], m[4 + j], m[8 + j], m[12 + j]);
    const float *vp = U("u_vp"), *fogp = U("u_fog");
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->vp, vp[0], vp[1], vp[2], vp[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->misc, U("u_vpz")[0], U("u_lclamp")[0], fogp[0], fogp[1]);
    const float *cs = U("u_cscale"), *md = U("u_mod"), *mc = U("u_matcol");
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->cscale, cs[0], cs[1], cs[2], cs[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->modc, md[0], md[1], md[2], md[3]);
    if (mat) C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->matsel, mc[0], mc[1], mc[2], mc[3]);
    else C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->matsel, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->sel, lit ? 1.f : 0.f, 0.f, vtx ? 1.f : 0.f, uvx ? 1.f : 0.f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->fsel, fog ? 1.f : 0.f, fog ? 0.f : 1.f, 0.f, 0.f);
    const float *u0 = U("u_uv0"), *u1 = U("u_uv1"), *u3 = U("u_uv3");
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->uvrow0, u0[0], u0[1], u1[0], u1[1]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->uvrow1, u3[0], u3[1], 0.f, 0.f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->uvscale, su, sv, 0.f, 0.f);
    const float *ld = U("u_ldir"), *lc = U("u_lcol"), *la = U("u_lamb");
    for (int k = 0; k < 3; k++) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->ldir + k, ld[4 * k], ld[4 * k + 1], ld[4 * k + 2], 0.f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->lcol + k, lc[4 * k], lc[4 * k + 1], lc[4 * k + 2], 0.f);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->lamb, la[0], la[1], la[2], la[3]);
    if (textured) C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->cnorm, 1.f / 256, 1.f / 256, 1.f / 256, 1.f / 256);
    else C3D_FVUnifSet(GPU_VERTEX_SHADER, ml->cnorm, 1.f / 255, 1.f / 255, 1.f / 255, 1.f / 256);

    C3D_TexEnv *e0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e0);
    if (tex) {
        int linear = Ui("u_linear"), clamp = Ui("u_clamp");
        GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
        GPU_TEXTURE_WRAP_PARAM w = clamp ? GPU_CLAMP_TO_EDGE : GPU_REPEAT;
        C3D_TexSetFilter(tex, f, f);
        C3D_TexSetWrap(tex, w, w);
        C3D_TexBind(0, tex);
        C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_MODULATE);
        C3D_TexEnvScale(e0, C3D_RGB, GPU_TEVSCALE_2);
    } else {
        C3D_TexEnvSrc(e0, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
    }
    C3D_TexEnvScale(e0, C3D_Alpha, GPU_TEVSCALE_2);
    C3D_TexEnv *e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e1);
    if (fog) {
        const float *fc = U("u_fogcol");
        unsigned r = (unsigned)(fc[0] * 255.f + .5f), g = (unsigned)(fc[1] * 255.f + .5f), b = (unsigned)(fc[2] * 255.f + .5f);
        C3D_TexBind(1, &R.fog_ramp);
        R.unit1_bound = 1;
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE1);
        C3D_TexEnvOpRgb(e1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(e1, (r & 255) | (g & 255) << 8 | (b & 255) << 16 | 0xFF000000u);
    }
    for (int i = 2; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    if (!fog && R.unit1_bound) { C3D_TexBind(1, &R.white) /* not NULL: citro3d dereferences it */; R.unit1_bound = 0; }

    DrawMode dm = { tex, su, sv, 0, 0, 1, 1, 1, 1, 0, 0, 0 };
    apply_state(&dm, 1); /* GS TEST: alpha * 128 >= 1 */
    if (mesh_mode == R3DS_MESH_GPU_INDEXED) C3D_DrawElements(GPU_TRIANGLES, d->count, itype, idx);
    else C3D_DrawArrays(GPU_TRIANGLES, 0, d->count);
    R.draws++;
    R.frame_verts += d->count;
}

void fl_default_state(void)
{
    r_flush();
    R.state_ad4 = 0x102;
    R.state_ad0 = 0x38B032;
    R.blend_ok = 0;
    set_blend_from_state();
    R.ztest = -1;
    fl_set_render_state(0x6C, 1);
    fl_set_render_state(0x62, 0);
    fl_set_scissor(0, 0, FL_SCREEN_W, FL_SCREEN_H);
}

void fl_set_scissor(int x, int y, int w, int h)
{
    if (x < 0) x = 0;
    if (x + w > FL_SCREEN_W) w = FL_SCREEN_W - x;
    if (y < 0) y = 0;
    if (y + h > FL_SCREEN_H) h = FL_SCREEN_H - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    r_flush();
    R.scissor[0] = x; R.scissor[1] = y; R.scissor[2] = w; R.scissor[3] = h;
}

void fl_set_clear_color(uint32_t argb)
{
    if (R.flip_clear && argb != R.clear_color && R.canvas_bound && !R.drawn && !R.discard) R.clear_pending = 1;
    R.clear_color = argb;
}
void fl_set_origin(int x, int y) { R.origin_x = x; R.origin_y = y; }

void fl_frame_begin(void)
{
    R.last_draws = R.draws;
    R.last_verts = R.frame_verts;
    R.last_mesh_draws = R.mesh_draws;
    R.draws = 0;
    R.frame_verts = 0;
    R.mesh_draws = 0;
    r_flush();
    if (!R.flip_clear) R.clear_pending = 1;
    R.canvas_bound = 0;
    if (!R.discard) r_canvas_bind();
    fl_set_render_state(0x5D, 0x32);
    fl_set_render_state(0x62, 0x10000);
    fl_set_scissor(R.scissor[0], R.scissor[1], R.scissor[2], R.scissor[3]);
}

void fl_frame_end(void) { r_flush(); }
void r_stats(int *d, int *v) { if (d) *d = R.last_draws; if (v) *v = R.last_verts; }

/* ---------------- display / present ---------------- */
void fl_set_display(int w, int h)
{
    if (w <= 0 || w > FL_SCREEN_W) w = FL_SCREEN_W;
    if (h <= 0 || h > FL_SCREEN_H) h = FL_SCREEN_H;
    R.disp_w = w;
    R.disp_h = h;
}

void fl_get_display(int *w, int *h)
{
    if (w) *w = R.disp_w > 0 ? R.disp_w : FL_SCREEN_W;
    if (h) *h = R.disp_h > 0 ? R.disp_h : FL_SCREEN_H;
}

void r_present_rect(int W, int H, const RPresent *p, int *x, int *y, int *w, int *h)
{
    (void)p;
    if (R.present_mode == R3DS_PRESENT_STRETCH) {
        *x = 0; *y = 0; *w = W; *h = H;
        return;
    }
    /* the PS2 picture is 4:3 whatever its pixel size */
    int ch = H, cw = H * 4 / 3;
    if (cw > W) { cw = W; ch = W * 3 / 4; }
    *x = (W - cw) / 2;
    *y = (H - ch) / 2;
    *w = cw;
    *h = ch;
}

void r3ds_present(void)
{
    r_flush();
    R.canvas_bound = 0;
    target_top();
    int x, y, w, h, DW, DH;
    r_present_rect(400, 240, NULL, &x, &y, &w, &h);
    fl_get_display(&DW, &DH);
    /* canvas rows 0..DH-1 (top down) are texture rows 480-DH..479 from the bottom */
    float u0 = 0.f, u1 = (float)DW / 1024.f;
    float t_top = (float)FL_SCREEN_H / 512.f, t_bot = (float)(FL_SCREEN_H - DH) / 512.f;
    float xs[4] = { (float)x, (float)(x + w), (float)(x + w), (float)x };
    float ys[4] = { (float)y, (float)y, (float)(y + h), (float)(y + h) };
    float us[4] = { u0, u1, u1, u0 }, vs[4] = { t_top, t_top, t_bot, t_bot };
    static const int ord[6] = { 0, 1, 2, 0, 2, 3 };
    RVtx q[6];
    memset(q, 0, sizeof q);
    for (int k = 0; k < 6; k++) {
        int i = ord[k];
        q[k].x = xs[i]; q[k].y = ys[i]; q[k].z = 1.f; q[k].w = 1.f;
        q[k].u = us[i]; q[k].v = vs[i];
        q[k].c[0] = q[k].c[1] = q[k].c[2] = q[k].c[3] = 255;
    }
    DrawMode m = { &R.canvas, 1.f, 1.f, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    emit(q, 6, &m);
}

void r_present_ex(int W, int H, const RPresent *p) { (void)W; (void)H; (void)p; r3ds_present(); }
void r_present(int W, int H, int mode) { (void)W; (void)H; (void)mode; r3ds_present(); }
void r_canvas_rect(int W, int H, int mode, int *x, int *y, int *w, int *h) { (void)mode; r_present_rect(W, H, NULL, x, y, w, h); }
int r_read_window(int W, int H, uint8_t *rgba) { (void)W; (void)H; (void)rgba; return -1; }
int r_read_canvas(uint8_t *rgba) { (void)rgba; return -1; }

/* ---------------- TIM2 ---------------- */
uint32_t fl_tex_load_tim2(const uint8_t *data, size_t size, int pic)
{
    Tim2Pic tp;
    if (tim2_decode(data, size, pic, &tp)) { tim2_free(&tp); return 0; }
    uint32_t h = 0;
    if (tp.index) {
        h = fl_tex_create(tp.w, tp.h, tp.image_type == 4 ? FL_TEX_IDX4 : FL_TEX_IDX8, tp.index);
        if (h) h |= fl_pal_create(tp.clut_colors ? tp.clut_colors : 16, tp.clut);
    } else {
        h = fl_tex_create(tp.w, tp.h, tp.image_type == 1 ? FL_TEX_RGBA16 : tp.image_type == 2 ? FL_TEX_RGBA24 : FL_TEX_RGBA32, tp.rgba);
    }
    tim2_free(&tp);
    return h;
}

/* ---------------- 2D primitives (as r_gl.c) ---------------- */
static int bind_current(int *pal_row)
{
    int mode;
    GLuint id = r_tex_gl(R.cur_texture, &mode, pal_row);
    if (!id) return 0;
    r_use(id, mode);
    return 1;
}

static void put(RVtx *o, const FlVec4 *v, const float *uv, const uint8_t c[4], int pal)
{
    o->x = r_snap_x(v->x);
    o->y = r_snap_y(v->y);
    o->z = r_depth_of(v->z);
    o->w = (v->w > 0.f) ? 1.f / v->w : 1.f;
    if (uv) { o->u = uv[0]; o->v = uv[1]; }
    memcpy(o->c, c, 4);
    o->pal = (float)pal;
}

int fl_tri_tex(const FlTri *t)
{
    int pal;
    if (!bind_current(&pal)) return 0;
    RVtx *o = r_verts(3);
    for (int k = 0; k < 3; k++) {
        uint8_t c[4];
        r_color_tex(t->argb[k], c);
        put(&o[k], &t->v[k], t->uv[k], c, pal);
    }
    return 1;
}

int fl_tri(const FlTri *t)
{
    r_use(0, RM_FLAT);
    RVtx *o = r_verts(3);
    for (int k = 0; k < 3; k++) {
        uint8_t c[4];
        r_color_flat(t->argb[k], c);
        FlVec4 v = t->v[k];
        v.w = 1.f;
        put(&o[k], &v, NULL, c, 0);
    }
    return 1;
}

int fl_sprite_tex(const FlSprite *s)
{
    int pal;
    if (!bind_current(&pal)) return 0;
    uint8_t c[4];
    r_color_tex(s->argb, c);
    FlVec4 a = s->v[0], b = s->v[1];
    a.z = b.z;
    float k = (a.w != 0.f) ? b.w / a.w : 1.f;
    float uv0[2] = { s->uv[0][0], s->uv[0][1] }, uv1[2] = { s->uv[1][0] * k, s->uv[1][1] * k };
    FlVec4 tr = { b.x, a.y, b.z, 1.f }, bl = { a.x, b.y, b.z, 1.f };
    float uv_tr[2] = { uv1[0], uv0[1] }, uv_bl[2] = { uv0[0], uv1[1] };
    RVtx *o = r_verts(6);
    put(&o[0], &a, uv0, c, pal);
    put(&o[1], &tr, uv_tr, c, pal);
    put(&o[2], &b, uv1, c, pal);
    put(&o[3], &a, uv0, c, pal);
    put(&o[4], &b, uv1, c, pal);
    put(&o[5], &bl, uv_bl, c, pal);
    for (int i = 0; i < 6; i++) o[i].w = 1.f;
    return 1;
}

int fl_quad_tex(const FlQuad *q)
{
    int pal;
    if (!bind_current(&pal)) return 0;
    const FlVec4 *v[4] = { &q->t[0].v[0], &q->t[0].v[1], &q->t[0].v[2], &q->t[1].v[2] };
    const float *uv[4] = { q->t[0].uv[0], q->t[0].uv[1], q->t[0].uv[2], q->t[1].uv[2] };
    uint32_t col[4] = { q->t[0].argb[0], q->t[0].argb[1], q->t[0].argb[2], q->t[1].argb[2] };
    static const int strip[6] = { 0, 1, 2, 1, 3, 2 };
    RVtx *o = r_verts(6);
    for (int k = 0; k < 6; k++) {
        uint8_t c[4];
        r_color_tex(col[strip[k]], c);
        put(&o[k], v[strip[k]], uv[strip[k]], c, pal);
    }
    return 1;
}

int fl_fill_screen(int mode, uint32_t c)
{
    /* func_003ED1C0: mode 0 writes only Z = 0, mode 1 colour (A = 0) and Z;
     * the scissor applies (a quad here, see the file comment) */
    if (mode != 0 && mode != 1) return 0;
    r_flush();
    if (R.discard) return 0;
    r_canvas_bind();
    RVtx q[6];
    memset(q, 0, sizeof q);
    static const float xs[6] = { 0, FL_SCREEN_W, FL_SCREEN_W, 0, FL_SCREEN_W, 0 };
    static const float ys[6] = { 0, 0, FL_SCREEN_H, 0, FL_SCREEN_H, FL_SCREEN_H };
    for (int k = 0; k < 6; k++) {
        q[k].x = xs[k]; q[k].y = ys[k]; q[k].z = -1.f; q[k].w = 1.f;
        q[k].c[0] = (uint8_t)(c >> 16); q[k].c[1] = (uint8_t)(c >> 8); q[k].c[2] = (uint8_t)c; q[k].c[3] = 0;
    }
    DrawMode m = { NULL, 1.f, 1.f, 0, 0, 0, mode == 1 ? 2 : 3, 0, 1, 0, 0, 0 };
    emit(q, 6, &m);
    return 0;
}

/* ---------------- overlay (top screen pixels) ---------------- */
void r_overlay_begin(int w, int h)
{
    r_flush();
    R.in_overlay = 1;
    R.canvas_bound = 0;
    target_top();
    set_view(w, h, 1);
    r_use(0, RM_FLAT);
}

static void ov_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb)
{
    RVtx *o = r_verts(3);
    float xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    for (int k = 0; k < 3; k++) {
        o[k].x = xs[k]; o[k].y = ys[k]; o[k].z = 0; o[k].w = 1;
        o[k].c[0] = (uint8_t)(argb >> 16); o[k].c[1] = (uint8_t)(argb >> 8);
        o[k].c[2] = (uint8_t)argb; o[k].c[3] = (uint8_t)(argb >> 24);
    }
}

void r_overlay_disc(float cx, float cy, float r, uint32_t argb)
{
    r_use(0, RM_FLAT);
    const int n = 24;
    for (int i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        ov_tri(cx, cy, cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), argb);
    }
}

void r_overlay_ring(float cx, float cy, float r, float wd, uint32_t argb)
{
    r_use(0, RM_FLAT);
    const int n = 32;
    float ri = r - wd;
    for (int i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        ov_tri(cx + r * c0, cy + r * s0, cx + r * c1, cy + r * s1, cx + ri * c0, cy + ri * s0, argb);
        ov_tri(cx + ri * c0, cy + ri * s0, cx + r * c1, cy + r * s1, cx + ri * c1, cy + ri * s1, argb);
    }
}

void r_overlay_rect(float x, float y, float w, float h, uint32_t argb)
{
    r_use(0, RM_FLAT);
    ov_tri(x, y, x + w, y, x + w, y + h, argb);
    ov_tri(x, y, x + w, y + h, x, y + h, argb);
}

void r_overlay_end(void)
{
    r_flush();
    R.in_overlay = 0;
}
