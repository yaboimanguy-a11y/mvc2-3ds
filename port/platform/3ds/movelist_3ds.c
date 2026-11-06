/*
 * The 3DS app's bottom screen (citro2d): the move list of player 1's
 * characters, with every input drawn as what to press on the 3DS.
 *
 * movelist.txt (next to the app; port/tools/make_movelist.py) has one block
 * per character id.  The page follows player 1's point character during a
 * fight (the game's pointer at D_004C2794 + 0x18, read by func_00327C20) and
 * otherwise shows the team (player records 0, 2, 4 at D_004F20B0, stride
 * 0x5B8; byte 1 = character id, as port/host/arcade.c forces them).
 *
 * Inputs: motions become arrows (QCF = down, down-forward, forward), the
 * buttons 3DS badges with the game's default BUTTON CONFIG (USER_GUIDE.md):
 * Y light punch, X heavy punch, B light kick, A heavy kick, L / R assists;
 * "P" (any punch) is Y/X, "PP" (both) Y+X, and so on.
 *
 * Touch: header = previous / next team member, list = next page, footer
 * left = frame skip, footer right = 4:3 / stretch.  The screen is drawn only
 * when something changed (movelist_render between the frame's draws and
 * C3D_FrameEnd), so it costs nothing most frames.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../host/host.h"
#include "../../host/host_loop.h"
#include "movelist_3ds.h"
#include "update_3ds.h"

#define MAX_ID 0x3B
#define W 320.f
#define HEAD_H 32.f
#define FOOT_Y 214.f
#define LIST_Y (HEAD_H + 4.f)
#define LIST_BOTTOM 204.f
#define ROW_H 18.f
#define Z 0.5f

typedef struct Move { char *name, *input; } Move;
typedef struct Entry { char *name; Move *moves; int n; } Entry;

static Entry tab[MAX_ID];
static int loaded;
static int shown = -1, page, npages, team_pos, manual;
static int team[3], nteam, point = -1;
static char status_line[64];
static int st_skip = 1, st_stretch;
static int dirty = 1, ui_ok;
static int settings_open;            /* the settings screen instead of the list */
static int upd_state;                /* UPD_* of update_3ds.h */
static int st_logging, st_unlock = 1, st_unlock_active = 1;
static char upd_line[96];
static OnlineView ov;
static int ov_on;
static C3D_RenderTarget *bottom;
static C2D_TextBuf tbuf;
static int page_first[64];   /* first move of each page */

/* ---------------- colours ---------------- */
#define RGB(r, g, b) C2D_Color32(r, g, b, 255)
#define RGBA(r, g, b, a) C2D_Color32(r, g, b, a)
static u32 c_bg0, c_bg1, c_white, c_dim, c_gold, c_cyan, c_row, c_head0, c_head1;
static u32 c_btn_y, c_btn_x, c_btn_b, c_btn_a, c_shoulder;

static void colours(void)
{
    c_bg0 = RGB(18, 22, 44);
    c_bg1 = RGB(8, 10, 24);
    c_white = RGB(240, 240, 248);
    c_dim = RGB(150, 156, 178);
    c_gold = RGB(255, 204, 64);
    c_cyan = RGB(110, 200, 255);
    c_row = RGBA(255, 255, 255, 14);
    c_head0 = RGB(120, 36, 160);
    c_head1 = RGB(30, 70, 180);
    c_btn_y = RGB(58, 160, 90);    /* New 3DS button colours */
    c_btn_x = RGB(60, 110, 214);
    c_btn_b = RGB(232, 178, 58);
    c_btn_a = RGB(214, 70, 62);
    c_shoulder = RGB(96, 100, 120);
}

/* ---------------- data ---------------- */
static char *dup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

int movelist_load(const char *dir)
{
    char path[300], line[256];
    snprintf(path, sizeof path, "%s/movelist.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    Entry *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        size_t l = strcspn(line, "\r\n");
        line[l] = 0;
        if (!l || line[0] == '#') continue;
        if (line[0] == '@') {
            char *e;
            long id = strtol(line + 1, &e, 16);
            cur = NULL;
            if (id < 0 || id >= MAX_ID) continue;
            cur = &tab[id];
            while (*e == ' ') e++;
            free(cur->name);
            cur->name = dup(e, strlen(e));
            continue;
        }
        if (!cur) continue;
        char *bar = strchr(line, '|');
        Move *n = (Move *)realloc(cur->moves, sizeof(Move) * (size_t)(cur->n + 1));
        if (!n) break;
        cur->moves = n;
        n[cur->n].name = dup(line, bar ? (size_t)(bar - line) : l);
        n[cur->n].input = dup(bar ? bar + 1 : "", bar ? strlen(bar + 1) : 0);
        cur->n++;
    }
    fclose(f);
    loaded = 1;
    return 0;
}

static int valid_id(int id) { return id >= 0 && id < MAX_ID && !(id >= 0x18 && id <= 0x1A) && tab[id].name; }

/* ---------------- the game ---------------- */
#define PLAYERS ((u8 *)EE_BSS_PTR(0x004F20B0))
#define PSTRIDE 0x5B8

static int point_id(void)
{
    /* only in a fight (game state 2 x 2..): elsewhere the pointer may be stale */
    int st = mvc2_game_state();
    if ((st >> 16) != 2 || (st & 0xFF) < 2) return -1;
    u8 *battle = EE_BSS(u8 *, 0x004C2794);
    if (!battle || ((uintptr_t)battle & 3)) return -1;
    u8 *p = *(u8 **)(battle + 0x18);
    if (p < PLAYERS || p >= PLAYERS + 6 * PSTRIDE || (p - PLAYERS) % PSTRIDE) return -1;
    return p[1];
}

static void read_team(void)
{
    nteam = 0;
    for (int t = 0; t < 3; t++) {
        int id = PLAYERS[t * 2 * PSTRIDE + 1];
        if (valid_id(id)) team[nteam++] = id;
    }
}

/* ---------------- drawing primitives ---------------- */
static float text_w(const char *s, float sc)
{
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    return w;
}

static float text(const char *s, float x, float y, float sc, u32 col, int right)
{
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    C2D_DrawText(&t, C2D_WithColor, right ? x - w : x, y, Z, sc, sc, col);
    return w;
}

/* an arrow in one of 8 directions (dx, dy in -1..1), centred at (cx, cy) */
static void arrow(float cx, float cy, int dx, int dy, u32 col)
{
    float len = sqrtf((float)(dx * dx + dy * dy));
    float ux = dx / len, uy = dy / len, px = -uy, py = ux, s = 5.0f;
    float tipx = cx + ux * s, tipy = cy + uy * s;
    float bx = cx + ux * (s - 4.5f), by = cy + uy * (s - 4.5f);
    C2D_DrawLine(cx - ux * s, cy - uy * s, col, bx, by, col, 2.0f, Z);
    C2D_DrawTriangle(tipx, tipy, col, bx + px * 3.6f, by + py * 3.6f, col, bx - px * 3.6f, by - py * 3.6f, col, Z);
}

static void badge(float cx, float cy, const char *letter, u32 fill, int shoulder)
{
    if (shoulder)
        C2D_DrawRectSolid(cx - 7.5f, cy - 5.5f, Z, 15.f, 11.f, fill);
    else
        C2D_DrawCircleSolid(cx, cy, Z, 6.5f, fill);
    float w = text_w(letter, 0.40f);
    text(letter, cx - w / 2, cy - 7.0f, 0.40f, c_white, 0);
}

/* ---------------- inputs: notation -> what to press ----------------
 * One pass both measures (draw = 0) and draws, so the layout is exact. */
typedef struct { float x, y; int draw; } Pen;

static void put_arrow(Pen *p, int dx, int dy)
{
    if (p->draw) arrow(p->x + 6.f, p->y, dx, dy, c_white);
    p->x += 12.f;
}

static void put_text(Pen *p, const char *s, float sc, u32 col)
{
    float w = p->draw ? text(s, p->x, p->y - 7.5f, sc, col, 0) : text_w(s, sc);
    p->x += w + 2.f;
}

static void put_badge(Pen *p, int which)
{
    static const char *L[] = { "Y", "X", "B", "A", "L", "R" };
    u32 fill = which == 0 ? c_btn_y : which == 1 ? c_btn_x : which == 2 ? c_btn_b : which == 3 ? c_btn_a : c_shoulder;
    if (p->draw) badge(p->x + 7.f, p->y, L[which], fill, which >= 4);
    p->x += 15.f;
}

static void put_sep(Pen *p, const char *s) { put_text(p, s, 0.40f, c_dim); }

static int is(const char *w, size_t n, const char *k) { return strlen(k) == n && !strncmp(w, k, n); }

static void put_dir(Pen *p, char d)
{
    switch (d) {
    case 'U': put_arrow(p, 0, -1); break;
    case 'D': put_arrow(p, 0, 1); break;
    case 'F': put_arrow(p, 1, 0); break;
    case 'B': put_arrow(p, -1, 0); break;
    }
}

/* one word of the notation; 0 = not notation (drawn as a dim note) */
static int put_word(Pen *p, const char *w, size_t n)
{
    static const struct { const char *k; const char *seq; } mot[] = {
        { "QCF", "D,DF,F" }, { "QCB", "D,DB,B" }, { "DP", "F,D,DF" }, { "RDP", "B,D,DB" },
        { "HCF", "B,DB,D,DF,F" }, { "HCB", "F,DF,D,DB,B" }, { "U~F", "U,UF,F" }, { "F~D", "F,DF,D" },
        { "D", "D" }, { "U", "U" }, { "F", "F" }, { "B", "B" }, { "DF", "DF" }, { "DB", "DB" },
        { "UF", "UF" }, { "UB", "UB" },
    };
    for (unsigned i = 0; i < sizeof mot / sizeof mot[0]; i++) {
        if (!is(w, n, mot[i].k)) continue;
        for (const char *s = mot[i].seq; *s; ) {
            int dx = 0, dy = 0;
            while (*s && *s != ',') {
                if (*s == 'U') dy = -1;
                else if (*s == 'D') dy = 1;
                else if (*s == 'F') dx = 1;
                else if (*s == 'B') dx = -1;
                s++;
            }
            put_arrow(p, dx, dy);
            if (*s == ',') s++;
        }
        return 1;
    }
    if (is(w, n, "360")) {
        if (p->draw) {
            float cx = p->x + 7.f, cy = p->y;
            for (int k = 0; k < 10; k++) {
                float a0 = k * 0.6f, a1 = a0 + 0.45f;
                C2D_DrawLine(cx + 5.5f * cosf(a0), cy + 5.5f * sinf(a0), c_white,
                             cx + 5.5f * cosf(a1), cy + 5.5f * sinf(a1), c_white, 1.6f, Z);
            }
        }
        p->x += 15.f;
        return 1;
    }
    /* buttons: 0 Y LP, 1 X HP, 2 B LK, 3 A HK, 4 L A1, 5 R A2 */
    if (is(w, n, "LP")) { put_badge(p, 0); return 1; }
    if (is(w, n, "HP")) { put_badge(p, 1); return 1; }
    if (is(w, n, "LK")) { put_badge(p, 2); return 1; }
    if (is(w, n, "HK")) { put_badge(p, 3); return 1; }
    if (is(w, n, "P")) { put_badge(p, 0); put_sep(p, "/"); put_badge(p, 1); return 1; }
    if (is(w, n, "K")) { put_badge(p, 2); put_sep(p, "/"); put_badge(p, 3); return 1; }
    if (is(w, n, "PP")) { put_badge(p, 0); put_sep(p, "+"); put_badge(p, 1); return 1; }
    if (is(w, n, "KK")) { put_badge(p, 2); put_sep(p, "+"); put_badge(p, 3); return 1; }
    if (is(w, n, "A1")) { put_badge(p, 4); return 1; }
    if (is(w, n, "A2")) { put_badge(p, 5); return 1; }
    return 0;
}

/* an input string from movelist.txt; returns the width */
static float put_input(Pen *p, const char *s)
{
    float x0 = p->x;
    if (s[0] == 'j' && s[1] == '.') {
        put_text(p, "air", 0.38f, c_cyan);
        s += 2;
    }
    while (*s) {
        if (*s == ' ') { p->x += 2.f; s++; continue; }
        if (*s == '+') { put_sep(p, "+"); s++; continue; }
        if (*s == ',') { put_sep(p, ","); s++; continue; }
        if (*s == '/') { put_sep(p, "or"); s++; continue; }
        if (*s == '*') { put_text(p, "(air ok)", 0.36f, c_cyan); s++; continue; }
        if (s[0] == '[' && (s[1] == 'B' || s[1] == 'D') && s[2] == ']') { /* charge */
            put_text(p, "hold", 0.38f, c_dim);
            put_dir(p, s[1]);
            put_text(p, "then", 0.38f, c_dim);
            s += 3;
            continue;
        }
        const char *e = s;
        while (*e && *e != ' ' && *e != '+' && *e != ',' && *e != '/' && *e != '*') e++;
        if (e == s) { s++; continue; }
        if (!put_word(p, s, (size_t)(e - s))) {
            char buf[48];
            size_t n = (size_t)(e - s) < sizeof buf - 1 ? (size_t)(e - s) : sizeof buf - 1;
            memcpy(buf, s, n);
            buf[n] = 0;
            put_text(p, buf, 0.38f, c_dim);
        }
        s = e;
    }
    return p->x - x0;
}

/* ---------------- layout ---------------- */
static int is_hyper(const Move *m)
{
    const char *n = m->name, *i = m->input;
    if (strstr(n, "Level 3")) return 1;
    if (strstr(n, "Dash") || strstr(n, "Flying") || strstr(n, "Flight") || strstr(n, "Teleport") ||
        strstr(n, "Floating") || strstr(n, "Backstep") || strstr(n, "Coffin"))
        return 0;
    return strstr(i, "PP") || strstr(i, "KK") || strstr(i, "A1+A2");
}

static const float name_scale = 0.46f;

/* row height of move m: one line, or two when the input does not fit beside the name */
static float row_h(const Move *m)
{
    if (m->name[0] == '-' && m->name[1] == '-') return ROW_H;
    Pen p = { 0, 0, 0 };
    float iw = put_input(&p, m->input), nw = text_w(m->name, name_scale);
    return nw + iw + 20.f <= W ? ROW_H : 2 * ROW_H - 2.f;
}

static void paginate(const Entry *e)
{
    float y = LIST_Y;
    npages = 1;
    page_first[0] = 0;
    for (int k = 0; k < e->n; k++) {
        float h = row_h(&e->moves[k]);
        if (y + h > LIST_BOTTOM && k > page_first[npages - 1]) {
            if (npages < 64) page_first[npages++] = k;
            y = LIST_Y;
        }
        y += h;
    }
    if (page >= npages) page = 0;
}

static void draw_header(const char *title, int with_team)
{
    C2D_DrawRectangle(0, 0, Z, W, HEAD_H, c_head0, c_head1, c_head0, c_head1);
    C2D_DrawRectSolid(0, HEAD_H - 2, Z, W, 2, RGBA(255, 255, 255, 60));
    text(title, with_team ? 24.f : 10.f, 5.f, 0.68f, c_white, 0);
    if (with_team) {
        u32 a = RGBA(255, 255, 255, 200);
        C2D_DrawTriangle(6, 16, a, 14, 9, a, 14, 23, a, Z);
        C2D_DrawTriangle(W - 6, 16, a, W - 14, 9, a, W - 14, 23, a, Z);
    }
}

/* a cog: teeth round a disc with a hole */
static void draw_gear(float cx, float cy, float r, u32 col, u32 hole)
{
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.785398f;
        C2D_DrawCircleSolid(cx + cosf(a) * r * 0.86f, cy + sinf(a) * r * 0.86f, Z, r * 0.27f, col);
    }
    C2D_DrawCircleSolid(cx, cy, Z, r * 0.78f, col);
    C2D_DrawCircleSolid(cx, cy, Z, r * 0.32f, hole);
}

#define GEAR_W 40.f   /* the settings button: the footer's left end */

static void draw_footer(void)
{
    u32 bar = RGB(14, 10, 26);
    C2D_DrawRectSolid(0, FOOT_Y, Z, W, 240 - FOOT_Y, bar);
    C2D_DrawRectSolid(0, FOOT_Y, Z, W, 1, RGBA(255, 255, 255, 40));
    C2D_DrawRectSolid(4, FOOT_Y + 3, Z, GEAR_W - 8, 240 - FOOT_Y - 6, settings_open ? RGB(120, 36, 160) : RGB(46, 44, 72));
    draw_gear(GEAR_W / 2, FOOT_Y + 13, 8.f, c_white, settings_open ? RGB(120, 36, 160) : RGB(46, 44, 72));
    const char *hint = settings_open ? "tap the cog to go back" : "settings";
    text(hint, GEAR_W + 2, FOOT_Y + 8, 0.36f, c_dim, 0);
    if (status_line[0]) text(status_line, W - 8, FOOT_Y + 8, 0.40f, c_white, 1);
}

/* ---------------- settings screen ---------------- */
#define SET_Y0 (HEAD_H + 6.f)
#define SET_ROW 31.f
#define SET_H 27.f
enum { SET_SKIP, SET_PICTURE, SET_LOG, SET_UNLOCK, SET_UPDATE, SET_N };

static void set_row(int i, const char *label, const char *value, u32 vcol)
{
    float y = SET_Y0 + i * SET_ROW;
    C2D_DrawRectSolid(8, y, Z, W - 16, SET_H, RGBA(255, 255, 255, 22));
    text(label, 18, y + 4, 0.48f, c_white, 0);
    if (value) {
        float vw = text_w(value, 0.44f) + 16;
        C2D_DrawRectSolid(W - 16 - vw, y + 3, Z, vw, SET_H - 6, vcol);
        text(value, W - 16 - vw / 2 - text_w(value, 0.44f) / 2, y + 4, 0.44f, c_white, 0);
    }
}

static void draw_settings(void)
{
    draw_header("Settings", 0);
    {
        char v[24];
        snprintf(v, sizeof v, "v%s", update_version());
        text(v, W - 10, 9.f, 0.46f, c_white, 1);
    }
    u32 on = RGB(60, 150, 90), off = RGB(90, 90, 110), blue = RGB(70, 80, 140);
    set_row(SET_SKIP, "Frame skip", st_skip ? "ON" : "OFF", st_skip ? on : off);
    set_row(SET_PICTURE, "Picture", st_stretch ? "Widescreen" : "4:3", blue);
    set_row(SET_LOG, "Logging (mvc2_log.txt)", st_logging ? "ON" : "OFF", st_logging ? on : off);
    set_row(SET_UNLOCK, "Characters", st_unlock ? "All unlocked" : "Buy in the shop", blue);
    const char *ub = upd_state == UPD_AVAILABLE ? "Install" : upd_state == UPD_BUSY ? "Wait..." : "Check";
    set_row(SET_UPDATE, "Check for updates", ub, upd_state == UPD_AVAILABLE ? c_btn_a : blue);
    float y = SET_Y0 + SET_N * SET_ROW;
    const char *l = upd_line[0] ? upd_line : "Updates: the newest release from GitHub (needs Wi-Fi)";
    if (st_unlock != st_unlock_active && !upd_line[0])
        l = "Characters: restart the game to switch (each mode has its own save)";
    u32 lc = upd_state == UPD_ERROR ? c_btn_a : upd_state == UPD_DONE || upd_state == UPD_AVAILABLE ? c_gold : c_dim;
    text(l, 14, y + 1, 0.38f, lc, 0);
}

/* ---------------- online screen ---------------- */
#define OV_Y0 (HEAD_H + 5.f)
#define OV_LH 16.f
#define KEY_W 74.f
#define KEY_H 22.f
#define KEY_X0 10.f
#define KEY_Y0 (HEAD_H + 50.f)
static const char *keys[4][4] = {
    { "1", "2", "3", "DEL" }, { "4", "5", "6", "." }, { "7", "8", "9", ":" }, { "CLR", "0", "", "" },
};

/* the buttons' boxes: one row at the bottom, or rows of two (three for
 * five or six) without the keypad */
static void ov_btn_box(int k, float *x, float *y, float *w, float *h)
{
    int n = ov.nbtn, per = (ov.keypad || n <= 2) ? n : n <= 4 ? 2 : 3, rows = (n + per - 1) / per;
    int r = k / per, c = k % per;
    float gap = 8.f, bw = (W - 16.f - gap * (per - 1)) / per, bh = rows >= 2 ? 26.f : 30.f;
    float ytop = FOOT_Y - 6.f - rows * bh - (rows - 1) * 6.f;
    *x = 8.f + c * (bw + gap);
    *y = ytop + r * (bh + 6.f);
    *w = bw;
    *h = bh;
}

static void draw_online(void)
{
    draw_header(ov.title, 0);
    u32 tones[5] = { c_white, c_dim, c_gold, RGB(255, 110, 100), RGB(110, 220, 140) };
    int nl = ov.keypad ? 1 : OV_LINES;
    for (int i = 0; i < nl; i++) {
        if (!ov.line[i][0]) continue;
        if (ov.line_id[i]) C2D_DrawRectSolid(6.f, OV_Y0 + i * OV_LH - 1.f, Z, W - 12.f, OV_LH - 1.f, RGBA(255, 255, 255, 26));
        text(ov.line[i], 10.f, OV_Y0 + i * OV_LH, 0.42f, tones[ov.tone[i] % 5], 0);
    }
    if (ov.keypad) {
        float ey = HEAD_H + 22.f;
        C2D_DrawRectSolid(10.f, ey, Z, W - 20.f, 24.f, RGBA(255, 255, 255, 30));
        C2D_DrawRectSolid(10.f, ey + 23.f, Z, W - 20.f, 1.f, c_cyan);
        text(ov.entry[0] ? ov.entry : "_", 16.f, ey + 2.f, 0.62f, ov.entry[0] ? c_white : c_dim, 0);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                const char *k = keys[r][c];
                if (!*k) continue;
                float x = KEY_X0 + c * (KEY_W + 2.f), y = KEY_Y0 + r * (KEY_H + 2.f);
                C2D_DrawRectSolid(x, y, Z, KEY_W, KEY_H, (c >= 3 || (r == 3 && c == 0)) ? RGB(60, 56, 92) : RGB(46, 44, 72));
                text(k, x + KEY_W / 2 - text_w(k, 0.5f) / 2, y + 2.f, 0.5f, c_white, 0);
            }
    }
    for (int k = 0; k < ov.nbtn; k++) {
        float x, y, w, h;
        ov_btn_box(k, &x, &y, &w, &h);
        C2D_DrawRectSolid(x, y, Z, w, h, ov.btn_tone[k] ? RGB(120, 36, 160) : RGB(52, 50, 84));
        C2D_DrawRectSolid(x, y, Z, w, 1.f, RGBA(255, 255, 255, 70));
        text(ov.btn[k], x + w / 2 - text_w(ov.btn[k], 0.5f) / 2, y + h / 2 - 8.f, 0.5f, c_white, 0);
    }
}

static int online_touch_at(int x, int y)
{
    if (!ov.keypad && y >= OV_Y0) {
        int i = (int)((y - OV_Y0 + 1.f) / OV_LH);
        if (i >= 0 && i < OV_LINES && ov.line_id[i] && ov.line[i][0]) {
            float bx, by, bw, bh;
            ov_btn_box(0, &bx, &by, &bw, &bh);
            if (y < by - 4) return MOVELIST_ONLINE + ov.line_id[i];
        }
    }
    for (int k = 0; k < ov.nbtn; k++) {
        float bx, by, bw, bh;
        ov_btn_box(k, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by - 2 && y < by + bh + 2) return MOVELIST_ONLINE + ov.btn_id[k];
    }
    if (ov.keypad && x >= KEY_X0 && y >= KEY_Y0) {
        int c = (int)((x - KEY_X0) / (KEY_W + 2.f)), r = (int)((y - KEY_Y0) / (KEY_H + 2.f));
        if (c < 4 && r < 4 && *keys[r][c]) {
            const char *k = keys[r][c];
            int ch = !strcmp(k, "DEL") ? '<' : !strcmp(k, "CLR") ? '#' : k[0];
            return MOVELIST_ONLINE_KEY + ch;
        }
    }
    return 0;
}

void movelist_online(const OnlineView *v)
{
    if (v) ov = *v;
    ov_on = v != NULL;
    dirty = 1;
}

static void draw_moves(const Entry *e)
{
    draw_header(e->name, nteam > 1);
    paginate(e);
    int k0 = page_first[page], k1 = page + 1 < npages ? page_first[page + 1] : e->n;
    float y = LIST_Y;
    for (int k = k0; k < k1; k++) {
        const Move *m = &e->moves[k];
        float h = row_h(m);
        if (m->name[0] == '-' && m->name[1] == '-') {   /* a form: Dark Sakura, Mech Zangief */
            text(m->name + 3, 8.f, y + 1.f, 0.44f, c_cyan, 0);
            C2D_DrawRectSolid(8.f, y + h - 2.f, Z, W - 16.f, 1.f, c_cyan);
            y += h;
            continue;
        }
        if (((k - k0) & 1) == 0) C2D_DrawRectSolid(0, y, Z, W, h, c_row);
        int hyper = is_hyper(m);
        if (hyper) C2D_DrawRectSolid(0, y, Z, 3.f, h, c_gold);
        int follow = m->name[0] == '>';   /* a follow-up of the move above */
        const char *nm = follow ? m->name + 1 : m->name;
        while (*nm == ' ') nm++;
        if (follow) text("-", 9.f, y + 1.f, name_scale, c_dim, 0);
        text(nm, follow ? 18.f : 8.f, y + 1.f, name_scale, hyper ? c_gold : c_white, 0);
        Pen meas = { 0, 0, 0 };
        float iw = put_input(&meas, m->input);
        Pen p = { W - 6.f - iw, h > ROW_H ? y + ROW_H + 6.f : y + 9.f, 1 };
        put_input(&p, m->input);
        y += h;
    }
    /* circles last: they switch citro2d into its circle mode */
    if (nteam > 1)
        for (int t = 0; t < nteam; t++)
            C2D_DrawCircleSolid(W - 28.f - (nteam - 1 - t) * 10.f, 16.f, Z, t == team_pos ? 3.5f : 2.2f,
                                t == team_pos ? c_white : RGBA(255, 255, 255, 120));
    if (npages > 1)
        for (int i = 0; i < npages; i++)
            C2D_DrawCircleSolid(W / 2 - (npages - 1) * 6.f + i * 12.f, LIST_BOTTOM + 5.f, Z, i == page ? 3.f : 2.f,
                                i == page ? c_white : RGBA(255, 255, 255, 90));
}

static void draw_help(void)
{
    draw_header("Marvel vs. Capcom 2", 0);
    text(loaded ? "Your point character's moves show here in a fight." : "No movelist.txt next to mvc2.3dsx.",
         8, LIST_Y + 2.f, 0.42f, c_dim, 0);
    /* the face buttons as on the 3DS */
    float cx = 58, cy = 92;
    static const struct { float dx, dy; int b; } fb[4] = { { 0, -16, 1 }, { -16, 0, 0 }, { 16, 0, 3 }, { 0, 16, 2 } };
    for (int i = 0; i < 4; i++) put_badge(&(Pen){ cx + fb[i].dx - 7.f, cy + fb[i].dy, 1 }, fb[i].b);
    struct { int b; const char *what; } lbl[] = {
        { 0, "light punch" }, { 1, "heavy punch" }, { 2, "light kick" }, { 3, "heavy kick" },
        { 4, "assist 1" }, { 5, "assist 2" },
    };
    float yy = 124;
    for (unsigned i = 0; i < sizeof lbl / sizeof lbl[0]; i++, yy += 14.f) {
        put_badge(&(Pen){ 10, yy, 1 }, lbl[i].b);
        text(lbl[i].what, 30, yy - 7.5f, 0.38f, c_white, 0);
    }
    /* notation, drawn the way the list draws it */
    struct { const char *what, *in; } ex[] = {
        { "Quarter circle", "QCF" }, { "Dragon punch", "DP" }, { "Half circle", "HCB" },
        { "Charge", "[B]F" }, { "Super jump", "D U" }, { "Dash", "F F" },
        { "Snapback", "QCF+A1" }, { "Team hyper", "A1+A2" }, { "Any punch", "P" }, { "Both punches", "PP" },
    };
    yy = 58;
    for (unsigned i = 0; i < sizeof ex / sizeof ex[0]; i++, yy += 14.5f) {
        text(ex[i].what, 124, yy - 7, 0.38f, c_dim, 0);
        Pen m = { 0, 0, 0 };
        float iw = put_input(&m, ex[i].in);
        put_input(&(Pen){ W - 6.f - iw, yy, 1 }, ex[i].in);
    }
    text("Gold = hyper combo (uses super meter)", 124, LIST_BOTTOM - 6, 0.36f, c_gold, 0);
}

/* ---------------- public ---------------- */
int movelist_ui_init(void)
{
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) return -1;
    bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(4096);
    if (!bottom || !tbuf) return -1;
    colours();
    ui_ok = 1;
    dirty = 1;
    return 0;
}

int movelist_render(void)
{
    if (!ui_ok || !dirty) return 0;
    dirty = 0;
    C2D_TextBufClear(tbuf);
    C2D_Prepare();
    /* citro2d leaves blending, the alpha test and the scissor as it finds
     * them and assumes citro3d's defaults; the game's renderer (r_c3d.c)
     * ends the frame with blending off (the present pass), which painted
     * every glyph's and circle's transparent pixels opaque */
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C2D_TargetClear(bottom, c_bg1);
    C2D_SceneBegin(bottom);
    C2D_DrawRectangle(0, 0, 0.1f, W, 240, c_bg0, c_bg0, c_bg1, c_bg1);
    if (settings_open) draw_settings();
    else if (ov_on) draw_online();
    else if (shown >= 0) draw_moves(&tab[shown]);
    else draw_help();
    draw_footer();
    C2D_Flush();
    return 1;
}

static void show(int id)
{
    if (id == shown) return;
    shown = id;
    page = 0;
    team_pos = -1;
    for (int t = 0; t < nteam; t++)
        if (team[t] == id) team_pos = t;
    dirty = 1;
}

void movelist_update(void)
{
    if (!loaded) return;
    read_team();
    int p = point_id();
    if (p >= 0 && valid_id(p) && p != point) {
        point = p;
        manual = 0;       /* a tag: follow it */
        show(p);
    } else if (p < 0) {
        point = -1;
        if (!manual) show(nteam ? team[0] : -1);
    }
}

void movelist_status(const char *s, int frame_skip, int stretch)
{
    if (s) snprintf(status_line, sizeof status_line, "%s", s);
    st_skip = frame_skip;
    st_stretch = stretch;
    dirty = 1;
}

void movelist_settings(int logging, int unlock_all, int unlock_active)
{
    st_unlock_active = unlock_active;
    st_logging = logging;
    st_unlock = unlock_all;
    dirty = 1;
}

void movelist_update_status(int state, const char *line)
{
    upd_state = state;
    snprintf(upd_line, sizeof upd_line, "%s", line ? line : "");
    if (settings_open) dirty = 1;
}

int movelist_touch(int x, int y)
{
    if (y >= FOOT_Y) {
        if (x < GEAR_W + 60) {
            settings_open = !settings_open;
            dirty = 1;
        }
        return 0;
    }
    if (settings_open) {
        int i = (int)((y - SET_Y0) / SET_ROW);
        if (y < SET_Y0 || i >= SET_N || y > SET_Y0 + i * SET_ROW + SET_H) return 0;
        dirty = 1;
        if (i == SET_SKIP) return MOVELIST_TOGGLE_SKIP;
        if (i == SET_PICTURE) return MOVELIST_TOGGLE_ASPECT;
        if (i == SET_LOG) return MOVELIST_TOGGLE_LOG;
        if (i == SET_UNLOCK) return MOVELIST_TOGGLE_UNLOCK;
        if (upd_state == UPD_BUSY) return 0;
        return upd_state == UPD_AVAILABLE ? MOVELIST_UPDATE_INSTALL : MOVELIST_UPDATE_CHECK;
    }
    if (ov_on) {
        int c = online_touch_at(x, y);
        if (c) dirty = 1;
        return c;
    }
    if (!loaded) return 0;
    if (y < HEAD_H) {
        if (nteam > 0) {
            int t = team_pos < 0 ? 0 : team_pos;
            t = x < 160 ? (t + nteam - 1) % nteam : (t + 1) % nteam;
            manual = 1;
            shown = -2;   /* force the change */
            show(team[t]);
        }
        return 0;
    }
    if (shown >= 0 && npages > 1) {
        page = (page + 1) % npages;
        dirty = 1;
    }
    return 0;
}

void movelist_redraw(void) { dirty = 1; }
int movelist_dirty(void) { return ui_ok && dirty; }
