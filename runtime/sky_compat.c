/* sky_compat.c - SDK entry points that only Skyline plugins import (the game itself never calls them), answered here so
 * that a plugin which calls one does not stop the game with "calls X, which nothing provides" (nnro.c resolve ->
 * hostrt.c extra_import_lookup). #included by skyline.c; sky_register() registers the table.
 *
 * First need: UltimateTrainingModpack (v7.1.1). Its main() builds a telemetry event from the console's pseudo device id
 * and the last opened user, and its updater / telemetry use nn::ssl, nn::socket and nnsocketGetAddrInfo (an offline port
 * answers every one of those with "no network"); its Rust std uses clock_gettime for SystemTime / Instant.
 *
 * Conventions: integer arguments in c->x[0..7], the result in x0 (a nn::Result is 0 for success, else
 * (description << 9) | module). Nothing here touches the game's own imports: the table goes to the plugin-only
 * handlers (register_plugin_imports), and for a name the game imports too nnro.c's resolve takes the game's handler. */

#define SC_A(n) (c->x[n])
#define SC_RET(v) (c->x[0] = (uint64_t)(v))

/* nn::oe::GetPseudoDeviceId(nn::util::Uuid *out): the console's per-application pseudo device id, 16 bytes. A fixed value
 * here (there is no console): the same on every run, like the console's. */
static void sc_oe_GetPseudoDeviceId(Ctx *c) {
    static const uint8_t id[16] = {0x4f, 0x70, 0x65, 0x6e, 0x53, 0x6d, 0x61, 0x73, 0x68, 0x2d, 0x55, 0x6c, 0x74, 0x69, 0x6d, 0x61};
    if (SC_A(0)) memcpy((void *)(uintptr_t)SC_A(0), id, sizeof id);
    SC_RET(0);
}

/* nn::account::GetLastOpenedUser(Uid *out) -> Result: the one user of this port, {1, 0}, the uid nnfs.c / nnmisc.c
 * (ShowUserSelector) hand out everywhere else. */
static void sc_acc_GetLastOpenedUser(Ctx *c) {
    if (SC_A(0)) { wr64(SC_A(0), 1); wr64(SC_A(0) + 8, 0); }
    SC_RET(0);
}

/* nn::ssl: no SSL on this port. Initialize / Finalize and the setters succeed, the constructors leave the object as the
 * caller made it, Context::Create / Connection::Create / DoHandshake / Read / Write fail with one ssl Result (module 123),
 * so a plugin's HTTPS client gives up at the first step and reports an error instead of hanging. */
#define SC_SSL_FAIL 0x27bu                       /* module 123 (ssl), description 1 */
static void sc_ret0(Ctx *c) { SC_RET(0); }
static void sc_nothing(Ctx *c) { (void)c; }
static void sc_ssl_fail(Ctx *c) { SC_RET(SC_SSL_FAIL); }
static void sc_ssl_GetLastError(Ctx *c) { if (SC_A(1)) wr32(SC_A(1), SC_SSL_FAIL); SC_RET(0); }   /* (this, nn::Result *out) */

/* nn::socket for a plugin's own TCP client: the plugin's Socket / Recv / Close / Fcntl / Send go to the game's socket layer
 * (nnnet_c4a.inc, names main imports); only these overloads are the plugin's alone. Connect and Poll fail with ENETUNREACH
 * (51, the BSD value of the SDK's errno), nnsocketGetAddrInfo answers EAI_NONAME (8) with no result list. The errno is the
 * game's socket layer's (its SetLastErrno / GetLastErrno handlers, through main's import table, so that GetLastError
 * reports the errno of whichever call failed last); a private copy stands in while that layer is not registered. */
static volatile LONG g_sc_sock_errno;
static void sc_sock_set_errno(Ctx *c, int e) {
    ImportFn set = import_lookup("nnsocketSetLastErrno");
    g_sc_sock_errno = e;
    if (set) { uint64_t x0 = c->x[0]; c->x[0] = (uint64_t)(uint32_t)e; set(c); c->x[0] = x0; }
}
static void sc_sock_fail(Ctx *c) { sc_sock_set_errno(c, 51); SC_RET((uint64_t)(int64_t)-1); }
static void sc_sock_GetLastError(Ctx *c) {
    ImportFn get = import_lookup("nnsocketGetLastErrno");
    if (get) get(c); else SC_RET((uint64_t)(uint32_t)g_sc_sock_errno);
}
static void sc_sock_GetAddrInfo(Ctx *c) { if (SC_A(3)) wr64(SC_A(3), 0); sc_sock_set_errno(c, 51); SC_RET(8); }   /* (node, service, hints, addrinfo **res) */

/* clock_gettime(clockid_t, struct timespec *) of the plugins' C library (musl, nnmusl): CLOCK_REALTIME (0) from the system
 * clock, every other clock (MONOTONIC 1, PROCESS/THREAD cputime, BOOTTIME ...) from the performance counter;
 * timespec { int64 tv_sec; int64 tv_nsec }. Returns 0, or -1 without an output pointer. */
static void sc_clock_gettime(Ctx *c) {
    uint64_t out = SC_A(1);
    int64_t sec, nsec;
    if (!out) { SC_RET((uint64_t)(int64_t)-1); return; }
    if ((uint32_t)SC_A(0) == 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t t = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL;      /* 100 ns units since 1970 */
        sec = (int64_t)(t / 10000000ULL);
        nsec = (int64_t)(t % 10000000ULL) * 100;
    } else {
        LARGE_INTEGER f, n;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&n);
        sec = n.QuadPart / f.QuadPart;
        nsec = (int64_t)((double)(n.QuadPart % f.QuadPart) * 1e9 / (double)f.QuadPart);
    }
    wr64(out, (uint64_t)sec);
    wr64(out + 8, (uint64_t)nsec);
    SC_RET(0);
}

#define SC_SSL(n) "_ZN2nn3ssl" n
#define SC_SO(n) "_ZN2nn6socket" n
static const ImportDef sky_compat_imports[] = {
    {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", sc_oe_GetPseudoDeviceId},
    {"_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", sc_acc_GetLastOpenedUser},
    {SC_SSL("10InitializeEv"), sc_ret0}, {SC_SSL("8FinalizeEv"), sc_ret0},
    {SC_SSL("7ContextC1Ev"), sc_nothing}, {SC_SSL("7Context6CreateENS1_10SslVersionE"), sc_ssl_fail}, {SC_SSL("7Context7DestroyEv"), sc_ret0},
    {SC_SSL("10ConnectionC1Ev"), sc_nothing}, {SC_SSL("10Connection6CreateEPNS0_7ContextE"), sc_ssl_fail}, {SC_SSL("10Connection7DestroyEv"), sc_ret0},
    {SC_SSL("10Connection11SetHostNameEPKcj"), sc_ret0}, {SC_SSL("10Connection19SetSocketDescriptorEi"), sc_ret0},
    {SC_SSL("10Connection9SetOptionENS1_10OptionTypeEb"), sc_ret0}, {SC_SSL("10Connection15SetVerifyOptionENS1_12VerifyOptionE"), sc_ret0},
    {SC_SSL("10Connection11DoHandshakeEv"), sc_ssl_fail}, {SC_SSL("10Connection4ReadEPcj"), sc_ssl_fail}, {SC_SSL("10Connection5WriteEPKcPij"), sc_ssl_fail},
    {SC_SSL("10Connection12GetLastErrorEPNS_6ResultE"), sc_ssl_GetLastError},
    {SC_SO("7ConnectEiPKNS0_8SockAddrEj"), sc_sock_fail}, {SC_SO("4PollEPNS0_6PollFdEmi"), sc_sock_fail}, {SC_SO("12GetLastErrorEv"), sc_sock_GetLastError},
    {"nnsocketGetAddrInfo", sc_sock_GetAddrInfo}, {"nnsocketFreeAddrInfo", sc_nothing},
    {"clock_gettime", sc_clock_gettime},
};
static void sky_compat_register(void) {                   /* the plugin-only table: a name the game imports too (nn::ssl::Context::Create) keeps the game's handler (nnro.c resolve) */
    register_plugin_imports(sky_compat_imports, (int)(sizeof sky_compat_imports / sizeof sky_compat_imports[0]));
}
