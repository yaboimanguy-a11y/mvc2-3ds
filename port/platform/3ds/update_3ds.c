/*
 * In-app updates from the project's GitHub releases (settings screen, "Check
 * for updates").
 *
 * A background thread asks the GitHub API for the latest release of
 * MVC2_UPDATE_REPO, compares its tag (v0.2) with this build's version and,
 * when asked to install, downloads the release's .zip and writes the files
 * under 3ds/mvc2/ in it over the ones in the app's folder (each written as
 * .new first, then swapped in), and installs the release's .cia (AM
 * service) when the game runs as the CIA or the CIA is installed, so the
 * HOME Menu app and the folder stay on the same version.  Nothing else is
 * touched: the disc image, saves and logs stay.  The running program is
 * already in memory, so the new one starts the next time.
 *
 * HTTPS through libcurl + mbedTLS (devkitPro portlibs, 3ds-curl), checked
 * against cacert.pem in the app's folder (the SD card folder carries it).
 * Without the portlibs at build time (MVC2_HAVE_CURL unset) the button only
 * says so.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "update_3ds.h"

#ifndef MVC2_VERSION
#define MVC2_VERSION "0.0"
#endif
#ifndef MVC2_UPDATE_REPO
#define MVC2_UPDATE_REPO "yaboimanguy-a11y/mvc2-3ds"
#endif

const char *update_version(void) { return MVC2_VERSION; }

static LightLock lk;
static int lk_init;
static volatile int state = UPD_IDLE;
static char msg[96];
static int msg_serial, msg_seen = -1;
static Thread thr;
static char dir[256];
static char tag[32], zip_url[512], cia_url[512];
static int want_install;

static void set(int st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set(int st, const char *fmt, ...)
{
    va_list ap;
    LightLock_Lock(&lk);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    state = st;
    msg_serial++;
    LightLock_Unlock(&lk);
}

int update_state(char *out, size_t n, int *changed)
{
    if (!lk_init) {
        if (out && n) snprintf(out, n, "Tap to check GitHub for a newer version");
        if (changed) *changed = 0;
        return UPD_IDLE;
    }
    LightLock_Lock(&lk);
    int st = state;
    if (out && n) snprintf(out, n, "%s", msg);
    if (changed) *changed = msg_serial != msg_seen;
    msg_seen = msg_serial;
    LightLock_Unlock(&lk);
    return st;
}

#ifdef MVC2_HAVE_CURL
#include <curl/curl.h>
#include "../../host/inflate.h"

typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;

static size_t on_data(char *d, size_t sz, size_t nm, void *u)
{
    Buf *b = (Buf *)u;
    size_t k = sz * nm;
    if (b->n + k + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 65536;
        while (c < b->n + k + 1) c *= 2;
        if (c > (48u << 20)) return 0; /* nothing this big belongs to a release */
        uint8_t *q = (uint8_t *)realloc(b->p, c);
        if (!q) return 0;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, k);
    b->n += k;
    b->p[b->n] = 0;
    return k;
}

static int on_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
    (void)u; (void)ut; (void)un;
    static int last = -1;
    if (total > 0) {
        int pc = (int)(now * 100 / total);
        if (pc / 5 != last / 5) {
            last = pc;
            set(UPD_BUSY, "Downloading %s ... %d%%", tag, pc);
        }
    }
    return 0;
}

static int get(const char *url, Buf *b, int progress)
{
    char ca[300];
    snprintf(ca, sizeof ca, "%s/cacert.pem", dir);
    struct stat st;
    if (stat(ca, &st) != 0) return -2;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "mvc2-3ds-updater");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (progress) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    }
    struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    CURLcode r = curl_easy_perform(c);
    if (r != CURLE_OK) fprintf(stderr, "update: %s: %s\n", url, curl_easy_strerror(r));
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? 0 : -1;
}

/* the string value of "key": "..." after p (no escapes in what we need) */
static int json_str(const char *p, const char *key, char *out, size_t n, const char **after)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *q = strstr(p, k);
    if (!q) return -1;
    q += strlen(k);
    while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    if (*q != '"') return -1;
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < n) out[i++] = *q++;
    out[i] = 0;
    if (after) *after = q;
    return 0;
}

/* the game's CIA: title 0x000400000F3D3200 (port/platform/3ds/cia/mvc2.rsf) */
#define GAME_TITLE_ID 0x000400000F3D3200ULL

/* update the CIA too: when this is the CIA, or the CIA is installed next to
 * the 3dsx (an update keeps both on the same version) */
static int want_cia(void)
{
    if (!envIsHomebrew()) return 1;
    u64 tid = GAME_TITLE_ID;
    AM_TitleEntry info;
    int installed = 0;
    if (R_SUCCEEDED(amInit())) {
        installed = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &info));
        amExit();
    }
    return installed;
}

/* "v1.2.3" vs "1.2": <0, 0, >0 */
static int vcmp(const char *a, const char *b)
{
    while (*a && (*a < '0' || *a > '9')) a++;
    while (*b && (*b < '0' || *b > '9')) b++;
    for (;;) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        int ma = *a == '.', mb = *b == '.';
        if (!ma && !mb) return 0;
        if (ma) a++;
        if (mb) b++;
    }
}

static void do_check(void)
{
    set(UPD_BUSY, "Checking GitHub for updates ...");
    Buf b = { 0 };
    int r = get("https://api.github.com/repos/" MVC2_UPDATE_REPO "/releases/latest", &b, 0);
    if (r == -2) { set(UPD_ERROR, "cacert.pem is missing from the app folder"); return; }
    if (r) { free(b.p); set(UPD_ERROR, "Could not reach GitHub (is Wi-Fi on?)"); return; }
    const char *p = (const char *)b.p, *u = NULL;
    zip_url[0] = cia_url[0] = 0;
    if (json_str(p, "tag_name", tag, sizeof tag, NULL)) { free(b.p); set(UPD_ERROR, "No release found"); return; }
    /* the first .zip asset */
    const char *q = p;
    char url[512];
    while (!json_str(q, "browser_download_url", url, sizeof url, &u)) {
        size_t l = strlen(url);
        if (l > 4 && !strcmp(url + l - 4, ".zip") && !zip_url[0]) snprintf(zip_url, sizeof zip_url, "%s", url);
        if (l > 4 && !strcmp(url + l - 4, ".cia") && !cia_url[0]) snprintf(cia_url, sizeof cia_url, "%s", url);
        q = u;
    }
    free(b.p);
    if (vcmp(tag, MVC2_VERSION) <= 0) { set(UPD_UPTODATE, "Up to date (v%s, latest %s)", MVC2_VERSION, tag); return; }
    if (!zip_url[0] && !(cia_url[0] && want_cia())) { set(UPD_ERROR, "Release %s has nothing to install", tag); return; }
    set(UPD_AVAILABLE, "Update %s available - tap to install", tag);
}

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the release zip's 3ds/mvc2/<file> entries into the app folder */
static int install_zip(const uint8_t *z, size_t n, int *nfiles)
{
    static const char pre[] = "3ds/mvc2/";
    size_t e = n >= 22 ? n - 22 : 0;
    while (e > 0 && rd32(z + e) != 0x06054b50u) e--;
    if (rd32(z + e) != 0x06054b50u) return -1;
    uint32_t cnt = rd16(z + e + 10), cd = rd32(z + e + 16);
    *nfiles = 0;
    /* pass 0: unpack every file as .new (and check it); pass 1: swap them in */
    for (int pass = 0; pass < 2; pass++) {
        size_t o = cd;
        for (uint32_t i = 0; i < cnt; i++) {
            if (o + 46 > n || rd32(z + o) != 0x02014b50u) return -1;
            uint32_t meth = rd16(z + o + 10), crc = rd32(z + o + 16), csz = rd32(z + o + 20), usz = rd32(z + o + 24);
            uint32_t nl = rd16(z + o + 28), xl = rd16(z + o + 30), cl = rd16(z + o + 32), lo = rd32(z + o + 42);
            char name[128];
            if (nl >= sizeof name || o + 46 + nl > n) return -1;
            memcpy(name, z + o + 46, nl);
            name[nl] = 0;
            o += 46 + nl + xl + cl;
            const char *base = name + sizeof pre - 1;
            if (strncmp(name, pre, sizeof pre - 1) || !*base || strchr(base, '/') || strstr(base, "..")) continue;
            char dst[400], tmp[410];
            snprintf(dst, sizeof dst, "%s/%s", dir, base);
            snprintf(tmp, sizeof tmp, "%s.new", dst);
            if (pass == 1) {
                remove(dst);
                if (rename(tmp, dst)) return -3;
                (*nfiles)++;
                continue;
            }
            if (lo + 30 > n || rd32(z + lo) != 0x04034b50u) return -1;
            size_t at = lo + 30 + rd16(z + lo + 26) + rd16(z + lo + 28);
            if (at + csz > n || usz > (32u << 20)) return -1;
            uint8_t *out = (uint8_t *)malloc(usz ? usz : 1);
            if (!out) return -2;
            long got;
            if (meth == 0) { memcpy(out, z + at, usz); got = csz == usz ? (long)usz : -1; }
            else if (meth == 8) got = host_inflate(out, usz, z + at, csz);
            else got = -1;
            if (got != (long)usz || host_crc32(0, out, usz) != crc) { free(out); return -1; }
            FILE *f = fopen(tmp, "wb");
            size_t w = f ? fwrite(out, 1, usz, f) : 0;
            if (f && fclose(f)) w = 0;
            free(out);
            if (w != usz) return -3;
        }
    }
    return 0;
}

/* the release's .cia into the title (running as the CIA) */
static int install_cia(const uint8_t *p, size_t n)
{
    if (R_FAILED(amInit())) return -1;
    Handle h;
    int ok = R_SUCCEEDED(AM_StartCiaInstall(MEDIATYPE_SD, &h));
    if (!ok) { amExit(); return -1; }
    for (size_t o = 0; ok && o < n;) {
        u32 k = n - o > 0x40000 ? 0x40000 : (u32)(n - o), w = 0;
        ok = R_SUCCEEDED(FSFILE_Write(h, &w, o, p + o, k, 0)) && w == k;
        o += k;
    }
    if (ok) ok = R_SUCCEEDED(AM_FinishCiaInstall(h));
    else AM_CancelCIAInstall(h);
    amExit();
    return ok ? 0 : -1;
}

/* the release's zip (3ds/mvc2/ files) and, started as the CIA, its .cia */
static void do_install(void)
{
    int as_cia = cia_url[0] && want_cia(); /* the HOME Menu app as well as the folder */
    if (zip_url[0]) {
        set(UPD_BUSY, "Downloading %s ...", tag);
        Buf b = { 0 };
        if (get(zip_url, &b, 1)) { free(b.p); set(UPD_ERROR, "Download failed (is Wi-Fi on?)"); return; }
        set(UPD_BUSY, "Installing %s ...", tag);
        int nf = 0, r = install_zip(b.p, b.n, &nf);
        free(b.p);
        if (r == -3) { set(UPD_ERROR, "Could not write to the SD card"); return; }
        if (r || !nf) { set(UPD_ERROR, "The download was damaged; try again"); return; }
    }
    if (as_cia) {
        set(UPD_BUSY, "Downloading %s (CIA) ...", tag);
        Buf b = { 0 };
        if (get(cia_url, &b, 1)) { free(b.p); set(UPD_ERROR, "CIA download failed (is Wi-Fi on?)"); return; }
        set(UPD_BUSY, "Installing the %s CIA ...", tag);
        int r = install_cia(b.p, b.n);
        free(b.p);
        if (r) { set(UPD_ERROR, "Installing the CIA failed"); return; }
    }
    set(UPD_DONE, "Installed %s%s: quit and start the game again", tag, as_cia ? " (files and CIA)" : "");
}

static void thread_main(void *arg)
{
    (void)arg;
    static u32 *soc_buf;
    if (!soc_buf) soc_buf = (u32 *)memalign(0x1000, 0x100000);
    if (!soc_buf || socInit(soc_buf, 0x100000)) {
        set(UPD_ERROR, "Network start failed");
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (want_install) do_install();
    else do_check();
    curl_global_cleanup();
    socExit();
}

static void start(int install)
{
    if (thr) {
        if (state == UPD_BUSY) return;
        threadJoin(thr, U64_MAX);
        threadFree(thr);
        thr = NULL;
    }
    want_install = install;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    state = UPD_BUSY;
    /* off the game's core: core 2 (New 3DS), else the system core, else the game's */
    thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, 2, false);
    if (!thr) thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, 1, false);
    if (!thr) thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, -1, false);
    if (!thr) set(UPD_ERROR, "Could not start the updater");
}

void update_check(const char *app_dir)
{
    if (!lk_init) { LightLock_Init(&lk); lk_init = 1; }
    snprintf(dir, sizeof dir, "%s", app_dir);
    start(0);
}

void update_install(void)
{
    if (!lk_init || state != UPD_AVAILABLE) return;
    start(1);
}

void update_shutdown(void)
{
    if (thr) {
        threadJoin(thr, U64_MAX);
        threadFree(thr);
        thr = NULL;
    }
}

#else /* no libcurl at build time */

void update_check(const char *app_dir)
{
    (void)app_dir;
    if (!lk_init) { LightLock_Init(&lk); lk_init = 1; }
    set(UPD_ERROR, "This build has no updater (built without 3ds-curl)");
}
void update_install(void) {}
void update_shutdown(void) {}
#endif
