/*
 * The online lobby directory from the 3DS (lobby_3ds.h): HTTPS requests to
 * the Cloudflare Worker (port/server/lobby/worker.js) on a thread, so the
 * game never waits for the network.  One connection is kept open between
 * requests (a TLS handshake takes the 3DS about a second).
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lobby_3ds.h"
#include "sys_3ds.h"

#ifndef MVC2_LOBBY_URL
#define MVC2_LOBBY_URL ""
#endif

#define NQ 6
typedef struct Req {
    int used, ticket, state;    /* state: 0 queued, 1 running, 2 done, 3 failed */
    int forgotten;              /* nobody reads it: freed when done */
    int get, status;
    char path[48];
    char form[640];
    char *body;
} Req;

static char url[160], ca[300];
static Req q[NQ];
static int next_ticket = 1;
static LightLock lk = 1;
static LightEvent ev;
static Thread thr;
static volatile int quit;

void lobby_form(char *buf, size_t cap, const char *key, const char *value)
{
    size_t n = strlen(buf);
    if (n && n + 1 < cap) buf[n++] = '&';
    for (const char *k = key; *k && n + 1 < cap; k++) buf[n++] = *k;
    if (n + 1 < cap) buf[n++] = '=';
    for (const unsigned char *v = (const unsigned char *)(value ? value : ""); *v && n + 4 < cap; v++) {
        if ((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') || (*v >= '0' && *v <= '9') || strchr("-_.~:", *v))
            buf[n++] = (char)*v;
        else
            n += (size_t)snprintf(buf + n, cap - n, "%%%02X", *v);
    }
    buf[n < cap ? n : cap - 1] = 0;
}

const char *lobby_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *l = body; l && *l; ) {
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            const char *v = l + kl + 1;
            size_t n = strcspn(v, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return out;
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    return out;
}

const char *lobby_url(void) { return url; }

#ifdef MVC2_HAVE_CURL
#include <curl/curl.h>

typedef struct Buf { char *p; size_t n, cap; } Buf;

static size_t on_data(char *d, size_t sz, size_t nm, void *u)
{
    Buf *b = (Buf *)u;
    size_t k = sz * nm;
    if (b->n + k + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + k + 1) c *= 2;
        if (c > 256 * 1024) return 0;
        char *p = (char *)realloc(b->p, c);
        if (!p) return 0;
        b->p = p;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, k);
    b->n += k;
    b->p[b->n] = 0;
    return k;
}

/* curl's global state once for the app (the updater uses it too) */
void lobby_curl_global(void)
{
    static int done;
    static LightLock glk = 1;
    LightLock_Lock(&glk);
    if (!done) { curl_global_init(CURL_GLOBAL_DEFAULT); done = 1; }
    LightLock_Unlock(&glk);
}

static void worker(void *arg)
{
    (void)arg;
    CURL *c = NULL;
    int soc = 0;
    while (!quit) {
        Req *r = NULL;
        LightLock_Lock(&lk);
        for (int i = 0; i < NQ && !r; i++)
            if (q[i].used && q[i].state == 0) { r = &q[i]; r->state = 1; }
        LightLock_Unlock(&lk);
        if (!r) {
            /* nothing to do: let the network go after a while */
            LightEvent_WaitTimeout(&ev, 3000000000LL);
            LightLock_Lock(&lk);
            int any = 0;
            for (int i = 0; i < NQ; i++) any |= q[i].used && q[i].state == 0;
            LightLock_Unlock(&lk);
            if (!any && c) { curl_easy_cleanup(c); c = NULL; }
            if (!any && soc) { sys3ds_soc_release(); soc = 0; }
            continue;
        }
        char full[256];
        snprintf(full, sizeof full, "%s%s%s%s", url, r->path, r->get && r->form[0] ? "?" : "", r->get ? r->form : "");
        Buf b = { 0 };
        int status = 0, ok = 0;
        char why[96] = "";
        if (!soc) soc = sys3ds_soc_acquire() == 0;
        if (!soc) snprintf(why, sizeof why, "No network (is Wi-Fi on?)");
        else {
            if (!c) c = curl_easy_init();
            if (c) {
                curl_easy_reset(c);
                curl_easy_setopt(c, CURLOPT_URL, full);
                curl_easy_setopt(c, CURLOPT_USERAGENT, "mvc2-3ds-online");
                curl_easy_setopt(c, CURLOPT_CAINFO, ca);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
                curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
                curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
                if (!r->get) curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->form);
                CURLcode e = curl_easy_perform(c);
                long code = 0;
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
                status = (int)code;
                if (e == CURLE_OK) ok = 1;
                else {
                    snprintf(why, sizeof why, "Lobby not reachable (%s)", curl_easy_strerror(e));
                    curl_easy_cleanup(c);
                    c = NULL;
                }
            } else {
                snprintf(why, sizeof why, "Lobby: no HTTP client");
            }
        }
        LightLock_Lock(&lk);
        r->status = status;
        if (ok) { r->body = b.p ? b.p : strdup(""); r->state = 2; }
        else { free(b.p); r->body = strdup(why); r->state = 3; }
        if (r->forgotten) { free(r->body); r->body = NULL; r->used = 0; }
        LightLock_Unlock(&lk);
    }
    if (c) curl_easy_cleanup(c);
    if (soc) sys3ds_soc_release();
}

int lobby_available(void)
{
    return url[0] != 0;
}

static void start_thread(void)
{
    if (thr) return;
    lobby_curl_global();
    LightEvent_Init(&ev, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thr = threadCreate(worker, NULL, 128 * 1024, prio + 1, 1, false);
    if (!thr) thr = threadCreate(worker, NULL, 128 * 1024, prio + 1, -1, false);
}

void lobby_shutdown(void)
{
    if (!thr) return;
    quit = 1;
    LightEvent_Signal(&ev);
    threadJoin(thr, 20000000000ULL);
    threadFree(thr);
    thr = NULL;
}
#else
int lobby_available(void) { return 0; }
static void start_thread(void) {}
void lobby_shutdown(void) {}
#endif

void lobby_init(const char *dir)
{
    snprintf(url, sizeof url, "%s", MVC2_LOBBY_URL);
    snprintf(ca, sizeof ca, "%s/cacert.pem", dir);
    char p[300];
    snprintf(p, sizeof p, "%s/lobby_url.txt", dir);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[160];
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n \t")] = 0;
            if (!strncmp(line, "http", 4)) snprintf(url, sizeof url, "%s", line);
        }
        fclose(f);
    }
    size_t n = strlen(url);
    while (n && url[n - 1] == '/') url[--n] = 0;
}

int lobby_request(const char *path, const char *form, int get)
{
    if (!lobby_available()) return -1;
    start_thread();
    int t = -1;
    LightLock_Lock(&lk);
    for (int i = 0; i < NQ; i++)
        if (!q[i].used) {
            memset(&q[i], 0, sizeof q[i]);
            q[i].used = 1;
            q[i].ticket = t = next_ticket++;
            q[i].get = get;
            snprintf(q[i].path, sizeof q[i].path, "%s", path);
            snprintf(q[i].form, sizeof q[i].form, "%s", form ? form : "");
            break;
        }
    LightLock_Unlock(&lk);
#ifdef MVC2_HAVE_CURL
    if (t >= 0) LightEvent_Signal(&ev);
#endif
    return t;
}

int lobby_result(int ticket, char *body, size_t cap, int *status)
{
    int r = 0;
    LightLock_Lock(&lk);
    for (int i = 0; i < NQ; i++) {
        if (!q[i].used || q[i].ticket != ticket) continue;
        if (q[i].state >= 2) {
            r = q[i].state == 2 ? 1 : -1;
            if (body && cap) snprintf(body, cap, "%s", q[i].body ? q[i].body : "");
            if (status) *status = q[i].status;
            free(q[i].body);
            q[i].used = 0;
        }
        LightLock_Unlock(&lk);
        return r;
    }
    LightLock_Unlock(&lk);
    if (body && cap) snprintf(body, cap, "lost");
    return -1;
}

void lobby_forget(int ticket)
{
    LightLock_Lock(&lk);
    for (int i = 0; i < NQ; i++)
        if (q[i].used && q[i].ticket == ticket) {
            if (q[i].state == 1) { q[i].forgotten = 1; continue; }
            free(q[i].body);
            q[i].used = 0;
        }
    LightLock_Unlock(&lk);
}
