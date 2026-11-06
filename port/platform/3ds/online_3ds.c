/*
 * Online VS on the 3DS: rooms of up to eight consoles over Wi-Fi, 1v1 or
 * King of the Hill, public (listed) or private (joined by code), with no
 * game server: a room is hosted by a player's console (port/platform/net/
 * room.c) and a small lobby directory (port/server/lobby, a Cloudflare
 * Worker, reached through lobby_3ds.c) only lists rooms, turns 6-digit codes
 * into addresses and tells a room's host who is joining, so both consoles
 * send to each other at once and their routers let the traffic through.
 * Without a lobby address a room can still be joined by its IP address.
 *
 * VS on the main menu opens the Online VS screen instead of the local
 * two-player mode: Find rooms, Create room (mode, public / private), Join
 * with code (or an address), and the one-console Sync test.
 *
 * A match: every member of the room restarts the game to its power-on state
 * (port/host/game_restart.c) with the same empty memory card, everything
 * unlocked and the same clock, boots it with the same scripted presses into
 * VS's character select (fast, the picture skips frames, sound off), and from
 * there runs in lockstep: P1's and P2's pads from the two players' consoles,
 * a fixed ONLINE_DELAY frames of input delay; everyone else watches the same
 * match.  Each console fingerprints its game memory every frame and the links
 * compare them; a difference is reported, and both consoles write their
 * memory at an agreed frame (mvc2_desync_*.bin).
 *
 * The fight's result is read from the game (each side's three characters'
 * health) by every console; the host keeps the scores and, in King of the
 * Hill, starts the next round (the winner stays, the loser goes to the back
 * of the queue) a few seconds after the result, which restarts everyone's
 * game again.  In 1v1 the two keep playing VS (rematches) until one leaves.
 *
 * The leaderboard (port/server/lobby): after every fight both players'
 * consoles report the result with their player id and its secret key
 * (online_id.txt, made once with the 3DS's random generator); the server
 * counts it when both reports agree.  The Leaderboard screen shows the
 * ratings, and the same list is a web page at the lobby's address.
 *
 * The sync test (one console): the same restart and boot twice, then 50
 * seconds of the same random presses for both players, and every frame's
 * fingerprint of the second run compared with the first.
 */
#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../host/host_loop.h"
#include "../../host/host.h"
#include "../../audio/audio.h"
#include "../net/room.h"
#include "lobby_3ds.h"
#include "movelist_3ds.h"
#include "online_3ds.h"
#include "sys_3ds.h"
#include "update_3ds.h"

void fe_restart(void);
void r3ds_mesh_sync(void);
extern u32 host_ee_elf_crc;

#define ONLINE_CARD "mvc2_memcard_online"
#define TEST_PLAY 3000          /* frames of random play in the sync test */
#define NAV_LIMIT 9000          /* the boot must reach VS by then */
#define TICKS_MS (SYSCLOCK_ARM11 / 1000.0)
#define MAX_LISTED 40
#define PER_PAGE 5

enum { M_OFF, M_LOBBY, M_TEST };
enum { S_MENU, S_FIND, S_CREATE, S_CODE, S_JOINING, S_ROOM, S_TEST, S_MSG, S_BOARD };
enum { P_NONE, P_NAV, P_PLAY };
enum { B_FIND = 1, B_CREATE, B_CODE, B_TEST, B_BACK, B_CANCEL, B_JOIN, B_START, B_LEAVE, B_OK,
       B_MODE, B_VIS, B_OPEN, B_REFRESH, B_MORE, B_BOARD, B_ROW = 40 };

typedef struct Listed { char code[8], name[26], mode[6]; int players, max, playing; } Listed;

static struct {
    int mode, screen, phase;
    char dir[256];
    char local_card[64];
    int local_unlock;
    uint32_t build;
    char name[16];
    char pid[13], pkey[33];   /* leaderboard identity (online_id.txt) */
    /* network */
    int soc, fd;
    uint16_t port;
    uint32_t local_ip;
    NetAddr pub;
    int have_pub;
    NetAddr stun[3];
    int nstun;
    volatile int dns_done;
    Thread dns_thr;
    uint8_t tid[12];
    u64 t_stun, t_up;
    int stun_tries;
    Room room;
    /* the lobby directory */
    int c_mode, c_public;              /* Create room's choices */
    char lid[16], secret[24], code[8], lobby_err[96];
    int beat_ticket;
    u64 t_beat;
    int list_ticket;
    u64 t_list;
    Listed listed[MAX_LISTED];
    int nlisted, page, list_err;
    char list_msg[96];
    int join_ticket;
    char join_code[8];
    u64 t_join;
    char entry[32];
    char last_peer[32];
    /* the game */
    uint32_t sess;            /* the match the game runs */
    int restarted;            /* the game was restarted for online play / the test */
    int pending;              /* the test's next run: a restart at the next refresh */
    long nav_end, last_throttle;
    uint16_t buttons;
    int steps_this;
    double step_ms;
    u64 t_refresh;
    int muted, suppress;
    uint16_t prev_buttons;
    int dumped;
    long desync_logged;
    /* the fight's result */
    int fw_in, fw_seen[2], fw_ko, fw_done;
    int fights;               /* results so far in this match (the leaderboard's match id) */
    int report_ticket;
    char my_rating[24];
    /* the Leaderboard screen */
    int board_ticket, me_ticket, board_page, board_total;
    u64 t_board;
    char board[PER_PAGE][64], board_me[64], board_msg[96];
    /* the test */
    int pass;
    uint32_t *th;
    long th_n, th_cap, fail;
    MvcHashSlice fail_where;
    uint32_t regions[3][4];
    char msg[160], msg2[160];
    OnlineView view, shown;
    int view_on;
} O;

/* ---------------------------------------------------------------- log */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    char b[256];
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("online: %s\n", b);
}

static void fmt_addr(const NetAddr *a, char *b, int n) { net_format_addr(a, b, n, 1); }

/* ------------------------------------------------------------- sockets */
static int io_send(void *ctx, const NetAddr *to, const void *buf, int n)
{
    (void)ctx;
    if (O.fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to->port);
    sa.sin_addr.s_addr = htonl(to->ip);
    return (int)sendto(O.fd, buf, (size_t)n, 0, (struct sockaddr *)&sa, sizeof sa);
}

static int io_recv(void *ctx, NetAddr *from, void *buf, int cap)
{
    (void)ctx;
    for (int guard = 0; guard < 64 && O.fd >= 0; guard++) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int n = (int)recvfrom(O.fd, buf, (size_t)cap, 0, (struct sockaddr *)&sa, &sl);
        if (n <= 0) return 0;
        const uint8_t *b = (const uint8_t *)buf;
        if (n >= 20 && b[0] == 0x01 && b[1] == 0x01) {          /* a STUN answer */
            NetAddr a;
            if (!O.have_pub && net_stun_parse(b, n, O.tid, &a) == 0) {
                O.pub = a;
                O.have_pub = 1;
                O.room.pub = a;
                char s[32];
                fmt_addr(&a, s, sizeof s);
                say("internet address %s", s);
            }
            continue;
        }
        from->ip = ntohl(sa.sin_addr.s_addr);
        from->port = ntohs(sa.sin_port);
        return n;
    }
    return 0;
}

static uint64_t io_now(void *ctx)
{
    (void)ctx;
    return osGetTime();
}

static void dns_main(void *arg)
{
    static const struct { const char *host; uint16_t port; } srv[3] = {
        { "stun.l.google.com", 19302 }, { "stun1.l.google.com", 19302 }, { "stun.cloudflare.com", 3478 },
    };
    (void)arg;
    int k = 0;
    for (int i = 0; i < 3; i++) {
        struct hostent *h = gethostbyname(srv[i].host);
        if (h && h->h_addr_list && h->h_addr_list[0]) {
            struct in_addr a;
            memcpy(&a, h->h_addr_list[0], sizeof a);
            O.stun[k].ip = ntohl(a.s_addr);
            O.stun[k].port = srv[i].port;
            k++;
        }
    }
    O.nstun = k;
    O.dns_done = 1;
}

static void room_setup(void)
{
    NetIo io = { NULL, io_send, io_recv, io_now };
    NetHello h;
    memset(&h, 0, sizeof h);
    h.build = O.build;
    h.disc = host_ee_elf_crc;
    snprintf(h.version, sizeof h.version, "%s", update_version());
    room_init(&O.room, &io, &h, O.name, (NetAddr){ O.local_ip, O.port });
    memcpy(O.room.pid, O.pid, sizeof O.room.pid);
    O.room.pub = O.pub;
}

static int net_up(void)
{
    if (O.soc) { room_setup(); return 0; }
    if (sys3ds_soc_acquire()) {
        snprintf(O.msg, sizeof O.msg, "The network did not start (is Wi-Fi on?)");
        return -1;
    }
    O.soc = 1;
    O.local_ip = ntohl((uint32_t)gethostid());
    O.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (O.fd < 0 || !O.local_ip) {
        if (O.fd >= 0) close(O.fd);
        O.fd = -1;
        sys3ds_soc_release();
        O.soc = 0;
        snprintf(O.msg, sizeof O.msg, "Not connected: turn Wi-Fi on and connect to the internet");
        return -1;
    }
    O.port = 0;
    for (int p = NET_PORT; p < NET_PORT + 8 && !O.port; p++) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)p);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(O.fd, (struct sockaddr *)&sa, sizeof sa) == 0) O.port = (uint16_t)p;
    }
    fcntl(O.fd, F_SETFL, fcntl(O.fd, F_GETFL, 0) | O_NONBLOCK);
    O.have_pub = 0;
    O.dns_done = 0;
    O.nstun = 0;
    O.stun_tries = 0;
    O.t_stun = 0;
    O.t_up = osGetTime();
    for (int i = 0; i < 12; i++) O.tid[i] = (uint8_t)(rand() ^ (osGetTime() >> (i & 7)));
    room_setup();
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    O.dns_thr = threadCreate(dns_main, NULL, 32 * 1024, prio + 1, 1, false);
    if (!O.dns_thr) O.dns_thr = threadCreate(dns_main, NULL, 32 * 1024, prio + 1, -1, false);
    char a[32];
    fmt_addr(&(NetAddr){ O.local_ip, O.port }, a, sizeof a);
    say("network up: %s", a);
    return 0;
}

static void net_down(void)
{
    if (!O.soc) return;
    if (O.dns_thr) {
        threadJoin(O.dns_thr, U64_MAX);
        threadFree(O.dns_thr);
        O.dns_thr = NULL;
    }
    if (O.fd >= 0) close(O.fd);
    O.fd = -1;
    sys3ds_soc_release();
    O.soc = 0;
}

static int stun_settled(void)
{
    return O.have_pub || (O.dns_done && (!O.nstun || O.stun_tries >= 8)) || osGetTime() - O.t_up > 4000;
}

static void stun_poll(void)
{
    if (!O.soc || O.have_pub || !O.dns_done || !O.nstun || O.stun_tries >= 8) return;
    u64 t = osGetTime();
    if (t - O.t_stun < 400) return;
    uint8_t rq[20];
    net_stun_request(rq, O.tid);
    io_send(NULL, &O.stun[O.stun_tries % O.nstun], rq, 20);
    O.t_stun = t;
    O.stun_tries++;
}

/* the 3DS user name (System Settings), as plain ASCII */
static void user_name(char *out, size_t n)
{
    snprintf(out, n, "Player");
    if (R_FAILED(cfguInit())) return;
    u16 w[0x1C / 2];
    memset(w, 0, sizeof w);
    if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, w))) {
        size_t k = 0;
        for (int i = 0; i < 10 && w[i] && k + 1 < n; i++)
            out[k++] = (char)(w[i] >= 0x20 && w[i] < 0x7F ? w[i] : '?');
        if (k) out[k] = 0;
    }
    cfguExit();
}

/* The leaderboard identity: a public player id and a secret key, made once
 * from the 3DS's random generator and kept in online_id.txt (back it up to
 * keep your rating on another SD card). */
static void identity(void)
{
    static const char abc[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char p[300];
    snprintf(p, sizeof p, "%s/online_id.txt", O.dir);
    FILE *f = fopen(p, "r");
    if (f) {
        char a[64] = "", b[64] = "";
        if (fgets(a, sizeof a, f) && fgets(b, sizeof b, f)) {
            a[strcspn(a, "\r\n")] = 0;
            b[strcspn(b, "\r\n")] = 0;
            if (strlen(a) == 12 && strspn(a, abc) == 12 && strlen(b) >= 16 && strlen(b) < sizeof O.pkey) {
                snprintf(O.pid, sizeof O.pid, "%s", a);
                snprintf(O.pkey, sizeof O.pkey, "%s", b);
            }
        }
        fclose(f);
        if (O.pid[0]) return;
    }
    u8 rb[44];
    int ok = R_SUCCEEDED(psInit());
    if (!ok || R_FAILED(PS_GenerateRandomBytes(rb, sizeof rb))) {
        for (size_t i = 0; i < sizeof rb; i++) rb[i] = (u8)(svcGetSystemTick() >> (i % 7) ^ osGetTime() * 31 ^ rand());
    }
    if (ok) psExit();
    for (int i = 0; i < 12; i++) O.pid[i] = abc[rb[i] % 36];
    O.pid[12] = 0;
    for (int i = 0; i < 32; i++) O.pkey[i] = abc[rb[12 + i] % 36];
    O.pkey[32] = 0;
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", O.pid, O.pkey);
        fclose(f);
    }
    say("new player id %s", O.pid);
}

static void report_fight(int w)
{
    const RoomInfo *in = &O.room.info;
    int s = room_slot(&O.room);
    int fight = ++O.fights;
    if (s < 0 || !lobby_available() || !O.pid[0]) return;
    int other = in->p[1 - s];
    if (other < 0 || other >= ROOM_MAX || strlen(in->m[other].pid) != 12) return;
    char f[512] = "", match[24];
    snprintf(match, sizeof match, "%x-%d", (unsigned)in->session, fight);
    lobby_form(f, sizeof f, "pid", O.pid);
    lobby_form(f, sizeof f, "key", O.pkey);
    lobby_form(f, sizeof f, "name", O.name);
    lobby_form(f, sizeof f, "match", match);
    lobby_form(f, sizeof f, "opp", in->m[other].pid);
    lobby_form(f, sizeof f, "result", w < 0 ? "draw" : w == s ? "win" : "loss");
    if (O.report_ticket > 0) lobby_forget(O.report_ticket);
    O.report_ticket = lobby_request("/v1/report", f, 0);
    if (O.report_ticket < 0) O.report_ticket = 0;
}

/* ------------------------------------------------------------- the game */
static void wipe_dir(const char *path, int depth)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char p[512];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (depth > 0) wipe_dir(p, depth - 1);
            rmdir(p);
        } else {
            remove(p);
        }
    }
    closedir(d);
}

static void mute(int on)
{
    if (on == O.muted) return;
    audio_set_master(on ? 0.0f : 1.0f);
    O.muted = on;
}

/* the game back to power-on and booted again: for online play (online = 1)
 * or for normal play */
static void restart_game(int online, int64_t epoch)
{
    u64 t0 = osGetTime();
    r3ds_mesh_sync();
    audio_restart();
    fe_restart();
    mvc2_restart();
    if (online) {
        char p[300];
        snprintf(p, sizeof p, "%s/%s", O.dir, ONLINE_CARD);
        wipe_dir(p, 2);
        setenv("MVC2_MEMCARD", ONLINE_CARD, 1);
        mvc2_unlock_all = 1;
        audio_set_deterministic(1);
        host_clock_fixed = 1;
        host_clock_epoch = epoch;
    } else {
        setenv("MVC2_MEMCARD", O.local_card, 1);
        mvc2_unlock_all = O.local_unlock;
        audio_set_deterministic(0);
        host_clock_fixed = 0;
    }
    mvc2_boot(1);
    say("game restarted for %s in %lu ms", online ? (O.mode == M_TEST ? "the sync test" : "a match") : "normal play",
        (unsigned long)(osGetTime() - t0));
    O.restarted = online;
    O.phase = online ? P_NAV : P_NONE;
    O.nav_end = -1;
    O.last_throttle = 0;
    O.dumped = 0;
    O.step_ms = 4.0;
    O.fw_in = O.fw_done = 0;
    O.fw_seen[0] = O.fw_seen[1] = 0;
    O.fw_ko = -1;
    O.fights = 0;
    O.desync_logged = 0;
    mute(online);
    movelist_redraw();
}

#define G ((u8 *)EE_BSS_PTR(0x004F4300))
static int at_vs_select(void) { return G[0] == 2 && G[1] == 1 && G[2] == 1; }

/* the scripted presses from power-on to VS's character select: START
 * through the logos and the title, then on the main menu RIGHT until the
 * cursor (g+8) is on VS (1) and START.  The same on every console. */
static uint16_t nav_buttons(long f)
{
    const u8 *g = G;
    if (g[0] == 3 && g[1] == 1 && g[2] == 1) {
        s16 cursor = *(const s16 *)(g + 8), wait = *(const s16 *)(g + 0xA);
        if (wait != 0 || (f & 1)) return 0;
        return cursor != 1 ? PS2_RIGHT : PS2_START;
    }
    if (g[0] == 3) return 0;
    if (g[0] == 1 && g[1] == 7) return f % 30 == 0 ? PS2_START : 0;
    if (g[0] == 1) return f % 40 == 0 ? PS2_START : f % 40 == 20 ? PS2_CROSS : 0;
    return 0;
}

/* the sync test's players: buttons held a few frames, never START / SELECT */
static uint16_t test_pad(long f, int player)
{
    uint32_t h = (uint32_t)(f / 6) * 2246822519u ^ (uint32_t)(player + 1) * 3266489917u;
    h ^= h >> 15;
    h *= 668265263u;
    h ^= h >> 13;
    uint16_t b = 0;
    static const uint16_t dirs[8] = { PS2_UP, PS2_DOWN, PS2_LEFT, PS2_RIGHT, PS2_DOWN | PS2_RIGHT,
                                      PS2_DOWN | PS2_LEFT, PS2_UP | PS2_RIGHT, 0 };
    b |= dirs[h & 7];
    if ((h >> 3) & 1) b |= PS2_SQUARE;
    if ((h >> 4) % 3 == 0) b |= PS2_CROSS;
    if ((h >> 6) % 5 == 0) b |= PS2_TRIANGLE;
    if ((h >> 9) % 5 == 0) b |= PS2_CIRCLE;
    if ((h >> 12) % 9 == 0) b |= PS2_L1;
    if ((h >> 16) % 11 == 0) b |= PS2_R1;
    return b;
}

static PadRaw pad_of(uint16_t b)
{
    PadRaw p;
    memset(&p, 0, sizeof p);
    p.lx = p.ly = p.rx = p.ry = 0x80;
    p.connected = 1;
    p.buttons = b;
    return p;
}

/* The fight's result, from the game: each side's three characters (player
 * records 0/2/4 and 1/3/5, health at +0x434).  A side with none left
 * standing lost (once both sides had been seen alive, so the first frames
 * of a fight, before the health is set, do not count); a fight that ends
 * otherwise (time over) goes to the side with more health left. */
#define PLAYERS ((u8 *)EE_BSS_PTR(0x004F20B0))
static void side_health(int out[2], int alive[2])
{
    for (int s = 0; s < 2; s++) {
        out[s] = alive[s] = 0;
        for (int t = 0; t < 3; t++) {
            s16 hp = *(s16 *)(PLAYERS + (s + t * 2) * 0x5B8 + 0x434);
            if (hp > 0) { out[s] += hp; alive[s] = 1; }
        }
    }
}

static int fight_watch(void)
{
    const u8 *g = G;
    int in = g[0] == 2 && g[1] == 1 && g[2] == 2;
    int hp[2], alive[2];
    if (in) {
        if (!O.fw_in) { O.fw_in = 1; O.fw_done = 0; O.fw_seen[0] = O.fw_seen[1] = 0; O.fw_ko = -1; }
        side_health(hp, alive);
        for (int s = 0; s < 2; s++) O.fw_seen[s] |= alive[s];
        if (!O.fw_done && O.fw_seen[0] && O.fw_seen[1] && alive[0] != alive[1]) {
            O.fw_done = 1;
            return alive[0] ? 0 : 1;   /* a knockout */
        }
        return -2;
    }
    if (O.fw_in) {
        O.fw_in = 0;
        if (!O.fw_done && O.fw_seen[0] && O.fw_seen[1]) {
            O.fw_done = 1;
            side_health(hp, alive);
            return hp[0] > hp[1] ? 0 : hp[1] > hp[0] ? 1 : -1;
        }
    }
    return -2;
}

/* -------------------------------------------------------------- screens */
static void show_msg(const char *a, const char *b)
{
    O.screen = S_MSG;
    snprintf(O.msg, sizeof O.msg, "%s", a ? a : "");
    snprintf(O.msg2, sizeof O.msg2, "%s", b ? b : "");
}

static void lobby_close_room(void)
{
    if (O.lid[0] && O.room.host) {
        char f[128] = "";
        lobby_form(f, sizeof f, "id", O.lid);
        lobby_form(f, sizeof f, "secret", O.secret);
        int t = lobby_request("/v1/close", f, 0);
        if (t >= 0) lobby_forget(t);
    }
    O.lid[0] = O.secret[0] = O.code[0] = 0;
    if (O.beat_ticket > 0) lobby_forget(O.beat_ticket);
    O.beat_ticket = 0;
}

static void leave_room(const char *why)
{
    if (O.room.phase == RP_IN_ROOM || O.room.phase == RP_JOINING) room_leave(&O.room, why);
    lobby_close_room();
    O.sess = 0;
    if (O.soc) room_setup();   /* idle again (not "closed") */
}

static void close_lobby(void)
{
    leave_room("");
    if (O.list_ticket > 0) lobby_forget(O.list_ticket);
    if (O.join_ticket > 0) lobby_forget(O.join_ticket);
    O.list_ticket = O.join_ticket = 0;
    net_down();
    O.mode = M_OFF;
    O.suppress = 1;
    movelist_online(NULL);
    O.view_on = 0;
}

/* online play is over: normal play again */
static void back_to_local(void)
{
    if (O.restarted) restart_game(0, 0);
    mute(0);
    free(O.th);
    O.th = NULL;
    O.th_n = O.th_cap = 0;
    close_lobby();
}

static void to_menu(void)
{
    leave_room("");
    O.screen = S_MENU;
    O.msg[0] = O.msg2[0] = 0;
}

static void begin_test(void)
{
    O.mode = M_TEST;
    O.screen = S_TEST;
    O.pass = 0;
    O.fail = -1;
    O.th_cap = NAV_LIMIT + TEST_PLAY + 8;
    O.th = (uint32_t *)calloc((size_t)O.th_cap, 4);
    O.th_n = 0;
    say("sync test: run 1 (record)");
    restart_game(1, 1700000000LL);
}

static void test_pass_done(void)
{
    mvc2_state_hash_regions(O.regions[O.pass]);
    say("sync test run %d done at frame %ld: data %08X bss %08X scratch %08X heap %08X", O.pass + 1, mvc2_host_frame,
        O.regions[O.pass][0], O.regions[O.pass][1], O.regions[O.pass][2], O.regions[O.pass][3]);
    if (O.pass == 0) {
        O.pass = 1;
        say("sync test: run 2 (compare)");
        O.pending = 1;
        return;
    }
    O.phase = P_NONE;
    mute(0);
    if (O.pass == 1 && O.fail < 0) {
        int same = !memcmp(O.regions[0], O.regions[1], sizeof O.regions[0]);
        say("sync test: PASSED (%ld frames identical%s)", O.th_n, same ? ", final memory identical" : "");
        show_msg("Sync test passed", same ? "Both runs were identical, frame for frame. Online play should stay in sync."
                                          : "Every frame matched (the final memory check differed: see the log).");
        return;
    }
    char a[160];
    snprintf(a, sizeof a, "Run 2 differed from run 1 at frame %ld (%s). Please send mvc2_log.txt and the two "
             "mvc2_synctest_*.bin files.", O.fail, O.nav_end >= 0 && O.fail < O.nav_end ? "while starting" : "while playing");
    show_msg("Sync test FAILED", a);
}

static void start_join_code(const char *code)
{
    if (net_up()) { show_msg("Online VS", O.msg); return; }
    snprintf(O.join_code, sizeof O.join_code, "%s", code);
    O.join_ticket = 0;
    O.t_join = osGetTime();
    O.screen = S_JOINING;
    O.msg2[0] = 0;
    say("joining room %s", code);
}

static void start_join_addr(NetAddr a)
{
    if (net_up()) { show_msg("Online VS", O.msg); return; }
    O.join_code[0] = 0;
    room_join(&O.room, a, (NetAddr){ 0, 0 });
    O.screen = S_JOINING;
    char s[32];
    fmt_addr(&a, s, sizeof s);
    say("joining %s", s);
}

void online_touch(int code)
{
    if (code >= MOVELIST_ONLINE_KEY) {
        int c = code - MOVELIST_ONLINE_KEY;
        size_t l = strlen(O.entry);
        if (c == '<') { if (l) O.entry[l - 1] = 0; }
        else if (c == '#') O.entry[0] = 0;
        else if (l < 21) { O.entry[l] = (char)c; O.entry[l + 1] = 0; }
        return;
    }
    int b = code - MOVELIST_ONLINE;
    if (b >= B_ROW && b < B_ROW + PER_PAGE) {
        int i = O.page * PER_PAGE + (b - B_ROW);
        if (O.screen == S_FIND && i < O.nlisted) start_join_code(O.listed[i].code);
        return;
    }
    switch (b) {
    case B_FIND:
        O.screen = S_FIND;
        O.page = 0;
        O.t_list = 0;
        O.list_msg[0] = 0;
        break;
    case B_CREATE:
        O.screen = S_CREATE;
        break;
    case B_BOARD:
        O.screen = S_BOARD;
        O.board_page = 0;
        O.t_board = 0;
        O.board_msg[0] = 0;
        break;
    case B_MODE:
        O.c_mode = O.c_mode == ROOM_KOTH ? ROOM_1V1 : ROOM_KOTH;
        if (O.screen == S_ROOM) room_set_mode(&O.room, O.c_mode);
        break;
    case B_VIS:
        O.c_public = !O.c_public;
        break;
    case B_OPEN:
        if (net_up()) { show_msg("Online VS", O.msg); break; }
        room_host(&O.room, O.c_mode);
        O.lobby_err[0] = 0;
        O.t_beat = 0;
        O.screen = S_ROOM;
        say("room open (%s, %s)", O.c_mode == ROOM_KOTH ? "King of the Hill" : "1v1", O.c_public ? "public" : "private");
        break;
    case B_CODE:
        snprintf(O.entry, sizeof O.entry, "%s", O.last_peer);
        O.msg2[0] = 0;
        O.screen = S_CODE;
        break;
    case B_JOIN: {
        NetAddr a;
        size_t n = strlen(O.entry);
        if (strchr(O.entry, '.')) {
            if (net_parse_addr(O.entry, &a)) { snprintf(O.msg2, sizeof O.msg2, "Not an address: type it like 84.12.3.200"); break; }
            start_join_addr(a);
        } else if (n == 6 && strspn(O.entry, "0123456789") == 6) {
            if (!lobby_available()) { snprintf(O.msg2, sizeof O.msg2, "No lobby server set: type the room's address"); break; }
            start_join_code(O.entry);
        } else {
            snprintf(O.msg2, sizeof O.msg2, "A room code has 6 digits");
            break;
        }
        snprintf(O.last_peer, sizeof O.last_peer, "%s", O.entry);
        char p[300];
        snprintf(p, sizeof p, "%s/online_last.txt", O.dir);
        FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "%s\n", O.last_peer); fclose(f); }
        break;
    }
    case B_START:
        if (room_can_start(&O.room)) {
            say("starting (%d in the room)", room_count(&O.room));
            room_start(&O.room);
        }
        break;
    case B_REFRESH:
        O.t_list = 0;
        O.t_board = 0;
        break;
    case B_MORE:
        if (O.screen == S_BOARD) {
            O.board_page = (O.board_page + 1) * PER_PAGE < O.board_total ? O.board_page + 1 : 0;
            O.t_board = 0;
        } else {
            O.page = (O.page + 1) * PER_PAGE < O.nlisted ? O.page + 1 : 0;
        }
        break;
    case B_TEST:
        begin_test();
        break;
    case B_CANCEL:
        to_menu();
        break;
    case B_LEAVE:
        if (O.mode == M_TEST) { say("sync test stopped"); back_to_local(); break; }
        say("left the room");
        if (O.restarted) back_to_local();   /* the game was online's: normal play again */
        else to_menu();
        break;
    case B_OK:
        if (O.restarted) back_to_local();
        else to_menu();
        break;
    case B_BACK:
        if (O.screen == S_MENU) close_lobby();
        else to_menu();
        break;
    }
}

/* --------------------------------------------------------------- view */
static void vline(int i, int tone, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void vline(int i, int tone, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(O.view.line[i], sizeof O.view.line[i], fmt, ap);
    va_end(ap);
    O.view.tone[i] = (unsigned char)tone;
}

static void vbtn(int id, const char *label, int accent)
{
    if (O.view.nbtn >= OV_BTNS) return;
    int k = O.view.nbtn++;
    O.view.btn_id[k] = id;
    snprintf(O.view.btn[k], sizeof O.view.btn[k], "%s", label);
    O.view.btn_tone[k] = (unsigned char)accent;
}

static void view_msg(void)
{
    OnlineView *v = &O.view;
    const char *src[2] = { O.msg, O.msg2 };
    int li = 0;
    for (int s = 0; s < 2 && li < OV_LINES; s++) {
        const char *p = src[s];
        while (*p && li < OV_LINES) {
            int n = (int)strlen(p), cut = n;
            if (n > 48) {
                cut = 48;
                while (cut > 20 && p[cut] != ' ') cut--;
            }
            snprintf(v->line[li], sizeof v->line[li], "%.*s", cut, p);
            v->tone[li] = (unsigned char)(s ? 0 : strstr(O.msg, "FAIL") || strstr(O.msg, "lost") || strstr(O.msg, "Could not") ||
                                                  strstr(O.msg, "No ") || strstr(O.msg, "closed") ? 3 : 2);
            li++;
            p += cut;
            while (*p == ' ') p++;
        }
        li++;
    }
    vbtn(B_OK, "OK", 1);
}

static void view_room(void)
{
    OnlineView *v = &O.view;
    const RoomInfo *in = &O.room.info;
    char a[40];
    int host = O.room.host;
    snprintf(v->title, sizeof v->title, "%.15s's room", in->m[0].used ? in->m[0].name : "?");
    if (O.code[0]) snprintf(a, sizeof a, "code %s%s", O.code, O.c_public && host ? ", listed" : "");
    else if (host && O.lobby_err[0]) snprintf(a, sizeof a, "not listed");
    else if (host && lobby_available()) snprintf(a, sizeof a, "getting a code...");
    else a[0] = 0;
    vline(0, 2, "%s%s%s", in->mode == ROOM_KOTH ? "King of the Hill" : "1v1", a[0] ? "  -  " : "", a);
    if (in->session && in->p[0] >= 0 && in->p[1] >= 0) {
        const char *role = room_slot(&O.room) == 0 ? "  (you: P1)" : room_slot(&O.room) == 1 ? "  (you: P2)" :
                           (in->session_mask & (1u << O.room.id)) ? "  (watching)" : "  (next match)";
        vline(1, 4, "%.12s vs %.12s%s", in->m[in->p[0]].name, in->m[in->p[1]].name, role);
    } else {
        vline(1, 1, "%s", room_count(&O.room) < 2 ? "Waiting for players..." :
                          host ? "Tap Start when everyone is here." : "Waiting for the host to start...");
    }
    if (in->result[0]) vline(2, 2, "Last: %s%s%s", in->result, O.my_rating[0] ? "  -  " : "", O.my_rating);
    else if (O.my_rating[0]) vline(2, 2, "%s", O.my_rating);
    /* the members, three per line: > you, * host, + next in line */
    int li = 3, nrow = 0;
    char row[80] = "";
    for (int i = 0; i < ROOM_MAX && li < 6; i++) {
        const RoomMember *m = &in->m[i];
        if (!m->used) continue;
        char e[32];
        snprintf(e, sizeof e, "%s%s%.8s %dW", i == O.room.id ? ">" : "", i == 0 ? "*" : in->mode == ROOM_KOTH && in->next == i ? "+" : "",
                 m->name, m->wins);
        if (nrow) strncat(row, "  ", sizeof row - strlen(row) - 1);
        strncat(row, e, sizeof row - strlen(row) - 1);
        if (++nrow == 3) { vline(li++, 0, "%s", row); row[0] = 0; nrow = 0; }
    }
    if (nrow && li < 6) vline(li++, 0, "%s", row);
    int ping = room_ping(&O.room);
    if (O.room.desync >= 0) vline(6, 3, "Out of sync (frame %ld): send the logs", (long)O.room.desync);
    else if (host && O.lobby_err[0]) vline(6, 3, "%s", O.lobby_err);
    else if (ping >= 0) vline(6, ping > 80 ? 3 : 1, "Ping %d ms%s", ping, ping > 80 ? ": the game will slow down" : "");
    else if (host) {
        fmt_addr(&(NetAddr){ O.local_ip, O.port }, a, sizeof a);
        vline(6, 1, "Same Wi-Fi: %s", a);
    }
    if (host && room_can_start(&O.room)) vbtn(B_START, "Start", 1);
    if (host && !in->session) vbtn(B_MODE, in->mode == ROOM_KOTH ? "Mode: KotH" : "Mode: 1v1", 0);
    vbtn(B_LEAVE, "Leave", 0);
}

static void build_view(void)
{
    OnlineView *v = &O.view;
    memset(v, 0, sizeof *v);
    snprintf(v->title, sizeof v->title, "Online VS");
    switch (O.screen) {
    case S_MENU:
        vline(0, 0, "Play VS against other New 3DS consoles.");
        vline(1, 1, "Find: public rooms.  Create: host a room");
        vline(2, 1, "(1v1 or King of the Hill).  Join with code:");
        vline(3, 1, "a friend's 6-digit room code or address.");
        if (!lobby_available()) vline(5, 3, "No lobby server set: rooms by address only.");
        else vline(5, 1, "Sync test: checks this console stays in sync.");
        if (lobby_available()) vbtn(B_FIND, "Find rooms", 1);
        vbtn(B_CREATE, "Create room", 1);
        vbtn(B_CODE, "Join code", 0);
        if (lobby_available()) vbtn(B_BOARD, "Leaderboard", 0);
        vbtn(B_TEST, "Sync test", 0);
        vbtn(B_BACK, "Back", 0);
        break;
    case S_BOARD:
        snprintf(v->title, sizeof v->title, "Leaderboard");
        vline(0, 2, "%s", O.board_me[0] ? O.board_me : "You: no results yet");
        if (O.board_msg[0]) vline(1, 3, "%s", O.board_msg);
        for (int k = 0; k < PER_PAGE; k++)
            if (O.board[k][0]) vline(1 + k, k == 0 && O.board_page == 0 ? 2 : 0, "%s", O.board[k]);
        if (!O.board[0][0] && !O.board_msg[0]) vline(1, 1, "%s", O.board_ticket > 0 ? "Loading..." : "No results yet.");
        {
            const char *u = lobby_url();
            vline(6, 1, "Web: %s", !strncmp(u, "https://", 8) ? u + 8 : u);
        }
        if (O.board_total > PER_PAGE) vbtn(B_MORE, "More", 0);
        vbtn(B_REFRESH, "Refresh", 0);
        vbtn(B_BACK, "Back", 0);
        break;
    case S_FIND: {
        snprintf(v->title, sizeof v->title, "Public rooms");
        if (O.list_msg[0]) vline(0, O.list_err ? 3 : 1, "%s", O.list_msg);
        else if (!O.nlisted) vline(0, 1, "%s", O.list_ticket > 0 ? "Looking..." : "No public rooms right now.");
        for (int k = 0; k < PER_PAGE; k++) {
            int i = O.page * PER_PAGE + k;
            if (i >= O.nlisted) break;
            const Listed *r = &O.listed[i];
            vline(1 + k, r->playing ? 0 : 4, "%.16s  %s %d/%d%s", r->name, !strcmp(r->mode, "koth") ? "KotH" : "1v1",
                  r->players, r->max, r->playing ? " playing" : "");
            v->line_id[1 + k] = B_ROW + k;
        }
        if (O.nlisted) vline(6, 1, "Tap a room to join.  %d room%s", O.nlisted, O.nlisted == 1 ? "" : "s");
        vbtn(B_REFRESH, "Refresh", 0);
        if (O.nlisted > PER_PAGE) vbtn(B_MORE, "More", 0);
        vbtn(B_BACK, "Back", 0);
        break;
    }
    case S_CREATE:
        snprintf(v->title, sizeof v->title, "Create a room");
        vline(0, 0, "%s", O.c_mode == ROOM_KOTH ? "King of the Hill: the winner stays on," : "1v1: you and the first to join play,");
        vline(1, 1, "%s", O.c_mode == ROOM_KOTH ? "the next in line challenges. Up to 8." : "rematches until one leaves; others watch.");
        vline(3, 0, "%s", O.c_public ? "Public: listed in Find rooms." : "Private: joined with its code only.");
        if (!lobby_available()) vline(5, 3, "No lobby server: joined by your address.");
        vbtn(B_MODE, O.c_mode == ROOM_KOTH ? "Mode: KotH" : "Mode: 1v1", 0);
        vbtn(B_VIS, O.c_public ? "Public" : "Private", 0);
        vbtn(B_OPEN, "Create", 1);
        vbtn(B_BACK, "Back", 0);
        break;
    case S_CODE:
        snprintf(v->title, sizeof v->title, "Join with code");
        v->keypad = 1;
        snprintf(v->entry, sizeof v->entry, "%s", O.entry);
        vline(0, O.msg2[0] ? 3 : 1, "%s", O.msg2[0] ? O.msg2 : "Room code (6 digits) or an IP address");
        vbtn(B_CANCEL, "Cancel", 0);
        vbtn(B_JOIN, "Join", 1);
        break;
    case S_JOINING: {
        snprintf(v->title, sizeof v->title, "Joining");
        char a[32];
        if (O.join_code[0] && O.room.phase != RP_JOINING) vline(0, 0, "Asking the lobby for room %s...", O.join_code);
        else {
            fmt_addr(&O.room.join_addr, a, sizeof a);
            vline(0, 0, "Connecting to %s...", a);
        }
        if (O.msg2[0]) vline(2, 3, "%s", O.msg2);
        vbtn(B_CANCEL, "Cancel", 0);
        break;
    }
    case S_ROOM:
        view_room();
        break;
    case S_TEST:
        snprintf(v->title, sizeof v->title, "Sync test: run %d of 2", O.pass > 1 ? 2 : O.pass + 1);
        vline(0, 0, "%s", O.pass == 2 ? "Repeating run 1 up to the difference..." :
              O.phase == P_NAV ? "Starting the game (fast)..." : "Random play for both players...");
        vline(1, 1, "Frame %ld", mvc2_host_frame / 60 * 60);
        if (O.fail >= 0) vline(2, 3, "Difference at frame %ld", O.fail);
        vline(4, 1, "Takes about two minutes. Stop: back to normal play.");
        vbtn(B_LEAVE, "Stop", 0);
        break;
    case S_MSG:
        snprintf(v->title, sizeof v->title, "%s", O.mode == M_TEST ? "Sync test" : "Online VS");
        view_msg();
        break;
    }
}

static void push_view(void)
{
    if (O.mode == M_OFF) return;
    build_view();
    if (!O.view_on || memcmp(&O.view, &O.shown, sizeof O.view)) {
        O.shown = O.view;
        O.view_on = 1;
        movelist_online(&O.view);
    }
}

/* ------------------------------------------------------------ the lobby */
static void parse_list(const char *body)
{
    O.nlisted = 0;
    for (const char *l = body; l && *l && O.nlisted < MAX_LISTED; ) {
        if (!strncmp(l, "room\t", 5)) {
            Listed r;
            memset(&r, 0, sizeof r);
            char state[12] = "";
            /* room<TAB>code name mode players max state (6 fields) */
            const char *f = l + 5;
            char fld[6][32];
            int k = 0;
            while (k < 6) {
                size_t n = strcspn(f, "\t\n");
                snprintf(fld[k], sizeof fld[k], "%.*s", (int)(n < 31 ? n : 31), f);
                k++;
                f += n;
                if (*f != '\t') break;
                f++;
            }
            if (k >= 6) {
                snprintf(r.code, sizeof r.code, "%s", fld[0]);
                snprintf(r.name, sizeof r.name, "%s", fld[1]);
                snprintf(r.mode, sizeof r.mode, "%s", fld[2]);
                r.players = atoi(fld[3]);
                r.max = atoi(fld[4]);
                snprintf(state, sizeof state, "%s", fld[5]);
                r.playing = !strcmp(state, "playing");
                O.listed[O.nlisted++] = r;
            }
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    if (O.page * PER_PAGE >= O.nlisted) O.page = 0;
}

static void board_poll(u64 t)
{
    char body[4096];
    int status = 0, r;
    if (O.screen != S_BOARD) return;
    if (O.board_ticket > 0 && (r = lobby_result(O.board_ticket, body, sizeof body, &status)) != 0) {
        O.board_ticket = 0;
        char e[96], tot[16];
        lobby_value(body, "error", e, sizeof e);
        if (r < 0 || strncmp(body, "ok", 2)) {
            snprintf(O.board_msg, sizeof O.board_msg, "%s", e[0] ? e : r < 0 ? body : "The lobby did not answer");
        } else {
            O.board_msg[0] = 0;
            O.board_total = atoi(lobby_value(body, "total", tot, sizeof tot));
            memset(O.board, 0, sizeof O.board);
            int k = 0;
            for (const char *l = body; l && *l && k < PER_PAGE; ) {
                if (!strncmp(l, "row\t", 4)) {
                    /* rank name rating wins losses draws best */
                    char fld[7][24];
                    const char *f = l + 4;
                    int n = 0;
                    while (n < 7) {
                        size_t m = strcspn(f, "\t\n");
                        snprintf(fld[n], sizeof fld[n], "%.*s", (int)(m < 23 ? m : 23), f);
                        n++;
                        f += m;
                        if (*f != '\t') break;
                        f++;
                    }
                    if (n == 7)
                        snprintf(O.board[k++], sizeof O.board[0], "%s. %.14s  %s  %s-%s%s%s", fld[0], fld[1], fld[2], fld[3], fld[4],
                                 atoi(fld[6]) >= 3 ? "  best streak " : "", atoi(fld[6]) >= 3 ? fld[6] : "");
                }
                l = strchr(l, '\n');
                if (l) l++;
            }
        }
    }
    if (O.me_ticket > 0 && (r = lobby_result(O.me_ticket, body, sizeof body, &status)) != 0) {
        O.me_ticket = 0;
        char rank[16], rating[16], w[16], l[16], d[16];
        if (r > 0 && !strncmp(body, "ok", 2) && !strstr(body, "none"))
            snprintf(O.board_me, sizeof O.board_me, "You: #%s, rating %s (%s-%s, %s draw%s)", lobby_value(body, "rank", rank, sizeof rank),
                     lobby_value(body, "rating", rating, sizeof rating), lobby_value(body, "wins", w, sizeof w),
                     lobby_value(body, "losses", l, sizeof l), lobby_value(body, "draws", d, sizeof d), atoi(d) == 1 ? "" : "s");
        else if (r > 0)
            O.board_me[0] = 0;
    }
    if (O.board_ticket <= 0 && (O.t_board == 0 || t - O.t_board > 30000)) {
        char q[64] = "", n[16];
        snprintf(n, sizeof n, "%d", O.board_page * PER_PAGE);
        lobby_form(q, sizeof q, "from", n);
        lobby_form(q, sizeof q, "n", "5");
        O.board_ticket = lobby_request("/v1/leaderboard", q, 1);
        if (O.board_ticket < 0) O.board_ticket = 0;
        if (O.me_ticket <= 0 && O.pid[0]) {
            q[0] = 0;
            lobby_form(q, sizeof q, "pid", O.pid);
            O.me_ticket = lobby_request("/v1/player", q, 1);
            if (O.me_ticket < 0) O.me_ticket = 0;
        }
        O.t_board = t;
    }
}

static void lobby_poll(void)
{
    char body[4096];
    int status = 0, r;
    u64 t = osGetTime();
    board_poll(t);
    /* a fight's result: what the leaderboard made of it */
    if (O.report_ticket > 0 && (r = lobby_result(O.report_ticket, body, sizeof body, &status)) != 0) {
        O.report_ticket = 0;
        char rt[16];
        lobby_value(body, "rating", rt, sizeof rt);
        if (r > 0 && strstr(body, "counted") && rt[0]) snprintf(O.my_rating, sizeof O.my_rating, "your rating %s", rt);
        say("leaderboard: %.60s", body);
    }
    /* the room list */
    if (O.screen == S_FIND) {
        if (O.list_ticket > 0 && (r = lobby_result(O.list_ticket, body, sizeof body, &status)) != 0) {
            O.list_ticket = 0;
            O.list_err = r < 0 || strncmp(body, "ok", 2) != 0;
            if (!O.list_err) { parse_list(body); O.list_msg[0] = 0; }
            else {
                char e[96];
                lobby_value(body, "error", e, sizeof e);
                snprintf(O.list_msg, sizeof O.list_msg, "%s", e[0] ? e : r < 0 ? body : "The lobby did not answer");
            }
        }
        if (O.list_ticket <= 0 && (O.t_list == 0 || t - O.t_list > 10000)) {
            char q[64] = "";
            char b[16];
            snprintf(b, sizeof b, "%08X", (unsigned)O.build);
            lobby_form(q, sizeof q, "build", b);
            O.list_ticket = lobby_request("/v1/list", q, 1);
            O.t_list = t;
        }
    }
    /* joining by code: when the internet address is known (or not coming) */
    if (O.screen == S_JOINING && O.join_code[0] && O.room.phase != RP_JOINING && O.room.phase != RP_IN_ROOM) {
        if (O.join_ticket == 0 && stun_settled()) {
            char f[400] = "", b[16], a[32];
            snprintf(b, sizeof b, "%08X", (unsigned)O.build);
            lobby_form(f, sizeof f, "code", O.join_code);
            lobby_form(f, sizeof f, "build", b);
            lobby_form(f, sizeof f, "name", O.name);
            if (O.have_pub) { fmt_addr(&O.pub, a, sizeof a); lobby_form(f, sizeof f, "addr", a); }
            fmt_addr(&(NetAddr){ O.local_ip, O.port }, a, sizeof a);
            lobby_form(f, sizeof f, "lan", a);
            O.join_ticket = lobby_request("/v1/join", f, 0);
            if (O.join_ticket < 0) { O.join_ticket = 0; show_msg("Could not join", "The lobby is busy, try again"); }
        } else if (O.join_ticket > 0 && (r = lobby_result(O.join_ticket, body, sizeof body, &status)) != 0) {
            O.join_ticket = 0;
            char e[96], ha[32], hl[32], myip[32];
            lobby_value(body, "error", e, sizeof e);
            if (r < 0 || strncmp(body, "ok", 2)) {
                show_msg("Could not join", e[0] ? e : r < 0 ? body : "The lobby did not answer");
            } else {
                NetAddr a = { 0, 0 }, lan = { 0, 0 }, me = { 0, 0 };
                net_parse_addr(lobby_value(body, "addr", ha, sizeof ha), &a);
                net_parse_addr(lobby_value(body, "lan", hl, sizeof hl), &lan);
                net_parse_addr(lobby_value(body, "ip", myip, sizeof myip), &me);
                /* the same router as the host: its own network address first */
                if (lan.ip && me.ip == a.ip) { NetAddr t2 = a; a = lan; lan = t2; }
                room_join(&O.room, a, lan);
                say("room %s: host %s / %s", O.join_code, ha, hl);
            }
        }
    }
    /* our room in the directory: opened, kept alive, joiners passed on */
    if (O.room.host && O.room.phase == RP_IN_ROOM && lobby_available()) {
        if (O.beat_ticket > 0 && (r = lobby_result(O.beat_ticket, body, sizeof body, &status)) != 0) {
            O.beat_ticket = 0;
            if (r > 0 && !strncmp(body, "ok", 2)) {
                lobby_value(body, "id", O.lid, sizeof O.lid);
                lobby_value(body, "secret", O.secret, sizeof O.secret);
                char c[8];
                lobby_value(body, "code", c, sizeof c);
                if (strcmp(c, O.code)) say("room code %s", c);
                snprintf(O.code, sizeof O.code, "%s", c);
                O.lobby_err[0] = 0;
                for (const char *l = body; l && *l; ) {
                    if (!strncmp(l, "join=", 5)) {
                        char ja[32] = "", jl[32] = "";
                        const char *f = l + 5;
                        size_t n = strcspn(f, "\t\n");
                        snprintf(ja, sizeof ja, "%.*s", (int)(n < 31 ? n : 31), f);
                        if (f[n] == '\t') {
                            f += n + 1;
                            n = strcspn(f, "\t\n");
                            snprintf(jl, sizeof jl, "%.*s", (int)(n < 31 ? n : 31), f);
                        }
                        NetAddr pa = { 0, 0 }, la = { 0, 0 };
                        if (!net_parse_addr(ja, &pa)) room_punch(&O.room, pa);
                        if (jl[0] && !net_parse_addr(jl, &la) && O.have_pub && pa.ip == O.pub.ip)
                            room_punch(&O.room, la);   /* behind the same router */
                        say("joining through the lobby: %s (%s)", ja, jl);
                    }
                    l = strchr(l, '\n');
                    if (l) l++;
                }
            } else {
                char e[96];
                lobby_value(body, "error", e, sizeof e);
                snprintf(O.lobby_err, sizeof O.lobby_err, "Lobby: %s", e[0] ? e : r < 0 ? body : "no answer");
            }
        }
        int every = O.room.info.session ? 15000 : 5000;
        if (O.beat_ticket <= 0 && (O.t_beat == 0 ? stun_settled() : t - O.t_beat >= (u64)every)) {
            char f[640] = "", b[16], a[32], n[8];
            lobby_form(f, sizeof f, "id", O.lid);
            lobby_form(f, sizeof f, "secret", O.secret);
            lobby_form(f, sizeof f, "code", O.code);
            snprintf(b, sizeof b, "%.12s's room", O.name);
            lobby_form(f, sizeof f, "name", b);
            lobby_form(f, sizeof f, "mode", O.room.info.mode == ROOM_KOTH ? "koth" : "1v1");
            lobby_form(f, sizeof f, "public", O.c_public ? "1" : "0");
            snprintf(b, sizeof b, "%08X", (unsigned)O.build);
            lobby_form(f, sizeof f, "build", b);
            snprintf(b, sizeof b, "%08X", (unsigned)host_ee_elf_crc);
            lobby_form(f, sizeof f, "disc", b);
            lobby_form(f, sizeof f, "ver", update_version());
            if (O.have_pub) { fmt_addr(&O.pub, a, sizeof a); lobby_form(f, sizeof f, "addr", a); }
            fmt_addr(&(NetAddr){ O.local_ip, O.port }, a, sizeof a);
            lobby_form(f, sizeof f, "lan", a);
            snprintf(n, sizeof n, "%d", room_count(&O.room));
            lobby_form(f, sizeof f, "players", n);
            lobby_form(f, sizeof f, "max", "8");
            lobby_form(f, sizeof f, "state", O.room.info.session ? "playing" : "open");
            O.beat_ticket = lobby_request("/v1/host", f, 0);
            if (O.beat_ticket < 0) O.beat_ticket = 0;
            O.t_beat = t;
        }
    }
}

/* ---------------------------------------------------------------- api */
void online_init(const char *dir, uint32_t build, const char *local_card, int local_unlock)
{
    memset(&O, 0, sizeof O);
    O.fd = -1;
    O.build = build;
    O.c_mode = ROOM_KOTH;
    O.c_public = 1;
    snprintf(O.dir, sizeof O.dir, "%s", dir);
    online_set_local(local_card, local_unlock);
    user_name(O.name, sizeof O.name);
    lobby_init(dir);
    identity();
    char p[300];
    snprintf(p, sizeof p, "%s/online_last.txt", dir);
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(O.last_peer, sizeof O.last_peer, f)) O.last_peer[strcspn(O.last_peer, "\r\n")] = 0;
        fclose(f);
    }
    say("you are \"%s\"; lobby %s", O.name, lobby_available() ? lobby_url() : "not set (rooms by address only)");
}

void online_set_local(const char *local_card, int local_unlock)
{
    snprintf(O.local_card, sizeof O.local_card, "%s", local_card);
    O.local_unlock = local_unlock;
}

void online_shutdown(void)
{
    leave_room("quit the game");
    net_down();
    lobby_shutdown();
    free(O.th);
    O.th = NULL;
}

int online_take_vs(uint16_t buttons)
{
    uint16_t pressed = (uint16_t)(buttons & ~O.prev_buttons);
    O.prev_buttons = buttons;
    if (O.suppress) {
        if (buttons) return 1;
        O.suppress = 0;
    }
    if (O.mode != M_OFF || !mvc2_can_restart()) return 0;
    const u8 *g = G;
    if (!(g[0] == 3 && g[1] == 1 && g[2] == 1)) return 0;
    if (*(const s16 *)(g + 8) != 1 || *(const s16 *)(g + 0xA) != 0) return 0;
    if (!(pressed & (PS2_START | PS2_CROSS | PS2_CIRCLE | PS2_SQUARE | PS2_TRIANGLE))) return 0;
    say("VS chosen: Online VS");
    O.mode = M_LOBBY;
    O.screen = S_MENU;
    O.suppress = 1;
    push_view();
    return 1;
}

int online_lobby_open(void) { return O.mode != M_OFF; }
int online_blocks_pad(void) { return O.mode != M_OFF || O.suppress; }
int online_drives_game(void) { return O.restarted && O.mode != M_OFF; }

void online_refresh(void)
{
    if (O.pending) {
        O.pending = 0;
        restart_game(1, 1700000000LL);
    }
    O.t_refresh = svcGetSystemTick();
    O.steps_this = 0;
    if (O.mode == M_OFF) return;
    if (O.soc) {
        room_poll(&O.room);
        stun_poll();
    }
    lobby_poll();
    if (O.mode == M_LOBBY) {
        int ph = O.room.phase;
        if (O.screen == S_JOINING && ph == RP_IN_ROOM) {
            O.screen = S_ROOM;
            O.c_mode = O.room.info.mode;
            say("in the room (member %d)", O.room.id);
        }
        if (ph == RP_CLOSED && O.screen != S_MSG) {
            say("room: %s", room_message(&O.room));
            O.sess = 0;
            O.phase = P_NONE;
            mute(0);
            show_msg(room_message(&O.room), O.restarted ? "Back to normal play." : "");
        }
        /* the match this console takes part in: a new one restarts the game */
        uint32_t s = ph == RP_IN_ROOM ? room_session(&O.room) : 0;
        if (s != O.sess) {
            O.sess = s;
            if (s) {
                const RoomInfo *in = &O.room.info;
                say("match: %s vs %s, %s", in->m[in->p[0]].name, in->m[in->p[1]].name,
                    room_slot(&O.room) >= 0 ? (room_slot(&O.room) ? "you are P2" : "you are P1") : "watching");
                restart_game(1, O.room.info.epoch);
            } else {
                O.phase = P_NONE;
                mute(0);
            }
        }
        if (O.room.desync >= 0 && O.desync_logged != O.room.desync + 1) {
            O.desync_logged = O.room.desync + 1;
            say("OUT OF SYNC at frame %ld (with member %d)", (long)O.room.desync, O.room.desync_member);
        }
    }
    push_view();
}

int online_plan(int steps_wanted, uint16_t buttons)
{
    if (!online_drives_game() || O.phase == P_NONE || O.pending) return 0;
    O.buttons = buttons;
    if (O.phase == P_NAV || O.mode == M_TEST) {
        /* as many steps as fit in about 13 ms, the last one drawn */
        int n = (int)(13.0 / (O.step_ms > 0.5 ? O.step_ms : 0.5));
        return n < 1 ? 1 : n > 40 ? 40 : n;
    }
    /* a match: the frames whose both pads are known */
    long cur = mvc2_host_frame;
    int slot = room_slot(&O.room);
    if (slot >= 0) {
        if (room_frame_advantage(&O.room, (int32_t)cur) >= 1 && cur - O.last_throttle >= 10) {
            O.last_throttle = cur;    /* well ahead of the other player: give them a refresh */
            return 0;
        }
        if (steps_wanted < 1) steps_wanted = 1;
        room_set_input(&O.room, (int32_t)(cur + steps_wanted - 1), buttons);
        u64 deadline = O.t_refresh + (u64)(11 * TICKS_MS);
        while (!room_ready(&O.room, (int32_t)cur) && svcGetSystemTick() < deadline && O.room.phase == RP_IN_ROOM) {
            svcSleepThread(1000000LL);
            room_poll(&O.room);
        }
    } else {
        steps_wanted = 3;             /* watching: catch up when behind */
    }
    int n = 0;
    while (n < steps_wanted && room_ready(&O.room, (int32_t)(cur + n))) n++;
    return n;
}

void online_pads(PadRaw *p0, PadRaw *p1)
{
    long f = mvc2_host_frame;
    if (O.phase == P_NAV) {
        *p0 = pad_of(nav_buttons(f));
        *p1 = pad_of(0);
    } else if (O.mode == M_TEST) {
        *p0 = pad_of(test_pad(f, 0));
        *p1 = pad_of(test_pad(f, 1));
    } else {
        uint16_t in[2];
        if (room_slot(&O.room) >= 0) room_set_input(&O.room, (int32_t)f, O.buttons);
        room_inputs(&O.room, (int32_t)f, in);
        *p0 = pad_of(in[0]);
        *p1 = pad_of(in[1]);
    }
}

static void test_dump(const char *tag, long f)
{
    char p[300];
    snprintf(p, sizeof p, "%s/mvc2_synctest_%s.bin", O.dir, tag);
    int r = mvc2_state_dump(p, f, O.fail_where.heap_lo, O.fail_where.heap_hi);
    say("wrote %s (frame %ld)%s", p, f, r ? ": FAILED" : "");
}

int online_after_step(void)
{
    long f = mvc2_host_frame - 1;   /* the frame just run */
    O.steps_this++;
    {
        double ms = (double)(svcGetSystemTick() - O.t_refresh) / TICKS_MS / O.steps_this;
        O.step_ms = O.step_ms * 0.8 + ms * 0.2;
    }
    MvcHashSlice where;
    uint32_t h = mvc2_state_hash_frame(f, &where);
    if (O.phase == P_NAV) {
        if (at_vs_select()) {
            O.nav_end = f + 1;
            O.phase = P_PLAY;
            say("in VS at frame %ld", O.nav_end);
            if (O.mode == M_LOBBY) {
                room_begin(&O.room, (int32_t)O.nav_end);
                room_fingerprint(&O.room, (int32_t)f, h);
                mute(0);
                return 0;   /* from here, only frames whose both pads are known */
            }
        } else if (f > NAV_LIMIT) {
            char m[96];
            snprintf(m, sizeof m, "The game did not reach VS (stuck at %d/%d/%d). Please send mvc2_log.txt.", G[0], G[1], G[2]);
            O.phase = P_NONE;
            mute(0);
            if (O.mode == M_LOBBY) leave_room("could not start");
            show_msg(O.mode == M_TEST ? "Sync test FAILED" : "Could not start the match", m);
            return 0;
        }
    }
    if (O.mode == M_TEST) {
        if (O.pass == 0) {
            if (f < O.th_cap) { O.th[f] = h; O.th_n = f + 1; }
        } else if (O.pass == 1 && f < O.th_n && O.fail < 0 && O.th[f] != h) {
            O.fail = f;
            O.fail_where = where;
            say("sync test: run 2 differs at frame %ld (slice %d of the data/bss, heap %06X..%06X): %08X vs %08X",
                f, where.slice, where.heap_lo, where.heap_hi, h, O.th[f]);
            test_dump("run2", f);
            O.pass = 2;
            say("sync test: run 3 (replay of run 1 to frame %ld)", f);
            O.pending = 1;
            return 0;
        } else if (O.pass == 2 && f == O.fail) {
            if (O.th[f] == h) say("sync test: run 3 matches run 1 at frame %ld", f);
            else say("sync test: run 3 differs from run 1 too at frame %ld (%08X)", f, h);
            test_dump("run3", f);
            O.phase = P_NONE;
            test_pass_done();
            return 0;
        }
        long end = (O.nav_end >= 0 ? O.nav_end : NAV_LIMIT) + TEST_PLAY;
        if (O.phase == P_PLAY && f + 1 >= end && O.pass < 2) {
            if (O.pass == 0) O.th_n = f + 1;
            O.phase = P_NONE;
            test_pass_done();
            return 0;
        }
        return 1;
    }
    room_fingerprint(&O.room, (int32_t)f, h);
    if (O.room.dump_at == f && !O.dumped) {
        char p[300];
        snprintf(p, sizeof p, "%s/mvc2_desync_m%d_f%ld.bin", O.dir, O.room.id, f);
        int r = mvc2_state_dump(p, f, 0, 0x200000);
        say("wrote %s%s", p, r ? ": FAILED" : "");
        O.dumped = 1;
    }
    if (O.phase == P_PLAY) {
        int w = fight_watch();
        if (w != -2) {
            const RoomInfo *in = &O.room.info;
            say("fight over at frame %ld: %s", f, w < 0 ? "draw" : in->m[in->p[w] >= 0 ? in->p[w] : 0].name);
            room_result(&O.room, w);
            report_fight(w);
        }
    }
    return O.phase != P_NONE;
}

int online_status(char *buf, int n)
{
    if (O.mode == M_LOBBY && O.phase == P_PLAY) {
        int s = room_slot(&O.room), p = room_ping(&O.room);
        if (s >= 0) snprintf(buf, (size_t)n, "online P%d %dms", s + 1, p < 0 ? 0 : p);
        else snprintf(buf, (size_t)n, "watching");
        return 1;
    }
    return 0;
}
