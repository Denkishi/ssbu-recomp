/* Included by nnnet.c (round "close4", helper A): nn::nifm leftovers, nn::socket / nnsocket* (bsd + resolver) on
 * Winsock, nn::ssl / nnssl*, nn::nsd::ResolveEx, nn::ldn, nn::ec::ShopServiceAccessor.
 *
 * Sources: [sdk] = binaries\13.0.4\sdk.elf (addresses quoted), [suyu] = reference\suyu\src\core (file named).
 * Console state modelled: no network connection (the nifm network request is never accepted), wireless
 * communication not available (a PC has no local-wireless radio), one local user.
 *
 * ------------------------------------------------------------------ nn::nifm
 * [sdk 0x3218f0] detail::GetGeneralServicePointer and [sdk 0x321990] GetNetworkConnectionPointer return 0xca6e
 *     (module 110, description 101) while the library is not initialised (byte .bss 0xcc9910 != 1).
 * [sdk 0x322900] GetCurrentPrimaryIpAddress(in_addr *): null pointer -> abort; IGeneralService command at vtable
 *     +0x70 (GetCurrentIpAddress); 4 bytes copied out on success.
 * [sdk 0x322ae0] GetCurrentIpConfigInfo(ip, mask, gateway, dns1, dns2): any null -> abort; command at +0x88;
 *     the five addresses are bytes 1..12 of the IpAddressSetting and 1..8 of the DnsSetting.
 * [sdk 0x323620] GetCurrentProxySetting(ProxySetting *): GetCurrentNetworkProfile ([sdk 0x322e60], command +0x38),
 *     converted by [sdk 0x324950] (plain copies), then 0xaa bytes at +0xe2 of the converted profile are copied out.
 *     The module's answers are the reference's ([suyu] hle/service/nifm/nifm.cpp) for a host that is not connected:
 *     GetCurrentIpAddress = success, 0.0.0.0; GetCurrentIpConfigInfo = success, all zero; GetCurrentNetworkProfile
 *     = success, a zero profile (so the proxy setting is 0xaa zero bytes: proxy disabled).
 * [sdk 0x322d90] HandleNetworkRequestErrorResult(): false when not initialised; otherwise [sdk 0x324550]
 *     NetworkConnection::HandleNetworkRequestErrorResult -> [sdk 0x31cbc0] RequestClient::PrepareLibraryApplet asks
 *     the request object for the applet to show (IRequest command at +0xc0, GetAppletInfo). [suyu] answers success
 *     with applet id 0 and an empty parameter block; with an empty block the SDK returns 0x1906e (110-200) and the
 *     function gives false. A console shows the system's network-error applet here; that applet is not modelled.
 *
 * ------------------------------------------------------------------ nn::socket (bsd:u) and the resolver (sfdnsres)
 * [sdk] every nn::socket::X is a jump to nn::socket::detail::X and every nnsocketX a jump to nn::socket::X
 *     (0x36ac.. / 0x36b5.., 4 bytes each; Fcntl copies its one variadic int, 0x36afa0 / 0x36b6a0).
 * [sdk 0x36f8f0 / 0x36f5d0] Initialize(pool, poolSize, allocatorPoolSize, concurrency): aborts when already
 *     initialised, pool null or not page aligned, sizes not page aligned, allocator pool < 0x1000, pool <= allocator
 *     pool, concurrency not 1..14; pool < 0xa0000 -> Result 0x21b (module 27, description 1); else registers with
 *     bsd:u (works without a connection), enables the resolver, 0.
 * [sdk 0x36f980] Finalize: abort when not initialised; the session is closed (the module closes the sockets), 0.
 * [sdk 0x36fa60 / 0x36fac0] errno IS the C library's errno (__errno_location); SetLastErrno does nothing and
 *     GetLastErrno gives 22 while the library is not initialised.
 * [sdk 0x374b80 ...] every bsd call aborts when the library is not initialised; the module returns (value, errno)
 *     and the SDK stores errno when the value is negative. SDK-side argument checks, before the module is asked:
 *       Recv / Send / RecvFrom / SendTo  size 0 -> 0; buffer null -> -1 EINVAL(22); size >= 4 GiB -> -1 EFAULT(14)
 *       RecvFrom with no address / null or zero length -> Recv; SendTo with no address / zero length -> Send
 *       Bind / Connect   address null -> -1 EINVAL  (the SockAddr overload of Bind also for length 0)
 *       Accept           address null with *length != 0 -> -1 EINVAL
 *       GetSockName / GetPeerName / GetSockOpt   pointer null or *length 0 -> -1 EINVAL;  SetSockOpt value null -> EINVAL
 *       Poll             fds null with count != 0 -> -1 EINVAL; count >= 2^29 -> -1 EFAULT
 *     The typed overloads (Family, Type, Protocol, MsgFlag, Level, Option, SockAddr) pass the same numbers
 *     ([sdk 0x374a40] masks the family to 8 bits).
 * The module: a real socket layer on Winsock, following [suyu] hle/service/sockets/bsd.cpp + internal_network/
 *     network.cpp (128 descriptors from 0, AF_INET only, O_NONBLOCK 0x800 with F_GETFL 3 / F_SETFL 4, MSG_PEEK 2,
 *     MSG_DONTWAIT 0x80, SOL_SOCKET 0xffff with the BSD option numbers, BSD sockaddr_in {len, family, port, addr},
 *     pollfd {int fd; short events, revents} with POLLIN 1 PRI 2 OUT 4 ERR 8 HUP 0x10 NVAL 0x20 RDNORM 0x40 RDBAND
 *     0x80 WRBAND 0x100). errno numbers are the SDK's own C library's ([sdk] table 0x9f0bb5 + texts 0xaa8204:
 *     9 EBADF, 11 EAGAIN, 22 EINVAL, 24 EMFILE, 32 EPIPE, 88 ENOTSOCK, 90 EMSGSIZE, 92 ENOPROTOOPT, 93, 94, 95,
 *     97 EAFNOSUPPORT, 98 EADDRINUSE, 99 EADDRNOTAVAIL, 100 ENETDOWN, 101 ENETUNREACH, 103, 104, 105, 106 EISCONN,
 *     107 ENOTCONN, 108, 110 ETIMEDOUT, 111 ECONNREFUSED, 113 EHOSTUNREACH, 114 EALREADY, 115 EINPROGRESS).
 *     No connection: only the loopback network 127.0.0.0/8 exists. Connect / SendTo to any other address fail with
 *     ENETUNREACH (what a host without a network gives: Winsock WSAENETUNREACH, [suyu] network.cpp -> NETUNREACH);
 *     Bind to an address other than 0.0.0.0 / 127.x fails with EADDRNOTAVAIL; a socket bound to 0.0.0.0 is bound to
 *     the loopback interface on the PC (nothing is ever opened towards the PC's real network) and GetSockName still
 *     reports 0.0.0.0. NOT VERIFIED on a console: the exact errno of the bsd module for an unreachable network, the
 *     descriptor numbering and limit, sin_len of returned addresses (16 here), Fcntl commands other than 3 / 4
 *     (EINVAL here), options outside SOL_SOCKET / TCP_NODELAY (accepted without effect, as the reference does).
 * Resolver: [sdk 0x37fb40] GetAddrInfo: not initialised -> 11 (EAI_SYSTEM); node and service null -> 8; result
 *     pointer null -> 11 + errno 22; then the service; on any failure the result pointer is null.
 *     [sdk 0x37ef80] GetHostByName(name, options, count): not initialised -> null + errno 22; name null -> null,
 *     h_errno 1, errno 22. [sdk 0x37e9e0] GAIStrError: "Error string is not available" ([sdk] 0x9e3ef4) when not
 *     initialised, else the text comes from the module. [sdk 0x37e760 / 0x37eb30] ResolverGetOption / SetOption:
 *     the keys 0x11171..0x11176 (type 0x67, size 8) live in the SDK's own table .data 0xc0cd28 (read / written in
 *     the mapped SDK image); 0x11170 / 0x11177 -> EINVAL; key 10001 gives a random cancel handle; anything else is
 *     the module's: Get -> -1 ([suyu] sfdnsres.cpp has no such command), Set -> 0 ([suyu] ResolverSetOptionRequest).
 *     [sdk 0x36cc00 / 0x36c790] result layouts: addrinfo {flags, family, socktype, protocol, addrlen, canonname
 *     @0x18, addr @0x20, next @0x28}; nn::socket::AddrInfo {.., family u8 @4, .., addr @0x18, canonname @0x20, next};
 *     [sdk 0x36d890] hostent {name, aliases, addrtype @0x10, length @0x14, addr_list @0x18}.
 *     The module ([suyu] sfdnsres.cpp, "verified on console" tables there): a name that needs DNS cannot be resolved
 *     without a network: EAI_AGAIN (2), h_errno TRY_AGAIN (2), errno 0. A numeric IPv4 address needs no DNS and is
 *     converted (Winsock getaddrinfo with AI_NUMERICHOST, AF_INET). The error texts are the reference's
 *     (sockets_translate.cpp); they are not in sdk.elf. NOT VERIFIED: a console's answer for "localhost".
 *     Results are allocated on the host heap, not in the pool given to Initialize.
 *
 * ------------------------------------------------------------------ nn::ssl
 * [sdk 0x386720] Initialize: use count (.bss 0xcd10a8) is incremented on EVERY call; a call that finds it non-zero
 *     returns 0x1927b (module 123, description 201). [sdk 0x3867e0] Finalize: count 0 -> 0xca7b (123-101); count
 *     still > 0 after the decrement -> 0; reaching 0 closes the session -> 0.
 * [sdk 0x3832f0] Context::Create(version): no session -> 0xca7b; the object is 8 bytes: the address of an 8-byte
 *     chunk that holds the service object, with its four 16-bit pieces in reverse order. [sdk 0x3836d0] GetContextId
 *     = those 8 bytes; null out -> 0xe27b (123-113). [sdk 0x3833f0] Destroy: 0xca7b / 0xdc7b (invalid context,
 *     123-110) / 0xd67b (123-107) while connections exist. [sdk 0x3836f0] ImportServerPki(outId, data, size, format):
 *     0xca7b; null id or data -> 0xe27b; invalid context -> 0xdc7b.
 * [sdk 0x383c70 / 0x383ca0 / 0x383dd0] Connection {chunk @0, context id @8, socket @0x10 (-1), certificate buffer
 *     @0x18 + size @0x20, last Result @0x24}: Create: 0xca7b, null context -> 0xe27b, bad context -> 0xdc7b;
 *     Destroy: 0xca7b, 0xda7b (invalid connection, 123-109), closes the socket @0x10 when it is >= 0.
 *     SetSocketDescriptor / SetHostName (null -> 0xe27b) / SetVerifyOption / SetIoMode / SetSessionCacheMode /
 *     SetOption / DoHandshake / Read / Write (null -> 0xe27b, size 0 -> 0xe07b) / Pending (null -> 0xe27b) /
 *     GetVerifyCertError (null -> 0xe27b) / GetConnectionId: 0xca7b, then 0xda7b, then the module.
 * [sdk 0x385d30 ...] the nnssl* C functions return 0xe27b for a null object and call the C++ function.
 * The module ([suyu] hle/service/ssl/ssl.cpp): CreateContext, CreateConnection, ImportServerPki (id 0),
 *     SetHostName, SetVerifyOption, SetSessionCacheMode, SetOption succeed; SetSocketDescriptor duplicates the
 *     descriptor (unknown descriptor -> 0xd47b, 123-106) and hands back -1, or the duplicate when the option
 *     DoNotCloseSocket (0) is set; SetIoMode without a socket -> 0xce7b (123-103), else it sets the socket's
 *     blocking mode; DoHandshake without a socket -> 0xce7b; Pending -> 0.
 *     OPEN: DoHandshake with a socket, Read, Write and GetVerifyCertError need a TLS implementation on a connected
 *     socket (only a loopback peer can exist here); those paths stop with a message instead of inventing a Result.
 *
 * ------------------------------------------------------------------ nn::nsd
 * [sdk 0x3287a0 -> 0x3290f0] ResolveEx(Fqdn *out, const Fqdn &in): the nsd service command at +0x58; only the
 *     command's own Result is returned. [suyu] hle/service/sockets/nsd.cpp: the name is returned unchanged (0x100
 *     bytes), success. NOT VERIFIED: the real module substitutes the environment identifier in names that contain '%'.
 *
 * ------------------------------------------------------------------ nn::ldn
 * [sdk 0x2bc7c0] Initialize: creates the user service and calls its Initialize (vtable +0x100); only on success the
 *     service is stored in .bss 0xcae718 / 0xcae720. [sdk 0x2bcda0] GetState: 0 while no service is stored. Every
 *     other function loads the service pointer and calls through it without a check ([sdk 0x2bcfb0] etc.).
 * Platform fact modelled: this machine has NO local-wireless radio; that is a console whose wireless communication
 *     is switched off. [suyu] hle/service/ldn/user_local_communication_service.cpp Initialize: no usable interface
 *     -> ResultAirplaneModeEnabled (module 203, description 23) = 0x2ecb. So Initialize fails with 0x2ecb, GetState
 *     stays 0 (None), and the game ([game] 0x7103704504) puts the Result in an ErrorResultVariant and does not go on;
 *     it calls Finalize / SetWirelessControllerRestriction only when GetState() != 0 ([game] 0x7103703800,
 *     0x71016a60c4). Any other nn::ldn call after a failed Initialize dereferences a null service pointer on a
 *     console: those handlers stop with that message. (The alternative, "wireless on, nobody around", would need the
 *     real module's NetworkInfo contents for CreateNetwork, which the reference invents: not established.)
 *
 * ------------------------------------------------------------------ nn::ec::ShopServiceAccessor
 * [sdk 0x18e690] InitializeForShopServiceAccessors(buffer, size): builds the proxy and opens the nim:eca session
 *     ([suyu] hle/service/nim/nim.cpp: succeeds) -> 0. [sdk 0x193030] Finalize...: finalizes the accessors still
 *     registered -> 0. [sdk 0x18d740] ShopServiceAccessor(): {this, this} @0, service @0x10 = null, recursive mutex
 *     @0x18, list head {&, &} @0x38. [sdk 0x18d870] Initialize(service): CreateAccessorInterface ([suyu]: succeeds),
 *     service -> @0x10, linked into the proxy's list -> 0. [sdk 0x18d7d0] Finalize, [sdk 0x18d780] destructor.
 * [sdk 0x18da10] Request(): FIRST checks nn::nifm::IsAnyInternetRequestAccepted(GetClientId()); false -> 0x70a4
 *     (module 164, description 56) and nothing else happens. No request is ever accepted here.
 * [sdk 0x18e080 -> 0x18df20] AsyncResponse::GetSize: nothing attached (pointer @0x18 null) -> 0x190a4 (164-200),
 *     *out untouched. [sdk 0x18de10] ~AsyncResponse: Detach ([sdk 0x18de50]: unlink from the accessor @0x10), and
 *     the event @0x20 destroyed when the flag @0x48 is set. */

#define RETI(v) (c->x[0] = (uint64_t)(int64_t)(v))
#define M8(a) (*(uint8_t *)(uintptr_t)(a))
#define M16(a) (*(uint16_t *)(uintptr_t)(a))
#define M32(a) (*(uint32_t *)(uintptr_t)(a))
#define M64(a) (*(uint64_t *)(uintptr_t)(a))
#define A4 (c->x[4])
#define A5 (c->x[5])

static void c4a_call(Ctx *c, const char *name, uint64_t x0) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("nnnet: no handler for %s\n", name); guest_trap("nnnet: missing handler", c->x[30] - 4); }
    c->x[0] = x0;
    f(c);
}

/* ================================================================== nn::nifm */
static int g_nifm_init;
static ImportFn g_nifm_initialize_prev;
static void nifm_Initialize(Ctx *c) {                    /* the existing handler, plus the "initialised" flag of [sdk] 0xcc9910 */
    if (g_nifm_initialize_prev) g_nifm_initialize_prev(c); else RET(0);
    if ((uint32_t)c->x[0] == 0) g_nifm_init = 1;
}
#define NIFM_NOT_INITIALIZED 0xca6eu
static void nifm_GetCurrentPrimaryIpAddress(Ctx *c) {
    if (!A0) guest_trap("nn::nifm::GetCurrentPrimaryIpAddress: null pointer ([sdk 0x322998] abort)", c->x[30] - 4);
    if (!g_nifm_init) { RET(NIFM_NOT_INITIALIZED); return; }
    M32(A0) = 0;
    RET(0);
}
static void nifm_GetCurrentIpConfigInfo(Ctx *c) {
    for (int i = 0; i < 5; i++)
        if (!c->x[i]) guest_trap("nn::nifm::GetCurrentIpConfigInfo: null pointer ([sdk 0x322bec] abort)", c->x[30] - 4);
    if (!g_nifm_init) { RET(NIFM_NOT_INITIALIZED); return; }
    for (int i = 0; i < 5; i++) M32(c->x[i]) = 0;
    RET(0);
}
static void nifm_GetCurrentProxySetting(Ctx *c) {
    if (!g_nifm_init) { RET(NIFM_NOT_INITIALIZED); return; }
    memset((void *)(uintptr_t)A0, 0, 0xaa);
    RET(0);
}
static void nifm_HandleNetworkRequestErrorResult(Ctx *c) {
    if (g_nifm_init) trace_printf("     nn::nifm::HandleNetworkRequestErrorResult: no error applet to show (not modelled) -> false\n");
    RET(0);
}

/* ================================================================== nn::socket on Winsock */
#define SO_MAX_FD 128
#define SO_O_NONBLOCK 0x800
typedef struct { SOCKET s; LONG refs; int type; int bound_any; uint16_t vport, vpeer_port; uint32_t vpeer; } SoObj;   /* v*: ldn_online.inc */
static void ldn_port_remove(uint16_t vport);
static struct { SoObj *o; int flags; } g_so_fd[SO_MAX_FD];
static SRWLOCK g_so_lock = SRWLOCK_INIT;
static int g_so_init, g_so_wsa;
static __declspec(thread) int t_so_herrno;

static int *so_errno_ptr(Ctx *c) {
    uint64_t keep = c->x[0];
    c4a_call(c, "__errno_location", 0);
    int *p = (int *)(uintptr_t)c->x[0];
    c->x[0] = keep;
    return p;
}
static void so_set_errno(Ctx *c, int e) { if (g_so_init) *so_errno_ptr(c) = e; }
static void so_fail(Ctx *c, int e) { so_set_errno(c, e); RETI(-1); }
static void so_need_init(Ctx *c, const char *what) {
    if (!g_so_init) {
        printf("\n%s called before nn::socket::Initialize: the SDK aborts here.\n", what);
        guest_trap("nn::socket call before Initialize (SDK abort)", c->x[30] - 4);
    }
}
static int so_wsa_errno(int e, int sending) {
    switch (e) {
    case 0: return 0;
    case WSAEINTR: return 4;
    case WSAEBADF: case WSAENOTSOCK: return e == WSAEBADF ? 9 : 88;
    case WSAEACCES: return 13;
    case WSAEFAULT: return 14;
    case WSAEINVAL: return 22;
    case WSAEMFILE: return 24;
    case WSAEWOULDBLOCK: return 11;
    case WSAEINPROGRESS: return 115;
    case WSAEALREADY: return 114;
    case WSAEDESTADDRREQ: return 89;
    case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;
    case WSAENOPROTOOPT: return 92;
    case WSAEPROTONOSUPPORT: return 93;
    case WSAESOCKTNOSUPPORT: return 94;
    case WSAEOPNOTSUPP: return 95;
    case WSAEPFNOSUPPORT: return 96;
    case WSAEAFNOSUPPORT: return 97;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAENETRESET: return 102;
    case WSAECONNABORTED: return sending ? 32 : 103;      /* [suyu] network.cpp: Winsock reports a broken pipe on send this way */
    case WSAECONNRESET: return 104;
    case WSAENOBUFS: return 105;
    case WSAEISCONN: return 106;
    case WSAENOTCONN: return 107;
    case WSAESHUTDOWN: return sending ? 32 : 108;
    case WSAETIMEDOUT: return 110;
    case WSAECONNREFUSED: return 111;
    case WSAEHOSTDOWN: return 112;
    case WSAEHOSTUNREACH: return 113;
    default: return 5;                                    /* EIO */
    }
}
static SoObj *so_get(int fd, int *flags) {
    SoObj *o = NULL;
    AcquireSRWLockExclusive(&g_so_lock);
    if (fd >= 0 && fd < SO_MAX_FD && g_so_fd[fd].o) { o = g_so_fd[fd].o; o->refs++; if (flags) *flags = g_so_fd[fd].flags; }
    ReleaseSRWLockExclusive(&g_so_lock);
    return o;
}
static void so_put(SoObj *o) {
    int last;
    AcquireSRWLockExclusive(&g_so_lock);
    last = --o->refs == 0;
    ReleaseSRWLockExclusive(&g_so_lock);
    if (last) { if (o->vport) ldn_port_remove(o->vport); closesocket(o->s); free(o); }
}
static int so_install(SoObj *o, int flags) {             /* lowest free descriptor, or -1 */
    int fd = -1;
    AcquireSRWLockExclusive(&g_so_lock);
    for (int i = 0; i < SO_MAX_FD && fd < 0; i++) if (!g_so_fd[i].o) fd = i;
    if (fd >= 0) { g_so_fd[fd].o = o; g_so_fd[fd].flags = flags; o->refs++; }
    ReleaseSRWLockExclusive(&g_so_lock);
    return fd;
}
static int so_remove(int fd) {
    SoObj *o = NULL;
    AcquireSRWLockExclusive(&g_so_lock);
    if (fd >= 0 && fd < SO_MAX_FD) { o = g_so_fd[fd].o; g_so_fd[fd].o = NULL; g_so_fd[fd].flags = 0; }
    ReleaseSRWLockExclusive(&g_so_lock);
    if (!o) return 0;
    so_put(o);
    return 1;
}
static int so_dup(int fd) {                               /* bsd DuplicateSocket: a second descriptor for the same socket */
    int flags = 0, nfd;
    SoObj *o = so_get(fd, &flags);
    if (!o) return -1;
    nfd = so_install(o, flags);
    so_put(o);
    return nfd;
}
static void so_nonblock(SoObj *o, int on) { u_long v = on ? 1 : 0; ioctlsocket(o->s, FIONBIO, &v); }
static int so_is_loopback(uint32_t be_addr) { return (be_addr & 0xff) == 127; }      /* first byte in memory */
static int so_addr_in(uint64_t p, struct sockaddr_in *out) {                          /* guest BSD sockaddr_in -> host; 0 when not AF_INET */
    memset(out, 0, sizeof *out);
    if (M8(p + 1) != 2) return 0;
    out->sin_family = AF_INET;
    out->sin_port = M16(p + 2);
    out->sin_addr.S_un.S_addr = M32(p + 4);
    return 1;
}
static uint32_t so_addr_out(uint64_t p, uint32_t cap, const struct sockaddr_in *in) { /* returns the length the module reports */
    uint8_t b[16] = {0};
    b[0] = 16; b[1] = 2;
    memcpy(b + 2, &in->sin_port, 2);
    memcpy(b + 4, &in->sin_addr, 4);
    if (p && cap) memcpy((void *)(uintptr_t)p, b, cap < 16 ? cap : 16);
    return 16;
}

/* ================================================================== nn::ldn (ldn_online.inc: over a room server when configured) */
#define LDN_WIRELESS_OFF 0x2ecbu
void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);
static void ldn_no_service(Ctx *c) {
    printf("\nnn::ldn function called although nn::ldn::Initialize failed (no local-wireless radio: Result 0x2ecb).\n"
           "The SDK dereferences its null service pointer here; a console crashes at this point.\n");
    guest_trap("nn::ldn call without a service (Initialize failed)", c->x[30] - 4);
}
#include "ldn_online.inc"

static void socket_Initialize(Ctx *c) {                   /* (void *pool, size_t poolSize, size_t allocatorPoolSize, int concurrency) -> Result */
    uint64_t pool = A0, size = A1, alloc = A2;
    int conc = (int)A3;
    if (g_so_init || !pool || (unsigned)(conc - 1) >= 14 || size <= alloc || (pool & 0xfff) || (size & 0xfff) || (alloc & 0xfff) || alloc <= 0xfff)
        guest_trap("nn::socket::Initialize: [sdk 0x36f89c] abort (already initialised or bad arguments)", c->x[30] - 4);
    if (size < 0xa0000) { RET(0x21bu); return; }
    if (!g_so_wsa) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) == 0) g_so_wsa = 1; }
    g_so_init = 1;
    RET(0);
}
static void socket_Finalize(Ctx *c) {
    so_need_init(c, "nn::socket::Finalize");
    for (int i = 0; i < SO_MAX_FD; i++) so_remove(i);
    g_so_init = 0;
    RET(0);
}
static void socket_GetLastErrno2(Ctx *c) { if (!g_so_init) RETI(22); else RETI(*so_errno_ptr(c)); }
static void socket_SetLastErrno2(Ctx *c) { int e = (int)A0; so_set_errno(c, e); }

static void socket_Socket(Ctx *c) {                       /* (domain, type, protocol) */
    int domain = (int)(A0 & 0xff), type = (int)A1, protocol = (int)A2;
    so_need_init(c, "nn::socket::Socket");
    if (domain != 2) { so_fail(c, 97); return; }
    SOCKET s = socket(AF_INET, type, protocol);
    if (s == INVALID_SOCKET) { so_fail(c, so_wsa_errno(WSAGetLastError(), 0)); return; }
    SoObj *o = calloc(1, sizeof *o);
    o->s = s; o->type = type;
    int fd = so_install(o, 0);
    if (fd < 0) { closesocket(s); free(o); so_fail(c, 24); return; }
    RETI(fd);
}
static void socket_Close(Ctx *c) {
    so_need_init(c, "nn::socket::Close");
    if (!so_remove((int)A0)) { so_fail(c, 9); return; }
    RETI(0);
}
static void so_bind(Ctx *c, int len_check) {              /* (fd, const sockaddr *, len) */
    so_need_init(c, "nn::socket::Bind");
    if (!A1 || (len_check && !(uint32_t)A2)) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    int e = 0;
    if (!so_addr_in(A1, &a)) e = 97;
    else if (g_ldn.online && o->type == SOCK_DGRAM && (a.sin_addr.S_un.S_addr == 0 || ldn_is_virtual(a.sin_addr.S_un.S_addr))) {
        uint16_t vp = ntohs(a.sin_port);                  /* local wireless is up: a virtual port (ldn_online.inc) */
        int w = ldn_so_bind(o->s, &vp);
        if (w) e = so_wsa_errno(w, 0);
        else { o->vport = vp; o->bound_any = a.sin_addr.S_un.S_addr == 0; }
    }
    else if (a.sin_addr.S_un.S_addr != 0 && !so_is_loopback(a.sin_addr.S_un.S_addr)) e = 99;
    else {
        int any = a.sin_addr.S_un.S_addr == 0;
        if (any) a.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        if (bind(o->s, (struct sockaddr *)&a, sizeof a) != 0) e = so_wsa_errno(WSAGetLastError(), 0);
        else o->bound_any = any;
    }
    ldn_load_config();
    if (g_ldn.configured && o->type == SOCK_DGRAM && ldn_trace())
        printf("nn::socket::Bind fd %d udp %u.%u.%u.%u:%u -> %s%d (virtual port %u)\n", (int)A0, M8(A1 + 4), M8(A1 + 5), M8(A1 + 6), M8(A1 + 7),
               ntohs(M16(A1 + 2)), e ? "errno " : "ok ", e, o->vport);
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_Bind(Ctx *c) { so_bind(c, 0); }
static void socket_BindSockAddr(Ctx *c) { so_bind(c, 1); }
static void socket_Connect(Ctx *c) {                      /* (fd, const sockaddr *, len) */
    so_need_init(c, "nn::socket::Connect");
    if (!A1) { so_fail(c, 22); return; }
    int flags = 0, e = 0;
    SoObj *o = so_get((int)A0, &flags);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    if (!so_addr_in(A1, &a)) e = 97;
    else if (o->type == SOCK_DGRAM && ldn_is_virtual(a.sin_addr.S_un.S_addr)) {     /* a UDP "connection" to a virtual address */
        e = ldn_so_ensure_port(o);
        if (!e) { o->vpeer = a.sin_addr.S_un.S_addr; o->vpeer_port = a.sin_port; }
    }
    else if (!so_is_loopback(a.sin_addr.S_un.S_addr)) e = 101;                        /* no network: nothing but loopback is reachable */
    else if (connect(o->s, (struct sockaddr *)&a, sizeof a) != 0) {
        int w = WSAGetLastError();
        e = w == WSAEWOULDBLOCK ? 115 : w == WSAEISCONN ? 0 : so_wsa_errno(w, 0);     /* [suyu] ConnectImpl: "already connected" is success */
    }
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_Listen(Ctx *c) {
    so_need_init(c, "nn::socket::Listen");
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = listen(o->s, (int)A1) != 0 ? so_wsa_errno(WSAGetLastError(), 0) : 0;
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_Shutdown(Ctx *c) {
    so_need_init(c, "nn::socket::Shutdown");
    int how = (int)A1;
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = (unsigned)how > 2 ? 22 : shutdown(o->s, how) != 0 ? so_wsa_errno(WSAGetLastError(), 0) : 0;
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_Accept(Ctx *c) {                       /* (fd, sockaddr *, socklen_t *) */
    uint64_t addr = A1, lenp = A2;
    so_need_init(c, "nn::socket::Accept");
    if (!addr && lenp && M32(lenp)) { so_fail(c, 22); return; }
    uint32_t cap = addr && lenp ? M32(lenp) : 0;
    int flags = 0;
    SoObj *o = so_get((int)A0, &flags);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    int alen = sizeof a;
    memset(&a, 0, sizeof a);
    SOCKET s = accept(o->s, (struct sockaddr *)&a, &alen);
    int e = s == INVALID_SOCKET ? so_wsa_errno(WSAGetLastError(), 0) : 0, type = o->type;
    so_put(o);
    if (e) { so_fail(c, e); return; }
    SoObj *n = calloc(1, sizeof *n);
    n->s = s; n->type = type;
    so_nonblock(n, 0);                                    /* a new descriptor starts blocking */
    int fd = so_install(n, 0);
    if (fd < 0) { closesocket(s); free(n); so_fail(c, 24); return; }
    uint32_t out = so_addr_out(addr, cap, &a);
    if (lenp) M32(lenp) = cap ? out : 0;
    RETI(fd);
}
static void so_name(Ctx *c, int peer) {                   /* (fd, sockaddr *, socklen_t *) */
    uint64_t addr = A1, lenp = A2;
    so_need_init(c, peer ? "nn::socket::GetPeerName" : "nn::socket::GetSockName");
    if (!addr || !lenp || !M32(lenp)) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    struct sockaddr_in a;
    int alen = sizeof a;
    memset(&a, 0, sizeof a);
    int e = (peer ? getpeername(o->s, (struct sockaddr *)&a, &alen) : getsockname(o->s, (struct sockaddr *)&a, &alen)) != 0 ? so_wsa_errno(WSAGetLastError(), 0) : 0;
    if (!e && !peer && o->bound_any) a.sin_addr.S_un.S_addr = 0;
    if (!peer && o->vport) { e = 0; a.sin_family = AF_INET; a.sin_port = htons(o->vport); a.sin_addr.S_un.S_addr = o->bound_any ? 0 : htonl(g_ldn.vip); }
    if (peer && o->vpeer) { e = 0; a.sin_family = AF_INET; a.sin_port = o->vpeer_port; a.sin_addr.S_un.S_addr = o->vpeer; }
    if (e == 22 && !peer) { e = 0; a.sin_family = AF_INET; }                         /* Winsock refuses an unbound socket; BSD reports 0.0.0.0:0 */
    so_put(o);
    if (e) { so_fail(c, e); return; }
    M32(lenp) = so_addr_out(addr, M32(lenp), &a);
    RETI(0);
}
static void socket_GetSockName(Ctx *c) { so_name(c, 0); }
static void socket_GetPeerName(Ctx *c) { so_name(c, 1); }
static int so_host_flags(int f) { return (f & 2 ? MSG_PEEK : 0) | (f & 1 ? MSG_OOB : 0) | (f & 0x40 ? MSG_WAITALL : 0); }
static void so_recv(Ctx *c, uint64_t addr, uint64_t lenp) {   /* (fd, buf, size, flags [, sockaddr *, socklen_t *]) */
    uint64_t buf = A1, size = A2;
    int gf = (int)A3, flags = 0;
    so_need_init(c, "nn::socket::Recv");
    if (!size) { RETI(0); return; }
    if (!buf) { so_fail(c, 22); return; }
    if (size >> 32) { so_fail(c, 14); return; }
    SoObj *o = so_get((int)A0, &flags);
    if (!o) { so_fail(c, 9); return; }
    int temp = (gf & 0x80) && !(flags & SO_O_NONBLOCK), n, e = 0;
    struct sockaddr_in a;
    int alen = sizeof a;
    memset(&a, 0, sizeof a);
    if (temp) so_nonblock(o, 1);
    if (o->vport && o->type == SOCK_DGRAM) {              /* virtual port: datagrams carry a 6-byte source header (ldn_online.inc) */
        int cap = (int)(size > 0xffff ? 0xffff : size) + 6;
        char *tmp = malloc(cap);
        n = recv(o->s, tmp, cap, so_host_flags(gf));
        if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) n = cap;
        if (n >= 6) {
            a.sin_family = AF_INET;
            memcpy(&a.sin_addr.S_un.S_addr, tmp, 4); memcpy(&a.sin_port, tmp + 4, 2);
            n -= 6;
            memcpy((void *)(uintptr_t)buf, tmp + 6, n);
        } else if (n >= 0) n = 0;
        free(tmp);
    }
    else if (addr) n = recvfrom(o->s, (char *)(uintptr_t)buf, (int)(size > 0x7fffffff ? 0x7fffffff : size), so_host_flags(gf), (struct sockaddr *)&a, &alen);
    else n = recv(o->s, (char *)(uintptr_t)buf, (int)(size > 0x7fffffff ? 0x7fffffff : size), so_host_flags(gf));
    if (n < 0) {
        int w = WSAGetLastError();
        if (w == WSAEMSGSIZE) n = (int)size;              /* datagram longer than the buffer: BSD truncates silently */
        else e = so_wsa_errno(w, 0);
    }
    if (temp) so_nonblock(o, 0);
    int stream = o->type == SOCK_STREAM;
    so_put(o);
    if (e) { so_fail(c, e); return; }
    if (addr && lenp) M32(lenp) = stream ? 0 : so_addr_out(addr, M32(lenp), &a);     /* [suyu] RecvFromImpl: no address for a stream socket */
    RETI(n);
}
static void socket_Recv(Ctx *c) { so_recv(c, 0, 0); }
static void socket_RecvFrom(Ctx *c) { if (!A4 || !A5 || !M32(A5)) so_recv(c, 0, 0); else so_recv(c, A4, A5); }
static void so_send(Ctx *c, uint64_t addr) {              /* (fd, buf, size, flags [, const sockaddr *, len]) */
    uint64_t buf = A1, size = A2;
    int gf = (int)A3, flags = 0;
    so_need_init(c, "nn::socket::Send");
    if (!size) { RETI(0); return; }
    if (!buf) { so_fail(c, 22); return; }
    if (size >> 32) { so_fail(c, 14); return; }
    SoObj *o = so_get((int)A0, &flags);
    if (!o) { so_fail(c, 9); return; }
    int temp = (gf & 0x80) && !(flags & SO_O_NONBLOCK), n = -1, e = 0;
    struct sockaddr_in a;
    if (addr && !so_addr_in(addr, &a)) e = 97;
    else if (o->type == SOCK_DGRAM && ((addr && ldn_is_virtual(a.sin_addr.S_un.S_addr)) || (!addr && o->vpeer))) {
        if (!addr) { memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.S_un.S_addr = o->vpeer; a.sin_port = o->vpeer_port; }
        e = ldn_so_ensure_port(o);                        /* local wireless: through the room server (ldn_online.inc) */
        if (!e) { n = ldn_so_sendto(o->vport, &a, (const void *)(uintptr_t)buf, (int)(size > 0xffff ? 0xffff : size)); if (n < 0) e = 101; }
    }
    else if (addr && !so_is_loopback(a.sin_addr.S_un.S_addr)) e = 101;                 /* no network */
    else {
        if (temp) so_nonblock(o, 1);
        if (addr) n = sendto(o->s, (const char *)(uintptr_t)buf, (int)(size > 0x7fffffff ? 0x7fffffff : size), so_host_flags(gf) & MSG_OOB, (struct sockaddr *)&a, sizeof a);
        else n = send(o->s, (const char *)(uintptr_t)buf, (int)(size > 0x7fffffff ? 0x7fffffff : size), so_host_flags(gf) & MSG_OOB);
        if (n < 0) e = so_wsa_errno(WSAGetLastError(), 1);
        if (temp) so_nonblock(o, 0);
    }
    so_put(o);
    if (e) so_fail(c, e); else RETI(n);
}
static void socket_Send(Ctx *c) { so_send(c, 0); }
static void socket_SendTo(Ctx *c) { if (!A4 || !(uint32_t)A5) so_send(c, 0); else so_send(c, A4); }
static void socket_Fcntl(Ctx *c) {                        /* (fd, cmd, int) */
    int cmd = (int)A1, arg = (int)A2, fd = (int)A0, flags = 0;
    so_need_init(c, "nn::socket::Fcntl");
    SoObj *o = so_get(fd, &flags);
    if (!o) { so_fail(c, 9); return; }
    if (cmd == 3) RETI(flags);
    else if (cmd == 4) {
        so_nonblock(o, (arg & SO_O_NONBLOCK) != 0);
        AcquireSRWLockExclusive(&g_so_lock);
        if (g_so_fd[fd].o == o) g_so_fd[fd].flags = arg;
        ReleaseSRWLockExclusive(&g_so_lock);
        RETI(0);
    } else so_fail(c, 22);
    so_put(o);
}
static void socket_Poll(Ctx *c) {                         /* (pollfd *, nfds, timeout ms) */
    uint64_t fds = A0, n = A1;
    int timeout = (int)A2;
    so_need_init(c, "nn::socket::Poll");
    if (!fds && n) { so_fail(c, 22); return; }
    if (n >> 29) { so_fail(c, 14); return; }
    if ((int64_t)n <= 0) { so_set_errno(c, 0); RETI(-1); return; }                    /* [suyu] PollImpl: no entries -> -1 with errno 0 */
    if (timeout < -1) { so_fail(c, 22); return; }
    WSAPOLLFD *h = calloc((size_t)n, sizeof *h);
    SoObj **objs = calloc((size_t)n, sizeof *objs);
    int ready = 0, hn = 0, r = 0, e = 0;
    for (uint64_t i = 0; i < n; i++) {
        int fd = (int)M32(fds + i * 8);
        uint16_t ev = M16(fds + i * 8 + 4);
        M16(fds + i * 8 + 6) = 0;
        if (fd < 0) continue;                             /* a negative descriptor is ignored */
        objs[i] = so_get(fd, NULL);
        if (!objs[i]) { M16(fds + i * 8 + 6) = 0x20; ready++; continue; }
        h[hn].fd = objs[i]->s;
        h[hn].events = (SHORT)((ev & 0x41 ? POLLRDNORM : 0) | (ev & 0x82 ? POLLRDBAND : 0) | (ev & 0x4 ? POLLWRNORM : 0));
        hn++;
    }
    if (hn) {
        r = WSAPoll(h, (ULONG)hn, ready ? 0 : timeout);
        if (r < 0) e = so_wsa_errno(WSAGetLastError(), 0);
    } else if (!ready && timeout) Sleep(timeout < 0 ? INFINITE : (DWORD)timeout);
    hn = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (!objs[i]) continue;
        if (!e) {
            SHORT rv = h[hn].revents;
            uint16_t ev = M16(fds + i * 8 + 4), out = 0;
            if (rv & POLLRDNORM) out |= (uint16_t)(ev & 0x41 ? ev & 0x41 : 1);
            if (rv & POLLRDBAND) out |= (uint16_t)(ev & 0x82 ? ev & 0x82 : 2);
            if (rv & POLLWRNORM) out |= 4;
            if (rv & POLLERR) out |= 8;
            if (rv & POLLHUP) out |= 0x10;
            if (rv & POLLNVAL) out |= 0x20;
            M16(fds + i * 8 + 6) = out;
            if (out) ready++;
        }
        hn++;
        so_put(objs[i]);
    }
    free(h); free(objs);
    if (e) so_fail(c, e); else RETI(ready);
}
static void socket_GetSockOpt(Ctx *c) {                   /* (fd, level, name, void *, socklen_t *) */
    uint64_t val = A3, lenp = A4;
    int level = (int)A1, name = (int)A2;
    so_need_init(c, "nn::socket::GetSockOpt");
    if (!val || !lenp || !M32(lenp)) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = 0;
    uint32_t cap = M32(lenp);
    if (level == 0xffff || (level == 6 && name == 1)) {
        char tmp[64];
        int tl = sizeof tmp;
        if (name == 0x800 && level == 0xffff) { memset(tmp, 0, 4); tl = 4; }          /* SO_NOSIGPIPE: there are no signals */
        else if (getsockopt(o->s, level == 0xffff ? SOL_SOCKET : IPPROTO_TCP, name, tmp, &tl) != 0) e = so_wsa_errno(WSAGetLastError(), 0);
        if (!e) {
            uint8_t out[16] = {0};
            uint32_t ol = 4;
            if (level == 0xffff && name == 0x1007) { int v = so_wsa_errno(*(int *)tmp, 0); memcpy(out, &v, 4); }
            else if (level == 0xffff && name == 0x80) { LINGER *l = (LINGER *)tmp; int v[2] = {l->l_onoff, l->l_linger}; memcpy(out, v, 8); ol = 8; }
            else if (level == 0xffff && (name == 0x1005 || name == 0x1006)) { DWORD ms = *(DWORD *)tmp; int64_t tv[2] = {ms / 1000, (ms % 1000) * 1000}; memcpy(out, tv, 16); ol = 16; }
            else { int v = tl >= 4 ? *(int *)tmp : (tl >= 1 ? tmp[0] != 0 : 0); memcpy(out, &v, 4); }
            if (ol > cap) ol = cap;
            memcpy((void *)(uintptr_t)val, out, ol);
            M32(lenp) = ol;
        }
    }                                                     /* other levels: [suyu] GetSockOptImpl answers success without data */
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}
static void socket_SetSockOpt(Ctx *c) {                   /* (fd, level, name, const void *, len) */
    uint64_t val = A3;
    uint32_t len = (uint32_t)A4;
    int level = (int)A1, name = (int)A2;
    so_need_init(c, "nn::socket::SetSockOpt");
    if (!val) { so_fail(c, 22); return; }
    SoObj *o = so_get((int)A0, NULL);
    if (!o) { so_fail(c, 9); return; }
    int e = 0, r = 0;
    if (level == 0xffff && name == 0x80) {
        if (len < 8) e = 22;
        else { LINGER l; l.l_onoff = (u_short)(M32(val) != 0); l.l_linger = (u_short)M32(val + 4); r = setsockopt(o->s, SOL_SOCKET, SO_LINGER, (char *)&l, sizeof l); }
    } else if (level == 0xffff && (name == 0x1005 || name == 0x1006)) {
        DWORD ms = len >= 16 ? (DWORD)(M64(val) * 1000 + M64(val + 8) / 1000) : len >= 4 ? M32(val) : 0;
        if (len < 4) e = 22; else r = setsockopt(o->s, SOL_SOCKET, name, (char *)&ms, sizeof ms);
    } else if (level == 0xffff && name == 0x800) r = 0;   /* SO_NOSIGPIPE */
    else if (level == 0xffff || (level == 6 && name == 1)) {
        if (len < 4) e = 22;
        else { int v = (int)M32(val); r = setsockopt(o->s, level == 0xffff ? SOL_SOCKET : IPPROTO_TCP, name, (char *)&v, sizeof v); }
    }                                                     /* other levels: [suyu] SetSockOptImpl accepts and ignores */
    if (!e && r != 0) e = so_wsa_errno(WSAGetLastError(), 0);
    so_put(o);
    if (e) so_fail(c, e); else RETI(0);
}

/* ---------------------------------------------------------------- resolver */
static int so_resolver_enabled(void) { return g_so_init; }
static uint64_t so_option_table(void) { return sdk_image_ready() ? SDK_BASE + 0xc0cd28 : 0; }
static const char *const g_gai_text[] = {
    "Success", "Address family for hostname not supported", "Temporary failure in name resolution", "Invalid value for ai_flags",
    "Non-recoverable failure in name resolution", "ai_family not supported", "Memory allocation failure",
    "No address associated with hostname", "hostname nor servname provided, or not known", "servname not supported for ai_socktype",
    "ai_socktype not supported", "System error returned in errno", "Invalid value for hints", "Resolved protocol is unknown",
    "Argument buffer overflow"};
static void socket_GAIStrError2(Ctx *c) {
    uint32_t e = (uint32_t)A0;
    if (!so_resolver_enabled()) { RET(P("Error string is not available")); return; }
    RET(P(e < sizeof g_gai_text / sizeof g_gai_text[0] ? g_gai_text[e] : "Unknown error"));
}
static int so_gai_from_host(int w) {
    switch (w) {
    case 0: return 0;
    case WSATRY_AGAIN: return 2;
    case WSAEINVAL: return 3;
    case WSANO_RECOVERY: return 4;
    case WSAEAFNOSUPPORT: return 5;
    case WSA_NOT_ENOUGH_MEMORY: return 6;
    case WSAHOST_NOT_FOUND: return 8;
    case WSATYPE_NOT_FOUND: return 9;
    case WSAESOCKTNOSUPPORT: return 10;
    default: return 15;                                   /* [suyu] GetAddrInfoError::OTHER */
    }
}
static int so_is_numeric_host(const char *s) { struct in_addr a; return s && InetPtonA(AF_INET, s, &a) == 1; }
/* (node, service, hints, out). nn_layout: nn::socket::AddrInfo instead of struct addrinfo */
static void so_getaddrinfo(Ctx *c, int nn_layout) {
    const char *node = (const char *)(uintptr_t)A0, *service = (const char *)(uintptr_t)A1;
    uint64_t hints = A2, out = A3;
    if (!so_resolver_enabled()) { RETI(11); return; }
    if (!node && !service) { RETI(8); return; }
    if (!out) { so_set_errno(c, 22); RETI(11); return; }
    M64(out) = 0;
    if (node && !so_is_numeric_host(node)) {              /* needs DNS: no network. [suyu] sfdnsres.cpp: EAI_AGAIN, TRY_AGAIN, errno 0 */
        t_so_herrno = 2; so_set_errno(c, 0); RETI(2); return;
    }
    struct addrinfo h, *res = NULL;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_flags = AI_NUMERICHOST | (hints && (M32(hints) & 1) ? AI_PASSIVE : 0);
    if (hints) { h.ai_socktype = (int)M32(hints + 8); h.ai_protocol = (int)M32(hints + 0xc); }
    int w = getaddrinfo(node, service, &h, &res), g = so_gai_from_host(w);
    if (g) {
        t_so_herrno = g == 2 ? 2 : g == 9 ? 0 : 1;        /* [suyu] GetAddrInfoErrorToNetDbError / ...ToErrno */
        so_set_errno(c, g == 9 ? 22 : 0);
        RETI(g); return;
    }
    uint64_t first = 0, prev = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        if (p->ai_family != AF_INET) continue;
        uint64_t ai = P(calloc(1, 0x30)), sa = P(calloc(1, 16)), name = P(calloc(1, 1));
        struct sockaddr_in *in = (struct sockaddr_in *)p->ai_addr;
        if (nn_layout) M8(ai + 4) = 2; else M32(ai + 4) = 2;
        M32(ai + 8) = (uint32_t)p->ai_socktype; M32(ai + 0xc) = (uint32_t)p->ai_protocol; M32(ai + 0x10) = 16;
        so_addr_out(sa, 16, in);
        M8(sa) = 0;                                       /* [sdk 0x36e7e0] the deserialiser does not set sin_len */
        M64(ai + (nn_layout ? 0x18 : 0x20)) = sa;
        M64(ai + (nn_layout ? 0x20 : 0x18)) = name;
        if (prev) M64(prev + 0x28) = ai; else first = ai;
        prev = ai;
    }
    freeaddrinfo(res);
    if (!first) { t_so_herrno = 1; so_set_errno(c, 0); RETI(8); return; }
    t_so_herrno = 0; so_set_errno(c, 0);
    M64(out) = first;
    RETI(0);
}
static void socket_GetAddrInfo2(Ctx *c) { so_getaddrinfo(c, 0); }
static void socket_GetAddrInfoNn(Ctx *c) { so_getaddrinfo(c, 1); }
static void so_freeaddrinfo(Ctx *c, int nn_layout) {
    uint64_t ai = A0;
    while (ai) {
        uint64_t next = M64(ai + 0x28);
        free((void *)(uintptr_t)M64(ai + 0x18)); free((void *)(uintptr_t)M64(ai + 0x20));
        free((void *)(uintptr_t)ai);
        ai = next;
    }
    (void)nn_layout;
}
static void socket_FreeAddrInfo2(Ctx *c) { so_freeaddrinfo(c, 0); }
static void socket_FreeAddrInfoNn(Ctx *c) { so_freeaddrinfo(c, 1); }
static void socket_GetHostByNameWithOptions(Ctx *c) {     /* (const char *name, const ResolverOption *, size_t count) -> hostent * */
    static __declspec(thread) struct { uint64_t name, aliases; int32_t type, length; uint64_t list; uint64_t alias0; uint64_t addrs[2]; uint32_t addr; char text[64]; } t_he;
    const char *name = (const char *)(uintptr_t)A0;
    struct in_addr a;
    if (!so_resolver_enabled()) { so_set_errno(c, 22); RET(0); return; }
    if (!name) { t_so_herrno = 1; so_set_errno(c, 22); RET(0); return; }
    if (InetPtonA(AF_INET, name, &a) != 1 || strlen(name) >= sizeof t_he.text) { t_so_herrno = 2; so_set_errno(c, 0); RET(0); return; }
    strcpy(t_he.text, name);
    t_he.name = P(t_he.text); t_he.alias0 = 0; t_he.aliases = P(&t_he.alias0);
    t_he.type = 2; t_he.length = 4;
    t_he.addr = a.S_un.S_addr; t_he.addrs[0] = P(&t_he.addr); t_he.addrs[1] = 0; t_he.list = P(t_he.addrs);
    t_so_herrno = 0; so_set_errno(c, 0);
    RET(P(&t_he));
}
static void so_ResolverGetOption(Ctx *c) {                /* (ResolverOption *out, key) -> int; ResolverOption {u32 key, u32 type, u64 size, value} */
    uint64_t opt = A0, table = so_option_table();
    uint32_t key = (uint32_t)A1;
    if (!opt) { RETI(-1); return; }
    if ((key >> 3) == 0x222e) {
        if (key == 0x11177 || key == 0x11170 || !table) { *so_errno_ptr(c) = 22; RETI(-1); return; }
        M32(opt) = key; M32(opt + 4) = 0x67; M64(opt + 8) = 8; M64(opt + 0x10) = M64(table + (uint64_t)(key - 0x11171) * 8);
        RETI(0); return;
    }
    if (key == 0x2711) {                                  /* [sdk 0x382d50] CreateRandomId */
        uint32_t id = 0;
        HCRYPTPROV prov = 0;
        if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) { CryptGenRandom(prov, 4, (BYTE *)&id); CryptReleaseContext(prov, 0); }
        M32(opt + 0x10) = id; M64(opt + 8) = 4;
        RETI(0); return;
    }
    if (!so_resolver_enabled()) so_set_errno(c, 22);
    RETI(-1);
}
static void so_ResolverSetOption(Ctx *c) {                /* (const ResolverOption &) -> int */
    uint64_t opt = A0, table = so_option_table();
    uint32_t idx = M32(opt) - 0x11171u;
    if (idx <= 5 && M32(opt + 4) == 0x67 && M64(opt + 8) == 8 && table) { M64(table + (uint64_t)idx * 8) = M64(opt + 0x10); RETI(0); return; }
    if (!so_resolver_enabled()) { so_set_errno(c, 22); RETI(-1); return; }
    RETI(0);
}
static void nnsocket_ResolverGetOption(Ctx *c) { if (!A0) { *so_errno_ptr(c) = 22; RETI(-1); } else so_ResolverGetOption(c); }   /* [sdk 0x36b950] */
static void nnsocket_ResolverSetOption(Ctx *c) { if (!A0) { *so_errno_ptr(c) = 22; RETI(-1); } else so_ResolverSetOption(c); }   /* [sdk 0x36b980] */

/* ================================================================== nn::ssl */
#define SSL_NOT_INIT 0xca7bu
#define SSL_BAD_CONTEXT 0xdc7bu
#define SSL_BAD_CONNECTION 0xda7bu
#define SSL_BAD_POINTER 0xe27bu
typedef struct SslCtx { uint32_t magic; int connections; } SslCtx;
typedef struct SslConn { uint32_t magic; SslCtx *ctx; int fd; int keep_socket; } SslConn;      /* fd: the module's duplicate, -1 = none */
#define SSL_CTX_MAGIC 0x43584c53u
#define SSL_CONN_MAGIC 0x4e434c53u
static uint64_t g_ssl_count;                              /* [sdk] .bss 0xcd10a8 */
static int g_ssl_session;
static uint64_t ssl_scramble(uint64_t p) { return (p >> 48) | ((p >> 16) & 0xffff0000ull) | ((p << 16) & 0xffff00000000ull) | (p << 48); }
static void *ssl_chunk_object(uint64_t scrambled) {       /* the service object behind a context / connection value, or NULL */
    uint64_t chunk = ssl_scramble(scrambled);
    return chunk ? (void *)(uintptr_t)M64(chunk) : NULL;
}
static void nnssl_Initialize(Ctx *c) {
    int first = g_ssl_count == 0;
    g_ssl_count++;
    if (!first || g_ssl_session) { RET(0x1927bu); return; }
    g_ssl_session = 1;
    RET(0);
}
static void nnssl_Finalize(Ctx *c) {
    if (g_ssl_count == 0) { RET(SSL_NOT_INIT); return; }
    if (--g_ssl_count != 0) { RET(0); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    g_ssl_session = 0;
    RET(0);
}
static void ssl_Context_Create(Ctx *c) {                  /* (this, SslVersion) */
    uint64_t self = A0;
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    uint64_t *chunk = calloc(1, 8);
    SslCtx *x = calloc(1, sizeof *x);
    x->magic = SSL_CTX_MAGIC;
    *chunk = P(x);
    M64(self) = ssl_scramble(P(chunk));
    RET(0);
}
static SslCtx *ssl_ctx(uint64_t value) { SslCtx *x = ssl_chunk_object(value); return x && x->magic == SSL_CTX_MAGIC ? x : NULL; }
static SslConn *ssl_conn(uint64_t value) { SslConn *x = ssl_chunk_object(value); return x && x->magic == SSL_CONN_MAGIC ? x : NULL; }
static void ssl_Context_ImportServerPki(Ctx *c) {         /* (this, uint64_t *outId, const char *data, uint32_t size, format) */
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    if (!A1 || !A2) { RET(SSL_BAD_POINTER); return; }
    if (!ssl_ctx(M64(A0))) { RET(SSL_BAD_CONTEXT); return; }
    M64(A1) = 0;
    RET(0);
}
static void nnssl_ContextDestroy(Ctx *c) {
    uint64_t self = A0;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    SslCtx *x = ssl_ctx(M64(self));
    if (!x) { RET(SSL_BAD_CONTEXT); return; }
    if (x->connections) { RET(0xd67bu); return; }
    free((void *)(uintptr_t)ssl_scramble(M64(self)));
    x->magic = 0; free(x);
    M64(self) = 0;
    RET(0);
}
static void nnssl_ContextGetContextId(Ctx *c) {
    if (!A0 || !A1) { RET(SSL_BAD_POINTER); return; }
    M64(A1) = M64(A0);
    RET(0);
}
static void nnssl_ConnectionCreate(Ctx *c) {              /* (connection, context) */
    uint64_t self = A0, ctx = A1;
    if (!self || !ctx) { RET(SSL_BAD_POINTER); return; }
    M64(self) = 0; M64(self + 8) = 0; M32(self + 0x10) = 0xffffffffu;                 /* [sdk 0x383c70] constructor */
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    SslCtx *x = ssl_ctx(M64(ctx));
    if (!x) { RET(SSL_BAD_CONTEXT); return; }
    uint64_t *chunk = calloc(1, 8);
    SslConn *n = calloc(1, sizeof *n);
    n->magic = SSL_CONN_MAGIC; n->ctx = x; n->fd = -1;
    x->connections++;
    *chunk = P(n);
    M64(self) = ssl_scramble(P(chunk));
    M64(self + 8) = M64(ctx);
    RET(0);
}
/* common front of every Connection call: null object, not initialised, invalid connection */
static SslConn *ssl_conn_enter(Ctx *c, uint64_t self) {
    if (!self) { RET(SSL_BAD_POINTER); return NULL; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return NULL; }
    SslConn *n = ssl_conn(M64(self));
    if (!n) RET(SSL_BAD_CONNECTION);
    return n;
}
static void nnssl_ConnectionDestroy(Ctx *c) {
    uint64_t self = A0;
    SslConn *n = ssl_conn_enter(c, self);
    if (!n) return;
    if ((int32_t)M32(self + 0x10) >= 0) { so_remove((int)M32(self + 0x10)); M32(self + 0x10) = 0xffffffffu; }   /* [sdk 0x383e58] nn::socket::Close */
    if (n->fd >= 0 && n->fd != (int)M32(self + 0x10)) so_remove(n->fd);              /* the module closes its duplicate */
    n->ctx->connections--;
    free((void *)(uintptr_t)ssl_scramble(M64(self)));
    n->magic = 0; free(n);
    M64(self) = 0;
    RET(0);
}
static void nnssl_ConnectionSetSocketDescriptor(Ctx *c) { /* (connection, fd) */
    uint64_t self = A0;
    SslConn *n = ssl_conn_enter(c, self);
    if (!n) return;
    int d = g_so_init ? so_dup((int)A1) : -1;
    if (d < 0) { RET(0xd47bu); return; }
    n->fd = d;
    M32(self + 0x10) = n->keep_socket ? (uint32_t)d : 0xffffffffu;
    RET(0);
}
static void nnssl_ConnectionSetHostName(Ctx *c) {         /* (connection, const char *, uint32_t) */
    if (!A0) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    if (!A1) { RET(SSL_BAD_POINTER); return; }
    if (!ssl_conn(M64(A0))) { RET(SSL_BAD_CONNECTION); return; }
    RET(0);
}
static void nnssl_ConnectionSimpleOk(Ctx *c) { if (ssl_conn_enter(c, A0)) RET(0); }   /* SetVerifyOption, SetSessionCacheMode */
static void nnssl_ConnectionSetIoMode(Ctx *c) {           /* (connection, IoMode: 1 blocking, 2 non-blocking) */
    SslConn *n = ssl_conn_enter(c, A0);
    if (!n) return;
    if (n->fd < 0) { RET(0xce7bu); return; }
    SoObj *o = so_get(n->fd, NULL);
    if (o) { so_nonblock(o, (uint32_t)A1 == 2); so_put(o); }
    RET(0);
}
static void nnssl_ConnectionSetOption(Ctx *c) {           /* (connection, OptionType, bool) */
    SslConn *n = ssl_conn_enter(c, A0);
    if (!n) return;
    if ((uint32_t)A1 == 0) n->keep_socket = (int)(A2 & 1);                             /* [suyu] OptionType::DoNotCloseSocket = 0 */
    RET(0);
}
static void ssl_needs_tls(Ctx *c, const char *what) {
    printf("\n%s on an nn::ssl connection needs a TLS session over a connected socket. This port has no TLS implementation\n"
           "(without a network only a loopback peer could be connected at all).\n", what);
    guest_trap("nn::ssl: TLS is not implemented", c->x[30] - 4);
}
static void nnssl_ConnectionDoHandshake(Ctx *c) {         /* (connection, uint32_t *outCertSize, uint32_t *outCertCount) */
    uint64_t self = A0, osize = A1, ocount = A2;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) { RET(SSL_NOT_INIT); return; }
    if (!ssl_conn(M64(self))) { RET(SSL_BAD_CONNECTION); return; }
    if (ssl_conn(M64(self))->fd >= 0) ssl_needs_tls(c, "DoHandshake");
    if (!(M64(self + 0x18) && M32(self + 0x20))) {        /* [sdk 0x384be0..0x384c40] no certificate buffer: plain DoHandshake, outputs zeroed */
        if (osize) M32(osize) = 0;
        if (ocount) M32(ocount) = 0;
    }
    RET(0xce7bu);                                         /* [suyu] ResultNoSocket */
}
static void ssl_io(Ctx *c, const char *what) {            /* (connection, buffer, int *outSize, uint32_t size) */
    uint64_t self = A0;
    uint32_t r;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) r = SSL_NOT_INIT;
    else if (!A1 || !A2) r = SSL_BAD_POINTER;
    else if (!(uint32_t)A3) r = 0xe07bu;
    else if (!ssl_conn(M64(self))) r = SSL_BAD_CONNECTION;
    else { ssl_needs_tls(c, what); r = 0; }
    M32(self + 0x24) = r;
    RET(r);
}
static void nnssl_ConnectionRead(Ctx *c) { ssl_io(c, "Read"); }
static void nnssl_ConnectionWrite(Ctx *c) { ssl_io(c, "Write"); }
static void nnssl_ConnectionPending(Ctx *c) {             /* (connection, int *out) */
    uint64_t self = A0;
    uint32_t r;
    if (!self) { RET(SSL_BAD_POINTER); return; }
    if (!g_ssl_session) r = SSL_NOT_INIT;
    else if (!ssl_conn(M64(self))) r = SSL_BAD_CONNECTION;
    else if (!A1) r = SSL_BAD_POINTER;
    else { M32(A1) = 0; r = 0; }
    M32(self + 0x24) = r;
    RET(r);
}
static void nnssl_ConnectionGetVerifyCertError(Ctx *c) {  /* (connection, Result *out) */
    if (!ssl_conn_enter(c, A0)) return;
    if (!A1) { RET(SSL_BAD_POINTER); return; }
    ssl_needs_tls(c, "GetVerifyCertError");
}
static void nnssl_ConnectionGetConnectionId(Ctx *c) {
    if (!A0 || !A1) { RET(SSL_BAD_POINTER); return; }
    M64(A1) = M64(A0);
    RET(0);
}

/* ================================================================== nn::nsd */
static void nsd_ResolveEx(Ctx *c) {                       /* (Fqdn *out, const Fqdn &in), Fqdn = char[0x100] */
    char tmp[0x100];
    memcpy(tmp, (const void *)(uintptr_t)A1, sizeof tmp);
    tmp[sizeof tmp - 1] = 0;
    memset((void *)(uintptr_t)A0, 0, 0x100);
    strcpy((char *)(uintptr_t)A0, tmp);
    RET(0);
}


/* ================================================================== nn::ec::ShopServiceAccessor */
static uint64_t g_ec_list[2];                             /* the proxy's list of accessors ([sdk] proxy + 0x70) */
static int g_ec_ready;
static uint8_t g_ec_service_token[16];
static void ec_os(Ctx *c, const char *name, uint64_t x0) { uint64_t k1 = c->x[1], k2 = c->x[2]; c4a_call(c, name, x0); c->x[1] = k1; c->x[2] = k2; }
static void ec_unlink(uint64_t node, uint64_t head) {     /* [sdk 0x192b74] / [sdk 0x18ded0] */
    uint64_t next = M64(node + 8);
    if (next == node || head == node) return;
    uint64_t prev = M64(node), nn = M64(next);
    (void)nn;
    M64(prev + 8) = next;
    M64(next) = M64(node);
    M64(node + 8) = node;
    M64(node) = node;
}
static void ec_InitializeForAccessors(Ctx *c) {
    if (!g_ec_list[0]) { g_ec_list[0] = P(g_ec_list); g_ec_list[1] = P(g_ec_list); }
    g_ec_ready = 1;
    RET(0);
}
static void ec_Accessor_ctor(Ctx *c) {
    uint64_t self = A0;
    M64(self) = self; M64(self + 8) = self; M64(self + 0x10) = 0;
    c->x[1] = 1; c->x[2] = 0;
    c4a_call(c, "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi", self + 0x18);
    M64(self + 0x38) = self + 0x38; M64(self + 0x40) = self + 0x38;
    RET(self);
}
static void ec_response_detach(Ctx *c, uint64_t r) {      /* [sdk 0x18de50]; no service is ever attached (@0x18 stays null) */
    uint64_t acc = M64(r + 0x10);
    if (M64(r + 0x18)) guest_trap("nn::ec AsyncResponse: a request is attached, which this port never does", c->x[30] - 4);
    if (acc) {
        ec_os(c, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", acc + 0x18);
        ec_unlink(r, acc + 0x38);
        ec_os(c, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", acc + 0x18);
    }
    M64(r + 0x10) = 0;
}
static uint32_t ec_accessor_finalize(Ctx *c, uint64_t self) {
    ec_os(c, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", self + 0x18);
    if (M64(self + 0x10)) {
        while (M64(self + 0x40) != self + 0x38) ec_response_detach(c, M64(self + 0x40));
        M64(self + 0x10) = 0;
        ec_os(c, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", self + 0x18);
        ec_unlink(self, P(g_ec_list));
    } else ec_os(c, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", self + 0x18);
    return 0;
}
static void ec_FinalizeForAccessors(Ctx *c) {
    if (g_ec_ready) {
        while (g_ec_list[1] != P(g_ec_list)) ec_accessor_finalize(c, g_ec_list[1]);
        g_ec_ready = 0;
    }
    RET(0);
}
static void ec_Accessor_dtor(Ctx *c) {
    uint64_t self = A0;
    ec_accessor_finalize(c, self);
    c4a_call(c, "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE", self + 0x18);
    RET(self);
}
static void ec_Accessor_Initialize(Ctx *c) {              /* (this, const ShopService &) */
    uint64_t self = A0;
    if (!g_ec_ready) {
        printf("\nnn::ec::ShopServiceAccessor::Initialize before nn::ec::InitializeForShopServiceAccessors: the SDK has no proxy (null pointer) here.\n");
        guest_trap("nn::ec::ShopServiceAccessor::Initialize without the proxy", c->x[30] - 4);
    }
    ec_os(c, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", self + 0x18);
    M64(self + 0x10) = P(g_ec_service_token);
    ec_os(c, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", self + 0x18);
    {                                                     /* [sdk 0x192b14] Proxy::Attach */
        uint64_t head = P(g_ec_list), x8 = M64(self), x9 = M64(head);
        M64(self) = x9;
        M64(x8 + 8) = head;
        x9 = M64(head);
        M64(x9 + 8) = self;
        M64(head) = x8;
    }
    RET(0);
}
static void ec_Accessor_Request(Ctx *c) { RET(0x70a4u); } /* IsAnyInternetRequestAccepted is false */
static void ec_AsyncResponse_dtor(Ctx *c) {
    uint64_t self = A0;
    ec_response_detach(c, self);
    if (M8(self + 0x48)) c4a_call(c, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE", self + 0x20);
    RET(self);
}
static void ec_AsyncResponse_GetSize2(Ctx *c) {           /* (this, size_t *out): nothing attached */
    if (M64(A0 + 0x18)) guest_trap("nn::ec AsyncResponse::GetSize: a request is attached, which this port never does", c->x[30] - 4);
    RET(EC_NO_RESPONSE);
}

/* ================================================================== registration */
#define SOK(n) "_ZN2nn6socket" n
#define LDNK(n) "_ZN2nn3ldn" n
static void c4a_register(void) {
    static const ImportDef defs[] = {
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_GetCurrentPrimaryIpAddress},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_GetCurrentIpConfigInfo},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_GetCurrentProxySetting},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_HandleNetworkRequestErrorResult},
        {SOK("10InitializeEPvmmi"), socket_Initialize}, {SOK("8FinalizeEv"), socket_Finalize},
        {SOK("12GetLastErrnoEv"), socket_GetLastErrno2}, {"nnsocketGetLastErrno", socket_GetLastErrno2}, {"nnsocketSetLastErrno", socket_SetLastErrno2},
        {SOK("6SocketEiii"), socket_Socket}, {SOK("6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE"), socket_Socket}, {"nnsocketSocket", socket_Socket},
        {SOK("5CloseEi"), socket_Close}, {"nnsocketClose", socket_Close},
        {SOK("4BindEiPK8sockaddrj"), socket_Bind}, {SOK("4BindEiPKNS0_8SockAddrEj"), socket_BindSockAddr}, {"nnsocketBind", socket_Bind},
        {SOK("7ConnectEiPK8sockaddrj"), socket_Connect}, {"nnsocketConnect", socket_Connect},
        {SOK("6ListenEii"), socket_Listen}, {SOK("8ShutdownEii"), socket_Shutdown}, {SOK("6AcceptEiP8sockaddrPj"), socket_Accept},
        {SOK("11GetSockNameEiP8sockaddrPj"), socket_GetSockName}, {"nnsocketGetSockName", socket_GetSockName}, {"nnsocketGetPeerName", socket_GetPeerName},
        {SOK("4RecvEiPvmi"), socket_Recv}, {"nnsocketRecv", socket_Recv},
        {SOK("8RecvFromEiPvmiP8sockaddrPj"), socket_RecvFrom}, {SOK("8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj"), socket_RecvFrom},
        {SOK("4SendEiPKvmi"), socket_Send}, {"nnsocketSend", socket_Send},
        {SOK("6SendToEiPKvmiPK8sockaddrj"), socket_SendTo}, {SOK("6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj"), socket_SendTo},
        {SOK("5FcntlEiiz"), socket_Fcntl}, {"nnsocketFcntl", socket_Fcntl},
        {SOK("4PollEP6pollfdmi"), socket_Poll}, {"nnsocketPoll", socket_Poll},
        {SOK("10GetSockOptEiiiPvPj"), socket_GetSockOpt}, {"nnsocketGetSockOpt", socket_GetSockOpt},
        {SOK("10SetSockOptEiiiPKvj"), socket_SetSockOpt}, {SOK("10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj"), socket_SetSockOpt}, {"nnsocketSetSockOpt", socket_SetSockOpt},
        {SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), socket_GetAddrInfo2}, {SOK("11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_"), socket_GetAddrInfoNn},
        {SOK("12FreeAddrInfoEP8addrinfo"), socket_FreeAddrInfo2}, {SOK("12FreeAddrInfoEPNS0_8AddrInfoE"), socket_FreeAddrInfoNn},
        {SOK("11GAIStrErrorENS0_7AiErrnoE"), socket_GAIStrError2},
        {"nnsocketGetHostByNameWithOptions", socket_GetHostByNameWithOptions},
        {"nnsocketResolverGetOption", nnsocket_ResolverGetOption}, {"nnsocketResolverSetOption", nnsocket_ResolverSetOption},
        {"nnsslInitialize", nnssl_Initialize}, {"nnsslFinalize", nnssl_Finalize},
        {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_Context_Create},
        {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_Context_ImportServerPki},
        {"nnsslContextDestroy", nnssl_ContextDestroy}, {"nnsslContextGetContextId", nnssl_ContextGetContextId},
        {"nnsslConnectionCreate", nnssl_ConnectionCreate}, {"nnsslConnectionDestroy", nnssl_ConnectionDestroy},
        {"nnsslConnectionSetSocketDescriptor", nnssl_ConnectionSetSocketDescriptor}, {"nnsslConnectionSetHostName", nnssl_ConnectionSetHostName},
        {"nnsslConnectionSetVerifyOption", nnssl_ConnectionSimpleOk}, {"nnsslConnectionSetSessionCacheMode", nnssl_ConnectionSimpleOk},
        {"nnsslConnectionSetIoMode", nnssl_ConnectionSetIoMode}, {"nnsslConnectionSetOption", nnssl_ConnectionSetOption},
        {"nnsslConnectionDoHandshakeWithCertBuffer", nnssl_ConnectionDoHandshake},
        {"nnsslConnectionRead", nnssl_ConnectionRead}, {"nnsslConnectionWrite", nnssl_ConnectionWrite},
        {"nnsslConnectionPending", nnssl_ConnectionPending}, {"nnsslConnectionGetVerifyCertError", nnssl_ConnectionGetVerifyCertError},
        {"nnsslConnectionGetConnectionId", nnssl_ConnectionGetConnectionId},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_ResolveEx},
        {LDNK("10InitializeEv"), ldn_Initialize}, {LDNK("8GetStateEv"), ldn_GetState},
        {LDNK("8FinalizeEv"), ldn_Finalize}, {LDNK("22AttachStateChangeEventEPNS_2os15SystemEventTypeE"), ldn_AttachStateChangeEvent},
        {LDNK("32SetWirelessControllerRestrictionENS0_29WirelessControllerRestrictionE"), ldn_SetWirelessControllerRestriction},
        {LDNK("11OpenStationEv"), ldn_OpenStation}, {LDNK("12CloseStationEv"), ldn_CloseStation}, {LDNK("15OpenAccessPointEv"), ldn_OpenAccessPoint},
        {LDNK("16CloseAccessPointEv"), ldn_CloseAccessPoint}, {LDNK("10DisconnectEv"), ldn_Disconnect}, {LDNK("14DestroyNetworkEv"), ldn_DestroyNetwork},
        {LDNK("14GetNetworkInfoEPNS0_11NetworkInfoE"), ldn_GetNetworkInfo}, {LDNK("16GetNetworkConfigEPNS0_13NetworkConfigE"), ldn_GetNetworkConfig},
        {LDNK("14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE"), ldn_GetIpv4Address},
        {LDNK("20GetSecurityParameterEPNS0_17SecurityParameterE"), ldn_GetSecurityParameter}, {LDNK("16SetAdvertiseDataEPKvm"), ldn_SetAdvertiseData},
        {LDNK("22SetStationAcceptPolicyENS0_12AcceptPolicyE"), ldn_SetStationAcceptPolicy}, {LDNK("6RejectENS0_11Ipv4AddressE"), ldn_Reject},
        {LDNK("4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi"), ldn_Scan},
        {LDNK("11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi"), ldn_ScanPrivate},
        {LDNK("13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE"), ldn_CreateNetwork},
        {LDNK("20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE"), ldn_CreateNetworkPrivate},
        {LDNK("7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE"), ldn_Connect},
        {LDNK("14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE"), ldn_ConnectPrivate},
        {LDNK("19GetDisconnectReasonEv"), ldn_GetDisconnectReason},
        {"_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", ec_InitializeForAccessors}, {"_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", ec_FinalizeForAccessors},
        {"_ZN2nn2ec19ShopServiceAccessorC1Ev", ec_Accessor_ctor}, {"_ZN2nn2ec19ShopServiceAccessorD1Ev", ec_Accessor_dtor},
        {"_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", ec_Accessor_Initialize},
        {"_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", ec_Accessor_Request},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponseD1Ev", ec_AsyncResponse_dtor},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", ec_AsyncResponse_GetSize2},
    };
    int n = (int)(sizeof defs / sizeof defs[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(defs[i].name)) printf("nnnet: \"%s\" is not an import of the game (typo?)\n", defs[i].name);
    g_nifm_initialize_prev = import_lookup("_ZN2nn4nifm10InitializeEv");
    register_imports(defs, n);
    {
        static const ImportDef wrap = {"_ZN2nn4nifm10InitializeEv", nifm_Initialize};
        if (g_nifm_initialize_prev != nifm_Initialize) register_imports(&wrap, 1);
    }
}

/* ================================================================== self-test */
static uint64_t c4a_h1(Ctx *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5;
    h(name)(c);
    return c->x[0];
}
#define H(name, ...) c4a_h6(c, name, (uint64_t[6]){__VA_ARGS__})
static uint64_t c4a_h6(Ctx *c, const char *name, const uint64_t *a) { return c4a_h1(c, name, a[0], a[1], a[2], a[3], a[4], a[5]); }
static int c4a_errno(Ctx *c) { return (int)H(SOK("12GetLastErrnoEv"), 0); }
static void c4a_sa(uint8_t *sa, uint8_t a, uint8_t b, uint8_t cc, uint8_t d, uint16_t port) {
    memset(sa, 0, 16); sa[0] = 16; sa[1] = 2; sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port; sa[4] = a; sa[5] = b; sa[6] = cc; sa[7] = d;
}
#define C4A_CHECK(text) do { printf("  %s: %s\n", text, ok ? "ok" : "FAILED"); bad += !ok; cases++; } while (0)
static int c4a_selftest(Ctx *c) {
    int bad = 0, ok, cases = 0;
    /* ---- nifm */
    uint32_t ip[5] = {0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee};
    uint8_t proxy[0xb0];
    int was = g_nifm_init;
    g_nifm_init = 0;
    ok = H("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", P(ip)) == 0xca6e && ip[0] == 0xeeeeeeee;
    ok = ok && H("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", P(proxy)) == 0xca6e;
    ok = ok && H("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", 0) == 0;
    C4A_CHECK("nifm: before Initialize 0xca6e (110-101), nothing written");
    ok = H("_ZN2nn4nifm10InitializeEv", 0) == 0 && g_nifm_init == 1;
    ok = ok && H("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", P(ip)) == 0 && ip[0] == 0;
    ok = ok && H("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", P(ip), P(ip + 1), P(ip + 2), P(ip + 3), P(ip + 4)) == 0 && !ip[1] && !ip[2] && !ip[3] && !ip[4];
    memset(proxy, 0xee, sizeof proxy);
    ok = ok && H("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", P(proxy)) == 0 && proxy[0] == 0 && proxy[0xa9] == 0 && proxy[0xaa] == 0xee;
    ok = ok && H("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", 0) == 0;
    C4A_CHECK("nifm: no connection: address 0.0.0.0, config zero, proxy setting 0xaa zero bytes, error result not handled");
    g_nifm_init = was;
    /* ---- sockets */
    uint8_t *pool = VirtualAlloc(NULL, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t sa[16], sb[16], buf[64];
    uint32_t len;
    ok = (int)H(SOK("12GetLastErrnoEv"), 0) == 22 && !strcmp((const char *)(uintptr_t)H(SOK("11GAIStrErrorENS0_7AiErrnoE"), 2), "Error string is not available");
    uint64_t list = 0x1234;
    ok = ok && (int)H(SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), P("127.0.0.1"), 0, 0, P(&list)) == 11 && list == 0x1234;
    ok = ok && H("nnsocketGetHostByNameWithOptions", P("127.0.0.1"), 0, 0) == 0;
    C4A_CHECK("socket: before Initialize: GetLastErrno 22, GAIStrError \"not available\", GetAddrInfo 11, GetHostByName null");
    ok = H(SOK("10InitializeEPvmmi"), P(pool), 0x90000, 0x20000, 4) == 0x21b && !g_so_init;
    ok = ok && H(SOK("10InitializeEPvmmi"), P(pool), 0x100000, 0x20000, 4) == 0 && g_so_init;
    C4A_CHECK("socket: Initialize: pool below 0xa0000 -> 0x21b, then success");
    int u1 = (int)H(SOK("6SocketEiii"), 2, 2, 0), u2 = (int)H("nnsocketSocket", 2, 2, 17);
    ok = u1 == 0 && u2 == 1;
    ok = ok && (int)H(SOK("6SocketEiii"), 10, 1, 0) == -1 && c4a_errno(c) == 97;
    C4A_CHECK("socket: two UDP sockets get descriptors 0 and 1; AF_INET6 refused with EAFNOSUPPORT");
    c4a_sa(sa, 127, 0, 0, 1, 0);
    ok = (int)H(SOK("4BindEiPK8sockaddrj"), u1, P(sa), 16) == 0;
    len = 16; memset(sb, 0xee, 16);
    ok = ok && (int)H(SOK("11GetSockNameEiP8sockaddrPj"), u1, P(sb), P(&len)) == 0 && len == 16 && sb[1] == 2 && sb[4] == 127 && sb[7] == 1 && (sb[2] | sb[3]);
    uint16_t port = (uint16_t)(sb[2] << 8 | sb[3]);
    c4a_sa(sa, 10, 1, 2, 3, 4000);
    ok = ok && (int)H(SOK("4BindEiPK8sockaddrj"), u2, P(sa), 16) == -1 && c4a_errno(c) == 99;
    ok = ok && (int)H(SOK("4BindEiPK8sockaddrj"), u2, 0, 16) == -1 && c4a_errno(c) == 22;
    ok = ok && (int)H(SOK("4BindEiPKNS0_8SockAddrEj"), u2, P(sa), 0) == -1 && c4a_errno(c) == 22;
    len = 0;
    ok = ok && (int)H("nnsocketGetSockName", u1, P(sb), P(&len)) == -1 && c4a_errno(c) == 22;
    C4A_CHECK("socket: Bind 127.0.0.1:0 + GetSockName (port assigned); Bind 10.1.2.3 -> EADDRNOTAVAIL; null / zero length -> EINVAL");
    ok = (int)H(SOK("5FcntlEiiz"), u1, 3, 0) == 0 && (int)H(SOK("5FcntlEiiz"), u1, 4, 0x800) == 0 && (int)H("nnsocketFcntl", u1, 3, 0) == 0x800;
    ok = ok && (int64_t)H(SOK("4RecvEiPvmi"), u1, P(buf), sizeof buf, 0) == -1 && c4a_errno(c) == 11;
    ok = ok && (int)H(SOK("5FcntlEiiz"), 77, 3, 0) == -1 && c4a_errno(c) == 9;
    C4A_CHECK("socket: Fcntl F_SETFL O_NONBLOCK / F_GETFL; Recv on the empty non-blocking socket -> EAGAIN; bad descriptor -> EBADF");
    c4a_sa(sa, 127, 0, 0, 1, port);
    ok = (int64_t)H(SOK("6SendToEiPKvmiPK8sockaddrj"), u2, P("ping"), 4, 0, P(sa), 16) == 4;
    struct { int32_t fd; int16_t ev, rev; } pf[3] = {{u1, 1, 0x7fff}, {-1, 1, 0x7fff}, {99, 1, 0}};
    ok = ok && (int)H(SOK("4PollEP6pollfdmi"), P(pf), 2, 2000) == 1 && pf[0].rev == 1 && pf[1].rev == 0;
    ok = ok && (int)H("nnsocketPoll", P(pf), 3, 0) == 2 && pf[2].rev == 0x20;
    ok = ok && (int)H(SOK("4PollEP6pollfdmi"), 0, 2, 0) == -1 && c4a_errno(c) == 22;
    len = 16; memset(sb, 0, 16); memset(buf, 0, sizeof buf);
    ok = ok && (int64_t)H(SOK("8RecvFromEiPvmiP8sockaddrPj"), u1, P(buf), sizeof buf, 0, P(sb), P(&len)) == 4 && !memcmp(buf, "ping", 4) && len == 16 && sb[4] == 127 && sb[1] == 2;
    C4A_CHECK("socket: UDP loopback SendTo -> Poll (POLLIN; negative fd ignored; unknown fd POLLNVAL) -> RecvFrom with the sender's address");
    c4a_sa(sa, 93, 184, 216, 34, 80);
    ok = (int64_t)H(SOK("6SendToEiPKvmiPK8sockaddrj"), u2, P("ping"), 4, 0, P(sa), 16) == -1 && c4a_errno(c) == 101;
    ok = ok && (int64_t)H(SOK("6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj"), u2, P("ping"), 0, 0, P(sa), 16) == 0;
    ok = ok && (int64_t)H(SOK("4SendEiPKvmi"), u2, 0, 4, 0) == -1 && c4a_errno(c) == 22;
    ok = ok && (int64_t)H("nnsocketRecv", u2, P(buf), 0x100000000ull, 0) == -1 && c4a_errno(c) == 14;
    int t0 = (int)H(SOK("6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE"), 2, 1, 6);
    ok = ok && t0 >= 0 && (int)H(SOK("7ConnectEiPK8sockaddrj"), t0, P(sa), 16) == -1 && c4a_errno(c) == 101;
    ok = ok && (int)H("nnsocketConnect", t0, 0, 16) == -1 && c4a_errno(c) == 22;
    C4A_CHECK("socket: no network: SendTo / Connect to 93.184.216.34 -> ENETUNREACH (101); size 0 -> 0; null buffer EINVAL; 4 GiB EFAULT");
    int ls = (int)H(SOK("6SocketEiii"), 2, 1, 0), one = 1, v = -1;
    c4a_sa(sa, 0, 0, 0, 0, 0);
    ok = ls >= 0 && (int)H(SOK("10SetSockOptEiiiPKvj"), ls, 0xffff, 4, P(&one), 4) == 0;
    ok = ok && (int)H(SOK("4BindEiPK8sockaddrj"), ls, P(sa), 16) == 0 && (int)H(SOK("6ListenEii"), ls, 4) == 0;
    len = 16;
    ok = ok && (int)H(SOK("11GetSockNameEiP8sockaddrPj"), ls, P(sb), P(&len)) == 0 && sb[4] == 0 && sb[7] == 0;
    port = (uint16_t)(sb[2] << 8 | sb[3]);
    c4a_sa(sa, 127, 0, 0, 1, port);
    ok = ok && (int)H(SOK("7ConnectEiPK8sockaddrj"), t0, P(sa), 16) == 0;
    len = 16;
    int as = (int)H(SOK("6AcceptEiP8sockaddrPj"), ls, P(sb), P(&len));
    ok = ok && as >= 0 && len == 16 && sb[4] == 127;
    ok = ok && (int64_t)H(SOK("4SendEiPKvmi"), t0, P("hello"), 5, 0) == 5;
    memset(buf, 0, sizeof buf);
    ok = ok && (int64_t)H(SOK("4RecvEiPvmi"), as, P(buf), sizeof buf, 0) == 5 && !memcmp(buf, "hello", 5);
    ok = ok && (int64_t)H(SOK("4RecvEiPvmi"), as, P(buf), sizeof buf, 0x80) == -1 && c4a_errno(c) == 11;      /* MSG_DONTWAIT */
    len = 16;
    ok = ok && (int)H("nnsocketGetPeerName", t0, P(sb), P(&len)) == 0 && (uint16_t)(sb[2] << 8 | sb[3]) == port;
    len = 4;
    ok = ok && (int)H(SOK("10GetSockOptEiiiPvPj"), t0, 0xffff, 0x1007, P(&v), P(&len)) == 0 && v == 0 && len == 4;
    len = 4;
    ok = ok && (int)H("nnsocketGetSockOpt", t0, 0xffff, 0x1008, P(&v), P(&len)) == 0 && v == 1;
    ok = ok && (int)H(SOK("10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj"), t0, 0xffff, 8, 0, 4) == -1 && c4a_errno(c) == 22;
    ok = ok && (int)H(SOK("8ShutdownEii"), t0, 1) == 0 && (int64_t)H(SOK("4RecvEiPvmi"), as, P(buf), sizeof buf, 0) == 0;
    ok = ok && (int)H(SOK("5CloseEi"), as) == 0 && (int)H(SOK("5CloseEi"), as) == -1 && c4a_errno(c) == 9;
    C4A_CHECK("socket: TCP over loopback: SO_REUSEADDR, Bind 0.0.0.0 (reported as such), Listen, Connect, Accept, Send / Recv, MSG_DONTWAIT,\n"
              "          GetPeerName, SO_ERROR 0, SO_TYPE 1, Shutdown -> end of stream, Close twice -> EBADF");
    /* ---- resolver */
    list = 0x1234;
    ok = (int)H(SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), 0, 0, 0, P(&list)) == 8 && list == 0x1234;
    ok = ok && (int)H(SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), P("127.0.0.1"), 0, 0, 0) == 11 && c4a_errno(c) == 22;
    ok = ok && (int)H(SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), P("www.example.com"), P("80"), 0, P(&list)) == 2 && list == 0 && t_so_herrno == 2 && c4a_errno(c) == 0;
    uint32_t hints[12] = {0, 2, 1, 6};
    ok = ok && (int)H(SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), P("127.0.0.1"), P("8080"), P(hints), P(&list)) == 0 && list;
    if (ok) {
        uint64_t a = M64(list + 0x20);
        ok = M32(list + 4) == 2 && M32(list + 8) == 1 && M32(list + 0x10) == 16 && M8(a + 1) == 2 && M8(a + 2) == 0x1f && M8(a + 3) == 0x90 && M8(a + 4) == 127 && M8(a + 7) == 1 && M64(list + 0x28) == 0;
        H(SOK("12FreeAddrInfoEP8addrinfo"), list);
    }
    list = 0;
    ok = ok && (int)H(SOK("11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_"), P("10.0.0.7"), 0, 0, P(&list)) == 0 && list && M8(list + 4) == 2 && M8(M64(list + 0x18) + 4) == 10 && M8(M64(list + 0x18) + 7) == 7;
    if (list) H(SOK("12FreeAddrInfoEPNS0_8AddrInfoE"), list);
    C4A_CHECK("resolver: GetAddrInfo: no arguments 8, null result 11 + EINVAL, a DNS name without network EAI_AGAIN (2) / TRY_AGAIN,\n"
              "          numeric 127.0.0.1:8080 and 10.0.0.7 converted (both result layouts)");
    uint64_t he = H("nnsocketGetHostByNameWithOptions", P("192.168.0.9"), 0, 0);
    ok = he && !strcmp((const char *)(uintptr_t)M64(he), "192.168.0.9") && M32(he + 0x10) == 2 && M32(he + 0x14) == 4 && M8(M64(M64(he + 0x18))) == 192 && M8(M64(M64(he + 0x18)) + 3) == 9 && M64(M64(he + 0x18) + 8) == 0;
    ok = ok && H("nnsocketGetHostByNameWithOptions", P("www.example.com"), 0, 0) == 0 && t_so_herrno == 2;
    ok = ok && H("nnsocketGetHostByNameWithOptions", 0, 0, 0) == 0 && t_so_herrno == 1 && c4a_errno(c) == 22;
    ok = ok && !strcmp((const char *)(uintptr_t)H(SOK("11GAIStrErrorENS0_7AiErrnoE"), 2), "Temporary failure in name resolution");
    ok = ok && !strcmp((const char *)(uintptr_t)H(SOK("11GAIStrErrorENS0_7AiErrnoE"), 99), "Unknown error");
    C4A_CHECK("resolver: GetHostByName numeric / DNS name (TRY_AGAIN) / null (HOST_NOT_FOUND + EINVAL); GAIStrError texts");
    uint64_t opt[4] = {0, 0, 0, 0}, saved;
    ok = (int)H("nnsocketResolverGetOption", 0, 0x11171) == -1 && c4a_errno(c) == 22;
    ok = ok && (int)H("nnsocketResolverGetOption", P(opt), 0x11171) == 0 && (uint32_t)opt[0] == 0x11171 && (uint32_t)(opt[0] >> 32) == 0x67 && opt[1] == 8 && opt[2] == 0x200;
    saved = opt[2];
    opt[2] = 0x300;
    ok = ok && (int)H("nnsocketResolverSetOption", P(opt)) == 0;
    opt[2] = 0;
    ok = ok && (int)H("nnsocketResolverGetOption", P(opt), 0x11171) == 0 && opt[2] == 0x300;
    opt[2] = saved; H("nnsocketResolverSetOption", P(opt));
    ok = ok && (int)H("nnsocketResolverGetOption", P(opt), 0x11170) == -1 && c4a_errno(c) == 22;
    ok = ok && (int)H("nnsocketResolverGetOption", P(opt), 0x2711) == 0 && opt[1] == 4;
    ok = ok && (int)H("nnsocketResolverGetOption", P(opt), 5) == -1;
    C4A_CHECK("resolver: options: key 0x11171 read from the SDK image's table (0x200), set / get / restore; 0x11170 EINVAL; random id; unknown key -1");
    /* ---- ssl (uses the sockets above) */
    uint64_t sctx[2] = {0, 0xeeeeeeeeeeeeeeeeull}, id = 0, sconn[8];
    int pend = 77;
    uint32_t cs = 9, cn = 9;
    while (g_ssl_count) H("nnsslFinalize", 0);
    ok = H("nnsslFinalize", 0) == 0xca7b && H("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", P(sctx), 1) == 0xca7b;
    ok = ok && H("nnsslConnectionCreate", P(sconn), P(sctx)) == 0xca7b && (int32_t)sconn[2] == -1;
    ok = ok && H("nnsslInitialize", 0) == 0 && H("nnsslInitialize", 0) == 0x1927b && H("nnsslFinalize", 0) == 0;
    C4A_CHECK("ssl: Finalize / Context::Create before Initialize 0xca7b; second Initialize 0x1927b and one Finalize takes it back");
    ok = H("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", P(sctx), 1) == 0 && sctx[0] && sctx[1] == 0xeeeeeeeeeeeeeeeeull && ssl_scramble(ssl_scramble(sctx[0])) == sctx[0];
    ok = ok && H("nnsslContextGetContextId", P(sctx), P(&id)) == 0 && id == sctx[0] && H("nnsslContextGetContextId", P(sctx), 0) == 0xe27b && H("nnsslContextGetContextId", 0, P(&id)) == 0xe27b;
    id = 5;
    ok = ok && H("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", P(sctx), P(&id), P("x"), 1, 1) == 0 && id == 0;
    ok = ok && H("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", P(sctx), 0, P("x"), 1, 1) == 0xe27b;
    uint64_t zero = 0;
    ok = ok && H("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", P(&zero), P(&id), P("x"), 1, 1) == 0xdc7b;
    C4A_CHECK("ssl: Context::Create (8-byte object), GetContextId, ImportServerPki (id 0; null 0xe27b; empty context 0xdc7b)");
    memset(sconn, 0xee, sizeof sconn);
    ok = H("nnsslConnectionCreate", 0, P(sctx)) == 0xe27b && H("nnsslConnectionCreate", P(sconn), P(&zero)) == 0xdc7b;
    ok = ok && H("nnsslConnectionCreate", P(sconn), P(sctx)) == 0 && sconn[1] == sctx[0] && (int32_t)sconn[2] == -1;
    ok = ok && H("nnsslContextDestroy", P(sctx)) == 0xd67b;
    ok = ok && H("nnsslConnectionGetConnectionId", P(sconn), P(&id)) == 0 && id == sconn[0];
    ok = ok && H("nnsslConnectionSetHostName", P(sconn), 0, 0) == 0xe27b && H("nnsslConnectionSetHostName", P(sconn), P("host"), 4) == 0;
    ok = ok && H("nnsslConnectionSetVerifyOption", P(sconn), 1) == 0 && H("nnsslConnectionSetSessionCacheMode", P(sconn), 0) == 0 && H("nnsslConnectionSetOption", P(sconn), 2, 1) == 0;
    ok = ok && H("nnsslConnectionSetIoMode", P(sconn), 2) == 0xce7b;
    sconn[3] = 0; sconn[4] &= ~0xffffffffull;             /* no certificate buffer */
    ok = ok && H("nnsslConnectionDoHandshakeWithCertBuffer", P(sconn), P(&cs), P(&cn)) == 0xce7b && cs == 0 && cn == 0;
    ok = ok && H("nnsslConnectionPending", P(sconn), P(&pend)) == 0 && pend == 0 && H("nnsslConnectionPending", P(sconn), 0) == 0xe27b && (uint32_t)(sconn[4] >> 32) == 0xe27b;
    ok = ok && H("nnsslConnectionRead", P(sconn), 0, P(&pend), 4) == 0xe27b && H("nnsslConnectionWrite", P(sconn), P("x"), P(&pend), 0) == 0xe07b;
    ok = ok && H("nnsslConnectionSetSocketDescriptor", P(sconn), 99) == 0xd47b;
    ok = ok && H("nnsslConnectionSetSocketDescriptor", P(sconn), u1) == 0 && (int32_t)sconn[2] == -1 && H("nnsslConnectionSetIoMode", P(sconn), 1) == 0;
    ok = ok && H("nnsslConnectionDestroy", P(sconn)) == 0 && sconn[0] == 0 && H("nnsslConnectionDestroy", P(sconn)) == 0xda7b && H("nnsslConnectionRead", 0, 0, 0, 0) == 0xe27b;
    ok = ok && H("nnsslContextDestroy", P(sctx)) == 0 && sctx[0] == 0 && H("nnsslContextDestroy", P(sctx)) == 0xdc7b && H("nnsslContextDestroy", 0) == 0xe27b;
    ok = ok && H("nnsslFinalize", 0) == 0 && H("nnsslFinalize", 0) == 0xca7b;
    ok = ok && (int)H(SOK("5FcntlEiiz"), u1, 3, 0) >= 0;  /* the application's own descriptor survives the connection */
    C4A_CHECK("ssl: connection: create / ids / setters; no socket: SetIoMode + DoHandshake 0xce7b; Pending 0; argument checks 0xe27b, 0xe07b;\n"
              "          unknown descriptor 0xd47b; descriptor duplicated and released; Destroy twice 0xda7b; context with a connection 0xd67b");
    /* ---- sockets: Finalize closes everything */
    ok = H(SOK("8FinalizeEv"), 0) == 0 && !g_so_init && !g_so_fd[u1].o && !g_so_fd[ls].o && (int)H(SOK("12GetLastErrnoEv"), 0) == 22;
    C4A_CHECK("socket: Finalize closes the descriptors; GetLastErrno is 22 again");
    VirtualFree(pool, 0, MEM_RELEASE);
    /* ---- nsd */
    char fq[0x110], fin[0x100];
    memset(fq, 0x7e, sizeof fq); memset(fin, 0, sizeof fin);
    strcpy(fin, "api.example.srv.nintendo.net");
    ok = H("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", P(fq), P(fin)) == 0 && !strcmp(fq, fin) && fq[0xff] == 0 && fq[0x100] == 0x7e;
    C4A_CHECK("nsd: ResolveEx returns the name unchanged in a 0x100-byte Fqdn");
    /* ---- ldn */
    ldn_load_config();
    if (g_ldn.configured) printf("  ldn: a room server is configured (config\\online.ini): the no-radio case is not tested\n");
    else {
        ok = H(LDNK("10InitializeEv"), 0) == 0x2ecb && H(LDNK("8GetStateEv"), 0) == 0;
        C4A_CHECK("ldn: Initialize 0x2ecb (203-23, wireless communication not available), state None");
    }
    /* ---- ec */
    uint64_t acc[16], acc2[16], resp[16], size = 123;
    memset(acc, 0xee, sizeof acc); memset(acc2, 0xee, sizeof acc2); memset(resp, 0xee, sizeof resp);
    ok = H("_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", P(buf), 0x1000) == 0;
    H("_ZN2nn2ec19ShopServiceAccessorC1Ev", P(acc)); H("_ZN2nn2ec19ShopServiceAccessorC1Ev", P(acc2));
    ok = ok && acc[0] == P(acc) && acc[1] == P(acc) && acc[2] == 0 && acc[7] == P(acc + 7) && acc[8] == P(acc + 7) && acc[9] == 0xeeeeeeeeeeeeeeeeull;
    uint32_t service = 0;
    ok = ok && H("_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", P(acc), P(&service)) == 0 && acc[2] != 0;
    ok = ok && H("_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", P(acc2), P(&service)) == 0;
    ok = ok && g_ec_list[0] == P(acc2) && g_ec_list[1] == P(acc) && acc[0] == P(g_ec_list) && acc[1] == P(acc2) && acc2[0] == P(acc) && acc2[1] == P(g_ec_list);
    H("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseC1Ev", P(resp));
    ok = ok && H("_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", P(acc), P(resp), P(buf), 0, P("/v1/x"), P(buf), 1000) == 0x70a4;
    ok = ok && resp[3] == 0 && H("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", P(resp), P(&size)) == 0x190a4 && size == 123;
    H("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseD1Ev", P(resp));
    H("_ZN2nn2ec19ShopServiceAccessorD1Ev", P(acc));
    ok = ok && acc[2] == 0 && acc[0] == P(acc) && g_ec_list[0] == P(acc2) && g_ec_list[1] == P(acc2);
    ok = ok && H("_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", 0) == 0 && acc2[2] == 0 && g_ec_list[0] == P(g_ec_list) && g_ec_list[1] == P(g_ec_list);
    H("_ZN2nn2ec19ShopServiceAccessorD1Ev", P(acc2));
    C4A_CHECK("ec: accessor constructor layout, Initialize links it into the proxy list, Request 0x70a4 (no accepted network request),\n"
              "          GetSize 0x190a4 without writing, destructors and FinalizeForShopServiceAccessors unlink");
    printf("  close4/A: %d cases, %d failed\n", cases, bad);
    return bad;
}
