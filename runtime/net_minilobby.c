/* net_minilobby.c: a lobby inside the game, so LAN mode and direct-over-internet mode need no server of ours.
 * C port of tools_net/ssbu_lobby.py (same wire protocol "SU" version 1, see the top of net_relay.c); design adapted
 * from SNO2207/WindowSmash, used with the author's permission.
 * #included by net_relay.c: only static functions and data (every name starts with ml_ / ML_ / minilobby_).
 *   minilobby_start / minilobby_stop / minilobby_counts / minilobby_socket / minilobby_open_to (see below).
 * Standalone test: clang -DMINILOBBY_MAIN -x c net_minilobby.c -o minilobby.exe -lws2_32
 *                  minilobby.exe <port> [1 = no PEERS]  */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

enum { ML_PROTO = 1, ML_JOIN = 1, ML_WELCOME, ML_REJECT, ML_PING, ML_PONG, ML_DATA, ML_LEAVE, ML_LIST, ML_LISTING,
       ML_LIST_END, ML_PEERS, ML_PUNCH, ML_DIRECT, ML_BCAST };
enum { ML_ROOMS = 17, ML_ROOM_INFO, ML_ROOMS_END };     /* O1 room browser (net_relay.c head, ssbu_lobby.py on_rooms) */
enum { ML_MATCH = 20, ML_MATCH_STATE };                 /* O2 matchmaking (net_relay.c head, ssbu_lobby.py on_match) */
enum { ML_MS_IDLE, ML_MS_SEARCHING, ML_MS_MATCHED, ML_MS_TIMEOUT, ML_MS_PEER_LEFT, ML_MS_TOGETHER, ML_MS_EXPIRED };
enum { ML_KIND_ROOM = 1, ML_KIND_ROLLBACK = 2 };        /* (R3 net3, lobby 1.4) MATCH op 1 / op 2: a Local Wireless search, a rollback-match search */
#define ML_SEARCH_FOR 120000                            /* ms; SSBU_LOBBY_SEARCH_SECS / SSBU_LOBBY_MATCH_SECS change them */
#define ML_MATCH_FOR 180000
#define ML_MODS_MAX 32
#define ML_MOD_NAME 31
#define ML_MODS_BYTES (1 + ML_MODS_MAX * (5 + ML_MOD_NAME))
#define ML_ROOMS_MAX 32
enum { ML_M_ADVERTISE = 1, ML_M_JOIN, ML_M_ACCEPT, ML_M_REJECT, ML_M_KEEP, ML_M_LEAVE, ML_M_GONE, ML_M_KICK };
#define ML_NAME_LEN 32
#define ML_BUILD_LEN 40
#define ML_JOIN_LEN (4 + 8 + 2 * ML_NAME_LEN + ML_BUILD_LEN + 4 + 2 + 1)
#define ML_SUBNET 0x0A0D0000u
#define ML_BCAST0 0x0A0DFFFFu
#define ML_BCAST1 0xFFFFFFFFu
#define ML_LDN_PORT 65511
#define ML_NI_SIZE 0x480
#define ML_NI_SESSION 0x10
#define ML_NI_POLICY 0x62
#define ML_NI_NODE_MAX 0x66
#define ML_NI_NODE_COUNT 0x67
#define ML_NI_NODES 0x68
#define ML_NODE_LEN 0x40
#define ML_LIST_MAX 24
#define ML_CLIENT_TIMEOUT 10000
#define ML_ROOM_TIMEOUT 10000
#define ML_LISTED_FOR 4000
#define ML_MAX_DATA 1500
#define ML_MAX_CLIENTS 256
#define ML_MAX_ROOMS 256
#define ML_PEER_LEN 56   /* ip4 name32 pubip4 port2 lanip4 lanport2 secret8 */
#define ML_OPEN_MAX 8

typedef struct ml_room ml_room;
typedef struct ml_client {
    int used;
    struct sockaddr_in at;
    uint8_t nonce[8];
    uint32_t ip;
    char name[ML_NAME_LEN + 1], build[ML_BUILD_LEN + 1], code[ML_NAME_LEN + 1];
    uint32_t lan_ip;
    uint16_t lan_port;
    int direct;
    uint64_t cookie;
    ULONGLONG seen;
    ml_room *room;            /* the room this player hosts */
    int has_claim;
    uint8_t claim[16];        /* session id of the room it asked to join */
    uint32_t peers_version;
    int peers_valid, peers_len;
    uint8_t peers_sent[7 * ML_PEER_LEN];
    int mods_len;             /* O1: its mod-set block (0: an older game that does not send it) */
    uint8_t mods[ML_MODS_BYTES];
    /* O2 matchmaking */
    int has_search, searching, room_said;
    uint32_t search_id;
    ULONGLONG search_since, match_at;
    struct ml_client *mpeer;  /* the player it is matched with (NULL: none) */
    int mrole;                /* 1 it hosts, 2 it joins */
    int has_final;            /* how its last search ended (sent again when asked about that search) */
    uint32_t final_id, final_peer;
    int final_state, final_role;
    char final_name[ML_NAME_LEN + 1];
    int kind;                 /* (1.4) what it searches: ML_KIND_ROOM / ML_KIND_ROLLBACK */
    uint32_t rb_peer;         /* (1.4) the player of its last rollback match (RB to it relayed on the public code) */
    int rb_seen;              /* (1.4, rollback match) 1 = this one's RB to its peer was relayed */
} ml_client;

struct ml_room {
    int used;
    ml_client *owner;
    uint8_t session[16];
    uint8_t info[ML_NI_SIZE];
    uint32_t nodes[7];        /* station addresses (node 0 is the owner) */
    int nnodes;
    ULONGLONG created, updated;
    struct { uint32_t a, b; uint64_t s; } pairs[64];
    int npairs, pnext;
};

static ml_client ml_clients[ML_MAX_CLIENTS];
static ml_room ml_rooms[ML_MAX_ROOMS];
static uint32_t ml_next_ip = ML_SUBNET + 2;
static unsigned long long ml_relayed, ml_ldn_msgs, ml_dropped;
static CRITICAL_SECTION ml_lock;
static int ml_lock_ready;
static SOCKET ml_sock = INVALID_SOCKET;
static volatile LONG ml_running;
static HANDLE ml_thread;
static int ml_no_peers;
static struct { struct sockaddr_in at; int used; } ml_open[ML_OPEN_MAX];
static uint64_t ml_rng_state;
static ULONGLONG ml_search_for = ML_SEARCH_FOR, ml_match_for = ML_MATCH_FOR;

#define ml_log(...) do { printf("[online] lobby: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* ---- small helpers */
static uint32_t ml_get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t ml_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t ml_get64(const uint8_t *p) { return (uint64_t)ml_get32(p) << 32 | ml_get32(p + 4); }
static uint8_t *ml_put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; return p + 4; }
static uint8_t *ml_put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = (uint8_t)v; return p + 2; }
static uint8_t *ml_put64(uint8_t *p, uint64_t v) { return ml_put32(ml_put32(p, (uint32_t)(v >> 32)), (uint32_t)v); }
static uint8_t *ml_hdr(uint8_t *p, int kind) { p[0] = 'S'; p[1] = 'U'; p[2] = ML_PROTO; p[3] = (uint8_t)kind; return p + 4; }

static void ml_text(char *dst, size_t cap, const uint8_t *raw, size_t n) {   /* NUL-terminated string field */
    size_t i = 0;
    while (i < n && i + 1 < cap && raw[i]) { dst[i] = (char)raw[i]; i++; }
    dst[i] = 0;
}

static uint64_t ml_random64(void) {
    static int tried;
    uint64_t v = 0;
#ifdef _WIN32
    static BOOLEAN (WINAPI *rtlgen)(PVOID, ULONG);
    if (!tried) {
        HMODULE m = LoadLibraryA("advapi32.dll");
        rtlgen = m ? (BOOLEAN (WINAPI *)(PVOID, ULONG))GetProcAddress(m, "SystemFunction036") : NULL;
        ml_rng_state = GetTickCount64() * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId();
        tried = 1;
    }
    if (rtlgen && rtlgen(&v, sizeof v)) return v;
#else
    if (!tried) { ml_rng_state = GetTickCount64() * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId(); tried = 1; }
    if (!getentropy(&v, sizeof v)) return v;           /* (the system's random bytes; unistd.h) */
#endif
    ml_rng_state ^= ml_rng_state << 13; ml_rng_state ^= ml_rng_state >> 7; ml_rng_state ^= ml_rng_state << 17;
    return ml_rng_state;
}

static const char *ml_vip(uint32_t ip) {
    static char b[4][24];
    static int i;
    char *s = b[i++ & 3];
    snprintf(s, 24, "10.13.%u.%u", ip >> 8 & 255, ip & 255);
    return s;
}

static int ml_same_at(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static int ml_is_bcast(uint32_t ip) { return ip == ML_BCAST0 || ip == ML_BCAST1; }
static int ml_same_part(const ml_client *a, const ml_client *b) { return !strcmp(a->build, b->build) && !strcmp(a->code, b->code); }

static void ml_send(const void *data, int len, const struct sockaddr_in *at) {
    if (ml_sock != INVALID_SOCKET) sendto(ml_sock, (const char *)data, len, 0, (const struct sockaddr *)at, sizeof *at);
}

/* ---- tables */
static ml_client *ml_by_at(const struct sockaddr_in *at) {
    for (int i = 0; i < ML_MAX_CLIENTS; i++) if (ml_clients[i].used && ml_same_at(&ml_clients[i].at, at)) return &ml_clients[i];
    return NULL;
}
static ml_client *ml_by_ip(uint32_t ip) {
    for (int i = 0; i < ML_MAX_CLIENTS; i++) if (ml_clients[i].used && ml_clients[i].ip == ip) return &ml_clients[i];
    return NULL;
}
static ml_room *ml_find_room(const uint8_t *session) {
    for (int i = 0; i < ML_MAX_ROOMS; i++) if (ml_rooms[i].used && !memcmp(ml_rooms[i].session, session, 16)) return &ml_rooms[i];
    return NULL;
}
static int ml_has_node(const ml_room *r, uint32_t ip) {
    for (int i = 0; i < r->nnodes; i++) if (r->nodes[i] == ip) return 1;
    return 0;
}
static int ml_members(const ml_room *r, ml_client **out) {   /* the owner, then the players that claimed this room */
    int n = 0;
    out[n++] = r->owner;
    for (int i = 0; i < r->nnodes; i++) {
        ml_client *c = ml_by_ip(r->nodes[i]);
        if (c && c->has_claim && !memcmp(c->claim, r->session, 16)) out[n++] = c;
    }
    return n;
}
static ml_room *ml_room_of(ml_client *c) {
    if (c->room) return c->room;
    ml_room *r = c->has_claim ? ml_find_room(c->claim) : NULL;
    return r && ml_has_node(r, c->ip) ? r : NULL;
}
static uint32_t ml_new_ip(void) {
    for (int i = 0; i < 0x10000; i++) {
        uint32_t ip = ml_next_ip;
        ml_next_ip = ip >= ML_SUBNET + 0xFFFE ? ML_SUBNET + 2 : ip + 1;
        if ((ip & 0xFF) != 0 && (ip & 0xFF) != 255 && !ml_by_ip(ip)) return ip;
    }
    return 0;
}

/* ---- direct paths: each member of a room learns the others' addresses and a pair secret */
static void ml_send_peers(ml_client *c) {
    if (ml_no_peers) return;
    ml_room *room = ml_room_of(c);
    uint8_t body[7 * ML_PEER_LEN], pkt[9 + sizeof body];
    int n = 0;
    if (room && c->direct) {
        ml_client *mem[8];
        int cnt = ml_members(room, mem);
        for (int i = 0; i < cnt; i++) {
            ml_client *m = mem[i];
            if (m == c || !m->direct || n >= 7) continue;
            uint32_t a = c->ip < m->ip ? c->ip : m->ip, b = c->ip < m->ip ? m->ip : c->ip;
            uint64_t secret = 0;
            for (int k = 0; k < room->npairs; k++) if (room->pairs[k].a == a && room->pairs[k].b == b) secret = room->pairs[k].s;
            if (!secret) {
                int slot = room->npairs < 64 ? room->npairs++ : room->pnext++ & 63;
                room->pairs[slot].a = a; room->pairs[slot].b = b;
                secret = room->pairs[slot].s = ml_random64() | 1;
            }
            uint8_t *p = body + n * ML_PEER_LEN;
            memset(p, 0, ML_PEER_LEN);
            ml_put32(p, m->ip);
            memcpy(p + 4, m->name, strlen(m->name));
            ml_put32(p + 36, ntohl(m->at.sin_addr.s_addr));
            ml_put16(p + 40, ntohs(m->at.sin_port));
            ml_put32(p + 42, m->lan_ip);
            ml_put16(p + 46, m->lan_port);
            ml_put64(p + 48, secret);
            n++;
        }
    }
    int blen = n * ML_PEER_LEN;
    if (c->peers_valid && c->peers_len == blen && !memcmp(c->peers_sent, body, blen)) return;
    c->peers_valid = 1; c->peers_len = blen;
    memcpy(c->peers_sent, body, blen);
    uint8_t *p = ml_put32(ml_hdr(pkt, ML_PEERS), ++c->peers_version);
    *p++ = (uint8_t)n;
    memcpy(p, body, blen);
    ml_send(pkt, (int)(p + blen - pkt), &c->at);
}

static void ml_close_room(ml_room *room, const char *why) {
    uint8_t gone[4 + 12 + 4 + 16], *p;
    ml_client *mem[8];
    int cnt = ml_members(room, mem);
    p = ml_hdr(gone, ML_DATA);
    p = ml_put32(p, room->owner->ip); p = ml_put32(p, ML_BCAST0); p = ml_put16(p, ML_LDN_PORT); p = ml_put16(p, ML_LDN_PORT);
    *p++ = 'L'; *p++ = ML_M_GONE; *p++ = 0; *p++ = 0;
    memcpy(p, room->session, 16);
    for (int i = 1; i < cnt; i++) {
        ml_send(gone, sizeof gone, &mem[i]->at);
        mem[i]->has_claim = 0;
        ml_send_peers(mem[i]);
    }
    ml_client *owner = room->owner;
    if (owner->room == room) owner->room = NULL;
    room->used = 0;
    if (owner->used) ml_send_peers(owner);
    ml_log("room of %s closed (%s)", owner->name, why);
}

static void ml_match_end(ml_client *c, int state, int role, const ml_client *peer, const char *why, ULONGLONG now);
static void ml_drop(ml_client *c, const char *why) {
    if (c->mpeer) {
        ml_client *o = c->mpeer;
        char w[96];
        c->mpeer = NULL;
        snprintf(w, sizeof w, "%s left the lobby", c->name);
        ml_match_end(o, ML_MS_PEER_LEFT, o->mrole, c, w, GetTickCount64());
    }
    c->searching = 0;
    if (c->room) ml_close_room(c->room, "host left");
    if (c->has_claim) {
        ml_room *r = ml_find_room(c->claim);
        if (r && ml_has_node(r, c->ip)) {   /* tell its host at once */
            uint8_t pkt[4 + 12 + 4 + 16], *p = ml_hdr(pkt, ML_DATA);
            p = ml_put32(p, c->ip); p = ml_put32(p, r->owner->ip); p = ml_put16(p, ML_LDN_PORT); p = ml_put16(p, ML_LDN_PORT);
            *p++ = 'L'; *p++ = ML_M_LEAVE; *p++ = 0; *p++ = 0;
            memcpy(p, r->session, 16);
            ml_send(pkt, sizeof pkt, &r->owner->at);
        }
    }
    c->used = 0;
    ml_log("%s (%s) left: %s", c->name, ml_vip(c->ip), why);
}

/* ---- packets */
static void ml_on_join(const uint8_t *p, int len, const struct sockaddr_in *at, ml_client *c, ULONGLONG now) {
    if (len < ML_JOIN_LEN) return;
    const uint8_t *nonce = p + 4;
    char code[ML_NAME_LEN + 1], name[ML_NAME_LEN + 1], build[ML_BUILD_LEN + 1];
    ml_text(code, sizeof code, p + 12, 32);
    ml_text(name, sizeof name, p + 44, 32);
    if (!name[0]) snprintf(name, sizeof name, "Player");
    ml_text(build, sizeof build, p + 76, 40);
    uint32_t lan_ip = ml_get32(p + 116);
    uint16_t lan_port = ml_get16(p + 120);
    int direct = p[122] & 1;
    if (c && memcmp(c->nonce, nonce, 8)) { ml_drop(c, "restarted"); c = NULL; }
    if (!c) {   /* the same game from a new port (its router remapped it)? */
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            ml_client *o = &ml_clients[i];
            if (o->used && !memcmp(o->nonce, nonce, 8) && !strcmp(o->build, build) && !strcmp(o->code, code)) { o->at = *at; c = o; break; }
        }
    }
    if (!c) {
        uint32_t ip = ml_new_ip();
        for (int i = 0; ip && i < ML_MAX_CLIENTS && !c; i++) if (!ml_clients[i].used) c = &ml_clients[i];
        if (!c) {
            uint8_t rej[4 + 8 + 64], *r = ml_hdr(rej, ML_REJECT);
            memset(r, 0, 8 + 64);
            memcpy(r, nonce, 8);
            memcpy(r + 8, "the lobby is full", 17);
            ml_send(rej, sizeof rej, at);
            return;
        }
        memset(c, 0, sizeof *c);
        c->used = 1; c->at = *at; c->ip = ip; c->cookie = ml_random64();
        memcpy(c->nonce, nonce, 8);
        snprintf(c->name, sizeof c->name, "%s", name); snprintf(c->build, sizeof c->build, "%s", build); snprintf(c->code, sizeof c->code, "%s", code);
        ml_log("%s joined (code '%s', build %s) as %s from %u.%u.%u.%u:%d, LAN %u.%u.%u.%u:%u, direct %s", name, code, build, ml_vip(ip),
               ((uint8_t *)&at->sin_addr)[0], ((uint8_t *)&at->sin_addr)[1], ((uint8_t *)&at->sin_addr)[2], ((uint8_t *)&at->sin_addr)[3], ntohs(at->sin_port), lan_ip >> 24, lan_ip >> 16 & 255, lan_ip >> 8 & 255, lan_ip & 255,
               lan_port, direct ? "yes" : "no");
    }
    c->seen = now; c->lan_ip = lan_ip; c->lan_port = lan_port; c->direct = direct;
    if (len > ML_JOIN_LEN + 2 && p[ML_JOIN_LEN] == 'M' && p[ML_JOIN_LEN + 1] == 'S') {      /* O1: the game's mods */
        const uint8_t *m = p + ML_JOIN_LEN + 2;
        int n = len - ML_JOIN_LEN - 2, o = 1, ok = n >= 1 && m[0] <= ML_MODS_MAX;
        for (int i = 0; ok && i < m[0]; i++) {
            if (o + 5 > n || m[o + 4] > ML_MOD_NAME || o + 5 + m[o + 4] > n) ok = 0;
            else o += 5 + m[o + 4];
        }
        c->mods_len = ok ? o : 0;
        if (ok) memcpy(c->mods, m, (size_t)o);
    }
    uint8_t w[4 + 8 + 4 + 8], *q = ml_hdr(w, ML_WELCOME);
    memcpy(q, nonce, 8);
    q = ml_put64(ml_put32(q + 8, c->ip), c->cookie);
    ml_send(w, sizeof w, at);
}

static int ml_room_rank(const ml_room *r) {   /* open rooms first, then fuller ones */
    int open = r->info[ML_NI_POLICY] != 1 && r->info[ML_NI_NODE_COUNT] < r->info[ML_NI_NODE_MAX];
    return (open ? 0 : 1000) - r->info[ML_NI_NODE_COUNT];
}

/* the rooms c may see, best first: other = 0 its own build only (the game's list of an older game), 1 every build of its
 * lobby code (O1: a newer game's list, and the room browser), its own build's rooms first */
static int ml_select(ml_client *c, ULONGLONG now, int other, int with_own, ml_room **sel, int max) {
    int n = 0;
    const ml_client *mhost = c->mpeer && c->mrole == 2 ? c->mpeer : NULL;     /* O2: the matched host's room first */
    for (int i = 0; i < ML_MAX_ROOMS; i++) {
        ml_room *r = &ml_rooms[i];
        if (!r->used || (r->owner == c && !with_own) || now - r->updated > ML_LISTED_FOR) continue;
        if (other ? strcmp(r->owner->code, c->code) != 0 : !ml_same_part(r->owner, c)) continue;
        int k = n < max ? n++ : max;   /* insertion sort keeping the best max */
        sel[k] = r;
        while (k > 0) {
            int ra = ml_room_rank(sel[k]) + (ml_same_part(sel[k]->owner, c) ? 0 : 10000) - (sel[k]->owner == mhost ? 100000 : 0),
                rb = ml_room_rank(sel[k - 1]) + (ml_same_part(sel[k - 1]->owner, c) ? 0 : 10000) - (sel[k - 1]->owner == mhost ? 100000 : 0);
            if (!(ra < rb || (ra == rb && sel[k]->created < sel[k - 1]->created))) break;
            ml_room *t = sel[k]; sel[k] = sel[k - 1]; sel[k - 1] = t; k--;
        }
    }
    return n;
}

static void ml_on_list(ml_client *c, const uint8_t *seq, ULONGLONG now, int other) {
    ml_room *sel[ML_LIST_MAX + 1];
    int n = ml_select(c, now, other, 0, sel, ML_LIST_MAX);
    for (int i = 0; i < n; i++) {
        uint8_t pkt[4 + 4 + 4 + ML_NI_SIZE + ML_BUILD_LEN], *p = ml_hdr(pkt, ML_LISTING);
        memcpy(p, seq, 4);
        ml_put32(p + 4, sel[i]->owner->ip);
        memcpy(p + 8, sel[i]->info, ML_NI_SIZE);
        memset(p + 8 + ML_NI_SIZE, 0, ML_BUILD_LEN);       /* O1: the host's build (only asked with flag 1) */
        memcpy(p + 8 + ML_NI_SIZE, sel[i]->owner->build, strlen(sel[i]->owner->build));
        ml_send(pkt, other ? (int)sizeof pkt : (int)sizeof pkt - ML_BUILD_LEN, &c->at);
    }
    uint8_t end[4 + 4 + 1], *p = ml_hdr(end, ML_LIST_END);
    memcpy(p, seq, 4);
    p[4] = (uint8_t)n;
    ml_send(end, sizeof end, &c->at);
    if (n) ml_log("%s scanned: %d room(s)", c->name, n);
}

static void ml_on_rooms(ml_client *c, const uint8_t *seq, ULONGLONG now) {     /* O1: the room browser */
    ml_room *sel[ML_ROOMS_MAX + 1];
    int n = ml_select(c, now, 1, 1, sel, ML_ROOMS_MAX);
    for (int i = 0; i < n; i++) {
        ml_room *r = sel[i];
        ml_client *o = r->owner;
        uint8_t pkt[4 + 4 + 4 + 3 + ML_NAME_LEN + ML_BUILD_LEN + ML_MODS_BYTES], *p = ml_hdr(pkt, ML_ROOM_INFO);
        int open = r->info[ML_NI_POLICY] != 1 && r->info[ML_NI_NODE_COUNT] < r->info[ML_NI_NODE_MAX];
        memcpy(p, seq, 4);
        ml_put32(p + 4, o->ip);
        p[8] = r->info[ML_NI_NODE_COUNT]; p[9] = r->info[ML_NI_NODE_MAX];
        p[10] = (uint8_t)((open ? 1 : 0) | (o == c ? 2 : 0) | (o->mods_len ? 4 : 0));
        memset(p + 11, 0, ML_NAME_LEN + ML_BUILD_LEN);
        memcpy(p + 11, o->name, strlen(o->name));
        memcpy(p + 11 + ML_NAME_LEN, o->build, strlen(o->build));
        uint8_t *m = p + 11 + ML_NAME_LEN + ML_BUILD_LEN;
        if (o->mods_len) { memcpy(m, o->mods, (size_t)o->mods_len); m += o->mods_len; } else *m++ = 0;
        ml_send(pkt, (int)(m - pkt), &c->at);
    }
    uint8_t end[4 + 4 + 1], *p = ml_hdr(end, ML_ROOMS_END);
    memcpy(p, seq, 4);
    p[4] = (uint8_t)n;
    ml_send(end, sizeof end, &c->at);
}

/* ---- O2 matchmaking (ssbu_lobby.py match_send / match_end / on_match / match_room) */
static int ml_room_open(const ml_room *r, ULONGLONG now) {
    return r && r->info[ML_NI_POLICY] != 1 && r->info[ML_NI_NODE_COUNT] < r->info[ML_NI_NODE_MAX] && now - r->updated <= ML_LISTED_FOR;
}
/* MATCH_STATE: search id u32, state u8, role u8, flags u8 (1 the host's room is open), peer u32, peer name[32],
 * searching u16, players u16 (of c's build and code), seconds waited u16 */
static void ml_match_send(ml_client *c, ULONGLONG now) {
    uint8_t pkt[4 + 49], *p = ml_hdr(pkt, ML_MATCH_STATE);
    uint32_t sid = c->search_id, peer = 0;
    int state, role = 0, flags = 0, searching = 0, players = 0;
    const char *name = "";
    ULONGLONG since = 0;
    if (c->mpeer) {
        state = ML_MS_MATCHED; role = c->mrole; peer = c->mpeer->ip; name = c->mpeer->name; since = c->match_at;
        ml_client *host = c->mrole == 1 ? c : c->mpeer;
        if (ml_room_open(host->room, now)) flags |= 1;
    } else if (c->searching) { state = ML_MS_SEARCHING; since = c->search_since; }
    else if (c->has_final) { sid = c->final_id; state = c->final_state; role = c->final_role; peer = c->final_peer; name = c->final_name; }
    else state = ML_MS_IDLE;
    for (int i = 0; i < ML_MAX_CLIENTS; i++)
        if (ml_clients[i].used && ml_same_part(&ml_clients[i], c)) { players++; searching += ml_clients[i].searching; }
    ULONGLONG waited = since ? (now - since) / 1000 : 0;
    p = ml_put32(p, sid);
    *p++ = (uint8_t)state; *p++ = (uint8_t)role; *p++ = (uint8_t)flags;
    p = ml_put32(p, peer);
    memset(p, 0, ML_NAME_LEN); memcpy(p, name, strlen(name)); p += ML_NAME_LEN;
    p = ml_put16(p, (unsigned)searching); p = ml_put16(p, (unsigned)players); p = ml_put16(p, waited > 65535 ? 65535u : (unsigned)waited);
    ml_send(pkt, (int)(p - pkt), &c->at);
}
static void ml_match_end(ml_client *c, int state, int role, const ml_client *peer, const char *why, ULONGLONG now) {
    c->has_final = 1; c->final_id = c->search_id; c->final_state = state; c->final_role = role;
    c->final_peer = peer ? peer->ip : 0;
    snprintf(c->final_name, sizeof c->final_name, "%s", peer ? peer->name : "");
    c->searching = 0; c->mpeer = NULL;
    ml_match_send(c, now);
    ml_log("match: %s: %s", c->name, why);
}
static void ml_match_end_pair(ml_client *a, int state, const char *wa, const char *wb, ULONGLONG now) {  /* a and its peer */
    ml_client *b = a->mpeer;
    if (!b) return;
    ml_match_end(a, state, a->mrole, b, wa, now);
    ml_match_end(b, state, b->mrole, a, wb, now);
}
static void ml_match_room(ml_client *host, ml_room *room, ULONGLONG now) {
    ml_client *g = host->mpeer;
    if (!g || host->mrole != 1) return;
    if (ml_has_node(room, g->ip) && g->has_claim && !memcmp(g->claim, room->session, 16)) {
        char wa[96], wb[96];
        snprintf(wa, sizeof wa, "in a room with %s", g->name);
        snprintf(wb, sizeof wb, "in %s's room", host->name);
        ml_match_end_pair(host, ML_MS_TOGETHER, wa, wb, now);
        return;
    }
    if (ml_room_open(room, now) && !host->room_said) { host->room_said = 1; ml_match_send(g, now); ml_match_send(host, now); }
}
static void ml_on_match(ml_client *c, int op, uint32_t sid, ULONGLONG now) {
    char w[96];
    if (op == 0) {
        if (c->mpeer) {
            ml_client *o = c->mpeer;
            c->mpeer = NULL;
            snprintf(w, sizeof w, "%s cancelled the match", c->name);
            ml_match_end(o, ML_MS_PEER_LEFT, o->mrole, c, w, now);
        }
        if (c->searching) ml_log("match: %s stopped searching", c->name);
        c->searching = 0; c->has_search = 1; c->search_id = sid;
        c->has_final = 1; c->final_id = sid; c->final_state = ML_MS_IDLE; c->final_role = 0; c->final_peer = 0; c->final_name[0] = 0;
        ml_match_send(c, now);
        return;
    }
    if (op != ML_KIND_ROOM && op != ML_KIND_ROLLBACK) return;
    if (c->has_search && c->search_id == sid && (c->mpeer || c->searching || c->has_final)) { ml_match_send(c, now); return; }
    if (c->mpeer) {
        ml_client *o = c->mpeer;
        c->mpeer = NULL;
        snprintf(w, sizeof w, "%s searches again", c->name);
        ml_match_end(o, ML_MS_PEER_LEFT, o->mrole, c, w, now);
    }
    c->has_search = 1; c->search_id = sid; c->searching = 1; c->search_since = now; c->has_final = 0; c->kind = op;
    ml_log("match: %s searches an opponent%s (build %s)", c->name, op == ML_KIND_ROLLBACK ? " for a rollback match" : "", c->build);
    ml_client *o = NULL;
    for (int i = 0; i < ML_MAX_CLIENTS; i++) {
        ml_client *x = &ml_clients[i];
        if (x->used && x != c && x->searching && !x->mpeer && x->kind == op && ml_same_part(x, c) && (!o || x->search_since < o->search_since)) o = x;
    }
    if (!o) { ml_match_send(c, now); return; }
    int c_open = op == ML_KIND_ROOM && ml_room_open(c->room, now), o_open = op == ML_KIND_ROOM && ml_room_open(o->room, now);
    ml_client *host = c_open && !o_open ? c : o, *guest = host == c ? o : c;     /* an open room hosts, else the one that waited longer */
    host->mpeer = guest; guest->mpeer = host;
    host->mrole = 1; guest->mrole = 2;
    host->match_at = guest->match_at = now;
    host->searching = guest->searching = 0;
    host->room_said = 0;
    host->rb_seen = guest->rb_seen = 0;
    if (op == ML_KIND_ROLLBACK) { host->rb_peer = guest->ip; guest->rb_peer = host->ip; }
    ml_log("match: %s hosts, %s joins%s", host->name, guest->name, op == ML_KIND_ROLLBACK ? " (rollback match)" : "");
    ml_match_send(host, now);
    ml_match_send(guest, now);
    if (host->room && op == ML_KIND_ROOM) ml_match_room(host, host->room, now);
}

/* NetworkInfo's station addresses (node 0 = owner) into out[]; -1 if it is not one a game makes */
static int ml_room_nodes(const uint8_t *info, uint32_t owner, uint32_t *out) {
    int count = info[ML_NI_NODE_COUNT], most = info[ML_NI_NODE_MAX], n = 0;
    uint32_t ips[8];
    if (!(1 <= count && count <= most && most <= 8)) return -1;
    for (int i = 0; i < 8; i++) {
        const uint8_t *o = info + ML_NI_NODES + ML_NODE_LEN * i;
        if (!o[0xB]) { if (i == 0) return -1; continue; }
        uint32_t ip = (uint32_t)o[0] | o[1] << 8 | o[2] << 16 | (uint32_t)o[3] << 24;   /* little-endian */
        if ((ip & 0xFFFF0000u) != ML_SUBNET || (i == 0 && ip != owner)) return -1;
        for (int k = 0; k < n; k++) if (ips[k] == ip) return -1;
        ips[n++] = ip;
    }
    for (int i = 1; i < n; i++) out[i - 1] = ips[i];
    return n - 1;
}

static void ml_on_advertise(ml_client *c, const uint8_t *p, int len, const uint8_t *info, ULONGLONG now) {
    uint32_t nodes[8];
    int nn = ml_room_nodes(info, c->ip, nodes);
    if (nn < 0) { ml_dropped++; return; }
    const uint8_t *session = info + ML_NI_SESSION;
    ml_room *room = ml_find_room(session);
    if (room && room->owner != c) { ml_dropped++; return; }
    if (!room) {
        if (c->room) ml_close_room(c->room, "replaced by a new room");
        c->has_claim = 0;
        for (int i = 0; i < ML_MAX_ROOMS && !room; i++) if (!ml_rooms[i].used) room = &ml_rooms[i];
        if (!room) { ml_dropped++; return; }
        memset(room, 0, sizeof *room);
        room->used = 1; room->owner = c; room->created = now;
        memcpy(room->session, session, 16);
        c->room = room;
        ml_log("%s opened a room (%d players at most)", c->name, info[ML_NI_NODE_MAX]);
    }
    ml_client *before[8], *after[8];
    int nb = ml_members(room, before);
    int changed = nn != room->nnodes;
    for (int i = 0; i < nn && !changed; i++) changed = !ml_has_node(room, nodes[i]);
    memcpy(room->info, info, ML_NI_SIZE);
    memcpy(room->nodes, nodes, nn * sizeof nodes[0]);
    room->nnodes = nn; room->updated = now;
    int na = ml_members(room, after);
    if (changed) {
        char names[8 * (ML_NAME_LEN + 3)] = "";
        for (int i = 0; i < 8; i++) {
            const uint8_t *o = info + ML_NI_NODES + ML_NODE_LEN * i;
            if (!o[0xB]) continue;
            char nm[ML_NAME_LEN + 1];
            ml_text(nm, sizeof nm, o + 0xC, ML_NAME_LEN);
            if (names[0]) strcat(names, ", ");
            strcat(names, nm[0] ? nm : "?");
        }
        ml_log("room of %s: %d player(s): %s", c->name, info[ML_NI_NODE_COUNT], names);
        for (int i = 0; i < na; i++) ml_send_peers(after[i]);
        for (int i = 0; i < nb; i++) {
            int stays = 0;
            for (int k = 0; k < na; k++) stays |= before[i] == after[k];
            if (!stays) ml_send_peers(before[i]);
        }
    }
    for (int i = 0; i < nb + na; i++) {   /* before and after, each once */
        ml_client *m = i < nb ? before[i] : after[i - nb];
        int dup = 0;
        for (int k = 0; k < i; k++) dup |= m == (k < nb ? before[k] : after[k - nb]);
        if (m != c && !dup) ml_send(p, len, &m->at);
    }
    if (c->mpeer && c->mrole == 1 && c->kind == ML_KIND_ROOM) ml_match_room(c, room, now);
}

static void ml_on_ldn(ml_client *c, const uint8_t *p, int len, uint32_t dst, unsigned sport, unsigned dport, ULONGLONG now) {
    if (sport != ML_LDN_PORT || dport != ML_LDN_PORT || len < 20 || p[16] != 'L') { ml_dropped++; return; }
    ml_ldn_msgs++;
    int kind = p[17], blen = len - 20;
    const uint8_t *body = p + 20;
    if (ml_is_bcast(dst)) {
        if (kind == ML_M_ADVERTISE && blen >= ML_NI_SIZE) ml_on_advertise(c, p, len, body, now);
        else if (kind == ML_M_GONE && c->room && blen >= 16 && !memcmp(body, c->room->session, 16)) ml_close_room(c->room, "closed by its host");
        return;
    }
    ml_client *b = ml_by_ip(dst);
    if (!b || b == c || !ml_same_part(b, c)) { ml_dropped++; return; }
    if (kind == ML_M_JOIN) {
        if (!b->room || blen < 16 || memcmp(body, b->room->session, 16)) { ml_dropped++; return; }
        if (c->room) ml_close_room(c->room, "its host joins another room");
        c->has_claim = 1;
        memcpy(c->claim, b->room->session, 16);
        ml_log("%s asks to join %s's room", c->name, b->name);
    } else if (kind == ML_M_LEAVE) {
        if (c->has_claim && b->room && !memcmp(b->room->session, c->claim, 16)) {
            c->has_claim = 0;
            ml_send(p, len, &b->at);
            ml_send_peers(c);
            return;
        }
    }
    ml_send(p, len, &b->at);
}

static void ml_on_data(ml_client *c, const uint8_t *p, int len, const uint32_t *skip, int nskip, ULONGLONG now) {
    if (len - 16 > ML_MAX_DATA + ML_NI_SIZE) { ml_dropped++; return; }
    uint32_t src = ml_get32(p + 4), dst = ml_get32(p + 8);
    unsigned sport = ml_get16(p + 12), dport = ml_get16(p + 14);
    if (src != c->ip) { ml_dropped++; return; }
    if (sport == ML_LDN_PORT || dport == ML_LDN_PORT) { ml_on_ldn(c, p, len, dst, sport, dport, now); return; }
    ml_room *room = ml_room_of(c);
    if (!room) { ml_dropped++; return; }
    ml_client *mem[8];
    int cnt = ml_members(room, mem), sent = 0;
    for (int i = 0; i < cnt; i++) {
        int skipped = 0;
        for (int k = 0; k < nskip; k++) skipped |= skip[k] == mem[i]->ip;
        if (mem[i] == c || skipped) continue;
        if (ml_is_bcast(dst) || mem[i]->ip == dst) { ml_send(p, len, &mem[i]->at); sent++; }
    }
    ml_relayed += sent;
    if (!sent && !ml_is_bcast(dst)) ml_dropped++;
}

static void ml_packet(const uint8_t *p, int len, const struct sockaddr_in *at, ULONGLONG now) {
    if (len < 4 || p[0] != 'S' || p[1] != 'U' || p[2] != ML_PROTO) return;
    int kind = p[3];
    ml_client *c = ml_by_at(at);
    if (kind == ML_JOIN) {
        for (int i = 0; i < ML_OPEN_MAX; i++) if (ml_open[i].used && ml_same_at(&ml_open[i].at, at)) ml_open[i].used = 0;   /* NAT is open */
        ml_on_join(p, len, at, c, now);
        return;
    }
    if (!c) return;
    c->seen = now;
    int cookie_ok = len >= 12 && ml_get64(p + 4) == c->cookie;
    if (kind == ML_PING && len >= 21 && cookie_ok) {
        int players = 0, rooms = 0;
        for (int i = 0; i < ML_MAX_CLIENTS; i++) players += ml_clients[i].used && ml_same_part(&ml_clients[i], c);
        for (int i = 0; i < ML_MAX_ROOMS; i++) rooms += ml_rooms[i].used && ml_same_part(ml_rooms[i].owner, c);
        int searching = 0, code = 0;       /* O2: players searching (this build and code), players of this code (any build) */
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            searching += ml_clients[i].used && ml_clients[i].searching && ml_same_part(&ml_clients[i], c);
            code += ml_clients[i].used && !strcmp(ml_clients[i].code, c->code);
        }
        uint8_t pong[4 + 8 + 8], *q = ml_hdr(pong, ML_PONG);
        memcpy(q, p + 12, 8);
        ml_put16(ml_put16(ml_put16(ml_put16(q + 8, players), rooms), searching), code);
        ml_send(pong, sizeof pong, at);
    } else if (kind == ML_LEAVE && len >= 12 && cookie_ok) {
        ml_drop(c, "left");
    } else if (kind == ML_LIST && len >= 16 && cookie_ok) {
        ml_on_list(c, p + 12, now, len >= 17 && (p[16] & 1));
    } else if (kind == ML_ROOMS && len >= 16 && cookie_ok) {
        ml_on_rooms(c, p + 12, now);
    } else if (kind == ML_MATCH && len >= 17 && cookie_ok) {
        ml_on_match(c, p[12], ml_get32(p + 13), now);
    } else if (kind == 16 && len > 16 && len <= 1500 && cookie_ok) {      /* R3: a rollback message (net_relay.c T_RB) to the other players of its lobby code */
        uint32_t dst = ml_get32(p + 12);
        static uint8_t out[1600];   /* (only used under ml_lock) */
        int on = len - 16 + 8, sent = 0;
        /* (no "public code" restriction here, unlike ssbu_lobby.py: this lobby runs inside a host's game (HOST / JOIN / LAN modes), its
         * players connected on purpose, and the joiner of a room code sends to destination 0 = the host: R3 net3 run pd3) */
        memcpy(out, "SU\1\x10", 4); ml_put32(out + 4, c->ip); memcpy(out + 8, p + 16, len - 16);
        for (int i = 0; i < ML_MAX_CLIENTS; i++) {
            ml_client *o = &ml_clients[i];
            if (!o->used || o == c || !ml_same_part(o, c) || (dst && o->ip != dst)) continue;
            ml_send(out, on, &o->at); sent++;
        }
        ml_relayed += sent;
        if (!sent) ml_dropped++;
        if (dst && c->mpeer && c->kind == ML_KIND_ROLLBACK && c->mpeer->ip == dst) {   /* (1.4) RB both ways between a rollback pair: together */
            c->rb_seen = 1;
            if (c->mpeer->rb_seen) {
                char wa[96], wb[96]; ml_client *h = c->mrole == 1 ? c : c->mpeer;
                snprintf(wa, sizeof wa, "playing a rollback match with %s", h->mpeer->name);
                snprintf(wb, sizeof wb, "playing a rollback match with %s", h->name);
                ml_match_end_pair(h, ML_MS_TOGETHER, wa, wb, now);
            }
        }
    } else if (kind == ML_DATA && len >= 16) {
        ml_on_data(c, p, len, NULL, 0, now);
    } else if (kind == ML_BCAST && len >= 5) {
        int n = p[4], body = 5 + 4 * n;
        uint32_t skip[255];
        if (len < body) n = 0, body = 5 + 4 * p[4];   /* (no usable skip list) */
        for (int i = 0; i < n; i++) skip[i] = ml_get32(p + 5 + 4 * i);
        if (len >= body + 12) {
            static uint8_t buf[65536];   /* (only used under ml_lock) */
            memcpy(buf, "SU\1\x06", 4);
            memcpy(buf + 4, p + body, len - body);
            ml_on_data(c, buf, 4 + len - body, skip, n, now);
        }
    }
}

static void ml_sweep(ULONGLONG now) {
    for (int i = 0; i < ML_MAX_CLIENTS; i++)
        if (ml_clients[i].used && now - ml_clients[i].seen > ML_CLIENT_TIMEOUT) ml_drop(&ml_clients[i], "silent for 10 s");
    for (int i = 0; i < ML_MAX_ROOMS; i++)
        if (ml_rooms[i].used && now - ml_rooms[i].updated > ML_ROOM_TIMEOUT) ml_close_room(&ml_rooms[i], "no advertisement for 10 s");
    for (int i = 0; i < ML_MAX_CLIENTS; i++) {        /* O2: searches without an opponent, matches nobody followed */
        ml_client *c = &ml_clients[i];
        char w[96], wb[96];
        if (!c->used) continue;
        if (c->searching && !c->mpeer && now - c->search_since > ml_search_for) {
            snprintf(w, sizeof w, "no opponent in %u s", (unsigned)(ml_search_for / 1000));
            ml_match_end(c, ML_MS_TIMEOUT, 0, NULL, w, now);
        }
        if (c->mpeer && c->mrole == 1 && now - c->match_at > ml_match_for) {
            snprintf(w, sizeof w, "the match with %s expired", c->mpeer->name);
            snprintf(wb, sizeof wb, "the match with %s expired", c->name);
            ml_match_end_pair(c, ML_MS_EXPIRED, w, wb, now);
        }
    }
}

static DWORD WINAPI ml_thread_main(LPVOID arg) {
    static uint8_t buf[65536];
    ULONGLONG last_sweep, last_stats, last_open = 0;
    (void)arg;
    last_sweep = last_stats = GetTickCount64();
    while (ml_running) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(ml_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        ULONGLONG now = GetTickCount64();
        EnterCriticalSection(&ml_lock);
        if (n > 0 && ml_running) ml_packet(buf, n, &from, now);
        if (now - last_sweep >= 1000) { last_sweep = now; ml_sweep(now); }
        if (now - last_stats >= 60000) {
            int players = 0, rooms = 0;
            last_stats = now;
            for (int i = 0; i < ML_MAX_CLIENTS; i++) players += ml_clients[i].used;
            for (int i = 0; i < ML_MAX_ROOMS; i++) rooms += ml_rooms[i].used;
            if (players) ml_log("stats: %d player(s), %d room(s), relayed game datagrams %llu, ldn messages %llu, dropped %llu",
                                players, rooms, ml_relayed, ml_ldn_msgs, ml_dropped);
        }
        if (now - last_open >= 500) {   /* keep opening the host's NAT for joiners who gave their code */
            last_open = now;
            for (int i = 0; i < ML_OPEN_MAX; i++) if (ml_open[i].used) ml_send("SUo", 4, &ml_open[i].at);
        }
        LeaveCriticalSection(&ml_lock);
        if (n < 0 && WSAGetLastError() != WSAETIMEDOUT && WSAGetLastError() != WSAECONNRESET && ml_running) Sleep(10);
    }
    return 0;
}

/* ---- API */
static uint16_t minilobby_start(uint32_t bind_ip_host_order, uint16_t port, int no_peers) {
    WSADATA wsa;
    struct sockaddr_in a;
    int al = sizeof a;
    DWORD timeout = 50;
    BOOL off = FALSE;
    DWORD bytes = 0;
    if (ml_running) return 0;
    if (!ml_lock_ready) { InitializeCriticalSection(&ml_lock); ml_lock_ready = 1; }
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);   /* (no ICMP errors on receive) */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(bind_ip_host_order);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&a, &al)) { closesocket(s); return 0; }
    EnterCriticalSection(&ml_lock);
    memset(ml_clients, 0, sizeof ml_clients);
    memset(ml_rooms, 0, sizeof ml_rooms);
    memset(ml_open, 0, sizeof ml_open);
    ml_next_ip = ML_SUBNET + 2;
    ml_relayed = ml_ldn_msgs = ml_dropped = 0;
    ml_no_peers = no_peers;
    {   const char *e = getenv("SSBU_LOBBY_SEARCH_SECS"), *m = getenv("SSBU_LOBBY_MATCH_SECS");     /* (tests: shorter) */
        if (e && atof(e) > 0) ml_search_for = (ULONGLONG)(atof(e) * 1000);
        if (m && atof(m) > 0) ml_match_for = (ULONGLONG)(atof(m) * 1000); }
    ml_sock = s;
    ml_running = 1;
    LeaveCriticalSection(&ml_lock);
    ml_thread = CreateThread(NULL, 0, ml_thread_main, NULL, 0, NULL);
    if (!ml_thread) { ml_running = 0; ml_sock = INVALID_SOCKET; closesocket(s); return 0; }
    ml_log("lobby on UDP port %u (protocol SU %d%s)", ntohs(a.sin_port), ML_PROTO, no_peers ? ", everyone relayed" : "");
    return ntohs(a.sin_port);
}

static void minilobby_stop(void) {
    if (!ml_running) return;
    ml_running = 0;
    WaitForSingleObject(ml_thread, 2000);
    CloseHandle(ml_thread);
    ml_thread = NULL;
    EnterCriticalSection(&ml_lock);
    closesocket(ml_sock);
    ml_sock = INVALID_SOCKET;
    LeaveCriticalSection(&ml_lock);
}

static void minilobby_counts(int *players, int *rooms) {
    int p = 0, r = 0;
    if (ml_lock_ready) {
        EnterCriticalSection(&ml_lock);
        for (int i = 0; i < ML_MAX_CLIENTS; i++) p += ml_clients[i].used;
        for (int i = 0; i < ML_MAX_ROOMS; i++) r += ml_rooms[i].used;
        LeaveCriticalSection(&ml_lock);
    }
    if (players) *players = p;
    if (rooms) *rooms = r;
}

static SOCKET minilobby_socket(void) { return ml_running ? ml_sock : INVALID_SOCKET; }

static void minilobby_open_to(const struct sockaddr_in *at) {
    int slot = -1;
    if (!at || !ml_lock_ready) return;
    EnterCriticalSection(&ml_lock);
    for (int i = 0; i < ML_OPEN_MAX; i++) {
        if (ml_open[i].used && ml_same_at(&ml_open[i].at, at)) { slot = -2; break; }
        if (!ml_open[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) { ml_open[slot].at = *at; ml_open[slot].used = 1; }
    LeaveCriticalSection(&ml_lock);
}

#ifdef MINILOBBY_MAIN
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: minilobby <port> [1 = no PEERS]\n"); return 2; }
    if (!minilobby_start(0x7F000001u, (uint16_t)atoi(argv[1]), argc > 2 && argv[2][0] == '1')) { fprintf(stderr, "cannot start\n"); return 1; }
    for (;;) Sleep(1000);
}
#endif
