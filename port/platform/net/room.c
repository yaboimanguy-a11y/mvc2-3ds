/*
 * Online play: rooms, matches, lockstep (room.h).
 *
 * Datagrams, little-endian, all starting "MV2R" type version room_id(u32):
 *   HELLO    guest -> host (every 250 ms while joining; the host also sends
 *            them to guests the lobby server announced, to open its NAT):
 *            build, disc, version[16], name[16], lan address
 *   WELCOME  host -> guest: result (0 ok, 1 other program, 2 other disc,
 *            3 room closed, 4 full), member id, the guest's address as the
 *            host sees it, the host's build / disc / version
 *   ROOM     host -> members (on every change, and every 500 ms): RoomInfo
 *   GAME     any link, about once a frame during a match: session, base,
 *            cur, acks, round-trip clock, desync / dump frame, what the
 *            sender knows of each player's frame, then for each slot the
 *            inputs the other end has not acknowledged, then fingerprints
 *   LINK     player <-> player, to open a direct path (session, member)
 *   BYE      reason
 * State is resent until acknowledged (inputs) or simply resent (ROOM), so
 * a lost datagram only costs the time to the next one.
 */
#include <stdio.h>
#include <string.h>
#include "room.h"

enum { T_HELLO = 1, T_WELCOME, T_ROOM, T_GAME, T_LINK, T_BYE };
#define HDR 10
#define MAX_PKT 1200
#define MAX_SEND_INPUTS 64
#define MAX_SEND_HASHES 4
#define TIMEOUT_MS 8000
#define JOIN_TIMEOUT_MS 20000
#define ROUND_GAP_MS 6000      /* King of the Hill: the result shows this long before the next round */

/* ---- byte order ---- */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return r16(p) | (uint32_t)r16(p + 2) << 16; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }
static void waddr(uint8_t *p, const NetAddr *a) { w32(p, a->ip); w16(p + 4, a->port); }
static void raddr(NetAddr *a, const uint8_t *p) { a->ip = r32(p); a->port = r16(p + 4); }

static uint64_t now(const Room *r) { return r->io.now_ms(r->io.ctx); }
static int same(const NetAddr *a, const NetAddr *b) { return a->ip == b->ip && a->port == b->port; }

static uint32_t rnd32(Room *r)
{
    static uint32_t x;
    x ^= (uint32_t)now(r) * 2654435761u + 0x9E3779B9u + (uint32_t)(uintptr_t)r;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x ? x : 1;
}

static int hdr(const Room *r, uint8_t *p, int type)
{
    memcpy(p, "MV2R", 4);
    p[4] = (uint8_t)type;
    p[5] = NET_VERSION;
    w32(p + 6, r->room_id);
    return HDR;
}

static void send_to(Room *r, const NetAddr *to, const uint8_t *p, int len)
{
    if (!to->ip) return;
    r->io.send(r->io.ctx, to, p, len);
    r->tx_packets++;
}

static void closed(Room *r, const char *msg)
{
    r->phase = RP_CLOSED;
    r->sess = 0;
    snprintf(r->msg, sizeof r->msg, "%s", msg);
}

static void add_rtt(RoomLink *l, int ms)
{
    if (ms < 0 || ms > 5000) return;
    l->rtt[l->rtt_i] = ms;
    l->rtt_i = (l->rtt_i + 1) % 8;
    if (l->nrtt < 8) l->nrtt++;
}

static int link_ping(const RoomLink *l)
{
    int v[8], k = l->nrtt < 8 ? l->nrtt : 8;
    if (!k) return -1;
    memcpy(v, l->rtt, sizeof v);
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return v[k / 2];
}

static void link_reset_session(RoomLink *l)
{
    l->ack[0] = l->ack[1] = -1;
    l->base = -1;
    l->sess = 0;
    for (int i = 0; i < 64; i++) l->pend_f[i] = -1;
}

static void link_open(RoomLink *l, int member, NetAddr a, uint64_t t)
{
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->member = member;
    l->addr = a;
    l->t_rx = t;
    link_reset_session(l);
}

/* ---------------------------------------------------------------- setup */
void room_init(Room *r, const NetIo *io, const NetHello *me, const char *name, NetAddr lan)
{
    memset(r, 0, sizeof *r);
    r->io = *io;
    r->me = *me;
    snprintf(r->name, sizeof r->name, "%s", name && *name ? name : "Player");
    r->lan = lan;
    r->phase = RP_IDLE;
    r->slot = -1;
    r->base = -1;
    r->desync = r->dump_at = -1;
    r->info.p[0] = r->info.p[1] = r->info.next = -1;
    r->info.delay = 3;
}

void room_host(Room *r, int mode)
{
    NetIo io = r->io;
    NetHello me = r->me;
    char name[16];
    NetAddr lan = r->lan, pub = r->pub;
    char pid[13];
    memcpy(name, r->name, sizeof name);
    memcpy(pid, r->pid, sizeof pid);
    room_init(r, &io, &me, name, lan);
    memcpy(r->pid, pid, sizeof pid);
    r->pub = pub;
    r->host = 1;
    r->id = 0;
    r->phase = RP_IN_ROOM;
    r->room_id = rnd32(r);
    r->info.version = 1;
    r->info.mode = (uint8_t)mode;
    r->info.m[0].used = 1;
    memcpy(r->info.m[0].name, r->name, sizeof r->name);
    memcpy(r->info.m[0].pid, r->pid, sizeof r->pid);
    r->info.m[0].lan = r->lan;
    r->info.m[0].addr = r->pub;
    r->info.m[0].cur = -1;
    r->queue[0] = 0;
    r->nqueue = 1;
    snprintf(r->msg, sizeof r->msg, "Room open");
}

void room_join(Room *r, NetAddr host, NetAddr host_lan)
{
    NetIo io = r->io;
    NetHello me = r->me;
    char name[16];
    NetAddr lan = r->lan, pub = r->pub;
    char pid[13];
    memcpy(name, r->name, sizeof name);
    memcpy(pid, r->pid, sizeof pid);
    room_init(r, &io, &me, name, lan);
    memcpy(r->pid, pid, sizeof pid);
    r->pub = pub;
    if (!host.port) host.port = NET_PORT;
    if (host_lan.ip && !host_lan.port) host_lan.port = NET_PORT;
    r->phase = RP_JOINING;
    r->join_addr = host;
    r->join_lan = host_lan;
    r->t_join = now(r);
    snprintf(r->msg, sizeof r->msg, "Connecting");
}

void room_punch(Room *r, NetAddr guest)
{
    if (!r->host || !guest.ip) return;
    if (!guest.port) guest.port = NET_PORT;
    int k = 0;
    for (int i = 0; i < ROOM_MAX; i++) {
        if (same(&r->punch[i], &guest)) { k = i; break; }
        if (r->punch_until[i] < r->punch_until[k]) k = i;
    }
    r->punch[k] = guest;
    r->punch_until[k] = now(r) + 30000;
}

const char *room_message(const Room *r) { return r->msg; }

int room_count(const Room *r)
{
    int n = 0;
    for (int i = 0; i < ROOM_MAX; i++) n += r->info.m[i].used;
    return n;
}

int room_delay(const Room *r) { return r->info.delay ? r->info.delay : 3; }

int room_ping(const Room *r)
{
    if (!r->host) {
        int other = r->slot >= 0 ? r->info.p[1 - r->slot] : -1;
        if (other > 0 && r->link[other].used && r->link[other].up) return link_ping(&r->link[other]);
        return link_ping(&r->link[0]);
    }
    int other = r->slot >= 0 ? r->info.p[1 - r->slot] : -1;
    if (other > 0 && r->link[other].used) return link_ping(&r->link[other]);
    int best = -1;
    for (int i = 1; i < ROOM_MAX; i++)
        if (r->link[i].used && link_ping(&r->link[i]) > best) best = link_ping(&r->link[i]);
    return best;
}

/* ------------------------------------------------------------- the room */
static int info_put(const RoomInfo *in, uint8_t *p)
{
    int n = 0;
    w32(p + n, in->version); n += 4;
    p[n++] = in->mode;
    w32(p + n, in->session); n += 4;
    p[n++] = (uint8_t)in->p[0];
    p[n++] = (uint8_t)in->p[1];
    p[n++] = in->delay;
    w64(p + n, (uint64_t)in->epoch); n += 8;
    p[n++] = in->session_mask;
    p[n++] = (uint8_t)in->next;
    memcpy(p + n, in->result, 48); n += 48;
    for (int i = 0; i < ROOM_MAX; i++) {
        const RoomMember *m = &in->m[i];
        p[n++] = m->used;
        if (!m->used) continue;
        memcpy(p + n, m->name, 16); n += 16;
        memcpy(p + n, m->pid, 12); n += 12;
        waddr(p + n, &m->addr); n += 6;
        waddr(p + n, &m->lan); n += 6;
        w16(p + n, m->wins); n += 2;
        w16(p + n, m->played); n += 2;
        p[n++] = m->streak;
        w32(p + n, (uint32_t)m->cur); n += 4;
    }
    return n;
}

static int info_get(RoomInfo *in, const uint8_t *p, int len)
{
    int n = 0;
    RoomInfo t;
    memset(&t, 0, sizeof t);
    if (len < 70) return -1;
    t.version = r32(p + n); n += 4;
    t.mode = p[n++];
    t.session = r32(p + n); n += 4;
    t.p[0] = (int8_t)p[n++];
    t.p[1] = (int8_t)p[n++];
    t.delay = p[n++];
    t.epoch = (int64_t)r64(p + n); n += 8;
    t.session_mask = p[n++];
    t.next = (int8_t)p[n++];
    memcpy(t.result, p + n, 48); n += 48;
    t.result[47] = 0;
    for (int i = 0; i < ROOM_MAX; i++) {
        if (n >= len) return -1;
        RoomMember *m = &t.m[i];
        m->used = p[n++];
        if (!m->used) continue;
        if (n + 49 > len) return -1;
        memcpy(m->name, p + n, 16); n += 16;
        m->name[15] = 0;
        memcpy(m->pid, p + n, 12); n += 12;
        m->pid[12] = 0;
        raddr(&m->addr, p + n); n += 6;
        raddr(&m->lan, p + n); n += 6;
        m->wins = r16(p + n); n += 2;
        m->played = r16(p + n); n += 2;
        m->streak = p[n++];
        m->cur = (int32_t)r32(p + n); n += 4;
    }
    *in = t;
    return 0;
}

static void send_room(Room *r, int member)
{
    uint8_t p[MAX_PKT];
    int len = hdr(r, p, T_ROOM);
    len += info_put(&r->info, p + len);
    send_to(r, &r->link[member].addr, p, len);
}

static void changed(Room *r)
{
    r->info.version++;
    for (int i = 1; i < ROOM_MAX; i++)
        if (r->link[i].used) send_room(r, i);
    r->t_last_room = now(r);
}

/* which match this console runs, from the room */
static void follow_session(Room *r)
{
    uint32_t want = (r->info.session && (r->info.session_mask & (1u << r->id))) ? r->info.session : 0;
    if (want == r->sess) return;
    r->sess = want;
    r->slot = r->info.p[0] == r->id ? 0 : r->info.p[1] == r->id ? 1 : -1;
    r->base = -1;
    r->last[0] = r->last[1] = -1;
    r->known_cur[0] = r->known_cur[1] = -1;
    r->known_adv[0] = r->known_adv[1] = 0;
    memset(r->hash_f, 0xFF, sizeof r->hash_f);
    r->hash_last = -1;
    r->cur = 0;
    r->desync = -1;
    r->desync_member = -1;
    r->dump_at = -1;
    r->result_done = 0;
    for (int i = 0; i < ROOM_MAX; i++) {
        if (!r->host && i > 0) r->link[i].used = 0;   /* direct links: only between this match's players */
        if (r->link[i].used) link_reset_session(&r->link[i]);
    }
    if (!want || r->host || r->slot < 0) return;
    int other = r->info.p[1 - r->slot];
    if (other <= 0 || other >= ROOM_MAX) return;      /* the other player is the host: link 0 */
    const RoomMember *m = &r->info.m[other];
    NetAddr a = m->addr;
    if (m->addr.ip == r->pub.ip && m->lan.ip) a = m->lan;  /* behind the same router */
    link_open(&r->link[other], other, a, now(r));
    r->link[other].t_rx = 0;
}

static void remove_queue(Room *r, int id)
{
    int k = 0;
    for (int i = 0; i < r->nqueue; i++)
        if (r->queue[i] != id) r->queue[k++] = r->queue[i];
    r->nqueue = k;
}

static void new_session(Room *r, int p0, int p1)
{
    r->info.session = rnd32(r);
    r->info.p[0] = (int8_t)p0;
    r->info.p[1] = (int8_t)p1;
    r->info.epoch = 1700000000LL + (int64_t)(now(r) / 1000 % 100000000);
    r->info.session_mask = 0;
    for (int i = 0; i < ROOM_MAX; i++)
        if (r->info.m[i].used) r->info.session_mask |= (uint8_t)(1u << i);
    r->info.next = r->nqueue > 0 ? r->queue[0] : -1;
    r->t_result = 0;
    changed(r);
    follow_session(r);
}

static void end_session(Room *r, const char *result)
{
    r->info.session = 0;
    r->info.p[0] = r->info.p[1] = -1;
    if (result) snprintf(r->info.result, sizeof r->info.result, "%s", result);
    r->t_result = 0;
    changed(r);
    follow_session(r);
}

int room_can_start(const Room *r)
{
    return r->host && r->phase == RP_IN_ROOM && !r->info.session && room_count(r) >= 2;
}

void room_set_mode(Room *r, int mode)
{
    if (!r->host || r->info.session) return;
    r->info.mode = (uint8_t)mode;
    changed(r);
}

void room_start(Room *r)
{
    if (!room_can_start(r)) return;
    if (r->info.mode == ROOM_1V1) {
        int other = -1;
        for (int i = 1; i < ROOM_MAX && other < 0; i++)
            if (r->info.m[i].used) other = i;
        new_session(r, 0, other);
        return;
    }
    /* King of the Hill: the first two in the queue */
    int a = r->queue[0], b = r->queue[1];
    remove_queue(r, a);
    remove_queue(r, b);
    new_session(r, a, b);
}

static void next_round(Room *r)
{
    /* the winner keeps their side; a draw: the champion (P1's side when
     * unknown) stays */
    int slot = r->winner_slot >= 0 ? r->winner_slot : 0;
    int champ = r->info.p[slot];
    if (champ < 0 || !r->info.m[champ].used) {
        champ = r->nqueue ? r->queue[0] : -1;
        if (champ >= 0) remove_queue(r, champ);
        slot = 0;
    }
    if (champ < 0 || r->nqueue < 1) {
        end_session(r, "Waiting for a challenger");
        return;
    }
    int ch = r->queue[0];
    remove_queue(r, ch);
    if (slot == 0) new_session(r, champ, ch);
    else new_session(r, ch, champ);
}

void room_result(Room *r, int winner)
{
    /* King of the Hill: one result per match (the next round restarts);
     * 1v1: a result after every fight of the same match */
    if (!r->host || !r->info.session || (r->info.mode == ROOM_KOTH && r->result_done)) return;
    r->result_done = 1;
    int a = r->info.p[0], b = r->info.p[1];
    if (a < 0 || b < 0) return;
    RoomMember *ma = &r->info.m[a], *mb = &r->info.m[b];
    ma->played++;
    mb->played++;
    if (winner == 0 || winner == 1) {
        RoomMember *w = winner == 0 ? ma : mb, *l = winner == 0 ? mb : ma;
        w->wins++;
        if (w->streak < 255) w->streak++;
        l->streak = 0;
        r->winner_slot = winner;
        snprintf(r->info.result, sizeof r->info.result, "%s beat %s", w->name, l->name);
        if (r->info.mode == ROOM_KOTH) {
            int loser = winner == 0 ? b : a;
            remove_queue(r, loser);
            if (r->nqueue < ROOM_MAX) r->queue[r->nqueue++] = (int8_t)loser;
        }
    } else {
        r->winner_slot = -1;
        snprintf(r->info.result, sizeof r->info.result, "Draw: %s and %s", ma->name, mb->name);
        if (r->info.mode == ROOM_KOTH) {   /* a draw: the challenger goes to the back */
            remove_queue(r, b);
            if (r->nqueue < ROOM_MAX) r->queue[r->nqueue++] = (int8_t)b;
        }
    }
    if (r->info.mode == ROOM_KOTH) {
        r->info.next = r->nqueue ? r->queue[0] : -1;
        r->t_result = now(r);
    }
    changed(r);
}

void room_poll_rounds(Room *r)
{
    if (!r->host || r->info.mode != ROOM_KOTH || !r->t_result) return;
    if (now(r) - r->t_result < ROUND_GAP_MS) return;
    r->t_result = 0;
    next_round(r);
}

static void drop_member(Room *r, int id, const char *why)
{
    if (id <= 0 || id >= ROOM_MAX || !r->info.m[id].used) return;
    char name[16];
    memcpy(name, r->info.m[id].name, 16);
    r->info.m[id].used = 0;
    r->link[id].used = 0;
    remove_queue(r, id);
    snprintf(r->msg, sizeof r->msg, "%s %s", name, why);
    int playing = r->info.session && (r->info.p[0] == id || r->info.p[1] == id);
    if (!playing) { changed(r); return; }
    char res[48];
    if (r->info.mode == ROOM_KOTH) {
        /* the other player wins by default */
        int winner = r->info.p[0] == id ? 1 : 0;
        int w = r->info.p[winner];
        r->info.m[w].wins++;
        if (r->info.m[w].streak < 255) r->info.m[w].streak++;
        r->winner_slot = winner;
        snprintf(r->info.result, sizeof r->info.result, "%s wins (%s left)", r->info.m[w].name, name);
        r->result_done = 1;
        r->t_result = now(r) - ROUND_GAP_MS + 1500;
        changed(r);
        return;
    }
    snprintf(res, sizeof res, "%s left", name);
    end_session(r, res);
}

/* --------------------------------------------------------------- packets */
static int put_hello(const Room *r, uint8_t *p)
{
    w32(p, r->me.build);
    w32(p + 4, r->me.disc);
    memcpy(p + 8, r->me.version, 16);
    memcpy(p + 24, r->name, 16);
    waddr(p + 40, &r->lan);
    memcpy(p + 46, r->pid, 12);
    return 58;
}

static void send_welcome(Room *r, const NetAddr *to, int res, int id)
{
    uint8_t q[128];
    int l = hdr(r, q, T_WELCOME);
    q[l++] = (uint8_t)res;
    q[l++] = (uint8_t)id;
    waddr(q + l, to); l += 6;
    w32(q + l, r->me.build); l += 4;
    w32(q + l, r->me.disc); l += 4;
    memcpy(q + l, r->me.version, 16); l += 16;
    send_to(r, to, q, l);
}

static void on_hello(Room *r, const NetAddr *from, const uint8_t *b, int blen)
{
    if (!r->host || blen < 58 || r->phase != RP_IN_ROOM) return;
    for (int i = 1; i < ROOM_MAX; i++)
        if (r->info.m[i].used && same(&r->link[i].addr, from)) {
            send_welcome(r, from, 0, i);
            send_room(r, i);
            return;
        }
    uint32_t build = r32(b), disc = r32(b + 4);
    if (build != r->me.build) { send_welcome(r, from, 1, 0); return; }
    if (disc != r->me.disc) { send_welcome(r, from, 2, 0); return; }
    int id = -1;
    for (int i = 1; i < ROOM_MAX && id < 0; i++)
        if (!r->info.m[i].used) id = i;
    if (id < 0) { send_welcome(r, from, 4, 0); return; }
    RoomMember *m = &r->info.m[id];
    memset(m, 0, sizeof *m);
    m->used = 1;
    memcpy(m->name, b + 24, 16);
    m->name[15] = 0;
    if (!m->name[0]) snprintf(m->name, sizeof m->name, "Player %d", id + 1);
    m->addr = *from;
    raddr(&m->lan, b + 40);
    memcpy(m->pid, b + 46, 12);
    m->pid[12] = 0;
    m->cur = -1;
    link_open(&r->link[id], id, *from, now(r));
    r->link[id].up = 1;
    if (r->nqueue < ROOM_MAX) r->queue[r->nqueue++] = (int8_t)id;
    if (r->info.mode == ROOM_KOTH && r->info.session && r->info.next < 0) r->info.next = (int8_t)id;
    snprintf(r->msg, sizeof r->msg, "%s joined", m->name);
    send_welcome(r, from, 0, id);
    changed(r);
}

static void on_welcome(Room *r, const NetAddr *from, const uint8_t *b, int blen, uint32_t room_id)
{
    if (r->phase != RP_JOINING || blen < 32) return;
    if (!same(from, &r->join_addr) && !same(from, &r->join_lan)) return;
    int res = b[0];
    char v[17];
    memcpy(v, b + 16, 16);
    v[16] = 0;
    if (res == 1) {
        char m[96];
        snprintf(m, sizeof m, "Different app: the host has v%s (needs the same build)", v);
        closed(r, m);
        return;
    }
    if (res == 2) { closed(r, "Different game disc than the host's (same version needed)"); return; }
    if (res == 3) { closed(r, "The room is closed"); return; }
    if (res == 4) { closed(r, "The room is full"); return; }
    r->id = b[1];
    raddr(&r->pub, b + 2);
    r->room_id = room_id;
    r->phase = RP_IN_ROOM;
    link_open(&r->link[0], 0, *from, now(r));
    r->link[0].up = 1;
    snprintf(r->msg, sizeof r->msg, "In the room");
}

static int other_slot_member(const Room *r)
{
    return r->slot >= 0 ? r->info.p[1 - r->slot] : -1;
}

static void compare(Room *r, RoomLink *l, int32_t f, uint32_t h)
{
    int i = f % NET_RING;
    if (r->hash_f[i] != f) return;
    if (r->hash_mine[i] == h) return;
    if (r->desync < 0 || f < r->desync) {
        r->desync = f;
        r->desync_member = l->member;
    }
    if (r->host && r->dump_at < 0 && r->base >= 0) {
        int32_t ahead = r->hash_last;
        for (int k = 0; k < ROOM_MAX; k++)
            if (r->info.m[k].used && r->info.m[k].cur > ahead) ahead = r->info.m[k].cur;
        r->dump_at = ahead + 2 * room_delay(r) + 60;
    }
}

static void send_game(Room *r, RoomLink *l)
{
    uint8_t p[MAX_PKT];
    int len = hdr(r, p, T_GAME);
    w32(p + len, r->sess); len += 4;
    w32(p + len, (uint32_t)r->base); len += 4;
    w32(p + len, (uint32_t)r->hash_last); len += 4;
    w32(p + len, (uint32_t)r->last[0]); len += 4;
    w32(p + len, (uint32_t)r->last[1]); len += 4;
    w32(p + len, (uint32_t)now(r)); len += 4;
    w32(p + len, l->echo_rx ? l->echo_t : 0); len += 4;
    w32(p + len, l->echo_rx ? (uint32_t)(now(r) - l->echo_rx) : 0); len += 4;
    w32(p + len, (uint32_t)r->desync); len += 4;
    w32(p + len, (uint32_t)r->dump_at); len += 4;
    for (int s = 0; s < 2; s++) {
        int32_t c = r->slot == s ? r->hash_last : r->known_cur[s];
        int a = r->slot == s ? r->known_adv[s] : r->known_adv[s];
        w32(p + len, (uint32_t)c); len += 4;
        p[len++] = (uint8_t)(int8_t)a;
    }
    for (int s = 0; s < 2; s++) {
        int32_t first = l->ack[s] + 1;
        int cnt = 0;
        if (r->base >= 0) {
            if (first < r->base + room_delay(r)) first = r->base + room_delay(r);
            if (r->last[s] >= first) cnt = (int)(r->last[s] - first + 1);
            if (cnt > MAX_SEND_INPUTS) cnt = MAX_SEND_INPUTS;
        }
        w32(p + len, (uint32_t)first); len += 4;
        p[len++] = (uint8_t)cnt;
        for (int i = 0; i < cnt; i++) { w16(p + len, r->in[s][(first + i) % NET_RING]); len += 2; }
    }
    int nh = 0, at = len++;
    for (int32_t f = r->hash_last; f >= 0 && nh < MAX_SEND_HASHES; f--) {
        int i = f % NET_RING;
        if (r->hash_f[i] != f) break;
        w32(p + len, (uint32_t)f);
        w32(p + len + 4, r->hash_mine[i]);
        len += 8;
        nh++;
    }
    p[at] = (uint8_t)nh;
    send_to(r, &l->addr, p, len);
    l->t_tx = now(r);
}

static void on_game(Room *r, RoomLink *l, const uint8_t *p, int len)
{
    if (len < 50) { r->rx_bad++; return; }
    uint32_t sess = r32(p);
    l->sess = sess;
    if (!sess || sess != r->sess) return;
    int32_t base = (int32_t)r32(p + 4), cur = (int32_t)r32(p + 8);
    int32_t ack0 = (int32_t)r32(p + 12), ack1 = (int32_t)r32(p + 16);
    uint32_t t = r32(p + 20), echo_t = r32(p + 24), echo_age = r32(p + 28);
    int32_t desync = (int32_t)r32(p + 32), dump_at = (int32_t)r32(p + 36);
    if (echo_t) add_rtt(l, (int)((uint32_t)now(r) - echo_t - echo_age));
    l->echo_t = t ? t : 1;
    l->echo_rx = now(r);
    if (ack0 > l->ack[0]) l->ack[0] = ack0;
    if (ack1 > l->ack[1]) l->ack[1] = ack1;
    l->base = base;
    if (base >= 0 && r->base >= 0 && base != r->base && r->desync < 0) {
        r->desync = base < r->base ? base : r->base;
        r->desync_member = l->member;
    }
    if (r->host && l->member > 0 && l->member < ROOM_MAX) {
        r->info.m[l->member].cur = cur;
        if (desync >= 0 && (r->desync < 0 || desync < r->desync)) { r->desync = desync; r->desync_member = l->member; }
        if (desync >= 0 && r->dump_at < 0 && r->base >= 0) {
            int32_t ahead = r->hash_last > cur ? r->hash_last : cur;
            r->dump_at = ahead + 2 * room_delay(r) + 60;
        }
    }
    if (!r->host && l->member == 0 && dump_at >= 0 && r->dump_at < 0) r->dump_at = dump_at;
    int pos = 40;
    for (int s = 0; s < 2; s++) {
        int32_t c = (int32_t)r32(p + pos);
        int a = (int8_t)p[pos + 4];
        pos += 5;
        if (r->slot == s) continue;
        int from_player = r->info.p[s] == l->member;
        if (from_player || c > r->known_cur[s]) {
            if (c > r->known_cur[s] || from_player) { r->known_cur[s] = c; r->known_adv[s] = a; }
        }
    }
    for (int s = 0; s < 2; s++) {
        if (pos + 5 > len) { r->rx_bad++; return; }
        int32_t first = (int32_t)r32(p + pos);
        int cnt = p[pos + 4];
        pos += 5;
        if (pos + cnt * 2 > len) { r->rx_bad++; return; }
        if (r->slot != s && r->base >= 0) {
            for (int i = 0; i < cnt; i++) {
                int32_t f = first + i;
                if (f == r->last[s] + 1) {
                    r->in[s][f % NET_RING] = r16(p + pos + i * 2);
                    r->last[s] = f;
                }
            }
        }
        pos += cnt * 2;
    }
    if (pos >= len) return;
    int nh = p[pos++];
    for (int i = 0; i < nh && pos + 8 <= len; i++, pos += 8) {
        int32_t f = (int32_t)r32(p + pos);
        uint32_t h = r32(p + pos + 4);
        if (f < 0) continue;
        if (r->hash_f[f % NET_RING] == f) compare(r, l, f, h);
        else if (f > r->hash_last) { l->pend_f[f & 63] = f; l->pend_h[f & 63] = h; }
    }
}

static RoomLink *link_from(Room *r, const NetAddr *from)
{
    for (int i = 0; i < ROOM_MAX; i++)
        if (r->link[i].used && same(&r->link[i].addr, from)) return &r->link[i];
    return NULL;
}

static void on_packet(Room *r, const NetAddr *from, const uint8_t *p, int len)
{
    if (len < HDR || memcmp(p, "MV2R", 4)) { r->rx_bad++; return; }
    int type = p[4];
    uint32_t room_id = r32(p + 6);
    const uint8_t *b = p + HDR;
    int blen = len - HDR;
    if (p[5] != NET_VERSION) {
        if (type == T_HELLO && r->host) send_welcome(r, from, 1, 0);
        return;
    }
    if (type == T_HELLO) { on_hello(r, from, b, blen); return; }
    if (type == T_WELCOME) { on_welcome(r, from, b, blen, room_id); return; }
    if (r->phase != RP_IN_ROOM || room_id != r->room_id) return;
    RoomLink *l = link_from(r, from);
    if (!l && type == T_LINK && blen >= 5 && r->slot >= 0) {
        /* the other player reached us first from an address we did not try */
        int m = b[0];
        if (m == other_slot_member(r) && m > 0 && m < ROOM_MAX && r32(b + 1) == r->sess && !r->host) {
            r->link[m].addr = *from;
            l = &r->link[m];
        }
    }
    if (!l) return;
    l->t_rx = now(r);
    l->up = 1;
    r->rx_packets++;
    switch (type) {
    case T_ROOM:
        if (!r->host && l->member == 0) {
            RoomInfo in;
            if (info_get(&in, b, blen) == 0 && in.version != r->info.version) {
                r->info = in;
                if (!r->info.m[r->id].used) { closed(r, "You were removed from the room"); return; }
                follow_session(r);
            }
        }
        break;
    case T_GAME:
        on_game(r, l, b, blen);
        break;
    case T_LINK:
        break;
    case T_BYE:
        if (r->host) drop_member(r, l->member, "left");
        else if (l->member == 0) closed(r, "The host closed the room");
        else l->up = 0;
        break;
    }
}

/* -------------------------------------------------------------- polling */
void room_poll(Room *r)
{
    uint8_t buf[MAX_PKT + 64];
    NetAddr from;
    int len, guard = 0;
    while (guard++ < 256 && (len = r->io.recv(r->io.ctx, &from, buf, sizeof buf)) > 0)
        on_packet(r, &from, buf, len);
    uint64_t t = now(r);
    if (r->phase == RP_JOINING) {
        if (t - r->link[0].t_hello >= 250) {
            uint8_t p[128];
            int l = hdr(r, p, T_HELLO);
            l += put_hello(r, p + l);
            send_to(r, &r->join_addr, p, l);
            if (r->join_lan.ip) send_to(r, &r->join_lan, p, l);
            r->link[0].t_hello = t;
        }
        if (t - r->t_join > JOIN_TIMEOUT_MS)
            closed(r, "No answer from the room (the host may need to forward UDP port 27886)");
        return;
    }
    if (r->phase != RP_IN_ROOM) return;
    if (r->host) {
        for (int i = 0; i < ROOM_MAX; i++)
            if (r->punch_until[i] > t && t - r->link[0].t_hello >= 250) {
                uint8_t p[128];
                int l = hdr(r, p, T_HELLO);
                l += put_hello(r, p + l);
                send_to(r, &r->punch[i], p, l);
            }
        if (t - r->link[0].t_hello >= 250) r->link[0].t_hello = t;
        for (int i = 1; i < ROOM_MAX; i++)
            if (r->link[i].used && t - r->link[i].t_rx > TIMEOUT_MS) drop_member(r, i, "lost the connection");
        if (t - r->t_last_room >= 500) {
            for (int i = 1; i < ROOM_MAX; i++)
                if (r->link[i].used) send_room(r, i);
            r->t_last_room = t;
        }
        r->info.m[0].cur = r->hash_last;
        room_poll_rounds(r);
    } else {
        if (t - r->link[0].t_rx > TIMEOUT_MS) { closed(r, "Lost the connection to the room"); return; }
        /* direct link to the other player: knock until it answers */
        int o = other_slot_member(r);
        if (o > 0 && o < ROOM_MAX && r->link[o].used && !r->link[o].up && t - r->link[o].t_hello >= 100) {
            uint8_t p[32];
            int l = hdr(r, p, T_LINK);
            p[l++] = (uint8_t)r->id;
            w32(p + l, r->sess); l += 4;
            send_to(r, &r->link[o].addr, p, l);
            const RoomMember *m = &r->info.m[o];
            if (m->lan.ip && !same(&m->lan, &r->link[o].addr)) send_to(r, &m->lan, p, l);
            r->link[o].t_hello = t;
        }
    }
    /* the match's links: about once a frame (and at once for new input) */
    for (int i = 0; i < ROOM_MAX; i++) {
        RoomLink *l = &r->link[i];
        if (!l->used || (!r->host && i > 0 && !l->up) || (r->host && i == 0)) continue;
        if (r->sess && t - l->t_tx >= 16) send_game(r, l);
        else if (!r->sess && t - l->t_tx >= 1000) send_game(r, l);   /* keep-alive */
    }
}

void room_leave(Room *r, const char *why)
{
    if (r->phase == RP_IN_ROOM || r->phase == RP_JOINING) {
        uint8_t p[128];
        int len = hdr(r, p, T_BYE);
        memset(p + len, 0, 48);
        snprintf((char *)p + len, 48, "%s", why ? why : "");
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < ROOM_MAX; i++)
                if (r->link[i].used && !(r->host && i == 0)) send_to(r, &r->link[i].addr, p, len + 48);
    }
    if (r->phase != RP_CLOSED) closed(r, why && *why ? why : "Left the room");
}

/* ------------------------------------------------------------ the match */
uint32_t room_session(const Room *r) { return r->phase == RP_IN_ROOM ? r->sess : 0; }
int room_slot(const Room *r) { return r->slot; }

void room_begin(Room *r, int32_t base)
{
    int d = room_delay(r);
    r->base = base;
    for (int s = 0; s < 2; s++) {
        for (int f = base; f < base + d; f++) r->in[s][f % NET_RING] = 0;
        r->last[s] = base + d - 1;
    }
}

int room_began_all(const Room *r)
{
    if (r->base < 0) return 0;
    if (r->slot < 0) return 1;
    return r->known_cur[1 - r->slot] >= 0 || r->last[1 - r->slot] >= r->base + room_delay(r);
}

int room_ready(const Room *r, int32_t f)
{
    return r->base >= 0 && f >= r->base && r->last[0] >= f && r->last[1] >= f;
}

static void flush_now(Room *r)
{
    for (int i = 0; i < ROOM_MAX; i++) {
        RoomLink *l = &r->link[i];
        if (!l->used || (r->host && i == 0) || (!r->host && i > 0 && !l->up)) continue;
        send_game(r, l);
    }
}

void room_set_input(Room *r, int32_t cur, uint16_t b)
{
    if (r->base < 0 || r->slot < 0) return;
    int32_t t = cur + room_delay(r);
    if (t <= r->last[r->slot]) return;
    for (int32_t f = r->last[r->slot] + 1; f <= t; f++) r->in[r->slot][f % NET_RING] = b;
    r->last[r->slot] = t;
    flush_now(r);
}

void room_inputs(const Room *r, int32_t f, uint16_t out[2])
{
    if (r->base < 0 || f < r->base + room_delay(r)) { out[0] = out[1] = 0; return; }
    out[0] = r->in[0][f % NET_RING];
    out[1] = r->in[1][f % NET_RING];
}

void room_fingerprint(Room *r, int32_t f, uint32_t h)
{
    int i = f % NET_RING;
    r->hash_mine[i] = h;
    r->hash_f[i] = f;
    if (f > r->hash_last) r->hash_last = f;
    for (int k = 0; k < ROOM_MAX; k++) {
        RoomLink *l = &r->link[k];
        if (!l->used || l->pend_f[f & 63] != f) continue;
        compare(r, l, f, l->pend_h[f & 63]);
        l->pend_f[f & 63] = -1;
    }
}

static int local_adv(Room *r)
{
    if (r->slot < 0) return 0;
    int o = 1 - r->slot;
    int32_t oc = r->known_cur[o];
    if (oc < 0 || r->cur <= 0) return 0;
    int p = room_ping(r);
    int32_t one_way = p > 0 ? (int32_t)((p / 2 + 8) / 17) : 0;
    int32_t d = (r->cur - 1) - (oc + one_way);
    return d > 100 ? 100 : d < -100 ? -100 : (int)d;
}

int room_frame_advantage(Room *r, int32_t cur)
{
    r->cur = cur;
    if (r->slot < 0) return 0;
    int mine = local_adv(r);
    r->known_adv[r->slot] = mine;
    if (r->known_cur[1 - r->slot] < 0) return 0;
    int d = mine - r->known_adv[1 - r->slot];
    return d >= 0 ? d / 2 : -((-d + 1) / 2);
}

/* ------------------------------------------------------------ addresses */
int net_parse_addr(const char *s, NetAddr *out)
{
    unsigned a[4], port = 0;
    const char *c = s;
    while (*c == ' ') c++;
    for (int k = 0; k < 4; k++) {
        if (*c < '0' || *c > '9') return -1;
        unsigned v = 0;
        int digits = 0;
        while (*c >= '0' && *c <= '9' && digits < 4) { v = v * 10 + (unsigned)(*c - '0'); c++; digits++; }
        if (v > 255) return -1;
        a[k] = v;
        if (k < 3) { if (*c != '.') return -1; c++; }
    }
    if (*c == ':') {
        c++;
        if (*c < '0' || *c > '9') return -1;
        while (*c >= '0' && *c <= '9') { port = port * 10 + (unsigned)(*c - '0'); c++; if (port > 65535) return -1; }
    }
    while (*c == ' ') c++;
    if (*c) return -1;
    out->ip = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
    out->port = (uint16_t)(port ? port : NET_PORT);
    return 0;
}

void net_format_addr(const NetAddr *a, char *buf, int n, int with_port)
{
    if (with_port && a->port != NET_PORT)
        snprintf(buf, (size_t)n, "%u.%u.%u.%u:%u", a->ip >> 24, (a->ip >> 16) & 255, (a->ip >> 8) & 255, a->ip & 255, a->port);
    else
        snprintf(buf, (size_t)n, "%u.%u.%u.%u", a->ip >> 24, (a->ip >> 16) & 255, (a->ip >> 8) & 255, a->ip & 255);
}

#define STUN_COOKIE 0x2112A442u
static void wb32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rb32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rb16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int net_stun_request(uint8_t out[20], const uint8_t tid[12])
{
    out[0] = 0x00; out[1] = 0x01;
    out[2] = 0; out[3] = 0;
    wb32(out + 4, STUN_COOKIE);
    memcpy(out + 8, tid, 12);
    return 20;
}

int net_stun_parse(const uint8_t *buf, int n, const uint8_t tid[12], NetAddr *mapped)
{
    if (n < 20 || rb16(buf) != 0x0101 || rb32(buf + 4) != STUN_COOKIE || memcmp(buf + 8, tid, 12)) return -1;
    int len = rb16(buf + 2), pos = 20, got = -1;
    if (20 + len > n) len = n - 20;
    while (pos + 4 <= 20 + len) {
        int type = rb16(buf + pos), alen = rb16(buf + pos + 2);
        const uint8_t *a = buf + pos + 4;
        if (pos + 4 + alen > 20 + len) break;
        if ((type == 0x0020 || type == 0x8020) && alen >= 8 && a[1] == 1) {
            mapped->port = (uint16_t)(rb16(a + 2) ^ (STUN_COOKIE >> 16));
            mapped->ip = rb32(a + 4) ^ STUN_COOKIE;
            return 0;
        }
        if (type == 0x0001 && alen >= 8 && a[1] == 1 && got < 0) {
            mapped->port = rb16(a + 2);
            mapped->ip = rb32(a + 4);
            got = 0;
        }
        pos += 4 + ((alen + 3) & ~3);
    }
    return got;
}
