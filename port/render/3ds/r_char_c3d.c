/* Character sprite path for the Nintendo 3DS: func_00411C10 (slot setup) and
 * func_00412260 (draw), as port/render/r_char.c, with a tile cache instead
 * of the index atlas.
 *
 * The 3DS GPU has no palette lookup, so a tile is cached already coloured:
 * one 64x64 cell per (bank, tile, palette row, palette version) in shared
 * RGBA5551 pages (the bank palettes are ARGB1555, so nothing is lost).  This
 * is what the PS2 itself did (an LRU of decompressed tiles in 26 PSMT4 pages,
 * RENDER_PATH.md).  A palette change (hit flashes, super effects) makes new
 * cells; cells drawn in the current frame are never reused before the GPU
 * has drawn that frame.
 * Bank layout and placement: port/docs/ASSET_FORMATS.md section 2, chars.py.
 */
#include "../r_internal.h"
#include "../decode.h"
#include "r_c3d.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 512
#define CELL 64
#define CELLS_PER_PAGE ((PAGE / CELL) * (PAGE / CELL))
#define NPAGES 12
#define NCELLS (NPAGES * CELLS_PER_PAGE)
#define NBUCKETS 1024

typedef struct CharTile { uint8_t fw8, fh8, w8, h8; } CharTile;

struct FlCharBank {
    const uint8_t *d;
    size_t n;
    uint32_t tile_tab, frame_tab, pal_off;
    int n_tiles, n_frames, n_pals;
    CharTile *tiles;
    int pal_row;      /* 16 consecutive palette rows = slot.d[0..15] */
    uint32_t id;      /* cache key (bank pointers can be reused) */
};

typedef struct Cell {
    uint32_t bank, last;
    int32_t tile, row;
    uint32_t pal_ver;
    int16_t next;     /* bucket chain */
    uint8_t valid;
} Cell;

static struct {
    int init;
    C3D_Tex page[NPAGES];
    unsigned int page_id[NPAGES];
    Cell cell[NCELLS];
    int16_t bucket[NBUCKETS];
    int hand;
    uint32_t next_bank;
    uint8_t morton[64];
    uint8_t scratch[CELL * CELL + 64];
    int misses;
    uint8_t dirty[NPAGES];  /* written this frame: flushed once in r3ds_char_frame_finish */
} C;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t rd16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static int cache_init(void)
{
    if (C.init) return 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            C.morton[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                            ((x & 4) << 2) | ((y & 4) << 3));
    for (int p = 0; p < NPAGES; p++) {
        if (!C3D_TexInit(&C.page[p], PAGE, PAGE, GPU_RGBA5551)) return -1;
        memset(C.page[p].data, 0, C.page[p].size);
        C3D_TexFlush(&C.page[p]);
        C.page_id[p] = r3ds_ext_register(&C.page[p]);
    }
    for (int i = 0; i < NBUCKETS; i++) C.bucket[i] = -1;
    for (int i = 0; i < NCELLS; i++) C.cell[i].next = -1;
    C.next_bank = 1;
    C.init = 1;
    return 0;
}

static unsigned hash(uint32_t bank, int tile, int row)
{
    return (bank * 2654435761u ^ (uint32_t)tile * 40503u ^ (uint32_t)row * 97u) & (NBUCKETS - 1);
}

static void unlink_cell(int i)
{
    Cell *c = &C.cell[i];
    if (!c->valid) return;
    int16_t *pp = &C.bucket[hash(c->bank, c->tile, c->row)];
    while (*pp >= 0 && *pp != i) pp = &C.cell[*pp].next;
    if (*pp == i) *pp = c->next;
    c->next = -1;
    c->valid = 0;
}

/* a cell not drawn this frame: invalid ones first, else the least recently used */
static int take_cell(void)
{
    uint32_t frame = r3ds_frame_number();
    int best = -1;
    for (int k = 0; k < NCELLS; k++) {
        int i = (C.hand + k) % NCELLS;
        Cell *c = &C.cell[i];
        if (c->last == frame && c->last != 0) continue;
        if (!c->valid) { best = i; break; }
        if (frame - c->last > 120) { best = i; break; }
        if (best < 0 || c->last < C.cell[best].last) best = i;
    }
    if (best < 0) return -1;
    C.hand = (best + 1) % NCELLS;
    unlink_cell(best);
    return best;
}

static inline uint16_t to5551(uint32_t w)
{
    unsigned r = w >> 24, g = (w >> 16) & 255, b = (w >> 8) & 255, a = w & 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >= 128 ? 1 : 0));
}

/* decode tile ti (func_00413A80 + the square sub-block layout) into C.scratch */
static int decode_tile(const FlCharBank *b, int ti, int *pw, int *ph)
{
    const CharTile *t = &b->tiles[ti];
    int w = t->w8 * 8, h = t->h8 * 8;
    if (w <= 0 || h <= 0 || w > CELL || h > CELL) return -1;
    uint32_t o = b->tile_tab + rd32(b->d + b->tile_tab + 4 * ti);
    if (o + 4 > b->n) return -1;
    uint8_t raw[CELL * CELL / 2 + 32];
    lz8_decode(b->d + o + 4, b->n - o - 4, raw, (size_t)(w * h / 2));
    int s = w < h ? w : h, nsub = (w > h ? w : h) / s;
    size_t p = 0;
    for (int k = 0; k < nsub; k++) {
        int ox = w >= h ? k * s : 0, oy = w >= h ? 0 : k * s;
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x += 2) {
                uint8_t v = raw[p++];
                C.scratch[(oy + y) * w + ox + x] = v & 15; /* PSMT4: low nibble = left */
                C.scratch[(oy + y) * w + ox + x + 1] = v >> 4;
            }
    }
    *pw = w;
    *ph = h;
    return 0;
}

/* the cell of (bank, tile, row) with the row's current colours */
static int cell_for(const FlCharBank *b, int ti, int row)
{
    uint32_t pv;
    const uint32_t *pal = r3ds_pal_row(row, &pv);
    uint32_t frame = r3ds_frame_number();
    unsigned hk = hash(b->id, ti, row);
    for (int i = C.bucket[hk]; i >= 0; i = C.cell[i].next) {
        Cell *c = &C.cell[i];
        if (c->bank == b->id && c->tile == ti && c->row == row) {
            if (c->pal_ver == pv) { c->last = frame; return i; }
            break; /* stale colours: a new cell below, this one ages out */
        }
    }
    int w, h;
    if (decode_tile(b, ti, &w, &h)) return -1;
    int i = take_cell();
    if (i < 0) return -1;
    /* a stale cell of the same key leaves the chain so lookups find the new one */
    for (int j = C.bucket[hk]; j >= 0; j = C.cell[j].next)
        if (C.cell[j].bank == b->id && C.cell[j].tile == ti && C.cell[j].row == row) { unlink_cell(j); break; }
    uint16_t lut[16];
    for (int k = 0; k < 16; k++) lut[k] = to5551(pal[k]);
    C3D_Tex *pg = &C.page[i / CELLS_PER_PAGE];
    int ci = i % CELLS_PER_PAGE, cx = (ci % (PAGE / CELL)) * CELL, cy = (ci / (PAGE / CELL)) * CELL;
    uint16_t *dst = (uint16_t *)pg->data;
    int w1 = w < CELL ? w + 1 : w, h1 = h < CELL ? h + 1 : h; /* 1 px transparent guard for filtering */
    for (int y = 0; y < h1; y++)
        for (int x = 0; x < w1; x++) {
            /* memory rows run bottom-up (see r_c3d.c img_row): cell row y of a
             * cell at v = cy / PAGE is memory row PAGE - 1 - (cy + y) */
            uint32_t X = (uint32_t)(cx + x), Y = (uint32_t)(PAGE - 1 - (cy + y));
            uint32_t at = (((Y >> 3) * (PAGE >> 3) + (X >> 3)) << 6) | C.morton[((Y & 7) << 3) | (X & 7)];
            dst[at] = (x < w && y < h) ? lut[C.scratch[y * w + x] & 15] : 0;
        }
    C.dirty[i / CELLS_PER_PAGE] = 1;
    Cell *c = &C.cell[i];
    c->bank = b->id;
    c->tile = ti;
    c->row = row;
    c->pal_ver = pv;
    c->last = frame;
    c->valid = 1;
    c->next = C.bucket[hk];
    C.bucket[hk] = (int16_t)i;
    C.misses++;
    return i;
}

/* ---------------- banks (as r_char.c) ---------------- */
FlCharBank *fl_char_bank_create(const uint8_t *d, size_t n)
{
    if (cache_init()) return NULL;
    if (n < 0x20) return NULL;
    FlCharBank *b = (FlCharBank *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->d = d;
    b->n = n;
    b->id = C.next_bank++;
    b->tile_tab = rd32(d + 0x04);
    b->frame_tab = rd32(d + 0x0C);
    b->pal_off = rd32(d + 0x14);
    if (b->tile_tab + 4 > n || b->frame_tab + 4 > n || b->pal_off + 32 * 16 > n) { free(b); return NULL; }
    b->n_tiles = (int)(rd32(d + b->tile_tab) / 4);
    b->n_frames = (int)(rd32(d + b->frame_tab) / 4);
    b->n_pals = (int)((n - b->pal_off) / 32);
    if (b->n_pals > 48) b->n_pals = 48;
    b->tiles = (CharTile *)calloc((size_t)b->n_tiles, sizeof(CharTile));
    if (!b->tiles) { free(b); return NULL; }
    for (int i = 0; i < b->n_tiles; i++) {
        uint32_t o = b->tile_tab + rd32(d + b->tile_tab + 4 * i);
        if (o + 4 <= n) {
            b->tiles[i].fw8 = d[o]; b->tiles[i].fh8 = d[o + 1];
            b->tiles[i].w8 = d[o + 2]; b->tiles[i].h8 = d[o + 3];
        }
    }
    b->pal_row = r_pal_alloc_rows(16);
    if (b->pal_row < 0) { free(b->tiles); free(b); return NULL; }
    for (int p = 0; p < 16; p++) fl_char_load_palette(b, p, p);
    return b;
}

void fl_char_bank_destroy(FlCharBank *b)
{
    if (!b) return;
    for (int i = 0; i < NCELLS; i++)
        if (C.cell[i].valid && C.cell[i].bank == b->id) unlink_cell(i); /* .last keeps this frame's cells */
    free(b->tiles);
    free(b);
}

int fl_char_frame_count(const FlCharBank *b) { return b->n_frames; }
int fl_char_tile_count(const FlCharBank *b) { return b->n_tiles; }

void fl_char_set_palette_1555(FlCharBank *b, int dst, const uint16_t *c)
{
    uint8_t rgba[16 * 4];
    for (int k = 0; k < 16; k++) argb1555_to_rgba8(c[k], rgba + 4 * k);
    r_pal_write(b->pal_row + (dst & 15), 0, 16, rgba);
}

void fl_char_set_palette_rgba(FlCharBank *b, int dst, const uint8_t *rgba)
{
    /* the game rewrites its CLUT records every frame: only a real change
     * gets a new palette version (and new cells) */
    uint32_t ver;
    const uint32_t *cur = r3ds_pal_row(b->pal_row + (dst & 15), &ver);
    int same = 1;
    for (int k = 0; k < 16 && same; k++)
        same = cur[k] == ((uint32_t)rgba[4 * k] << 24 | (uint32_t)rgba[4 * k + 1] << 16 |
                          (uint32_t)rgba[4 * k + 2] << 8 | rgba[4 * k + 3]);
    if (!same) r_pal_write(b->pal_row + (dst & 15), 0, 16, rgba);
}

void fl_char_load_palette(FlCharBank *b, int dst, int src)
{
    if (src < 0 || src >= b->n_pals) return;
    uint16_t c[16];
    const uint8_t *p = b->d + b->pal_off + 32 * src;
    for (int k = 0; k < 16; k++) c[k] = (uint16_t)(p[2 * k] | p[2 * k + 1] << 8);
    fl_char_set_palette_1555(b, dst, c);
}

static const uint8_t *frame_rec(const FlCharBank *b, int f, int *nparts)
{
    if (f < 0 || f >= b->n_frames) return NULL;
    uint32_t o = b->frame_tab + rd32(b->d + b->frame_tab + 4 * f);
    if (o + 2 > b->n) return NULL;
    int n = rd16s(b->d + o);
    if (n < 0 || o + 2 + 8 * (size_t)n > b->n) return NULL;
    *nparts = n;
    return b->d + o + 2;
}

typedef struct PartRect { float x0, y0, x1, y1; int ti; uint16_t flags; } PartRect;

static int part_rect(const FlCharBank *b, const uint8_t *rec, int k, float *px, float *py, PartRect *r)
{
    int16_t dx = rd16s(rec + 8 * k), dy = rd16s(rec + 8 * k + 2);
    uint16_t flags = (uint16_t)rd16s(rec + 8 * k + 4);
    int ti = rd16s(rec + 8 * k + 6);
    *px -= dx;
    *py -= dy;
    if (ti < 0 || ti >= b->n_tiles) return -1;
    const CharTile *t = &b->tiles[ti];
    int w = t->w8 * 8, h = t->h8 * 8;
    r->x0 = (flags & 0x8000) ? *px + t->fw8 * 8 - w : *px;
    r->y0 = (flags & 0x4000) ? *py + t->fh8 * 8 - h : *py;
    r->x1 = r->x0 + w;
    r->y1 = r->y0 + h;
    r->ti = ti;
    r->flags = flags;
    return 0;
}

int fl_char_frame_bounds(FlCharBank *b, int f, int *x0, int *y0, int *x1, int *y1)
{
    int n;
    const uint8_t *rec = frame_rec(b, f, &n);
    if (!rec || !n) return -1;
    float px = 0, py = 0, a = 1e9f, c = 1e9f, e = -1e9f, g = -1e9f;
    for (int k = 0; k < n; k++) {
        PartRect r;
        if (part_rect(b, rec, k, &px, &py, &r)) continue;
        if (r.x0 < a) a = r.x0;
        if (r.y0 < c) c = r.y0;
        if (r.x1 > e) e = r.x1;
        if (r.y1 > g) g = r.y1;
    }
    *x0 = (int)a; *y0 = (int)c; *x1 = (int)e; *y1 = (int)g;
    return 0;
}

int fl_char_draw(FlCharBank *b, int f, const float m[16], uint32_t argb, int palette)
{
    int n;
    const uint8_t *rec = frame_rec(b, f, &n);
    if (!rec) return 0;
    if (r_discarding()) return n; /* a logic step that is not drawn (frame skip): no tile work */
    uint8_t col[4];
    {
        unsigned ch[4] = { (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24 };
        for (int i = 0; i < 4; i++) {
            unsigned g = (ch[i] + 1) >> 1, v = (g * 255 + 64) / 128;
            col[i] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    int axis = m[1] == 0.f && m[4] == 0.f;
    float z = r_depth_of(m[14]);
    float px = 0, py = 0;
    int drawn = 0;
    for (int k = 0; k < n; k++) {
        PartRect r;
        if (part_rect(b, rec, k, &px, &py, &r)) continue;
        int row = b->pal_row + (((r.flags >> 4) + palette) & 15);
        int ci = cell_for(b, r.ti, row);
        if (ci < 0) continue;
        const CharTile *t = &b->tiles[r.ti];
        r_use(C.page_id[ci / CELLS_PER_PAGE], RM_RGBA);
        int cc = ci % CELLS_PER_PAGE;
        float cx = (float)((cc % (PAGE / CELL)) * CELL), cy = (float)((cc / (PAGE / CELL)) * CELL);
        float u0 = cx / PAGE, v0 = cy / PAGE;
        float u1 = (cx + t->w8 * 8) / PAGE, v1 = (cy + t->h8 * 8) / PAGE;
        if (r.flags & 0x8000) { float s = u0; u0 = u1; u1 = s; }
        if (r.flags & 0x4000) { float s = v0; v0 = v1; v1 = s; }
        float qx[4] = { r.x0, r.x1, r.x1, r.x0 }, qy[4] = { r.y0, r.y0, r.y1, r.y1 };
        float qu[4] = { u0, u1, u1, u0 }, qv[4] = { v0, v0, v1, v1 };
        RVtx q[4];
        for (int i = 0; i < 4; i++) {
            float X = m[0] * qx[i] + m[4] * qy[i] + m[12];
            float Y = m[1] * qx[i] + m[5] * qy[i] + m[13];
            q[i].x = axis ? r_snap_x(X) : X;
            q[i].y = axis ? r_snap_y(Y) : Y;
            q[i].z = z;
            q[i].w = 1.f;
            q[i].u = qu[i];
            q[i].v = qv[i];
            memcpy(q[i].c, col, 4);
            q[i].pal = 0.f;
        }
        RVtx *o = r_verts(6);
        o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
        o[3] = q[0]; o[4] = q[2]; o[5] = q[3];
        drawn++;
    }
    return drawn;
}

/* r_c3d.c, before C3D_FrameEnd: the pages written this frame reach the GPU */
void r3ds_char_frame_finish(void)
{
    for (int p = 0; p < NPAGES; p++)
        if (C.dirty[p]) {
            GSPGPU_FlushDataCache(C.page[p].data, C.page[p].size);
            C.dirty[p] = 0;
        }
}

/* tile cache misses so far (decoded + coloured tiles) */
int r3ds_char_misses(void) { return C.misses; }
