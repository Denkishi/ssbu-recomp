/* SPDX-License-Identifier: GPL-3.0-or-later
 * Structure layouts, result codes and the host / station design follow, without copied code:
 *   Eden  src/core/hle/service/ldn/ldn_types.h      (GPL-3.0-or-later; yuzu lineage)
 *   Eden  src/core/hle/service/ldn/ldn_results.h    (GPL-2.0-or-later / GPL-3.0-or-later)
 *   Eden  src/core/hle/service/ldn/lan_discovery.cpp (GPL-2.0-or-later, yuzu Emulator Project)
 * See tools/online/NOTICE. Compiled only into a build made with scripts\setup.py --ldn-online on (SSBU_LDN_ONLINE).
 *
 * Included by nnnet_c4a.inc: nn::ldn (local wireless) carried over the internet through a room server
 * (tools/online/ldn_room_server.py), and the virtual-address path of the socket layer that goes with it.
 *
 * Configuration: <exe folder>\..\config\online.ini (or the environment, which wins):
 *     server   = host:port        SSBU_LDN_SERVER     a host the user types; there is no default server
 *     room     = <password>       SSBU_LDN_ROOM       players with the same password meet in the same room
 *     nickname = <name>           SSBU_LDN_NICKNAME   REQUIRED to go online: the name the other players see (the
 *                                                     game's account nickname); the Windows user name is never sent
 * Without a server, a room and a nickname, nothing changes: nn::ldn::Initialize answers "wireless off" (0x2ecb).
 * Refused as well while runahead = on (graphics.ini): a speculative frame would send and receive the game's packets,
 * and rolling it back would not roll back the other player. tools/online/online.ini.example is a template.
 * SSBU_LDN_TRACE=1 logs every nn::ldn call and every game datagram. The log never holds the server address, the room
 * password or a real network address (virtual addresses 169.254.1.N only).
 *
 * Everything that comes from the room is untrusted: frame lengths are bounded (64 KB), messages shorter than their
 * structure are dropped, and every NetworkInfo / NodeInfo from another player is sanitized (counts, lengths, string
 * ends) before the game can read it. Guest memory is written only inside the game's own nn::ldn and recv calls; the
 * receiving threads write host memory only. ldn_fuzz_selftest (SSBU_SDK_SELFTEST) feeds random messages through it.
 *
 * Both players must run the same game: a build fingerprint (main.elf, the game DLL, the script-module folder, and the
 * switches that change fighters: SSBU_MODS, SSBU_CHARACTERS, SSBU_LOOSE_FILES, SSBU_FIGHTER_SRC / _BIN, fighters_bin)
 * is sent on joining; the server refuses a member whose fingerprint differs from the room's.
 *
 * What the game sees: a console whose local-wireless radio works. Initialize connects to the server, which hands out a
 * virtual address 169.254.1.N/24. The LDN state machine follows [eden] hle/service/ldn/lan_discovery.cpp (the yuzu
 * "room" design): the host keeps the NetworkInfo; a scan is a broadcast that hosts answer with their NetworkInfo; a
 * station sends its NodeInfo to the host, which puts it in a free node slot and sends the updated NetworkInfo to every
 * station (SyncNetwork). Structures: [eden] ldn_types.h (NetworkInfo 0x480, NodeInfo 0x40, ScanFilter 0x60, ...).
 * Results: module 203 ([eden] ldn_results.h); Result = 203 | description << 9.
 *
 * The game's own traffic (Pia) uses ordinary UDP sockets addressed to the virtual addresses. A socket bound while the
 * LDN connection is up (to 0.0.0.0 or to the virtual address) gets a VIRTUAL port: the host socket is bound to
 * 127.0.0.1:<ephemeral> and the guest port is only remembered, so two copies of the game on one PC do not collide.
 * SendTo a virtual address is wrapped and sent to the server over UDP; datagrams from the server are handed to the
 * host socket of that virtual port through a local injector socket, with a 6-byte header (source address and port)
 * that Recv / RecvFrom strip again. Poll keeps working because the data really arrives on the host socket. */

#include <psapi.h>                                  /* K32EnumProcessModules: the game DLL of the fingerprint */

#define LDN_RESULT(d) (203u | ((uint32_t)(d) << 9))
#define LDN_ADVERTISE_TOO_LARGE LDN_RESULT(10)
#define LDN_INVALID_NODE_COUNT LDN_RESULT(30)
#define LDN_CONNECTION_FAILED LDN_RESULT(31)
#define LDN_BAD_STATE LDN_RESULT(32)
#define LDN_NO_IP LDN_RESULT(33)
#define LDN_AP_CONNECTION_FAILED LDN_RESULT(65)
#define LDN_MAX_NODES LDN_RESULT(67)

enum { LDN_NONE, LDN_INITIALIZED, LDN_AP_OPENED, LDN_AP_CREATED, LDN_STA_OPENED, LDN_STA_CONNECTED, LDN_ERROR };
enum { LM_SCAN = 1, LM_SCAN_RESP, LM_CONNECT, LM_DISCONNECT, LM_DESTROY, LM_SYNC, LM_PING, LM_GONE = 0xff };

/* Ping: every second a UDP echo to the room server (kind 4 -> 6, on the socket the game's datagrams use, so it is the
 * delay they see); the smoothed round trip is sent to the room every 2 s (LM_PING: u16 ms), so every player knows every
 * other player's ping to the server. ldn_ping_overlay() hands them to the renderer (nvn_vk_draw.c), which draws them over
 * the picture. online.ini ping_overlay = off (SSBU_LDN_PING=0) hides them. */
#define LDN_PEERS 16
static struct { uint32_t vip; int ms; DWORD at; } g_ldn_pings[LDN_PEERS];
static volatile LONG g_ldn_my_ping = -1;                  /* ms, -1 = not measured yet */
static int g_ldn_ping_overlay = 1;
static void ldn_peer_ping(uint32_t vip, int ms) {          /* g_ldn.cs held */
    int free_i = -1;
    for (int i = 0; i < LDN_PEERS; i++) {
        if (g_ldn_pings[i].vip == vip) { g_ldn_pings[i].ms = ms; g_ldn_pings[i].at = GetTickCount(); return; }
        if (free_i < 0 && (!g_ldn_pings[i].vip || GetTickCount() - g_ldn_pings[i].at > 15000)) free_i = i;
    }
    if (free_i >= 0) { g_ldn_pings[free_i].vip = vip; g_ldn_pings[free_i].ms = ms; g_ldn_pings[free_i].at = GetTickCount(); }
}

/* NetworkInfo offsets ([eden] ldn_types.h) */
#define NI_SIZE 0x480
#define NI_INTENT 0x00                  /* IntentId: local communication id u64, pad 2, scene id u16, pad 4 */
#define NI_SESSION 0x10
#define NI_BSSID 0x20
#define NI_SSID 0x26                    /* u8 length + 33 chars */
#define NI_CHANNEL 0x48
#define NI_LINK 0x4a
#define NI_NETTYPE 0x4b
#define NI_SECPARAM 0x50                /* LdnNetworkInfo at 0x50 */
#define NI_SECMODE 0x60
#define NI_ACCEPT 0x62
#define NI_NODE_MAX 0x66
#define NI_NODE_COUNT 0x67
#define NI_NODES 0x68                   /* 8 x NodeInfo (0x40) */
#define NI_ADV_SIZE 0x26a
#define NI_ADV 0x26c
#define NI_AUTH_ID 0x478
/* NodeInfo: ipv4 u32 @0, mac @4, node id s8 @0xa, connected u8 @0xb, user name 33 @0xc, version s16 @0x2e */
#define NODE_SIZE 0x40

typedef struct { uint16_t vport; uint16_t host_port; } LdnPort;
static struct {
    CRITICAL_SECTION cs;
    int cs_ready, configured, online;
    char host[256], room[128], nickname[33];
    uint8_t fingerprint[8];
    uint16_t port;
    SOCKET tcp, udp, inj;
    struct sockaddr_in server;
    uint32_t vip, mask, token;
    HANDLE threads[3];
    volatile LONG stop;
    int state;
    uint8_t info[NI_SIZE];
    uint8_t self_node[NODE_SIZE];
    int16_t disconnect_reason;
    uint64_t event;                     /* the game's SystemEventType, 0 = not attached */
    uint8_t last_connected[8];
    uint32_t host_vip;                  /* station: the host it joined */
    uint32_t clients[8]; int nclients;  /* host: stations' virtual addresses, by node slot 1..7 (0 = free) */
    uint8_t scan[16][NI_SIZE]; int nscan;
    uint8_t accept_policy;
    LdnPort ports[64]; int nports;
    uint16_t next_vport;
} g_ldn;

static int g_ldn_fuzzing;                                /* ldn_fuzz_selftest: the join / leave lines are not printed */
static int ldn_trace(void) { static int t = -1; if (t < 0) t = getenv("SSBU_LDN_TRACE") != NULL; return t; }   /* SSBU_LDN_TRACE: every call and datagram */
static uint32_t ldn_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void ldn_wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void ldn_ip_text(uint32_t v, char *out) { sprintf(out, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255); }
static int ldn_is_virtual(uint32_t be_addr) {             /* address as stored in a sockaddr (network order) */
    uint32_t v = ntohl(be_addr);
    return g_ldn.online && (((v & 0xffff0000u) == 0xa9fe0000u) || v == 0xffffffffu);
}

/* ---------------------------------------------------------------- configuration */
int gfx_runahead(void);                                   /* gfx_options.c: runahead = on | off */
static void ldn_trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void ldn_load_config(void) {
    if (g_ldn.cs_ready) return;
    InitializeCriticalSection(&g_ldn.cs);
    g_ldn.cs_ready = 1;
    g_ldn.tcp = g_ldn.udp = g_ldn.inj = INVALID_SOCKET;
    g_ldn.next_vport = 49152;
    char exe[MAX_PATH], path[MAX_PATH + 64], line[512], server[300] = "", nick[64] = "";
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\..\\config\\online.ini", exe);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        ldn_trim(k); ldn_trim(v);
        if (!_stricmp(k, "server")) snprintf(server, sizeof server, "%s", v);
        else if (!_stricmp(k, "room")) snprintf(g_ldn.room, sizeof g_ldn.room, "%s", v);
        else if (!_stricmp(k, "nickname")) snprintf(nick, sizeof nick, "%s", v);
        else if (!_stricmp(k, "ping_overlay")) g_ldn_ping_overlay = _stricmp(v, "off") && strcmp(v, "0");
    }
    if (f) fclose(f);
    const char *e;
    if ((e = getenv("SSBU_LDN_SERVER")) && *e) snprintf(server, sizeof server, "%s", e);
    if ((e = getenv("SSBU_LDN_ROOM")) && *e) snprintf(g_ldn.room, sizeof g_ldn.room, "%s", e);
    if ((e = getenv("SSBU_LDN_NICKNAME")) && *e) snprintf(nick, sizeof nick, "%s", e);
    if ((e = getenv("SSBU_LDN_PING")) && *e) g_ldn_ping_overlay = atoi(e) != 0;
    char *colon = strrchr(server, ':');
    g_ldn.port = 39010;
    if (colon) { *colon = 0; g_ldn.port = (uint16_t)atoi(colon + 1); }
    snprintf(g_ldn.host, sizeof g_ldn.host, "%s", server);
    if (!g_ldn.host[0] && !g_ldn.room[0]) return;     /* not asked for: the default model, nothing said */
    const char *why = NULL;
    /* the nickname the other players see: 1..32 bytes of UTF-8 (the account Nickname holds 33 with its NUL), no control characters */
    size_t nl = strlen(nick);
    int bad = nl == 0 || nl > 32;
    for (size_t i = 0; i < nl && !bad; i++) bad = (unsigned char)nick[i] < 0x20 || nick[i] == 0x7f;
    if (!g_ldn.host[0] || !g_ldn.room[0]) why = "online.ini needs both server and room";
    else if (bad) why = "online.ini needs nickname = <the name other players see> (1 to 32 bytes; the Windows user name is never sent)";
    else if (gfx_runahead()) why = "runahead = on in graphics.ini: local wireless is a lockstep of both games, which run-ahead's speculative frames would break";
    if (why) { printf("nn::ldn: local wireless over the internet is OFF: %s\n", why); return; }
    memcpy(g_ldn.nickname, nick, nl + 1);
    g_ldn.configured = 1;
    printf("nn::ldn: local wireless over the internet is ON (config\\online.ini / SSBU_LDN_*): room password and nickname set\n");
}
/* The account nickname while online is configured: the explicit one, never the Windows user name (nnfs.c acc_GetNickname). */
const char *ldn_online_nickname(void) { ldn_load_config(); return g_ldn.configured ? g_ldn.nickname : NULL; }

/* ---------------------------------------------------------------- server connection */
static int ldn_send_frame(uint8_t type, const void *a, uint32_t na, const void *b, uint32_t nb) {
    uint8_t head[5];
    uint32_t len = 1 + na + nb;
    memcpy(head, &len, 4); head[4] = type;
    char *buf = malloc(5 + na + nb);
    memcpy(buf, head, 5);
    if (na) memcpy(buf + 5, a, na);
    if (nb) memcpy(buf + 5 + na, b, nb);
    int total = 5 + na + nb, off = 0;
    while (off < total) {
        int n = send(g_ldn.tcp, buf + off, total - off, 0);
        if (n <= 0) break;
        off += n;
    }
    free(buf);
    return off == total;
}
static void ldn_send_msg(uint32_t dst_vip, uint8_t type, const void *data, uint32_t n) {
    uint8_t head[5];
    ldn_wr32(head, dst_vip); head[4] = type;
    if (g_ldn.tcp != INVALID_SOCKET) ldn_send_frame(10, head, 5, data, n);
}
static int ldn_recv_all(SOCKET s, void *buf, int n) {
    int off = 0;
    while (off < n) {
        int r = recv(s, (char *)buf + off, n - off, 0);
        if (r <= 0) return 0;
        off += r;
    }
    return 1;
}
static void ldn_signal(void) { if (g_ldn.event) os_signal_event(g_ldn.event); }
static void ldn_set_state(int s) {
    if (g_ldn.state != s) { g_ldn.state = s; ldn_signal(); }
}

/* host: node slots -> NetworkInfo, then the NetworkInfo to every station */
static void ldn_node_changes(void) {
    int changed = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t now = g_ldn.info[NI_NODES + i * NODE_SIZE + 0xb];
        if (now != g_ldn.last_connected[i]) { g_ldn.last_connected[i] = now; changed = 1; }
    }
    if (changed) ldn_signal();
}
static void ldn_update_nodes(void) {
    int count = 1;
    for (int i = 1; i < 8; i++) {
        uint8_t *n = g_ldn.info + NI_NODES + i * NODE_SIZE;
        if (!g_ldn.clients[i]) memset(n, 0, NODE_SIZE);
        n[0xa] = (uint8_t)i;
        n[0xb] = g_ldn.clients[i] ? 1 : 0;
        count += g_ldn.clients[i] != 0;
    }
    g_ldn.info[NI_NODE_COUNT] = (uint8_t)count;
    for (int i = 1; i < 8; i++) if (g_ldn.clients[i]) ldn_send_msg(g_ldn.clients[i], LM_SYNC, g_ldn.info, NI_SIZE);
    ldn_node_changes();
}
static void ldn_host_drop(uint32_t vip) {
    for (int i = 1; i < 8; i++) if (g_ldn.clients[i] == vip) {
        g_ldn.clients[i] = 0;
        char t[20]; ldn_ip_text(vip, t);
        if (!g_ldn_fuzzing) printf("nn::ldn: station %s left node slot %d\n", t, i);
        ldn_update_nodes();
    }
}
static void ldn_lost_host(int16_t reason) {
    g_ldn.host_vip = 0;
    if (g_ldn.state == LDN_STA_CONNECTED) {
        g_ldn.disconnect_reason = reason;
        if (!g_ldn_fuzzing) printf("nn::ldn: the host's network is gone (reason %d)\n", reason);
        ldn_set_state(LDN_STA_OPENED);
    }
}

/* Another player's NodeInfo / NetworkInfo, before the game can see it: flags 0/1, node ids 0..7, string ends, counts
 * and lengths within their arrays. `from` (the sender's virtual address as the server saw it) replaces what a node says
 * about its own address. */
static void ldn_fake_mac(uint8_t *mac);
static void ldn_sanitize_node(uint8_t *nd, int slot) {
    nd[0xa] = (uint8_t)slot;
    nd[0xb] = nd[0xb] ? 1 : 0;
    nd[0xc + 32] = 0;                                     /* user name: 33 bytes with its NUL */
    nd[0x2d] = 0;
}
static void ldn_sanitize_info(uint8_t *ni) {
    if (ni[NI_SSID] > 32) ni[NI_SSID] = 32;
    memset(ni + NI_SSID + 1 + ni[NI_SSID], 0, 33 - ni[NI_SSID]);
    if (ni[NI_NETTYPE] > 3) ni[NI_NETTYPE] = 2;
    if (!ni[NI_NODE_MAX] || ni[NI_NODE_MAX] > 8) ni[NI_NODE_MAX] = 8;
    if (ni[NI_NODE_COUNT] > ni[NI_NODE_MAX]) ni[NI_NODE_COUNT] = ni[NI_NODE_MAX];
    if (ni[NI_ACCEPT] > 3) ni[NI_ACCEPT] = 0;
    for (int i = 0; i < 8; i++) ldn_sanitize_node(ni + NI_NODES + i * NODE_SIZE, i);
    uint16_t adv;
    memcpy(&adv, ni + NI_ADV_SIZE, 2);
    if (adv > 384) adv = 384;
    memcpy(ni + NI_ADV_SIZE, &adv, 2);
    memset(ni + NI_ADV + adv, 0, 384 - adv);
}

static void ldn_on_message(uint32_t src, uint8_t type, const uint8_t *d, uint32_t n) {
    EnterCriticalSection(&g_ldn.cs);
    char t[20]; ldn_ip_text(src, t);
    switch (type) {
    case LM_SCAN:
        if (g_ldn.state == LDN_AP_CREATED) ldn_send_msg(src, LM_SCAN_RESP, g_ldn.info, NI_SIZE);
        break;
    case LM_SCAN_RESP:
        if (n == NI_SIZE && g_ldn.nscan < 16) {
            uint8_t ni[NI_SIZE];
            memcpy(ni, d, NI_SIZE);
            ldn_sanitize_info(ni);
            ldn_wr32(ni + NI_NODES, src);                 /* the host is the sender */
            int dup = 0;
            for (int i = 0; i < g_ldn.nscan; i++) if (ldn_rd32(g_ldn.scan[i] + NI_NODES) == src) { memcpy(g_ldn.scan[i], ni, NI_SIZE); dup = 1; }
            if (!dup) memcpy(g_ldn.scan[g_ldn.nscan++], ni, NI_SIZE);
        }
        break;
    case LM_CONNECT:
        if (g_ldn.state == LDN_AP_CREATED && n == NODE_SIZE) {
            int slot = 0, max = g_ldn.info[NI_NODE_MAX] ? g_ldn.info[NI_NODE_MAX] : 8;
            for (int i = 1; i < 8 && i < max && !slot; i++) if (!g_ldn.clients[i] || g_ldn.clients[i] == src) slot = i;
            if (!slot || g_ldn.accept_policy == 1) { if (!g_ldn_fuzzing) printf("nn::ldn: station %s refused (%s)\n", t, slot ? "accept policy" : "network full"); ldn_send_msg(src, LM_DESTROY, NULL, 0); break; }
            g_ldn.clients[slot] = src;
            uint8_t *nd = g_ldn.info + NI_NODES + slot * NODE_SIZE;
            memcpy(nd, d, NODE_SIZE);
            ldn_sanitize_node(nd, slot);
            ldn_wr32(nd, src);                            /* its address and MAC are the ones the server saw */
            { uint32_t keep = g_ldn.vip; g_ldn.vip = src; ldn_fake_mac(nd + 4); g_ldn.vip = keep; }
            nd[0xb] = 1;
            if (!g_ldn_fuzzing) printf("nn::ldn: station %s joined in node slot %d\n", t, slot);
            ldn_update_nodes();
        }
        break;
    case LM_DISCONNECT:
        if (g_ldn.state == LDN_AP_CREATED) ldn_host_drop(src);
        break;
    case LM_DESTROY:
        if (src == g_ldn.host_vip) ldn_lost_host(4);              /* DestroyedByUser */
        break;
    case LM_SYNC:
        if ((g_ldn.state == LDN_STA_OPENED || g_ldn.state == LDN_STA_CONNECTED) && n == NI_SIZE && src == g_ldn.host_vip) {
            memcpy(g_ldn.info, d, NI_SIZE);
            ldn_sanitize_info(g_ldn.info);
            ldn_wr32(g_ldn.info + NI_NODES, src);
            if (g_ldn.state == LDN_STA_OPENED) { if (!g_ldn_fuzzing) printf("nn::ldn: connected to the host's network\n"); ldn_set_state(LDN_STA_CONNECTED); }
            ldn_node_changes();
        }
        break;
    case LM_PING:
        if (n == 2) { uint16_t ms; memcpy(&ms, d, 2); ldn_peer_ping(src, ms > 9999 ? 9999 : ms); }
        break;
    case LM_GONE:
        if (g_ldn.state == LDN_AP_CREATED) ldn_host_drop(src);
        if (src == g_ldn.host_vip) ldn_lost_host(6);              /* SignalLost */
        break;
    }
    LeaveCriticalSection(&g_ldn.cs);
}

/* host socket of a virtual port: 0 = none */
static uint16_t ldn_port_lookup(uint16_t vport) {
    uint16_t hp = 0;
    EnterCriticalSection(&g_ldn.cs);
    for (int i = 0; i < g_ldn.nports; i++) if (g_ldn.ports[i].vport == vport) hp = g_ldn.ports[i].host_port;
    LeaveCriticalSection(&g_ldn.cs);
    return hp;
}
static void ldn_port_add(uint16_t vport, uint16_t host_port) {
    EnterCriticalSection(&g_ldn.cs);
    int i = 0;
    while (i < g_ldn.nports && g_ldn.ports[i].vport != vport) i++;
    if (i < 64) { g_ldn.ports[i].vport = vport; g_ldn.ports[i].host_port = host_port; if (i == g_ldn.nports) g_ldn.nports++; }
    LeaveCriticalSection(&g_ldn.cs);
}
static void ldn_port_remove(uint16_t vport) {
    EnterCriticalSection(&g_ldn.cs);
    for (int i = 0; i < g_ldn.nports; i++) if (g_ldn.ports[i].vport == vport) { g_ldn.ports[i] = g_ldn.ports[--g_ldn.nports]; break; }
    LeaveCriticalSection(&g_ldn.cs);
}
static volatile LONG g_ldn_sent, g_ldn_injected, g_ldn_dropped;
/* A UDP socket bound to 0.0.0.0:<dport> before local wireless came up (Pia opens its socket early) is on 127.0.0.1:
 * <dport>; the first datagram for that port makes it virtual (same port), so Recv strips the header from then on. */
static uint16_t ldn_adopt_socket(uint16_t dport) {
    uint16_t hp = 0;
    AcquireSRWLockExclusive(&g_so_lock);
    for (int i = 0; i < SO_MAX_FD && !hp; i++) {
        SoObj *o = g_so_fd[i].o;
        if (!o || o->type != SOCK_DGRAM || o->vport || !o->bound_any) continue;
        struct sockaddr_in a;
        int alen = sizeof a;
        if (getsockname(o->s, (struct sockaddr *)&a, &alen) == 0 && ntohs(a.sin_port) == dport) {
            o->vport = dport;
            hp = dport;
        }
    }
    ReleaseSRWLockExclusive(&g_so_lock);
    if (hp) { ldn_port_add(dport, hp); if (ldn_trace()) printf("nn::ldn: socket bound earlier to 0.0.0.0:%u now takes virtual traffic\n", dport); }
    return hp;
}
static void ldn_inject(uint32_t src_vip, uint16_t sport, uint16_t dport, const uint8_t *p, int n) {
    uint16_t hp = ldn_port_lookup(dport);
    if (!hp) hp = ldn_adopt_socket(dport);
    if (!hp) {
        if (InterlockedIncrement(&g_ldn_dropped) <= 5 && ldn_trace()) printf("nn::ldn: datagram for virtual port %u dropped: no socket on that port\n", dport);
        return;
    }
    if (g_ldn.inj == INVALID_SOCKET || n > 0xffff - 6) return;
    if (ldn_trace()) { char t[20]; ldn_ip_text(src_vip, t); printf("ldn-trace %lu R %d %s:%u -> %u %02x%02x%02x%02x%02x%02x%02x%02x\n", (unsigned long)GetTickCount(), n, t, sport, dport,
                  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }
    if (InterlockedIncrement(&g_ldn_injected) <= 12 && ldn_trace()) {
        char t[20]; ldn_ip_text(src_vip, t);
        printf("nn::ldn: recv #%ld %d bytes from %s:%u for virtual port %u (host port %u)\n", g_ldn_injected, n, t, sport, dport, hp);
    }
    uint8_t buf[0x10000];
    uint32_t be = htonl(src_vip);
    uint16_t bp = htons(sport);
    memcpy(buf, &be, 4); memcpy(buf + 4, &bp, 2);
    memcpy(buf + 6, p, n);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(hp);
    sendto(g_ldn.inj, (const char *)buf, n + 6, 0, (struct sockaddr *)&a, sizeof a);
}

static DWORD WINAPI ldn_tcp_thread(LPVOID unused) {
    (void)unused;
    uint8_t *buf = malloc(0x10000);
    while (!g_ldn.stop) {
        uint8_t head[5];
        if (!ldn_recv_all(g_ldn.tcp, head, 5)) break;
        uint32_t len = ldn_rd32(head);
        if (len < 1 || len > 0x10000) break;
        if (len > 1 && !ldn_recv_all(g_ldn.tcp, buf, len - 1)) break;
        if (head[4] == 10 && len >= 6) ldn_on_message(ldn_rd32(buf), buf[4], buf + 5, len - 6);
    }
    free(buf);
    if (!g_ldn.stop) {
        printf("nn::ldn: the connection to the room server was lost\n");
        EnterCriticalSection(&g_ldn.cs);
        if (g_ldn.state == LDN_STA_CONNECTED) ldn_lost_host(6);
        LeaveCriticalSection(&g_ldn.cs);
    }
    return 0;
}
static DWORD WINAPI ldn_udp_thread(LPVOID unused) {
    (void)unused;
    uint8_t *buf = malloc(0x10000);
    while (!g_ldn.stop) {
        int n = recv(g_ldn.udp, (char *)buf, 0x10000, 0);
        if (n < 0) { if (WSAGetLastError() == WSAECONNRESET) continue; if (g_ldn.stop) break; Sleep(10); continue; }
        if (n >= 13 && buf[0] == 2) {
            uint32_t src; uint16_t sport, dport;
            memcpy(&src, buf + 1, 4); memcpy(&sport, buf + 5, 2); memcpy(&dport, buf + 11, 2);
            ldn_inject(src, sport, dport, buf + 13, n - 13);
        } else if (n == 9 && buf[0] == 6) {               /* ping echo: the QPC value we sent */
            LARGE_INTEGER now, f;
            int64_t sent;
            memcpy(&sent, buf + 1, 8);
            QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
            int64_t us = (now.QuadPart - sent) * 1000000 / f.QuadPart;
            if (us >= 0 && us < 10000000) {
                LONG ms = (LONG)((us + 500) / 1000), prev = g_ldn_my_ping;
                InterlockedExchange(&g_ldn_my_ping, prev < 0 ? ms : (prev * 3 + ms + 2) / 4);     /* smoothed */
            }
        }
    }
    free(buf);
    return 0;
}
static DWORD WINAPI ldn_keepalive_thread(LPVOID unused) {
    (void)unused;
    for (int tick = 0; !g_ldn.stop; tick++) {                /* one tick a second */
        uint8_t reg[5] = {1};
        memcpy(reg + 1, &g_ldn.token, 4);
        if (tick % 2 == 0) send(g_ldn.udp, (const char *)reg, 5, 0);
        uint8_t ping[13] = {4};                               /* ping: kind 4, token, QPC now; echoed as kind 6 + the 8 bytes */
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        memcpy(ping + 1, &g_ldn.token, 4); memcpy(ping + 5, &now.QuadPart, 8);
        send(g_ldn.udp, (const char *)ping, 13, 0);
        LONG mine = g_ldn_my_ping;
        if (tick % 2 == 1 && mine >= 0) {
            uint16_t ms = (uint16_t)(mine > 9999 ? 9999 : mine);
            EnterCriticalSection(&g_ldn.cs);
            ldn_send_msg(0xffffffffu, LM_PING, &ms, 2);
            LeaveCriticalSection(&g_ldn.cs);
        }
        if (tick % 10 == 9) ldn_send_frame(3, NULL, 0, NULL, 0);
        static LONG said_sent = -1, said_in = -1;
        if (tick % 10 == 9 && ldn_trace() && (g_ldn_sent != said_sent || g_ldn_injected != said_in)) {
            said_sent = g_ldn_sent; said_in = g_ldn_injected;
            printf("nn::ldn: game datagrams so far: %ld sent, %ld received, %ld dropped; ping %ld ms\n", g_ldn_sent, g_ldn_injected, g_ldn_dropped, mine);
        }
        for (int i = 0; i < 10 && !g_ldn.stop; i++) Sleep(100);
    }
    return 0;
}

static void ldn_disconnect_server(void) {
    if (!g_ldn.online) return;
    g_ldn.stop = 1;
    g_ldn.online = 0;
    shutdown(g_ldn.tcp, SD_BOTH);
    closesocket(g_ldn.tcp); closesocket(g_ldn.udp);
    WaitForMultipleObjects(3, g_ldn.threads, TRUE, 3000);
    for (int i = 0; i < 3; i++) CloseHandle(g_ldn.threads[i]);
    g_ldn.tcp = g_ldn.udp = INVALID_SOCKET;
    printf("nn::ldn: left the room server\n");
}
/* ---------------------------------------------------------------- build fingerprint
 * FNV-1a 64 over what decides how the game plays; folders by the names and sizes of their files (paths differ between
 * PCs, contents of the same build do not). Order-independent for folders (entries are XOR-summed). */
static uint64_t ldn_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
static uint64_t ldn_fnv_str(uint64_t h, const char *s) { return ldn_fnv(h, s, strlen(s) + 1); }
static uint64_t ldn_fnv_file(uint64_t h, const char *path, int *ok) {
    FILE *f = fopen(path, "rb");
    *ok = f != NULL;
    if (!f) return ldn_fnv_str(h, "missing");
    static uint8_t buf[1 << 20];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = ldn_fnv(h, buf, n);
    fclose(f);
    return h;
}
static uint64_t ldn_fnv_folder(uint64_t h, const char *dir, int depth, int *files) {
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    uint64_t sum = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            uint64_t e = ldn_fnv_str(0xcbf29ce484222325ull, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char sub[MAX_PATH * 2];
                snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
                if (depth > 0) e = ldn_fnv_folder(e, sub, depth - 1, files);
            } else {
                e = ldn_fnv(e, &fd.nFileSizeLow, 4); e = ldn_fnv(e, &fd.nFileSizeHigh, 4);
                (*files)++;
            }
            sum ^= e;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    return ldn_fnv(h, &sum, 8);
}
static void ldn_make_fingerprint(void) {
    char exe[MAX_PATH], p[MAX_PATH * 2];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    uint64_t h = ldn_fnv_str(0xcbf29ce484222325ull, "ssbu-ldn-fingerprint-1");
    int ok = 0, n = 0;
    /* the game itself: main.elf (13.0.4 only, setup.py convert checks it) */
    if (__argc > 1) snprintf(p, sizeof p, "%s", __argv[1]); else snprintf(p, sizeof p, "%s\\..\\binaries\\13.0.4\\main.elf", exe);
    h = ldn_fnv_file(h, p, &ok);
    /* the lifted game DLL this exe was linked against (a new-fighter-kinds build links another one) */
    HMODULE mods[512];
    DWORD need = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
            char name[MAX_PATH];
            if (!GetModuleFileNameA(mods[i], name, MAX_PATH)) continue;
            const char *base = strrchr(name, '\\'); base = base ? base + 1 : name;
            if (_strnicmp(base, "ssbu_game", 9)) continue;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            h = ldn_fnv_str(h, base);
            if (GetFileAttributesExA(name, GetFileExInfoStandard, &fa)) { h = ldn_fnv(h, &fa.nFileSizeLow, 4); h = ldn_fnv(h, &fa.nFileSizeHigh, 4); }
        }
    /* the script modules, and every switch that replaces fighter code or files */
    const char *nro = getenv("SSBU_NRO_BIN");
    snprintf(p, sizeof p, "%s\\%s", exe, nro && nro[0] ? nro : "nro_bin_eh_o2");
    h = ldn_fnv_str(h, nro && nro[0] ? nro : "nro_bin_eh_o2"); h = ldn_fnv_folder(h, p, 0, &n);
    const char *src = getenv("SSBU_FIGHTER_SRC"), *fbin = getenv("SSBU_FIGHTER_BIN");
    if (!(src && src[0] == '0')) {
        snprintf(p, sizeof p, "%s\\%s", exe, fbin && fbin[0] ? fbin : "fighters_bin");
        h = ldn_fnv_str(h, "fighter source"); h = ldn_fnv_folder(h, p, 0, &n);
    }
    const char *dirs[] = {"SSBU_MODS", "SSBU_CHARACTERS"};
    for (int i = 0; i < 2; i++) {
        const char *d = getenv(dirs[i]);
        if (!d || !d[0]) continue;
        if (d[1] == ':' || d[0] == '\\') snprintf(p, sizeof p, "%s", d); else snprintf(p, sizeof p, "%s\\%s", exe, d);
        h = ldn_fnv_str(h, dirs[i]); h = ldn_fnv_folder(h, p, 2, &n);
    }
    const char *lf = getenv("SSBU_LOOSE_FILES");
    if (lf && lf[0] && lf[0] != '0') h = ldn_fnv_str(h, "SSBU_LOOSE_FILES");
    memcpy(g_ldn.fingerprint, &h, 8);
    printf("nn::ldn: build fingerprint %016llx (main.elf %s, game DLL, script modules, fighter source / mods / characters / loose files: %d files)\n",
           (unsigned long long)h, ok ? "read" : "NOT FOUND", n);
}

static int ldn_connect_server(void) {
    if (g_ldn.online) return 1;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    static int fp_done;
    if (!fp_done) { ldn_make_fingerprint(); fp_done = 1; }
    struct addrinfo hints = {0}, *res = NULL;
    char port[8];
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%u", g_ldn.port);
    if (getaddrinfo(g_ldn.host, port, &hints, &res) != 0 || !res) { printf("nn::ldn: the room server's name cannot be resolved\n"); return 0; }
    memcpy(&g_ldn.server, res->ai_addr, sizeof g_ldn.server);
    freeaddrinfo(res);
    g_ldn.tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(g_ldn.tcp, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    DWORD tmo = 5000;
    setsockopt(g_ldn.tcp, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    if (connect(g_ldn.tcp, (struct sockaddr *)&g_ldn.server, sizeof g_ldn.server) != 0) {
        printf("nn::ldn: cannot reach the room server (Winsock error %d)\n", WSAGetLastError());
        closesocket(g_ldn.tcp); g_ldn.tcp = INVALID_SOCKET;
        return 0;
    }
    /* HELLO, protocol 2: version, room password, nickname, build fingerprint (tools/online/ldn_room_server.py) */
    uint8_t hello[1 + 1 + 127 + 1 + 32 + 8];
    size_t rl = strlen(g_ldn.room), nl = strlen(g_ldn.nickname);
    if (rl > 127) rl = 127;
    hello[0] = 2; hello[1] = (uint8_t)rl; memcpy(hello + 2, g_ldn.room, rl);
    hello[2 + rl] = (uint8_t)nl; memcpy(hello + 3 + rl, g_ldn.nickname, nl);
    memcpy(hello + 3 + rl + nl, g_ldn.fingerprint, 8);
    uint8_t head[5] = {0}, body[260] = {0};
    uint32_t blen = 0;
    int got = ldn_send_frame(1, hello, (uint32_t)(3 + rl + nl + 8), NULL, 0) && ldn_recv_all(g_ldn.tcp, head, 5) &&
              (blen = ldn_rd32(head)) >= 1 && blen <= sizeof body + 1 && ldn_recv_all(g_ldn.tcp, body, (int)blen - 1);
    if (!got || head[4] != 2 || blen < 17) {
        if (got && head[4] == 4 && blen >= 2) printf("nn::ldn: the room server refused: %.*s\n", (int)(body[0] < blen - 2 ? body[0] : blen - 2), body + 1);
        else printf("nn::ldn: no valid answer from the room server\n");
        closesocket(g_ldn.tcp); g_ldn.tcp = INVALID_SOCKET;
        return 0;
    }
    tmo = 0;
    setsockopt(g_ldn.tcp, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    g_ldn.vip = ldn_rd32(body); g_ldn.token = ldn_rd32(body + 4); g_ldn.mask = ldn_rd32(body + 8);
    g_ldn.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    connect(g_ldn.udp, (struct sockaddr *)&g_ldn.server, sizeof g_ldn.server);
    if (g_ldn.inj == INVALID_SOCKET) {
        g_ldn.inj = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET; a.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        bind(g_ldn.inj, (struct sockaddr *)&a, sizeof a);
    }
    g_ldn.stop = 0;
    g_ldn.online = 1;
    g_ldn.threads[0] = CreateThread(NULL, 0, ldn_tcp_thread, NULL, 0, NULL);
    g_ldn.threads[1] = CreateThread(NULL, 0, ldn_udp_thread, NULL, 0, NULL);
    g_ldn.threads[2] = CreateThread(NULL, 0, ldn_keepalive_thread, NULL, 0, NULL);
    char t[20]; ldn_ip_text(g_ldn.vip, t);
    printf("nn::ldn: in the room as %s (%u member(s) now)\n", t, ldn_rd32(body + 12));
    return 1;
}

/* ---------------------------------------------------------------- the virtual-address path of the socket layer */
/* Called by so_bind for an address that is 0.0.0.0 or virtual while online: host socket on 127.0.0.1:0. */
static int ldn_so_bind(SOCKET s, uint16_t *vport_io) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) return WSAGetLastError();
    int alen = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &alen);
    uint16_t vp = *vport_io;
    if (!vp) {
        EnterCriticalSection(&g_ldn.cs);
        do { vp = g_ldn.next_vport++; if (g_ldn.next_vport < 49152) g_ldn.next_vport = 49152; } while (ldn_port_lookup(vp));
        LeaveCriticalSection(&g_ldn.cs);
    }
    if (ldn_port_lookup(vp)) return WSAEADDRINUSE;
    ldn_port_add(vp, ntohs(a.sin_port));
    *vport_io = vp;
    return 0;
}
/* A UDP socket that talks to a virtual address without a virtual port yet: a socket already bound to 127.0.0.1:P
 * (before local wireless came up) keeps P as its virtual port; an unbound one gets an ephemeral one. Returns an errno. */
static int ldn_so_ensure_port(SoObj *o) {
    if (o->vport) return 0;
    struct sockaddr_in a;
    int alen = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(o->s, (struct sockaddr *)&a, &alen) == 0 && a.sin_port) {
        if (ldn_port_lookup(ntohs(a.sin_port))) return 98;
        o->vport = ntohs(a.sin_port);
        ldn_port_add(o->vport, o->vport);
        return 0;
    }
    uint16_t vp = 0;
    int w = ldn_so_bind(o->s, &vp);
    if (w) return so_wsa_errno(w, 0);
    o->vport = vp;
    o->bound_any = 1;
    return 0;
}
static int ldn_so_sendto(uint16_t vport, const struct sockaddr_in *dst, const void *p, int n) {
    uint32_t dvip = ntohl(dst->sin_addr.S_un.S_addr);
    uint16_t dport = ntohs(dst->sin_port);
    if (ldn_trace()) { char t[20]; ldn_ip_text(dvip, t); printf("ldn-trace %lu S %u %s:%u <- %u %02x%02x%02x%02x%02x%02x%02x%02x\n", (unsigned long)GetTickCount(), n, t, dport, vport,
                  ((const uint8_t *)p)[0], ((const uint8_t *)p)[1], ((const uint8_t *)p)[2], ((const uint8_t *)p)[3], ((const uint8_t *)p)[4], ((const uint8_t *)p)[5], ((const uint8_t *)p)[6], ((const uint8_t *)p)[7]); }
    if (InterlockedIncrement(&g_ldn_sent) <= 12 && ldn_trace()) {
        char t[20]; ldn_ip_text(dvip, t);
        printf("nn::ldn: send #%ld %u bytes from virtual port %u to %s:%u\n", g_ldn_sent, n, vport, t, dport);
    }
    if (dvip == g_ldn.vip) { ldn_inject(g_ldn.vip, vport, dport, p, n); return n; }
    if (dvip == 0xffffffffu) dvip = (g_ldn.vip & g_ldn.mask) | ~g_ldn.mask;
    if (n > 0xffff - 13) return -1;
    uint8_t buf[0x10000];
    buf[0] = 2;
    memcpy(buf + 1, &g_ldn.token, 4); memcpy(buf + 5, &dvip, 4); memcpy(buf + 9, &dport, 2); memcpy(buf + 11, &vport, 2);
    memcpy(buf + 13, p, n);
    return send(g_ldn.udp, (const char *)buf, n + 13, 0) < 0 ? -1 : n;
}

/* ---------------------------------------------------------------- nn::ldn */
static void ldn_fake_mac(uint8_t *mac) { mac[0] = 0x02; mac[1] = 0x00; uint32_t be = htonl(g_ldn.vip); memcpy(mac + 2, &be, 4); }
static void ldn_make_node(uint8_t *node, uint64_t user_config, uint16_t version) {
    memset(node, 0, NODE_SIZE);
    ldn_wr32(node, g_ldn.vip);
    ldn_fake_mac(node + 4);
    node[0xb] = 1;
    memcpy(node + 0xc, (const void *)(uintptr_t)user_config, 33);
    memcpy(node + 0x2e, &version, 2);
}

static void ldn_Initialize(Ctx *c) {
    ldn_load_config();
    if (!g_ldn.configured) { RET(LDN_WIRELESS_OFF); return; }
    EnterCriticalSection(&g_ldn.cs);
    int ok = g_ldn.state != LDN_NONE || ldn_connect_server();
    if (ok && g_ldn.state == LDN_NONE) { g_ldn.state = LDN_INITIALIZED; g_ldn.disconnect_reason = 0; }
    LeaveCriticalSection(&g_ldn.cs);
    printf("nn::ldn::Initialize -> %s\n", ok ? "ok" : "wireless off (no room server)");
    RET(ok ? 0 : LDN_WIRELESS_OFF);
}
static void ldn_GetState(Ctx *c) {
    static int last = -1;
    int s = g_ldn.cs_ready ? g_ldn.state : 0;
    if (s != last && ldn_trace()) { printf("nn::ldn call GetState -> %d from %llx\n", s, (unsigned long long)(c->x[30] - 4)); last = s; }
    RET((uint32_t)s);
}
static int ldn_ready(Ctx *c, const char *what) {
    static struct { const char *name; int n; } seen[32];
    for (int i = 0; i < 32; i++) {                        /* the first calls of each function, with the state and caller */
        if (!seen[i].name) seen[i].name = what;
        if (seen[i].name == what) { if (seen[i].n++ < 4 && ldn_trace()) printf("nn::ldn call %s (state %d) from %llx\n", what, g_ldn.state, (unsigned long long)(c->x[30] - 4)); break; }
    }
    if (g_ldn.cs_ready && g_ldn.configured && g_ldn.state != LDN_NONE) return 1;
    (void)what;
    ldn_no_service(c);
    return 0;
}
static void ldn_Finalize(Ctx *c) {
    if (!ldn_ready(c, "Finalize")) return;
    EnterCriticalSection(&g_ldn.cs);
    if (g_ldn.state == LDN_AP_CREATED) for (int i = 1; i < 8; i++) if (g_ldn.clients[i]) ldn_send_msg(g_ldn.clients[i], LM_DESTROY, NULL, 0);
    if (g_ldn.state == LDN_STA_CONNECTED && g_ldn.host_vip) ldn_send_msg(g_ldn.host_vip, LM_DISCONNECT, g_ldn.self_node, NODE_SIZE);
    memset(g_ldn.clients, 0, sizeof g_ldn.clients);
    g_ldn.state = LDN_NONE;
    g_ldn.event = 0;
    LeaveCriticalSection(&g_ldn.cs);
    ldn_disconnect_server();
    printf("nn::ldn::Finalize\n");
    RET(0);
}
static void ldn_AttachStateChangeEvent(Ctx *c) {
    if (!ldn_ready(c, "AttachStateChangeEvent")) return;
    os_init_event(A0, 1);
    g_ldn.event = A0;
}
static void ldn_SetWirelessControllerRestriction(Ctx *c) { if (ldn_ready(c, "SetWirelessControllerRestriction")) RET(0); }
static void ldn_GetDisconnectReason(Ctx *c) { if (ldn_ready(c, "GetDisconnectReason")) RET((uint16_t)g_ldn.disconnect_reason); }
static void ldn_open_close(Ctx *c, int to_state, const char *what) {
    if (!ldn_ready(c, what)) return;
    EnterCriticalSection(&g_ldn.cs);
    if (to_state == LDN_INITIALIZED) {                    /* CloseAccessPoint / CloseStation */
        if (g_ldn.state == LDN_AP_CREATED) for (int i = 1; i < 8; i++) if (g_ldn.clients[i]) ldn_send_msg(g_ldn.clients[i], LM_DESTROY, NULL, 0);
        if (g_ldn.state == LDN_STA_CONNECTED && g_ldn.host_vip) ldn_send_msg(g_ldn.host_vip, LM_DISCONNECT, g_ldn.self_node, NODE_SIZE);
    } else g_ldn.disconnect_reason = 0;
    memset(g_ldn.clients, 0, sizeof g_ldn.clients);
    g_ldn.host_vip = 0;
    ldn_set_state(to_state);
    LeaveCriticalSection(&g_ldn.cs);
    if (ldn_trace()) printf("nn::ldn::%s\n", what);      /* the room list opens and closes the station for every scan */
    RET(0);
}
static void ldn_OpenAccessPoint(Ctx *c) { ldn_open_close(c, LDN_AP_OPENED, "OpenAccessPoint"); }
static void ldn_CloseAccessPoint(Ctx *c) { ldn_open_close(c, LDN_INITIALIZED, "CloseAccessPoint"); }
static void ldn_OpenStation(Ctx *c) { ldn_open_close(c, LDN_STA_OPENED, "OpenStation"); }
static void ldn_CloseStation(Ctx *c) { ldn_open_close(c, LDN_INITIALIZED, "CloseStation"); }

static void ldn_create(Ctx *c, uint64_t netcfg, uint64_t seccfg, uint64_t secparam, uint64_t usercfg) {
    EnterCriticalSection(&g_ldn.cs);
    if (g_ldn.state != LDN_AP_OPENED) { LeaveCriticalSection(&g_ldn.cs); RET(LDN_BAD_STATE); return; }
    uint8_t adv[2 + 384];
    memcpy(adv, g_ldn.info + NI_ADV_SIZE, sizeof adv);   /* advertise data set before the network is kept */
    memset(g_ldn.info, 0, NI_SIZE);
    memcpy(g_ldn.info + NI_ADV_SIZE, adv, sizeof adv);
    memcpy(g_ldn.info + NI_INTENT, (const void *)(uintptr_t)netcfg, 0x10);
    uint16_t channel = M16(netcfg + 0x10);
    if (!channel) channel = 6;
    memcpy(g_ldn.info + NI_CHANNEL, &channel, 2);
    g_ldn.info[NI_NODE_MAX] = M8(netcfg + 0x12) ? M8(netcfg + 0x12) : 8;
    uint16_t version = M16(netcfg + 0x14);
    ldn_fake_mac(g_ldn.info + NI_BSSID);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "ssbu%08x%08x", g_ldn.vip, (unsigned)GetTickCount());
    g_ldn.info[NI_SSID] = (uint8_t)strlen(ssid);
    memcpy(g_ldn.info + NI_SSID + 1, ssid, strlen(ssid));
    g_ldn.info[NI_LINK] = 2;                              /* Good */
    g_ldn.info[NI_NETTYPE] = 2;                           /* Ldn */
    memcpy(g_ldn.info + NI_SECMODE, (const void *)(uintptr_t)seccfg, 2);
    if (secparam) {
        memcpy(g_ldn.info + NI_SECPARAM, (const void *)(uintptr_t)secparam, 16);
        memcpy(g_ldn.info + NI_SESSION, (const void *)(uintptr_t)(secparam + 16), 16);
    } else {
        HCRYPTPROV prov;
        if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            CryptGenRandom(prov, 16, g_ldn.info + NI_SECPARAM);
            CryptGenRandom(prov, 16, g_ldn.info + NI_SESSION);
            CryptReleaseContext(prov, 0);
        }
    }
    uint64_t auth = ((uint64_t)GetTickCount() << 32) ^ g_ldn.vip;
    memcpy(g_ldn.info + NI_AUTH_ID, &auth, 8);
    g_ldn.info[NI_ACCEPT] = g_ldn.accept_policy;
    ldn_make_node(g_ldn.info + NI_NODES, usercfg, version);
    memcpy(g_ldn.self_node, g_ldn.info + NI_NODES, NODE_SIZE);
    memset(g_ldn.clients, 0, sizeof g_ldn.clients);
    memset(g_ldn.last_connected, 0, sizeof g_ldn.last_connected);
    ldn_set_state(LDN_AP_CREATED);
    ldn_update_nodes();
    LeaveCriticalSection(&g_ldn.cs);
    printf("nn::ldn::CreateNetwork: local communication id %016llx, scene %u, up to %u nodes, version %u\n",
           (unsigned long long)M64(netcfg), M16(netcfg + 0xa), M8(netcfg + 0x12), version);
    RET(0);
}
static void ldn_CreateNetwork(Ctx *c) { if (ldn_ready(c, "CreateNetwork")) ldn_create(c, A0, A1, 0, A2); }
static void ldn_CreateNetworkPrivate(Ctx *c) { if (ldn_ready(c, "CreateNetworkPrivate")) ldn_create(c, A0, A1, A2, A3); }
static void ldn_DestroyNetwork(Ctx *c) {
    if (!ldn_ready(c, "DestroyNetwork")) return;
    EnterCriticalSection(&g_ldn.cs);
    for (int i = 1; i < 8; i++) if (g_ldn.clients[i]) ldn_send_msg(g_ldn.clients[i], LM_DESTROY, NULL, 0);
    memset(g_ldn.clients, 0, sizeof g_ldn.clients);
    if (g_ldn.state == LDN_AP_CREATED) ldn_set_state(LDN_AP_OPENED);
    LeaveCriticalSection(&g_ldn.cs);
    printf("nn::ldn::DestroyNetwork\n");
    RET(0);
}
static void ldn_SetAdvertiseData(Ctx *c) {                /* (const void *, size_t) */
    if (!ldn_ready(c, "SetAdvertiseData")) return;
    if (A1 > 384) { RET(LDN_ADVERTISE_TOO_LARGE); return; }
    EnterCriticalSection(&g_ldn.cs);
    uint16_t n = (uint16_t)A1;
    memset(g_ldn.info + NI_ADV, 0, 384);
    if (n) memcpy(g_ldn.info + NI_ADV, (const void *)(uintptr_t)A0, n);
    memcpy(g_ldn.info + NI_ADV_SIZE, &n, 2);
    if (g_ldn.state == LDN_AP_CREATED) ldn_update_nodes();
    LeaveCriticalSection(&g_ldn.cs);
    RET(0);
}
static void ldn_SetStationAcceptPolicy(Ctx *c) {
    if (!ldn_ready(c, "SetStationAcceptPolicy")) return;
    g_ldn.accept_policy = (uint8_t)A0;
    g_ldn.info[NI_ACCEPT] = g_ldn.accept_policy;
    RET(0);
}
static void ldn_Reject(Ctx *c) {                          /* (Ipv4Address) */
    if (!ldn_ready(c, "Reject")) return;
    EnterCriticalSection(&g_ldn.cs);
    uint32_t vip = (uint32_t)A0;
    for (int i = 1; i < 8; i++) if (g_ldn.clients[i] == vip) ldn_send_msg(vip, LM_DESTROY, NULL, 0);
    ldn_host_drop(vip);
    LeaveCriticalSection(&g_ldn.cs);
    RET(0);
}
static void ldn_GetNetworkInfo(Ctx *c) {
    if (!ldn_ready(c, "GetNetworkInfo")) return;
    EnterCriticalSection(&g_ldn.cs);
    int ok = g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STA_CONNECTED;
    if (ok) memcpy((void *)(uintptr_t)A0, g_ldn.info, NI_SIZE);
    LeaveCriticalSection(&g_ldn.cs);
    RET(ok ? 0 : LDN_BAD_STATE);
}
static void ldn_GetNetworkConfig(Ctx *c) {
    if (!ldn_ready(c, "GetNetworkConfig")) return;
    EnterCriticalSection(&g_ldn.cs);
    int ok = g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STA_CONNECTED;
    if (ok) {
        uint8_t *o = (uint8_t *)(uintptr_t)A0;
        memset(o, 0, 0x20);
        memcpy(o, g_ldn.info + NI_INTENT, 0x10);
        memcpy(o + 0x10, g_ldn.info + NI_CHANNEL, 2);
        o[0x12] = g_ldn.info[NI_NODE_MAX];
        memcpy(o + 0x14, g_ldn.info + NI_NODES + 0x2e, 2);
    }
    LeaveCriticalSection(&g_ldn.cs);
    RET(ok ? 0 : LDN_BAD_STATE);
}
static void ldn_GetSecurityParameter(Ctx *c) {
    if (!ldn_ready(c, "GetSecurityParameter")) return;
    EnterCriticalSection(&g_ldn.cs);
    int ok = g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STA_CONNECTED;
    if (ok) { memcpy((void *)(uintptr_t)A0, g_ldn.info + NI_SECPARAM, 16); memcpy((void *)(uintptr_t)(A0 + 16), g_ldn.info + NI_SESSION, 16); }
    LeaveCriticalSection(&g_ldn.cs);
    RET(ok ? 0 : LDN_BAD_STATE);
}
static void ldn_GetIpv4Address(Ctx *c) {                  /* (Ipv4Address *, SubnetMask *) */
    if (!ldn_ready(c, "GetIpv4Address")) return;
    if (!g_ldn.online) { RET(LDN_NO_IP); return; }
    if (A0) M32(A0) = g_ldn.vip;
    if (A1) M32(A1) = g_ldn.mask;
    RET(0);
}

static int ldn_filter_ok(const uint8_t *info, uint64_t filter) {
    uint32_t flag = M32(filter + 0x5c);
    if ((flag & 1) && M64(filter) != *(const uint64_t *)(info + NI_INTENT)) return 0;
    if ((flag & 2) && memcmp((const void *)(uintptr_t)(filter + 0x10), info + NI_SESSION, 16)) return 0;
    if ((flag & 4) && M32(filter + 0x20) != info[NI_NETTYPE]) return 0;
    if ((flag & 0x10) && (M8(filter + 0x2a) != info[NI_SSID] || memcmp((const void *)(uintptr_t)(filter + 0x2b), info + NI_SSID + 1, info[NI_SSID]))) return 0;
    if ((flag & 0x20) && M16(filter + 0xa) != *(const uint16_t *)(info + NI_INTENT + 0xa)) return 0;
    return 1;
}
static int ldn_scan_now(void) {
    EnterCriticalSection(&g_ldn.cs);
    g_ldn.nscan = 0;
    ldn_send_msg(0xffffffffu, LM_SCAN, NULL, 0);
    LeaveCriticalSection(&g_ldn.cs);
    Sleep(400);                                           /* replies come back through the server: one round trip or two */
    return g_ldn.nscan;
}
static void ldn_scan(Ctx *c, const char *what) {          /* (NetworkInfo *out, int *outCount, int bufferCount, const ScanFilter &, int channel) */
    if (!ldn_ready(c, what)) return;
    uint64_t out = A0, countp = A1, filter = A3;
    int cap = (int)A2;
    if (g_ldn.state != LDN_STA_OPENED && g_ldn.state != LDN_AP_OPENED && g_ldn.state != LDN_STA_CONNECTED && g_ldn.state != LDN_AP_CREATED) { RET(LDN_BAD_STATE); return; }
    ldn_scan_now();
    EnterCriticalSection(&g_ldn.cs);
    int n = 0;
    for (int i = 0; i < g_ldn.nscan && n < cap; i++)
        if (ldn_filter_ok(g_ldn.scan[i], filter)) memcpy((void *)(uintptr_t)(out + (uint64_t)n++ * NI_SIZE), g_ldn.scan[i], NI_SIZE);
    int total = g_ldn.nscan;
    LeaveCriticalSection(&g_ldn.cs);
    if (countp) M32(countp) = (uint32_t)n;
    if (ldn_trace() || n) printf("nn::ldn::%s: %d network(s) in the room, %d after the filter (flags %x)\n", what, total, n, M32(filter + 0x5c));
    RET(0);
}
static void ldn_Scan(Ctx *c) { ldn_scan(c, "Scan"); }
static void ldn_ScanPrivate(Ctx *c) { ldn_scan(c, "ScanPrivate"); }
static void ldn_connect_to(Ctx *c, const uint8_t *info, uint64_t usercfg, uint16_t version) {
    EnterCriticalSection(&g_ldn.cs);
    if (g_ldn.state != LDN_STA_OPENED) { LeaveCriticalSection(&g_ldn.cs); RET(LDN_BAD_STATE); return; }
    if (!info[NI_NODE_COUNT]) { LeaveCriticalSection(&g_ldn.cs); RET(LDN_INVALID_NODE_COUNT); return; }
    g_ldn.host_vip = ldn_rd32(info + NI_NODES);
    ldn_make_node(g_ldn.self_node, usercfg, version);
    g_ldn.disconnect_reason = 0;
    memset(g_ldn.last_connected, 0, sizeof g_ldn.last_connected);
    ldn_send_msg(g_ldn.host_vip, LM_CONNECT, g_ldn.self_node, NODE_SIZE);
    LeaveCriticalSection(&g_ldn.cs);
    char t[20]; ldn_ip_text(g_ldn.host_vip, t);
    printf("nn::ldn::Connect to %s ...\n", t);
    for (int i = 0; i < 50 && g_ldn.state == LDN_STA_OPENED && g_ldn.host_vip; i++) Sleep(100);
    int ok = g_ldn.state == LDN_STA_CONNECTED;
    printf("nn::ldn::Connect -> %s\n", ok ? "connected" : "failed");
    RET(ok ? 0 : LDN_CONNECTION_FAILED);
}
static void ldn_Connect(Ctx *c) {                         /* (const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption) */
    if (!ldn_ready(c, "Connect")) return;
    uint8_t info[NI_SIZE];
    memcpy(info, (const void *)(uintptr_t)A0, NI_SIZE);
    ldn_connect_to(c, info, A2, (uint16_t)A3);
}
static void ldn_ConnectPrivate(Ctx *c) {                  /* (const NetworkConfig &, const SecurityConfig &, const SecurityParameter &, const UserConfig &, int, ConnectOption) */
    if (!ldn_ready(c, "ConnectPrivate")) return;
    uint64_t secparam = A2, usercfg = A3;
    uint16_t version = (uint16_t)A4;
    ldn_scan_now();
    uint8_t info[NI_SIZE];
    int found = 0;
    EnterCriticalSection(&g_ldn.cs);
    for (int i = 0; i < g_ldn.nscan && !found; i++)
        if (!memcmp(g_ldn.scan[i] + NI_SESSION, (const void *)(uintptr_t)(secparam + 16), 16)) { memcpy(info, g_ldn.scan[i], NI_SIZE); found = 1; }
    LeaveCriticalSection(&g_ldn.cs);
    if (!found) { printf("nn::ldn::ConnectPrivate: no network with that session id in the room\n"); RET(LDN_CONNECTION_FAILED); return; }
    ldn_connect_to(c, info, usercfg, version);
}
static void ldn_Disconnect(Ctx *c) {
    if (!ldn_ready(c, "Disconnect")) return;
    EnterCriticalSection(&g_ldn.cs);
    if (g_ldn.host_vip) ldn_send_msg(g_ldn.host_vip, LM_DISCONNECT, g_ldn.self_node, NODE_SIZE);
    g_ldn.host_vip = 0;
    g_ldn.disconnect_reason = 1;                          /* DisconnectedByUser */
    if (g_ldn.state == LDN_STA_CONNECTED) ldn_set_state(LDN_STA_OPENED);
    LeaveCriticalSection(&g_ldn.cs);
    printf("nn::ldn::Disconnect\n");
    RET(0);
}

/* ---------------------------------------------------------------- fuzz test of the room-message parser (c4a self-test)
 * Random and corrupted messages (every type, any length, structured NetworkInfo / NodeInfo with random fields) in the
 * host and station states; after each one every NetworkInfo the game could read must be within its bounds. Nothing is
 * sent (no server connection) and the state is reset afterwards. */
static int ldn_info_ok(const uint8_t *ni) {
    uint16_t adv;
    memcpy(&adv, ni + NI_ADV_SIZE, 2);
    if (ni[NI_SSID] > 32 || ni[NI_SSID + 1 + ni[NI_SSID]] != 0 || adv > 384) return 0;
    if (!ni[NI_NODE_MAX] || ni[NI_NODE_MAX] > 8 || ni[NI_NODE_COUNT] > ni[NI_NODE_MAX] || ni[NI_NETTYPE] > 3) return 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *nd = ni + NI_NODES + i * NODE_SIZE;
        if (nd[0xa] != i || nd[0xb] > 1 || nd[0xc + 32] != 0) return 0;
    }
    for (int k = adv; k < 384; k++) if (ni[NI_ADV + k]) return 0;
    return 1;
}
static uint64_t ldn_fz(uint64_t *r) { *r ^= *r << 13; *r ^= *r >> 7; *r ^= *r << 17; return *r; }   /* xorshift64 */
static int ldn_fuzz_selftest(int rounds) {
    ldn_load_config();
    if (g_ldn.online) { printf("  ldn fuzz: skipped (connected to a room)\n"); return 1; }
    uint64_t r = 0x9e3779b97f4a7c15ull;
    #define FZ() ldn_fz(&r)
    static uint8_t buf[0x600];
    int bad = 0, states[3] = {LDN_AP_CREATED, LDN_STA_OPENED, LDN_STA_CONNECTED};
    uint8_t types[8] = {LM_SCAN, LM_SCAN_RESP, LM_CONNECT, LM_DISCONNECT, LM_DESTROY, LM_SYNC, LM_GONE, LM_PING};
    uint64_t keep_event = g_ldn.event;
    g_ldn.event = 0;
    g_ldn_fuzzing = 1;
    for (int i = 0; i < rounds && !bad; i++) {
        if (i % 1000 == 0) {                              /* a fresh network now and then */
            memset(g_ldn.info, 0, NI_SIZE); g_ldn.info[NI_NODE_MAX] = 8; g_ldn.info[NI_NODE_COUNT] = 1;
            for (int k = 0; k < 8; k++) g_ldn.info[NI_NODES + k * NODE_SIZE + 0xa] = (uint8_t)k;
            memset(g_ldn.clients, 0, sizeof g_ldn.clients); g_ldn.nscan = 0;
            g_ldn.state = states[FZ() % 3];
            g_ldn.host_vip = 0xa9fe0101u + (uint32_t)(FZ() % 3);
        }
        uint32_t n;
        switch (FZ() % 4) { case 0: n = NI_SIZE; break; case 1: n = NODE_SIZE; break; case 2: n = (uint32_t)(FZ() % 8); break; default: n = (uint32_t)(FZ() % sizeof buf); }
        for (uint32_t k = 0; k < n; k++) buf[k] = (uint8_t)FZ();
        if (n == NI_SIZE && FZ() % 2) {                   /* plausible NetworkInfo with hostile fields */
            buf[NI_SSID] = (uint8_t)(FZ() % 64); buf[NI_NODE_MAX] = (uint8_t)(FZ() % 12); buf[NI_NODE_COUNT] = (uint8_t)(FZ() % 12);
            uint16_t adv = (uint16_t)(FZ() % 1024); memcpy(buf + NI_ADV_SIZE, &adv, 2);
        }
        uint8_t type = (FZ() % 5) ? types[FZ() % 8] : (uint8_t)FZ();
        uint32_t src = 0xa9fe0101u + (uint32_t)(FZ() % 4);
        ldn_on_message(src, type, buf, n);
        if ((g_ldn.state == LDN_STA_CONNECTED || g_ldn.state == LDN_STA_OPENED) && g_ldn.info[NI_NODE_MAX] && !ldn_info_ok(g_ldn.info)) bad = 1;
        for (int k = 0; k < g_ldn.nscan; k++) if (!ldn_info_ok(g_ldn.scan[k])) bad = 2;
        if (g_ldn.nscan > 16) bad = 3;
        if (g_ldn.state == LDN_AP_CREATED)
            for (int k = 1; k < 8; k++) if (g_ldn.clients[k] && (g_ldn.info[NI_NODES + k * NODE_SIZE + 0xc + 32] || g_ldn.info[NI_NODES + k * NODE_SIZE + 0xb] != 1)) bad = 4;
        if (bad) printf("  ldn fuzz: round %d, message type %u, %u bytes: check %d failed\n", i, type, n, bad);
        ldn_inject(src, (uint16_t)FZ(), (uint16_t)FZ(), buf, (int)(n % 2048));     /* no socket has a virtual port: dropped */
    }
    #undef FZ
    memset(g_ldn.info, 0, NI_SIZE); memset(g_ldn.clients, 0, sizeof g_ldn.clients);
    g_ldn.nscan = 0; g_ldn.host_vip = 0; g_ldn.state = LDN_NONE; g_ldn.event = keep_event;
    g_ldn_dropped = 0; g_ldn_fuzzing = 0;
    return !bad;
}

/* ---------------------------------------------------------------- ping for the renderer (nvn_vk_draw.c ldn_ping_draw)
 * While in a network (host or station): one entry per node slot 0..n-1 (the game numbers the players in that order:
 * host P1, then the stations as they joined; seen with 2 players): the ms of that player's round trip to the room
 * server, -1 not known yet, -2 slot empty. Returns n, 0 = nothing to draw. Render thread; takes the LDN lock briefly. */
int ldn_ping_overlay(int *ms, int max) {
    if (!g_ldn_ping_overlay || !g_ldn.online || !g_ldn.cs_ready) return 0;
    int n = 0;
    EnterCriticalSection(&g_ldn.cs);
    if (g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STA_CONNECTED) {
        for (int i = 0; i < 8 && i < max; i++) {
            const uint8_t *nd = g_ldn.info + NI_NODES + i * NODE_SIZE;
            ms[i] = -2;
            if (!nd[0xb]) continue;
            uint32_t vip = ldn_rd32(nd);
            if (vip == g_ldn.vip) ms[i] = (int)g_ldn_my_ping;
            else { ms[i] = -1; for (int k = 0; k < LDN_PEERS; k++) if (g_ldn_pings[k].vip == vip && GetTickCount() - g_ldn_pings[k].at < 15000) ms[i] = g_ldn_pings[k].ms; }
            n = i + 1;
        }
    }
    LeaveCriticalSection(&g_ldn.cs);
    return n;
}
