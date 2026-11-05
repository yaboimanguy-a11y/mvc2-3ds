/*
 * Self-checking test of the 3DS renderer (port/render/3ds), no game data.
 *
 * Draws a known scene through the fl API into the 640x480 canvas, presents
 * it, waits for the GPU, then reads the pixels back - from the canvas
 * texture in VRAM and from the top screen's frame buffer - and checks them
 * against what the GL renderer would show.  Results (PASS / FAIL, and for
 * the layout questions which hypothesis the hardware matches) go to the
 * bottom screen and to mvc2_rtest_log.txt next to the app.
 *
 * Scene, in canvas pixels (y down), clear colour dark blue (0, 0, 64):
 *   red     flat quad   (0, 0)-(160, 120)        top left
 *   green   flat quad   (480, 0)-(640, 120)      top right
 *   blue    flat quad   (0, 328)-(160, 448)      bottom left
 *   white   flat quad   (480, 328)-(640, 448)    bottom right
 *   RGBA texture sprite (240, 40)-(400, 200): texture top half yellow, bottom half magenta
 *   IDX8 texture sprite (240, 240)-(400, 400): 2x2 palette quadrants red green / blue white
 *   scissor (0, 200)-(100, 260), orange quad (0, 150)-(200, 300): orange only inside
 *   depth: green quad near (z -0.5) then red quad far (z 0.5) at (480, 200)-(560, 280): green
 *   blend: 50% white over red at (40, 40)-(120, 80) (the per-frame blend 0x32 = alpha)
 *   meshes, through the GL shim exactly as r_mesh.c draws them:
 *     vertex colour red  (560, 140)-(640, 200)
 *     fully fogged green (560, 210)-(640, 270)   (fog colour green, f = 0)
 *     textured           (560, 280)-(640, 320)   (the yellow / magenta texture, x2 modulate)
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../render/render.h"
#include "../../render/3ds/r_c3d.h"
#include "../../render/r_internal.h"   /* RM_*, r_tex_gl, the GL shim */

u32 __ctru_linear_heap_size = 24u << 20;

static FILE *test_log;
static int fails, passes;

static void out(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (test_log) { fputs(buf, test_log); fflush(test_log); }
}

/* ---- drawing helpers (game conventions: argb, GS alpha 0x80 = opaque) ---- */
static void flat_quad(float x0, float y0, float x1, float y1, float z, uint32_t argb)
{
    FlTri t;
    memset(&t, 0, sizeof t);
    float xs[3] = { x0, x1, x1 }, ys[3] = { y0, y0, y1 };
    for (int k = 0; k < 3; k++) { t.v[k].x = xs[k]; t.v[k].y = ys[k]; t.v[k].z = z; t.v[k].w = 1; t.argb[k] = argb; }
    fl_tri(&t);
    float xs2[3] = { x0, x1, x0 }, ys2[3] = { y0, y1, y1 };
    for (int k = 0; k < 3; k++) { t.v[k].x = xs2[k]; t.v[k].y = ys2[k]; }
    fl_tri(&t);
}

static void sprite(uint32_t tex, float x0, float y0, float x1, float y1)
{
    FlSprite s;
    memset(&s, 0, sizeof s);
    s.v[0].x = x0; s.v[0].y = y0; s.v[0].w = 1;
    s.v[1].x = x1; s.v[1].y = y1; s.v[1].w = 1;
    s.argb = 0xFFFFFFFFu; /* textured: halved -> 0x80 = 1.0 */
    s.uv[0][0] = 0; s.uv[0][1] = 0; s.uv[1][0] = 1; s.uv[1][1] = 1;
    fl_set_render_state(4, tex);
    fl_sprite_tex(&s);
}

static uint32_t tex_rgba, tex_idx;

static void make_textures(void)
{
    static uint8_t rgba[64 * 64 * 4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint8_t *p = rgba + (y * 64 + x) * 4;
            if (y < 32) { p[0] = 255; p[1] = 255; p[2] = 0; }   /* yellow */
            else { p[0] = 255; p[1] = 0; p[2] = 255; }          /* magenta */
            p[3] = 255;
        }
    tex_rgba = fl_tex_create(64, 64, FL_TEX_RGBA32, rgba);
    static uint8_t idx[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) idx[y * 32 + x] = (uint8_t)((x / 16) + 2 * (y / 16));
    static const uint8_t pal[4 * 4] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    tex_idx = fl_tex_create(32, 32, FL_TEX_IDX8, idx) | fl_pal_create(4, pal);
}

/* ---- meshes through the GL shim (r_mesh.c's path) ---- */
typedef struct MV { float pos[3], nrm[3], col[4], uv[2]; } MV;
static GLuint mesh_vao, mesh_vbo, mesh_ebo;
static GLint L(const char *n) { return glGetUniformLocation(1, n); }

static void make_meshes(void)
{
    static MV v[12];
    static uint16_t ix[18];
    const float rects[3][4] = { { 560, 140, 640, 200 }, { 560, 210, 640, 270 }, { 560, 280, 640, 320 } };
    for (int q = 0; q < 3; q++) {
        float x0 = rects[q][0] / 320.f - 1.f, x1 = rects[q][2] / 320.f - 1.f;
        float y0 = 1.f - rects[q][1] / 240.f, y1 = 1.f - rects[q][3] / 240.f;
        float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 }, us[4] = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
        for (int k = 0; k < 4; k++) {
            MV *m = &v[q * 4 + k];
            memset(m, 0, sizeof *m);
            m->pos[0] = xs[k]; m->pos[1] = ys[k];
            /* byte units: red, red (fogged away), white (textured: the texture shows) */
            m->col[0] = 255; m->col[1] = q == 2 ? 255.f : 0.f; m->col[2] = q == 2 ? 255.f : 0.f; m->col[3] = 255;
            m->uv[0] = us[k]; m->uv[1] = vs[k];
        }
        static const int t[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) ix[q * 6 + k] = (uint16_t)(q * 4 + t[k]);
    }
    glGenVertexArrays(1, &mesh_vao);
    glBindVertexArray(mesh_vao);
    glGenBuffers(1, &mesh_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MV), (void *)0);
    glGenBuffers(1, &mesh_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ix, ix, GL_STATIC_DRAW);
    glBindVertexArray(0);
}

/* q: 0 vertex colour, 1 fogged (f = 0), 2 textured */
static void draw_mesh(int q, uint32_t tex)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    glUniformMatrix4fv(L("u_mvp"), 1, GL_FALSE, ident);
    { float vp[4] = { 1, 0, 1, 0 }; glUniform4fv(L("u_vp"), 1, vp); }
    glUniform1f(L("u_vpz"), 1.f);
    glUniform1i(L("u_flags"), 1 | (q == 1 ? 16 : 0) | (q == 2 ? 32 : 0));
    { float cs[4] = { q == 2 ? 0.5f : 1.f, q == 2 ? 0.5f : 1.f, q == 2 ? 0.5f : 1.f, 0.5f }; glUniform4fv(L("u_cscale"), 1, cs); }
    glUniform1f(L("u_lclamp"), 128.f);
    { float one[4] = { 1, 1, 1, 1 }; glUniform4fv(L("u_mod"), 1, one); }
    { float fog[2] = { 1.f, 1.f }; glUniform2fv(L("u_fog"), 1, fog); }    /* far 1, w 1: f = 0 */
    { float fc[3] = { 0.f, 1.f, 0.f }; glUniform3fv(L("u_fogcol"), 1, fc); }
    glUniform1i(L("u_mode"), RM_RGBA);
    glUniform1f(L("u_palrow"), 0.f);
    glUniform1i(L("u_linear"), 0);
    glUniform1i(L("u_clamp"), 1);
    if (q == 2) {
        /* the vertex colour for the textured one: white (127 GS units = 1.0 after x2) */
        int mode, row;
        glBindTexture(GL_TEXTURE_2D, r_tex_gl(tex, &mode, &row));
    } else {
        glBindTexture(GL_TEXTURE_2D, r_white_texture());
    }
    glBindVertexArray(mesh_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)(uintptr_t)(q * 6 * 2));
    glBindVertexArray(0);
}

static void draw_scene(int meshes)
{
    fl_set_clear_color(0x000040);
    fl_frame_begin();                 /* blend 0x32 (alpha), nearest */
    flat_quad(0, 0, 160, 120, 0, 0xFFFF0000u);
    flat_quad(480, 0, 640, 120, 0, 0xFF00FF00u);
    flat_quad(0, 328, 160, 448, 0, 0xFF0000FFu);
    flat_quad(480, 328, 640, 448, 0, 0xFFFFFFFFu);
    sprite(tex_rgba, 240, 40, 400, 200);
    sprite(tex_idx, 240, 240, 400, 400);
    fl_set_scissor(0, 200, 100, 60);
    flat_quad(0, 150, 200, 300, 0, 0xFFFF8000u);
    fl_set_scissor(0, 0, 640, 480);
    flat_quad(480, 200, 560, 280, -0.5f, 0xFF00FF00u); /* near */
    flat_quad(480, 200, 560, 280, 0.5f, 0xFFFF0000u);  /* far: hidden */
    flat_quad(40, 40, 120, 80, 0, 0x80FFFFFFu);        /* 50% white over the red */
    fl_frame_end();
    if (meshes) {
        draw_mesh(0, 0);
        draw_mesh(1, 0);
        draw_mesh(2, tex_rgba);
    }
    fl_frame_flip();
}

/* ---- read back ---- */
static uint8_t mort[64];
static uint32_t canvas_texel(const C3D_Tex *t, int x, int y)
{
    uint32_t at = ((((uint32_t)y >> 3) * (t->width >> 3) + ((uint32_t)x >> 3)) << 6) | mort[((y & 7) << 3) | (x & 7)];
    return ((const uint32_t *)t->data)[at];
}

static int near_rgb(int r, int g, int b, int er, int eg, int eb)
{
    return abs(r - er) <= 24 && abs(g - eg) <= 24 && abs(b - eb) <= 24;
}

static void check(const char *what, int ok, int r, int g, int b)
{
    out("%s %s (got %d %d %d)\n", ok ? "PASS" : "FAIL", what, r, g, b);
    if (ok) passes++; else fails++;
}

extern C3D_Tex *r3ds_canvas_tex(void);

static void run_phase(int phase, const char *what, int frames, int meshes)
{
    out("phase %d: %s, %d frames ...\n", phase, what, frames);
    for (int f = 0; f < frames && aptMainLoop(); f++) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        r3ds_frame_start();
        draw_scene(meshes);
        r3ds_present();
        r3ds_frame_finish();
        C3D_FrameEnd(0);
        if (test_log) fflush(test_log);
    }
    C3D_FrameSync();
    for (int i = 0; i < 6; i++) gspWaitForVBlank();
    out("phase %d done (the GPU finished it)\n", phase);
}

int main(int argc, char **argv)
{
    char path[300] = "sdmc:/3ds/mvc2/mvc2_rtest_log.txt";
    if (argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6)) {
        const char *slash = strrchr(argv[0], '/');
        if (slash && slash - argv[0] < 200)
            snprintf(path, sizeof path, "%.*s/mvc2_rtest_log.txt", (int)(slash - argv[0]), argv[0]);
    }
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, false);  /* read back the one top frame buffer */
    consoleInit(GFX_BOTTOM, NULL);
    test_log = fopen(path, "w");
    out("MvC2 3DS renderer self-test\n");
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            mort[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4) || r_init() != 0) {
        out("FAIL init (citro3d / r_init)\n");
        goto done;
    }
    fl_set_flip_clear(1);
    fl_set_display(640, 448);
    make_textures();
    make_meshes();

    /* phases, the log saved every frame: if the console hangs, the last
     * line says in which phase.  1: 2D only.  2: + meshes on the CPU path
     * (what the game uses) - the pixel checks run after it.  Then, after
     * the checks, the GPU mesh shader paths that hung the game, safest
     * first, to find which part hangs: 3 without its fog output, 4 the
     * full shader, 5 the full shader with indexed draws. */
    run_phase(1, "2D only", 10, 0);
    r3ds_set_mesh_mode(R3DS_MESH_CPU);
    run_phase(2, "2D + meshes, CPU vertex path (the game's)", 30, 1);

    {
        /* 1. canvas layout: where does canvas (x, y) land in the texture? */
        C3D_Tex *cv = r3ds_canvas_tex();
        /* expected: 32+y.  The viewport covers window rows 0..479 (y up) of the
         * 512-row target, and the GPU writes window row w to memory row 511 - w
         * (bottom-up, like textures), so canvas row y (down) is memory row 32 + y */
        static const char *hyp[4] = { "479-y", "y", "511-y", "32+y (expected: bottom-up rows)" };
        int found = -1;
        for (int h = 0; h < 4; h++) {
            int yy[4] = { 479 - 10, 10, 511 - 10, 32 + 10 };
            uint32_t w = canvas_texel(cv, 10, yy[h]);
            int r = w >> 24, g = (w >> 16) & 255, b = (w >> 8) & 255;
            out("  canvas (10,10) as texel row %-36s = %3d %3d %3d\n", hyp[h], r, g, b);
            if (found < 0 && near_rgb(r, g, b, 255, 0, 0)) found = h;
        }
        out("%s canvas layout: %s\n", found == 3 ? "PASS" : "FAIL", found >= 0 ? hyp[found] : "no hypothesis matched");
        if (found == 3) passes++; else fails++;
        int (*row)(int) = NULL;
        (void)row;
        int hsel = found >= 0 ? found : 3;
#define CV(x, y, R, G, B, what) do { \
            int yy_[4] = { 479 - (y), (y), 511 - (y), 32 + (y) }; \
            uint32_t w_ = canvas_texel(cv, (x), yy_[hsel]); \
            int r_ = w_ >> 24, g_ = (w_ >> 16) & 255, b_ = (w_ >> 8) & 255; \
            check(what, near_rgb(r_, g_, b_, R, G, B), r_, g_, b_); } while (0)
        CV(10, 10, 255, 0, 0, "canvas red top-left (channel order)");
        CV(630, 10, 0, 255, 0, "canvas green top-right (x direction)");
        CV(10, 440, 0, 0, 255, "canvas blue bottom-left");
        CV(630, 440, 255, 255, 255, "canvas white bottom-right");
        CV(320, 200, 0, 0, 64, "canvas clear colour between quads");
        CV(320, 60, 255, 255, 0, "RGBA texture top half yellow (texture rows)");
        CV(320, 180, 255, 0, 255, "RGBA texture bottom half magenta");
        CV(270, 270, 255, 0, 0, "indexed texture top-left red (palette expansion)");
        CV(370, 270, 0, 255, 0, "indexed texture top-right green");
        CV(270, 370, 0, 0, 255, "indexed texture bottom-left blue");
        CV(370, 370, 255, 255, 255, "indexed texture bottom-right white");
        CV(50, 230, 255, 128, 0, "scissor: orange inside");
        CV(150, 230, 0, 0, 64, "scissor: clear colour outside (x)");
        CV(50, 170, 0, 0, 64, "scissor: clear colour outside (y)");
        CV(520, 240, 0, 255, 0, "depth: near green kept over far red");
        CV(80, 60, 255, 128, 128, "blend: 50% white over red");
        CV(600, 170, 255, 0, 0, "mesh: vertex colour red (transform, viewport, colour scale)");
        CV(600, 240, 0, 255, 0, "mesh: fogged to the fog colour (fog ramp, TEV stage 1)");
        CV(600, 288, 255, 255, 0, "mesh: textured top yellow (x2 modulate, uv)");
        CV(600, 312, 255, 0, 255, "mesh: textured bottom magenta");

        /* 2. the top screen: (x, y) landscape, y down, BGR8, x * 240 + (239 - y) */
        u16 fw, fh;
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
        out("  top frame buffer %ux%u\n", fw, fh);
#define TOP(x, y, R, G, B, what) do { \
            const u8 *p_ = fb + ((size_t)(x) * 240 + (239 - (y))) * 3; \
            check(what, near_rgb(p_[2], p_[1], p_[0], R, G, B), p_[2], p_[1], p_[0]); } while (0)
        TOP(60, 20, 255, 0, 0, "screen red top-left (4:3 at x 40..360)");
        TOP(340, 20, 0, 255, 0, "screen green top-right");
        TOP(60, 225, 0, 0, 255, "screen blue bottom-left");
        TOP(340, 225, 255, 255, 255, "screen white bottom-right");
        TOP(20, 120, 0, 0, 0, "screen black left bar");
        TOP(200, 30, 255, 255, 0, "screen yellow texture top");
        {
            /* the four orientations, if the corners failed */
            const u8 *p = fb + ((size_t)60 * 240 + (239 - 20)) * 3;
            const u8 *q = fb + ((size_t)(399 - 60) * 240 + 20) * 3;
            out("  screen (60,20)=%d %d %d  mirrored (339,219)=%d %d %d\n", p[2], p[1], p[0], q[2], q[1], q[0]);
        }
    }
    out("\n%d passed, %d failed so far.\n", passes, fails);
    out("\nGPU mesh shader bisection (a hang here ends the test; the last line says where):\n");
    r3ds_set_mesh_mode(R3DS_MESH_GPU_NOFOG);
    run_phase(3, "GPU mesh shader WITHOUT the fog output", 10, 1);
    out("RESULT phase 3: GPU mesh shader without fog output works\n");
    r3ds_set_mesh_mode(R3DS_MESH_GPU);
    run_phase(4, "GPU mesh shader, full", 10, 1);
    out("RESULT phase 4: full GPU mesh shader works\n");
    r3ds_set_mesh_mode(R3DS_MESH_GPU_INDEXED);
    run_phase(5, "GPU mesh shader, indexed draws", 10, 1);
    out("RESULT phase 5: indexed draws work\n");
    r3ds_set_mesh_mode(R3DS_MESH_CPU);
done:
    out("\n%d passed, %d failed. Log: %s\n", passes, fails, path);
    out("The top screen should show: red / green / blue / white corners,\n"
        "yellow over magenta and a 4-colour square in the middle.\nSTART: exit\n");
    if (test_log) fclose(test_log);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    r_shutdown();
    C3D_Fini();
    gfxExit();
    return 0;
}
