/*
 * port/platform/net/room.c on a desktop: up to six consoles in one process
 * over a simulated network (latency, jitter, loss, blocked paths), each
 * running a stand-in game whose state depends on every input of every frame.
 *
 *   cc -O2 -o room_test port/tools/room_test.c port/platform/net/room.c
 *   ./room_test
 *
 * Checks: joining (and refusals), 1v1 with people watching, King of the
 * Hill's rotation, players who cannot reach each other (relayed by the
 * host), someone leaving mid-match, the host leaving, a divergence found,
 * every console that took part in a match identical frame for frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/net/room.h"

#define NC 6
/* ---- simulated network ---- */
typedef struct Pkt { int to; uint64_t at; NetAddr from; int len; uint8_t d[1300]; } Pkt;
#define MAXQ 60000
static Pkt q[MAXQ];
static int nq;
static uint64_t clock_ms;
static uint32_t rng = 777;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static struct { int lat, jitter, loss_pct; } cfg;
static int blocked[NC][NC];      /* no path from a to b */
static NetAddr addr_of(int i) { NetAddr a = { 0x0A000001u + (uint32_t)i * 0x01000000u, (uint16_t)(NET_PORT + i) }; return a; }
static int who(const NetAddr *a) { for (int i = 0; i < NC; i++) { NetAddr b = addr_of(i); if (b.ip == a->ip && b.port == a->port) return i; } return -1; }

typedef struct Ctx { int id; } Ctx;
static int sim_send(void *ctx, const NetAddr *to, const void *buf, int n)
{
    int src = ((Ctx *)ctx)->id, dst = who(to);
    if (dst < 0 || nq >= MAXQ || blocked[src][dst]) return n;
    if ((int)(rnd() % 100) < cfg.loss_pct) return n;
    Pkt *p = &q[nq++];
    p->to = dst;
    p->at = clock_ms + (uint64_t)cfg.lat + (cfg.jitter ? rnd() % (unsigned)cfg.jitter : 0);
    p->from = addr_of(src);
    p->len = n;
    memcpy(p->d, buf, (size_t)n);
    return n;
}
static int sim_recv(void *ctx, NetAddr *from, void *buf, int cap)
{
    int id = ((Ctx *)ctx)->id, best = -1;
    for (int i = 0; i < nq; i++)
        if (q[i].to == id && q[i].at <= clock_ms && (best < 0 || q[i].at < q[best].at)) best = i;
    if (best < 0) return 0;
    Pkt p = q[best];
    q[best] = q[--nq];
    if (p.len > cap) return 0;
    *from = p.from;
    memcpy(buf, p.d, (size_t)p.len);
    return p.len;
}
static uint64_t sim_now(void *ctx) { (void)ctx; return clock_ms; }

/* ---- stand-in consoles ---- */
#define NAV_END 600
#define FIGHT 1500           /* a fight lasts this many frames after the boot */
#define MAXF 12000
#define MAXS 64              /* sessions recorded */
typedef struct Console {
    Ctx ctx;
    Room room;
    int on, id, nav_per_vsync, gone;
    uint32_t sess;            /* the match the game runs */
    int restart_left;         /* ms of "boot" left */
    int32_t cur;
    uint32_t st;
    int began, throttles, stalls, last_throttle, steps, corrupt_at, results;
    double next_vsync;
} Console;
static Console C[NC];
/* every console's game per session: [session index][frame] */
static uint32_t sess_ids[MAXS];
static int nsess;
static uint32_t (*rec)[NC][MAXF];
static int32_t reclen[MAXS][NC];

static int sess_index(uint32_t s)
{
    for (int i = 0; i < nsess; i++) if (sess_ids[i] == s) return i;
    if (nsess == MAXS) return -1;
    sess_ids[nsess] = s;
    return nsess++;
}

static uint16_t pad(Console *c)
{
    uint32_t h = (uint32_t)(c->cur / 5) * 2246822519u ^ (uint32_t)(c->id + 1) * 3266489917u;
    h ^= h >> 15;
    return (uint16_t)(h * 668265263u >> 16);
}

static void step(Console *c, uint16_t a, uint16_t b)
{
    c->st = c->st * 2654435761u + a * 7919u + b * 104729u + (uint32_t)c->cur;
    if (c->cur == c->corrupt_at) c->st ^= 0x10;
    int si = sess_index(c->sess);
    if (si >= 0 && c->cur < MAXF) { rec[si][c->id][c->cur] = c->st; if (c->cur + 1 > reclen[si][c->id]) reclen[si][c->id] = c->cur + 1; }
    room_fingerprint(&c->room, c->cur, c->st);
    c->cur++;
    /* the fight ends: every console sees the same result */
    if (c->cur > NAV_END && (c->cur - NAV_END) % FIGHT == 0) {
        room_result(&c->room, (int)(c->st >> 7) % 3 == 2 ? -1 : (int)(c->st >> 3) & 1);
        c->results++;
    }
}

static void vsync(Console *c, int ms_since)
{
    if (!c->on || c->gone) return;
    room_poll(&c->room);
    uint32_t want = room_session(&c->room);
    if (want != c->sess) {           /* a new match (or none): restart the game */
        c->sess = want;
        c->cur = 0;
        c->st = 12345;
        c->began = 0;
        c->restart_left = want ? 1500 + (int)(rnd() % 1500) : 0;
        return;
    }
    if (!c->sess) return;
    if (c->restart_left > 0) { c->restart_left -= ms_since; return; }
    if (c->cur < NAV_END) {
        for (int i = 0; i < c->nav_per_vsync && c->cur < NAV_END; i++) step(c, 0, 0);
        if (c->cur == NAV_END) room_begin(&c->room, NAV_END);
        return;
    }
    int slot = room_slot(&c->room);
    if (slot >= 0) {
        if (room_frame_advantage(&c->room, c->cur) >= 1 && c->steps - c->last_throttle >= 10) {
            c->throttles++;
            c->last_throttle = c->steps;
            return;
        }
        room_set_input(&c->room, c->cur, pad(c));
    }
    int n = 0;
    while (n < (slot >= 0 ? 2 : 3) && room_ready(&c->room, c->cur)) {
        uint16_t in[2];
        room_inputs(&c->room, c->cur, in);
        step(c, in[0], in[1]);
        n++;
        if (slot >= 0 && room_frame_advantage(&c->room, c->cur) >= 0) break;
        if (slot >= 0) room_set_input(&c->room, c->cur, pad(c));
    }
    if (!n) c->stalls++;
    c->steps++;
}

static void setup(int n, uint32_t build_odd)
{
    nq = 0;
    nsess = 0;
    memset(reclen, 0, sizeof reclen);
    memset(blocked, 0, sizeof blocked);
    for (int i = 0; i < NC; i++) {
        Console *c = &C[i];
        memset(c, 0, sizeof *c);
        c->id = i;
        c->ctx.id = i;
        c->on = i < n;
        c->corrupt_at = -1;
        c->nav_per_vsync = 6 + i * 3;
        c->next_vsync = (double)clock_ms + i * 3.7;
        NetIo io = { &c->ctx, sim_send, sim_recv, sim_now };
        NetHello h = { (i == n - 1 && build_odd) ? 0xBAD : 0xABCD, 0x1234, "0.3" };
        char name[16];
        snprintf(name, sizeof name, "P%c", 'A' + i);
        room_init(&c->room, &io, &h, name, addr_of(i));
        snprintf(c->room.pid, sizeof c->room.pid, "player%06d", i + 1);
    }
}

static void run_ms(int ms)
{
    for (int k = 0; k < ms; k++) {
        clock_ms++;
        for (int i = 0; i < NC; i++) {
            Console *c = &C[i];
            if (!c->on) continue;
            if (clock_ms >= c->next_vsync) {
                double per = 1000.0 / 59.8261 * (1.0 + i * 0.0002);
                vsync(c, (int)per);
                c->next_vsync += per;
            }
        }
    }
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void join_all(int n, int mode)
{
    room_host(&C[0].room, mode);
    for (int i = 1; i < n; i++) {
        room_join(&C[i].room, addr_of(0), (NetAddr){ 0, 0 });
        run_ms(400);
    }
    run_ms(1500);
}

/* every pair of consoles in each match: identical over the frames both ran */
static int identical(int *frames_out, int *pairs_out)
{
    int ok = 1, frames = 0, pairs = 0;
    for (int s = 0; s < nsess; s++)
        for (int a = 0; a < NC; a++)
            for (int b = a + 1; b < NC; b++) {
                int n = reclen[s][a] < reclen[s][b] ? reclen[s][a] : reclen[s][b];
                if (n <= NAV_END) continue;
                pairs++;
                for (int f = 0; f < n; f++)
                    if (rec[s][a][f] != rec[s][b][f]) {
                        printf("  match %d: consoles %d and %d differ at frame %d\n", s, a, b, f);
                        ok = 0;
                        break;
                    }
                if (n > frames) frames = n;
            }
    if (frames_out) *frames_out = frames;
    if (pairs_out) *pairs_out = pairs;
    return ok;
}

static void summary(const char *name)
{
    int frames, pairs;
    int ok = identical(&frames, &pairs);
    printf("%-40s matches %d, longest %d frames, %d console pairs identical: %s, ping %d ms\n", name, nsess, frames,
           pairs, ok ? "yes" : "NO", room_ping(&C[0].room));
    CHECK(ok, "%s: consoles differ", name);
}

int main(void)
{
    rec = calloc(MAXS, sizeof *rec);
    printf("room_test: simulated consoles, boot hands over at frame %d, fights last %d frames\n\n", NAV_END, FIGHT);

    /* 1v1: the host and the first to join play, two watch */
    cfg.lat = 25; cfg.jitter = 10; cfg.loss_pct = 2;
    setup(4, 0);
    join_all(4, ROOM_1V1);
    CHECK(room_count(&C[0].room) == 4, "1v1: %d members", room_count(&C[0].room));
    {
        int ids = 1;
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < 4; i++) {
                char want[13];
                snprintf(want, sizeof want, "player%06d", i + 1);
                if (strcmp(C[c].room.info.m[i].pid, want)) ids = 0;
            }
        printf("player ids: every console knows every member's id: %s\n", ids ? "yes" : "NO");
        CHECK(ids, "player ids not passed on");
    }
    CHECK(room_can_start(&C[0].room), "1v1: cannot start");
    room_start(&C[0].room);
    run_ms(90000);
    printf("1v1: P%c vs P%c, results so far: %s; wins A %d B %d; spectators C/D at frames %d/%d (players %d/%d)\n",
           'A' + C[0].room.info.p[0], 'A' + C[0].room.info.p[1], C[0].room.info.result, C[0].room.info.m[0].wins,
           C[0].room.info.m[1].wins, C[2].cur, C[3].cur, C[0].cur, C[1].cur);
    CHECK(C[0].room.info.m[0].played >= 2, "1v1: no results recorded");
    CHECK(C[2].cur > C[0].cur - 200 && C[3].cur > C[0].cur - 200, "1v1: spectators fell behind");
    CHECK(C[0].room.desync < 0 && C[2].room.desync < 0, "1v1: false desync");
    summary("1v1 with two watching");

    /* King of the Hill with five */
    setup(5, 0);
    join_all(5, ROOM_KOTH);
    room_start(&C[0].room);
    int rounds = 0;
    uint32_t last = 0;
    char log[512] = "";
    for (int t = 0; t < 400; t++) {
        run_ms(1000);
        uint32_t s = C[0].room.info.session;
        if (s && s != last) {
            last = s;
            rounds++;
            char b[32];
            snprintf(b, sizeof b, "%c%c ", 'A' + C[0].room.info.p[0], 'A' + C[0].room.info.p[1]);
            if (strlen(log) + strlen(b) < sizeof log) strcat(log, b);
        }
        if (rounds >= 7) break;
    }
    run_ms(30000);
    printf("King of the Hill rounds (P1 P2): %s\n  wins:", log);
    int everyone = 1;
    for (int i = 0; i < 5; i++) {
        printf(" %c=%d/%d", 'A' + i, C[0].room.info.m[i].wins, C[0].room.info.m[i].played);
        if (!C[0].room.info.m[i].played) everyone = 0;
    }
    printf("\n");
    CHECK(rounds >= 7, "KOTH: only %d rounds", rounds);
    CHECK(everyone, "KOTH: someone never played");
    summary("King of the Hill, five consoles");

    /* players who cannot reach each other: the host relays */
    setup(4, 0);
    join_all(4, ROOM_KOTH);
    blocked[1][2] = blocked[2][1] = 1;
    blocked[1][3] = blocked[3][1] = blocked[2][3] = blocked[3][2] = 1;
    room_start(&C[0].room);
    int relayed = 0;
    for (int t = 0; t < 300 && relayed < 2; t++) {
        run_ms(1000);
        int a = C[0].room.info.p[0], b = C[0].room.info.p[1];
        if (C[0].room.info.session && a > 0 && b > 0 && C[a].cur > NAV_END + FIGHT - 5) relayed++;
    }
    printf("relay: rounds between two guests that cannot reach each other: %d\n", relayed);
    CHECK(relayed >= 1, "relay: no relayed round finished");
    summary("players relayed by the host");

    /* someone leaves mid-match, then the host leaves */
    setup(4, 0);
    join_all(4, ROOM_KOTH);
    room_start(&C[0].room);
    run_ms(20000);
    int pl = C[0].room.info.p[1] > 0 ? C[0].room.info.p[1] : C[0].room.info.p[0];
    room_leave(&C[pl].room, "");
    C[pl].gone = 1;
    run_ms(12000);
    printf("leave: P%c left mid-match: host says \"%s\", result \"%s\", %d members, now P%c vs P%c\n", 'A' + pl,
           room_message(&C[0].room), C[0].room.info.result, room_count(&C[0].room), 'A' + C[0].room.info.p[0],
           'A' + C[0].room.info.p[1]);
    CHECK(room_count(&C[0].room) == 3 && C[0].room.info.session && C[0].room.info.p[0] != pl && C[0].room.info.p[1] != pl,
          "leave: the room did not carry on");
    room_leave(&C[0].room, "");
    C[0].gone = 1;
    run_ms(2000);
    printf("host leaves: a member says \"%s\"\n", room_message(&C[1 == pl ? 2 : 1].room));
    CHECK(C[1 == pl ? 2 : 1].room.phase == RP_CLOSED, "host left but members stayed");

    /* a watcher's game goes wrong */
    setup(3, 0);
    join_all(3, ROOM_1V1);
    C[2].corrupt_at = NAV_END + 500;
    room_start(&C[0].room);
    run_ms(40000);
    printf("divergence: host found it at frame %ld (member P%c), watcher at %ld; dump frame %ld / %ld\n",
           (long)C[0].room.desync, 'A' + (C[0].room.desync_member < 0 ? 0 : C[0].room.desync_member),
           (long)C[2].room.desync, (long)C[0].room.dump_at, (long)C[2].room.dump_at);
    CHECK(C[0].room.desync == NAV_END + 500 && C[0].room.desync_member == 2, "divergence not found");
    CHECK(C[1].room.desync < 0, "a player was blamed for the watcher's divergence");

    /* refusals */
    setup(2, 1);
    room_host(&C[0].room, ROOM_1V1);
    room_join(&C[1].room, addr_of(0), (NetAddr){ 0, 0 });
    run_ms(2000);
    printf("different build: \"%s\"\n", room_message(&C[1].room));
    CHECK(C[1].room.phase == RP_CLOSED && room_count(&C[0].room) == 1, "different build accepted");
    setup(2, 0);
    room_join(&C[1].room, addr_of(0), (NetAddr){ 0, 0 });
    C[0].on = 0;
    run_ms(21000);
    printf("nobody there: \"%s\"\n", room_message(&C[1].room));
    CHECK(C[1].room.phase == RP_CLOSED, "join never gave up");

    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails != 0;
}
