/*
 * Online play: rooms, matches and the lockstep (port/platform/net/room.c).
 *
 * A room is hosted by one player's console (member 0); up to ROOM_MAX
 * consoles join it over UDP.  The host decides who plays: in 1v1 the host
 * and the first player to join (the others watch); in King of the Hill the
 * winner stays and the next in the queue challenges.  A match (a
 * "session") is played by two members, on P1 and P2, and every other member
 * of the room watches it live: all of them restart the game and run the
 * same frames with the same two pads (delay-based lockstep), so a watcher's
 * game is the match itself, a few frames behind.
 *
 * Inputs: each player sends its pad for frame f + delay while it runs frame
 * f.  Every console keeps both players' inputs and passes on what the
 * console at the other end of each link has not acknowledged yet, so the
 * host relays the players' inputs to the watchers (and between the two
 * players when neither is the host and they cannot reach each other
 * directly; they try first).  Each console also sends a fingerprint of its
 * game memory per frame; links compare them (a difference = out of sync).
 *
 * Plain C over a datagram transport given by the caller (NetIo): the 3DS
 * (port/platform/3ds/online_3ds.c) and a desktop test with a simulated
 * network (port/tools/room_test.c).  No threads, no allocation: call
 * room_poll every frame and while waiting.
 */
#ifndef PORT_ROOM_H
#define PORT_ROOM_H
#include <stdint.h>

#define NET_PORT 27886
#define NET_VERSION 3
#define NET_RING 4096           /* frames of inputs / fingerprints kept */
#define NET_MAX_DELAY 8
#define ROOM_MAX 8

typedef struct NetAddr { uint32_t ip; uint16_t port; } NetAddr;   /* host byte order */

typedef struct NetIo {
    void *ctx;
    int (*send)(void *ctx, const NetAddr *to, const void *buf, int n);
    int (*recv)(void *ctx, NetAddr *from, void *buf, int cap);    /* <= 0: nothing waiting */
    uint64_t (*now_ms)(void *ctx);
} NetIo;

typedef struct NetHello {
    uint32_t build;        /* both must run the same program */
    uint32_t disc;         /* and the same game (the disc's executable) */
    char version[16];
} NetHello;

enum { ROOM_1V1, ROOM_KOTH };
enum RoomPhase {
    RP_IDLE,
    RP_JOINING,       /* sending hellos to the host */
    RP_IN_ROOM,       /* a member (or the host) */
    RP_CLOSED,        /* left / lost / refused: room_message says why */
};

typedef struct RoomMember {
    uint8_t used;
    char name[16];
    char pid[13];          /* the leaderboard's player id (12 letters / digits) */
    NetAddr addr;          /* as the host sees it (the internet address) */
    NetAddr lan;           /* its own address on its network */
    uint16_t wins, played;
    uint8_t streak;
    int32_t cur;           /* its last frame run, as the host last heard */
} RoomMember;

/* the room as the host keeps it; every member gets a copy */
typedef struct RoomInfo {
    uint32_t version;
    uint8_t mode;
    RoomMember m[ROOM_MAX];
    uint32_t session;      /* the match being played, 0 = none */
    int8_t p[2];           /* the members on P1 and P2 */
    uint8_t delay;
    int64_t epoch;         /* the game clock both consoles use */
    uint8_t session_mask;  /* the members taking part (playing or watching) */
    int8_t next;           /* King of the Hill: the next challenger, -1 */
    char result[48];       /* the last match's result */
} RoomInfo;

typedef struct RoomLink {
    uint8_t used, up;
    int member;
    NetAddr addr;
    uint64_t t_rx, t_tx, t_hello;
    uint32_t sess;         /* the session the far end is in */
    int32_t ack[2];        /* it has slot s's inputs through ack[s] */
    int32_t base;
    uint32_t echo_t;
    uint64_t echo_rx;
    int rtt[8], nrtt, rtt_i;
    /* fingerprints that arrived before ours */
    int32_t pend_f[64];
    uint32_t pend_h[64];
} RoomLink;

typedef struct Room {
    NetIo io;
    NetHello me;
    char name[16];
    char pid[13];          /* ours (room_init's caller sets it) */
    NetAddr lan;           /* our address on our network */
    NetAddr pub;           /* as the host sees us */
    int phase, host;       /* host: we run the room */
    int id;                /* our member id (host: 0) */
    uint32_t room_id;
    RoomInfo info;
    RoomLink link[ROOM_MAX];   /* host: per member; member: [0] the host, [other player] direct */
    NetAddr join_addr, join_lan;
    uint64_t t_join, t_last_room, t_result;
    NetAddr punch[ROOM_MAX];   /* the host also sends hellos to these (joins from the lobby server) */
    uint64_t punch_until[ROOM_MAX];
    int8_t queue[ROOM_MAX];    /* host, King of the Hill: who plays next, in order */
    int nqueue;
    int winner_slot;           /* host: the last match's winner (-1 draw / none) */
    char msg[96];
    /* the match this console takes part in */
    uint32_t sess;
    int slot;              /* 0 / 1 playing, -1 watching */
    int32_t base;
    int32_t last[2];       /* inputs of slot s known through last[s] */
    uint16_t in[2][NET_RING];
    int32_t known_cur[2];  /* the players' last frames, as heard */
    int known_adv[2];
    uint32_t hash_mine[NET_RING];
    int32_t hash_f[NET_RING];
    int32_t hash_last, cur;
    int32_t desync;        /* first frame whose fingerprints differ, -1 */
    int desync_member;
    int32_t dump_at;
    int result_done;
    uint32_t rx_packets, tx_packets, rx_bad;
} Room;

void room_init(Room *r, const NetIo *io, const NetHello *me, const char *name, NetAddr lan);
void room_host(Room *r, int mode);
void room_join(Room *r, NetAddr host, NetAddr host_lan);   /* host_lan.ip 0 = none */
void room_punch(Room *r, NetAddr guest);   /* host: a guest coming through the lobby server */
void room_poll(Room *r);
void room_leave(Room *r, const char *why);
const char *room_message(const Room *r);
int room_ping(const Room *r);              /* ms to the host (host: to the other player), -1 */
int room_count(const Room *r);             /* members */

/* the host */
int room_can_start(const Room *r);
void room_start(Room *r);                  /* the first match (and King of the Hill's rounds after it) */
void room_set_mode(Room *r, int mode);
/* any console: the match's result (slot 0 / 1 won, -1 draw); the host
 * acts on its own: scores, King of the Hill's next round */
void room_result(Room *r, int winner_slot);
void room_poll_rounds(Room *r);            /* host: starts the next round when it is time */

/* the match this console must run: 0 = none (or not part of it) */
uint32_t room_session(const Room *r);
int room_slot(const Room *r);              /* 0 / 1 playing, -1 watching */
void room_begin(Room *r, int32_t base);    /* the boot reached the character select at base */
int room_began_all(const Room *r);         /* the players' boots ended at the same frame */
int room_ready(const Room *r, int32_t frame);
void room_set_input(Room *r, int32_t cur, uint16_t buttons);
void room_inputs(const Room *r, int32_t frame, uint16_t out[2]);
void room_fingerprint(Room *r, int32_t frame, uint32_t h);
int room_frame_advantage(Room *r, int32_t cur);
int room_delay(const Room *r);

/* address text "1.2.3.4" or "1.2.3.4:5678" (no port = NET_PORT) */
int net_parse_addr(const char *s, NetAddr *out);
void net_format_addr(const NetAddr *a, char *buf, int n, int with_port);
/* STUN (RFC 5389) binding request / response, for the address the internet sees */
int net_stun_request(uint8_t out[20], const uint8_t tid[12]);
int net_stun_parse(const uint8_t *buf, int n, const uint8_t tid[12], NetAddr *mapped);
#endif
