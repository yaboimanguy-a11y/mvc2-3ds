#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "fs.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- sources ---- */
/* reads that reached a file (calls, bytes): fs_read_stats() */
static uint64_t stat_calls, stat_bytes;

void fs_read_stats(uint64_t *calls, uint64_t *bytes)
{
    if (calls) *calls = stat_calls;
    if (bytes) *bytes = stat_bytes;
}

static size_t file_read_at(void *ctx, uint64_t off, void *dst, size_t n)
{
    FILE *f = (FILE *)ctx;
    size_t got;
#if defined(_WIN32)
    if (_fseeki64(f, (long long)off, SEEK_SET)) return 0;
#else
    if (fseeko(f, (off_t)off, SEEK_SET)) return 0;
#endif
    got = fread(dst, 1, n, f);
    stat_calls++;
    stat_bytes += got;
    return got;
}
static void file_close(void *ctx) { fclose((FILE *)ctx); }

/* stdio buffer of an image file: the small header/table reads come from it
 * (newlib's default is 1 KB, which on the 3DS SD card means an access per
 * KB); big reads bypass it */
#define FILE_BUFFER (64u * 1024u)

int fs_source_open_file(FsSource *src, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    setvbuf(f, NULL, _IOFBF, FILE_BUFFER);
    fseeko(f, 0, SEEK_END);
    src->size = (uint64_t)ftello(f);
    src->ctx = f;
    src->read_at = file_read_at;
    src->close = file_close;
    return 0;
}

typedef struct { const uint8_t *p; size_t n; } MemSrc;
static size_t mem_read_at(void *ctx, uint64_t off, void *dst, size_t n)
{
    MemSrc *m = (MemSrc *)ctx;
    if (off >= m->n) return 0;
    if (n > m->n - off) n = (size_t)(m->n - off);
    memcpy(dst, m->p + off, n);
    return n;
}
static void mem_close(void *ctx) { free(ctx); }

void fs_source_memory(FsSource *src, const void *data, size_t size)
{
    MemSrc *m = (MemSrc *)malloc(sizeof *m);
    m->p = (const uint8_t *)data;
    m->n = size;
    src->ctx = m;
    src->read_at = mem_read_at;
    src->close = mem_close;
    src->size = size;
}

void fs_source_close(FsSource *src)
{
    if (src->close) src->close(src->ctx);
    memset(src, 0, sizeof *src);
}

/* ---- raw CD images (.bin) ---- */
/* A BIN stores whole 2352-byte CD sectors: 12 sync bytes, a 4-byte header,
 * then the 2048 user bytes (MODE1), or an 8-byte subheader first (MODE2
 * form 1, as on the PS2 CD-ROM).  MODE2/2336 dumps drop sync and header. */
/* Raw sectors are read RAW_BATCH at a time into buf (one file access per
 * run instead of one per 2048 bytes) and later reads of those sectors are
 * served from it: the game reads files sequentially. */
#define RAW_BATCH 32u
typedef struct {
    FsSource in;
    uint32_t sect, off;
    uint64_t nsect;                 /* whole raw sectors in the file */
    uint64_t buf_lba;               /* first sector in buf */
    uint32_t buf_n;                 /* sectors in buf (0 = empty) */
    uint8_t *buf;                   /* RAW_BATCH * sect bytes */
} RawSrc;

/* user data of sector lba, or NULL past the end / on a read error */
static const uint8_t *raw_sector(RawSrc *r, uint64_t lba)
{
    if (lba >= r->nsect) return NULL;
    if (r->buf_n == 0 || lba < r->buf_lba || lba >= r->buf_lba + r->buf_n) {
        uint64_t k = r->nsect - lba;
        size_t want, got;
        if (k > RAW_BATCH) k = RAW_BATCH;
        want = (size_t)k * r->sect;
        got = r->in.read_at(r->in.ctx, lba * r->sect, r->buf, want);
        r->buf_lba = lba;
        r->buf_n = (uint32_t)(got / r->sect);
        if (r->buf_n == 0) return NULL;
    }
    return r->buf + (size_t)(lba - r->buf_lba) * r->sect + r->off;
}

static size_t raw_read_at(void *ctx, uint64_t off, void *dst, size_t n)
{
    RawSrc *r = (RawSrc *)ctx;
    uint8_t *d = (uint8_t *)dst;
    size_t done = 0;
    while (done < n) {
        uint64_t lba = (off + done) / SECTOR;
        uint32_t in = (uint32_t)((off + done) % SECTOR);
        size_t k = SECTOR - in;
        const uint8_t *s = raw_sector(r, lba);
        if (!s) break;
        if (k > n - done) k = n - done;
        memcpy(d + done, s + in, k);
        done += k;
    }
    return done;
}
static void raw_close(void *ctx)
{
    RawSrc *r = (RawSrc *)ctx;
    fs_source_close(&r->in);
    free(r->buf);
    free(r);
}

int fs_source_cooked(FsSource *src)
{
    /* sector size, user-data offset: plain ISO, MODE2/2352, MODE1/2352, MODE2/2336 */
    static const uint32_t lay[][2] = { { SECTOR, 0 }, { 2352, 24 }, { 2352, 16 }, { 2336, 8 } };
    uint8_t pvd[6];
    for (unsigned i = 0; i < sizeof lay / sizeof lay[0]; i++) {
        uint64_t at = 16ull * lay[i][0] + lay[i][1]; /* the primary volume descriptor */
        if (src->read_at(src->ctx, at, pvd, 6) != 6 || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5))
            continue;
        if (i == 0) return 0;
        RawSrc *r = (RawSrc *)calloc(1, sizeof *r);
        if (!r) return -1;
        r->buf = (uint8_t *)malloc((size_t)RAW_BATCH * lay[i][0]);
        if (!r->buf) { free(r); return -1; }
        r->in = *src;
        r->sect = lay[i][0];
        r->off = lay[i][1];
        r->nsect = src->size / lay[i][0];
        src->ctx = r;
        src->read_at = raw_read_at;
        src->close = raw_close;
        src->size = r->in.size / r->sect * SECTOR;
        return 0;
    }
    return -1;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

/* The data track's file of a .cue: the first FILE line (the PS2 disc is a
 * single MODE2/2352 track; a data track always comes first).  Relative names
 * are next to the .cue.  0 = ok. */
static int cue_bin_path(const char *cue, char *out, size_t n)
{
    FILE *f = fopen(cue, "r");
    char line[512], name[384];
    int found = 0;
    if (!f) return -1;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "FILE", 4) || (p[4] != ' ' && p[4] != '\t')) continue;
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        size_t k = 0;
        if (*p == '"') {
            for (p++; *p && *p != '"' && k + 1 < sizeof name; p++) name[k++] = *p;
        } else {
            for (; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && k + 1 < sizeof name; p++)
                name[k++] = *p;
        }
        name[k] = 0;
        found = k > 0;
    }
    fclose(f);
    if (!found) return -1;
    const char *slash = strrchr(cue, '/');
    const char *bslash = strrchr(cue, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (name[0] == '/' || !slash)
        return snprintf(out, n, "%s", name) < (int)n ? 0 : -1;
    return snprintf(out, n, "%.*s/%s", (int)(slash - cue), cue, name) < (int)n ? 0 : -1;
}

int fs_source_open_disc(FsSource *src, const char *path)
{
    char bin[512];
    if (ends_with_ci(path, ".cue")) {
        /* The track file the .cue names; when that is missing (the files
         * were renamed, e.g. to mvc2.cue + mvc2.bin), the .bin with the
         * .cue's own name. */
        int named = cue_bin_path(path, bin, sizeof bin) == 0 && fs_source_open_file(src, bin) == 0;
        if (!named) {
            size_t l = strlen(path);
            static const char *ext[] = { ".bin", ".BIN", ".Bin" };
            int ok = 0;
            if (l >= sizeof bin) return -1;
            for (unsigned i = 0; !ok && i < 3; i++) {
                memcpy(bin, path, l - 4);
                memcpy(bin + l - 4, ext[i], 5);
                ok = fs_source_open_file(src, bin) == 0;
            }
            if (!ok) return -1;
        }
        if (fs_source_cooked(src)) {
            fs_source_close(src);
            return -1;
        }
        return 0;
    }
    if (fs_source_open_file(src, path)) return -1;
    if (fs_source_cooked(src)) {
        fs_source_close(src);
        return -1;
    }
    return 0;
}

/* ---- ISO9660 ---- */
static int iso_sectors(const Iso *iso, uint32_t lba, uint32_t n, uint8_t *dst)
{
    size_t want = (size_t)n * SECTOR;
    return iso->src.read_at(iso->src.ctx, (uint64_t)lba * SECTOR, dst, want) == want ? 0 : -1;
}

static void iso_add(Iso *iso, const char *path, uint32_t lba, uint32_t size)
{
    if (iso->n_files == iso->cap_files) {
        iso->cap_files = iso->cap_files ? iso->cap_files * 2 : 64;
        iso->files = (IsoFile *)realloc(iso->files, sizeof(IsoFile) * iso->cap_files);
    }
    IsoFile *f = &iso->files[iso->n_files++];
    snprintf(f->path, sizeof f->path, "%s", path);
    for (char *c = f->path; *c; c++) *c = (char)toupper((unsigned char)*c);
    f->lba = lba;
    f->size = size;
}

static int iso_walk(Iso *iso, uint32_t lba, uint32_t size, const char *prefix, int depth)
{
    if (depth > 8) return -1;
    uint32_t ns = (size + SECTOR - 1) / SECTOR;
    uint8_t *data = (uint8_t *)malloc((size_t)ns * SECTOR);
    if (!data || iso_sectors(iso, lba, ns, data)) { free(data); return -1; }
    size_t p = 0, total = (size_t)ns * SECTOR;
    while (p < total) {
        uint8_t ln = data[p];
        if (ln == 0) { p = (p / SECTOR + 1) * SECTOR; continue; } /* records never straddle sectors */
        if (p + 33 > total) break;
        uint32_t ext = rd32(data + p + 2), sz = rd32(data + p + 10);
        uint8_t flags = data[p + 25], nl = data[p + 32];
        const uint8_t *name = data + p + 33;
        p += ln;
        if (nl == 1 && (name[0] == 0 || name[0] == 1)) continue;
        char nm[64], full[96];
        int k;
        for (k = 0; k < nl && k < 63 && name[k] != ';'; k++) nm[k] = (char)name[k];
        nm[k] = 0;
        snprintf(full, sizeof full, "%s/%s", prefix, nm);
        if (flags & 2) iso_walk(iso, ext, sz, full, depth + 1);
        else iso_add(iso, full, ext, sz);
    }
    free(data);
    return 0;
}

int iso_open(Iso *iso, FsSource src)
{
    uint8_t pvd[SECTOR];
    memset(iso, 0, sizeof *iso);
    iso->src = src;
    if (iso_sectors(iso, 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) {
        fs_source_close(&iso->src);
        return -1;
    }
    const uint8_t *root = pvd + 156;
    return iso_walk(iso, rd32(root + 2), rd32(root + 10), "", 0);
}

void iso_close(Iso *iso)
{
    fs_source_close(&iso->src);
    free(iso->files);
    memset(iso, 0, sizeof *iso);
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == *b;
}

const IsoFile *iso_find(const Iso *iso, const char *path)
{
    for (int i = 0; i < iso->n_files; i++)
        if (ieq(iso->files[i].path, path)) return &iso->files[i];
    return NULL;
}

size_t iso_read(const Iso *iso, const IsoFile *f, uint64_t off, void *dst, size_t n)
{
    if (off >= f->size) return 0;
    if (n > f->size - off) n = (size_t)(f->size - off);
    return iso->src.read_at(iso->src.ctx, (uint64_t)f->lba * SECTOR + off, dst, n);
}

uint8_t *iso_load(const Iso *iso, const char *path, size_t *size_out)
{
    const IsoFile *f = iso_find(iso, path);
    if (!f) return NULL;
    uint8_t *buf = (uint8_t *)malloc(f->size ? f->size : 1);
    if (!buf || iso_read(iso, f, 0, buf, f->size) != f->size) { free(buf); return NULL; }
    if (size_out) *size_out = f->size;
    return buf;
}

/* ---- AFS ----
 * "AFS\0", u32 count, count x {u32 offset, u32 size}, then {u32 nameOfs, u32 nameSize}.
 * Name table (AFS02 only): 48-byte entries {char name[32]; u16 date[6]; u32 size}. */
int afs_open(Afs *afs, const Iso *iso, int ptid)
{
    char path[32];
    uint8_t hdr[8];
    memset(afs, 0, sizeof *afs);
    snprintf(path, sizeof path, "/PS2/AFS%02d.AFS", ptid);
    const IsoFile *f = iso_find(iso, path);
    if (!f) return -1;
    afs->iso = iso;
    afs->file = *f;
    afs->ptid = ptid;
    if (iso_read(iso, f, 0, hdr, 8) != 8 || memcmp(hdr, "AFS\0", 4)) return -1;
    uint32_t n = rd32(hdr + 4);
    if (n > 65536) return -1;
    uint8_t *tab = (uint8_t *)malloc(8 * ((size_t)n + 1));
    if (iso_read(iso, f, 8, tab, 8 * ((size_t)n + 1)) != 8 * ((size_t)n + 1)) { free(tab); return -1; }
    afs->entries = (AfsEntry *)calloc(n ? n : 1, sizeof(AfsEntry));
    afs->count = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        afs->entries[i].offset = rd32(tab + 8 * i);
        afs->entries[i].size = rd32(tab + 8 * i + 4);
    }
    uint32_t nt_off = rd32(tab + 8 * n), nt_size = rd32(tab + 8 * n + 4);
    free(tab);
    if (nt_off && nt_size >= 48 * n) {
        uint8_t *nt = (uint8_t *)malloc(nt_size);
        if (iso_read(iso, f, nt_off, nt, nt_size) == nt_size)
            for (uint32_t i = 0; i < n; i++) {
                memcpy(afs->entries[i].name, nt + 48 * i, 32);
                afs->entries[i].name[32] = 0;
            }
        free(nt);
    }
    return 0;
}

void afs_close(Afs *afs)
{
    free(afs->entries);
    memset(afs, 0, sizeof *afs);
}

int afs_find(const Afs *afs, const char *name)
{
    for (int i = 0; i < afs->count; i++)
        if (ieq(afs->entries[i].name, name)) return i;
    return -1;
}

uint8_t *afs_load(const Afs *afs, int index, size_t *size_out)
{
    if (index < 0 || index >= afs->count) return NULL;
    const AfsEntry *e = &afs->entries[index];
    uint8_t *buf = (uint8_t *)malloc(e->size + 16); /* +16: LZ decoders may over-read */
    if (!buf) return NULL;
    if (iso_read(afs->iso, &afs->file, e->offset, buf, e->size) != e->size) { free(buf); return NULL; }
    memset(buf + e->size, 0, 16);
    if (size_out) *size_out = e->size;
    return buf;
}

/* ---- disc ---- */
int disc_open_source(GameDisc *d, FsSource src)
{
    memset(d, 0, sizeof *d);
    if (iso_open(&d->iso, src)) return -1;
    int any = 0;
    for (int i = 0; i < 4; i++) {
        d->afs_ok[i] = afs_open(&d->afs[i], &d->iso, i) == 0;
        any |= d->afs_ok[i];
    }
    return any ? 0 : -1;
}

int disc_open(GameDisc *d, const char *iso_path)
{
    FsSource s;
    if (fs_source_open_disc(&s, iso_path)) return -1;
    return disc_open_source(d, s);
}

void disc_close(GameDisc *d)
{
    for (int i = 0; i < 4; i++) afs_close(&d->afs[i]);
    iso_close(&d->iso);
}

uint8_t *disc_load(GameDisc *d, int ptid, int index, size_t *size_out)
{
    if (ptid < 0 || ptid > 3 || !d->afs_ok[ptid]) return NULL;
    return afs_load(&d->afs[ptid], index, size_out);
}
