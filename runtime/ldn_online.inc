/* Included by nnnet_c4a.inc: nn::ldn (local wireless) carried over the internet through a room server
 * (tools/online/ldn_room_server.py), and the virtual-address path of the socket layer that goes with it.
 *
 * Configuration: <exe folder>\..\config\online.ini (or the environment, which wins):
 *     server = host:port          SSBU_LDN_SERVER
 *     room   = <password>         SSBU_LDN_ROOM      players with the same password meet in the same room
 *     name   = <player name>      SSBU_LDN_NAME      shown in the server's log only
 * Without a server and a room, nothing changes: nn::ldn::Initialize answers "wireless off" (0x2ecb) as before.
 * tools/online/online.ini.example is a template. SSBU_LDN_TRACE=1 logs every nn::ldn call and every game datagram.
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

#define LDN_RESULT(d) (203u | ((uint32_t)(d) << 9))
#define LDN_ADVERTISE_TOO_LARGE LDN_RESULT(10)
#define LDN_INVALID_NODE_COUNT LDN_RESULT(30)
#define LDN_CONNECTION_FAILED LDN_RESULT(31)
#define LDN_BAD_STATE LDN_RESULT(32)
#define LDN_NO_IP LDN_RESULT(33)
#define LDN_AP_CONNECTION_FAILED LDN_RESULT(65)
#define LDN_MAX_NODES LDN_RESULT(67)

enum { LDN_NONE, LDN_INITIALIZED, LDN_AP_OPENED, LDN_AP_CREATED, LDN_STA_OPENED, LDN_STA_CONNECTED, LDN_ERROR };
enum { LM_SCAN = 1, LM_SCAN_RESP, LM_CONNECT, LM_DISCONNECT, LM_DESTROY, LM_SYNC, LM_GONE = 0xff };

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
    char host[256], room[128], name[64];
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

static int ldn_trace(void) { static int t = -1; if (t < 0) t = getenv("SSBU_LDN_TRACE") != NULL; return t; }   /* SSBU_LDN_TRACE: every call and datagram */
static uint32_t ldn_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void ldn_wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void ldn_ip_text(uint32_t v, char *out) { sprintf(out, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255); }
static int ldn_is_virtual(uint32_t be_addr) {             /* address as stored in a sockaddr (network order) */
    uint32_t v = ntohl(be_addr);
    return g_ldn.online && (((v & 0xffff0000u) == 0xa9fe0000u) || v == 0xffffffffu);
}

/* ---------------------------------------------------------------- configuration */
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
    char exe[MAX_PATH], path[MAX_PATH + 64], line[512], server[300] = "";
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
        else if (!_stricmp(k, "name")) snprintf(g_ldn.name, sizeof g_ldn.name, "%s", v);
    }
    if (f) fclose(f);
    const char *e;
    if ((e = getenv("SSBU_LDN_SERVER")) && *e) snprintf(server, sizeof server, "%s", e);
    if ((e = getenv("SSBU_LDN_ROOM")) && *e) snprintf(g_ldn.room, sizeof g_ldn.room, "%s", e);
    if ((e = getenv("SSBU_LDN_NAME")) && *e) snprintf(g_ldn.name, sizeof g_ldn.name, "%s", e);
    if (!g_ldn.name[0]) snprintf(g_ldn.name, sizeof g_ldn.name, "player");
    char *colon = strrchr(server, ':');
    g_ldn.port = 39010;
    if (colon) { *colon = 0; g_ldn.port = (uint16_t)atoi(colon + 1); }
    snprintf(g_ldn.host, sizeof g_ldn.host, "%s", server);
    g_ldn.configured = g_ldn.host[0] && g_ldn.room[0];
    printf("nn::ldn: %s\n", g_ldn.configured ? "online through a room server (config\\online.ini / SSBU_LDN_*)" :
           "no room server configured (config\\online.ini): local wireless reports \"wireless off\"");
    if (g_ldn.configured) printf("nn::ldn: server %s:%u, room password set (%u characters), name \"%s\"\n",
                                 g_ldn.host, g_ldn.port, (unsigned)strlen(g_ldn.room), g_ldn.name);
}

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
        printf("nn::ldn: station %s left node slot %d\n", t, i);
        ldn_update_nodes();
    }
}
static void ldn_lost_host(int16_t reason) {
    g_ldn.host_vip = 0;
    if (g_ldn.state == LDN_STA_CONNECTED) {
        g_ldn.disconnect_reason = reason;
        printf("nn::ldn: the host's network is gone (reason %d)\n", reason);
        ldn_set_state(LDN_STA_OPENED);
    }
}

static void ldn_on_message(uint32_t src, uint8_t type, const uint8_t *d, uint32_t n) {
    EnterCriticalSection(&g_ldn.cs);
    char t[20]; ldn_ip_text(src, t);
    switch (type) {
    case LM_SCAN:
        if (g_ldn.state == LDN_AP_CREATED) ldn_send_msg(src, LM_SCAN_RESP, g_ldn.info, NI_SIZE);
        break;
    case LM_SCAN_RESP:
        if (n >= NI_SIZE && g_ldn.nscan < 16) {
            int dup = 0;
            for (int i = 0; i < g_ldn.nscan; i++) if (!memcmp(g_ldn.scan[i] + NI_BSSID, d + NI_BSSID, 6)) { memcpy(g_ldn.scan[i], d, NI_SIZE); dup = 1; }
            if (!dup) memcpy(g_ldn.scan[g_ldn.nscan++], d, NI_SIZE);
        }
        break;
    case LM_CONNECT:
        if (g_ldn.state == LDN_AP_CREATED && n >= NODE_SIZE) {
            int slot = 0, max = g_ldn.info[NI_NODE_MAX] ? g_ldn.info[NI_NODE_MAX] : 8;
            for (int i = 1; i < 8 && i < max && !slot; i++) if (!g_ldn.clients[i] || g_ldn.clients[i] == src) slot = i;
            if (!slot || g_ldn.accept_policy == 1) { printf("nn::ldn: station %s refused (%s)\n", t, slot ? "accept policy" : "network full"); ldn_send_msg(src, LM_DESTROY, NULL, 0); break; }
            g_ldn.clients[slot] = src;
            memcpy(g_ldn.info + NI_NODES + slot * NODE_SIZE, d, NODE_SIZE);
            printf("nn::ldn: station %s joined in node slot %d\n", t, slot);
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
        if ((g_ldn.state == LDN_STA_OPENED || g_ldn.state == LDN_STA_CONNECTED) && n >= NI_SIZE && src == g_ldn.host_vip) {
            memcpy(g_ldn.info, d, NI_SIZE);
            if (g_ldn.state == LDN_STA_OPENED) { printf("nn::ldn: connected to the host's network\n"); ldn_set_state(LDN_STA_CONNECTED); }
            ldn_node_changes();
        }
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
        }
    }
    free(buf);
    return 0;
}
static DWORD WINAPI ldn_keepalive_thread(LPVOID unused) {
    (void)unused;
    for (int tick = 0; !g_ldn.stop; tick++) {
        uint8_t reg[5] = {1};
        memcpy(reg + 1, &g_ldn.token, 4);
        send(g_ldn.udp, (const char *)reg, 5, 0);
        if (tick % 5 == 4) ldn_send_frame(3, NULL, 0, NULL, 0);
        static LONG said_sent = -1, said_in = -1;
        if (tick % 5 == 4 && ldn_trace() && (g_ldn_sent != said_sent || g_ldn_injected != said_in)) {
            said_sent = g_ldn_sent; said_in = g_ldn_injected;
            printf("nn::ldn: game datagrams so far: %ld sent, %ld received, %ld dropped\n", g_ldn_sent, g_ldn_injected, g_ldn_dropped);
        }
        for (int i = 0; i < 20 && !g_ldn.stop; i++) Sleep(100);
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
static int ldn_connect_server(void) {
    if (g_ldn.online) return 1;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    struct addrinfo hints = {0}, *res = NULL;
    char port[8];
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%u", g_ldn.port);
    if (getaddrinfo(g_ldn.host, port, &hints, &res) != 0 || !res) { printf("nn::ldn: cannot resolve %s\n", g_ldn.host); return 0; }
    memcpy(&g_ldn.server, res->ai_addr, sizeof g_ldn.server);
    freeaddrinfo(res);
    g_ldn.tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(g_ldn.tcp, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    DWORD tmo = 5000;
    setsockopt(g_ldn.tcp, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
    if (connect(g_ldn.tcp, (struct sockaddr *)&g_ldn.server, sizeof g_ldn.server) != 0) {
        printf("nn::ldn: cannot reach the room server %s:%u (error %d)\n", g_ldn.host, g_ldn.port, WSAGetLastError());
        closesocket(g_ldn.tcp); g_ldn.tcp = INVALID_SOCKET;
        return 0;
    }
    uint8_t hello[1 + 1 + 127 + 1 + 63];
    size_t rl = strlen(g_ldn.room), nl = strlen(g_ldn.name);
    if (rl > 127) rl = 127;
    if (nl > 63) nl = 63;
    hello[0] = 1; hello[1] = (uint8_t)rl; memcpy(hello + 2, g_ldn.room, rl);
    hello[2 + rl] = (uint8_t)nl; memcpy(hello + 3 + rl, g_ldn.name, nl);
    uint8_t head[5], body[64];
    if (!ldn_send_frame(1, hello, (uint32_t)(3 + rl + nl), NULL, 0) || !ldn_recv_all(g_ldn.tcp, head, 5) ||
        ldn_rd32(head) < 1 || ldn_rd32(head) > sizeof body + 1 || !ldn_recv_all(g_ldn.tcp, body, ldn_rd32(head) - 1) || head[4] != 2) {
        if (head[4] == 4) printf("nn::ldn: the room server refused: %.*s\n", body[0], body + 1);
        else printf("nn::ldn: no answer from the room server\n");
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
