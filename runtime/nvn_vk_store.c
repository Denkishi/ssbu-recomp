/* Track V, session 11: the CONTAINER STORE made by the host itself (included by nvn_vk_m4.c; not a build unit of its own).
 *
 * Until session 10 the folder ..\cache\vk_containers was written by tools_vk/bnsh_extract.py; a PC that has never run that
 * tool had no preparation and no index. This file does the same reading in the host, on the preparation thread's helpers:
 * data.arc (%SSBU_ROMFS% or ..\romfs) is read as a flat file, without its file table: every zstd frame is opened with THE
 * GAME'S OWN zstd decoder (the lifted functions the feasibility demo host.c proved byte-exact: 0x71039a4040 = decompress
 * stream, 0x71039a3e70 = free stream), its first bytes say what the file is, and files that can hold a shader container
 * (effect files "EFFN" / "VFXB", container files "BNSH", and - new in session 11 - layout archives "SARC", which hold the
 * "__ArchiveShader" / "ArchiveShader" / "ArchiveShader-y2rgb" containers) are decoded whole and searched for the header
 * "BNSH" + byte-order mark 0xFEFF at +0x0C; the container is file-size bytes (u32 at +0x1C) from there. Each distinct
 * container (by content) becomes <fnv64>.bnsh.
 *
 * The decoder state is made HERE (zeroed host memory + the fields the game's own creator 0x7103541ae0 writes: allocator
 * pair, opaque pointer, largest window), with host malloc / free as its allocator, so no call reaches the game's heap from
 * our threads; the decoder itself is pure (tables in the image's read-only data). One guest register context and one guest
 * stack per helper thread.
 * While a build is under way the folder holds "store_building.txt"; a start that finds it builds again. */
#include "hostrt.h"

#define VKS_DCTX_SIZE 0x271f0u
#define VKS_ZSTD_DECOMPRESS GA(0x71039a4040ULL)
#define VKS_ZSTD_FREE GA(0x71039a3e70ULL)
#define VKS_SLICE (64ull << 20)
#define VKS_ROOM 0x40000ull

static void vks_h_alloc(Ctx *c) { c->x[0] = (uint64_t)(uintptr_t)malloc(c->x[1] ? (size_t)c->x[1] : 1); }
static void vks_h_free(Ctx *c) { if (c->x[1]) free((void *)(uintptr_t)c->x[1]); }

static struct {
    HANDLE map;
    uint64_t total;
    volatile LONG64 next_slice, frames, opened, wanted, hits, dup, raw, bad, bytes_out, bytes_dec, scanned;
    volatile LONG ncont;
    uint64_t fn_alloc, fn_free;
    CRITICAL_SECTION lock;
    uint64_t seen[8192];
    int nseen;
    char dir[240];
    int nthread;
    volatile LONG fail;
} VKS;

static uint64_t vks_dctx_new(void) {
    uint8_t *d = calloc(1, VKS_DCTX_SIZE);
    if (!d) return 0;
    uint64_t a = (uint64_t)(uintptr_t)d;
    wr64(a + 28960, VKS.fn_alloc);
    wr64(a + 28968, VKS.fn_free);
    wr64(a + 28976, 0);
    wr64(a + 29064, 0x8000001ULL);          /* largest window the game's creator allows */
    return a;
}
static void vks_dctx_free(Ctx *c, uint64_t d) { c->x[0] = d; call_guest(c, VKS_ZSTD_FREE); }

static void vks_keep(const uint8_t *blob, uint32_t size, int raw) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, blob, size) ^ ((uint64_t)size << 40);
    char path[400];
    EnterCriticalSection(&VKS.lock);
    int k;
    for (k = 0; k < VKS.nseen; k++) if (VKS.seen[k] == h) break;
    if (k < VKS.nseen || VKS.nseen == 8192) { LeaveCriticalSection(&VKS.lock); InterlockedIncrement64(&VKS.dup); return; }
    VKS.seen[VKS.nseen++] = h;
    LeaveCriticalSection(&VKS.lock);
    snprintf(path, sizeof path, "%s/%016llx.bnsh", VKS.dir, (unsigned long long)h);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(blob, 1, size, f) != size) InterlockedIncrement(&VKS.fail);
    if (f) fclose(f);
    InterlockedIncrement(&VKS.ncont);
    InterlockedExchangeAdd64(&VKS.bytes_out, size);
    if (raw) InterlockedIncrement64(&VKS.raw);
}
static void vks_containers_in(const uint8_t *buf, size_t len) {
    size_t pos = 0;
    while (pos + 0x20 <= len) {
        const uint8_t *q = memchr(buf + pos, 'B', len - pos - 0x1f);
        if (!q) return;
        pos = (size_t)(q - buf);
        uint32_t m, size;
        memcpy(&m, q, 4);
        if (m == 0x48534E42u && q[0xC] == 0xff && q[0xD] == 0xfe) {
            memcpy(&size, q + 0x1C, 4);
            if (size >= 0x60 && size <= len - pos) { InterlockedIncrement64(&VKS.hits); vks_keep(q, size, 0); pos += size; continue; }
        }
        pos++;
    }
}
/* One zstd frame at `src` (at most `avail` bytes of the archive behind it). Returns the compressed bytes consumed when
 * the frame was decoded to its end (the caller skips them), 0 otherwise. */
static uint64_t vks_frame(Ctx *c, const uint8_t *src, uint64_t avail, const uint8_t *far_src, uint64_t far_avail) {
    uint8_t head[16];
    uint64_t d = vks_dctx_new(), in[3], out[3], ret, used = 0;
    if (!d) return 0;
    in[0] = (uint64_t)(uintptr_t)src; in[1] = avail < 0x22000 ? avail : 0x22000; in[2] = 0;
    out[0] = (uint64_t)(uintptr_t)head; out[1] = sizeof head; out[2] = 0;
    c->x[0] = d; c->x[1] = (uint64_t)(uintptr_t)out; c->x[2] = (uint64_t)(uintptr_t)in;
    call_guest(c, VKS_ZSTD_DECOMPRESS);
    ret = c->x[0];
    if (ret >= 0xffffffffffffff89ULL || out[2] < 4) { if (ret >= 0xffffffffffffff89ULL) InterlockedIncrement64(&VKS.bad); else InterlockedIncrement64(&VKS.opened); vks_dctx_free(c, d); return 0; }
    InterlockedIncrement64(&VKS.opened);
    uint32_t m;
    memcpy(&m, head, 4);
    if (m != 0x4E464645u /* EFFN */ && m != 0x42584656u /* VFXB */ && m != 0x48534E42u /* BNSH */ && m != 0x43524153u /* SARC */) { vks_dctx_free(c, d); return 0; }
    InterlockedIncrement64(&VKS.wanted);
    /* whole: the same stream goes on, into a buffer that grows; the input is now the far view (the whole frame) */
    size_t cap = 4u << 20, n = (size_t)out[2];
    uint8_t *buf = malloc(cap);
    int ok = buf != NULL, calls = 0;
    if (ok) memcpy(buf, head, n);
    in[0] = (uint64_t)(uintptr_t)far_src; in[1] = far_avail;            /* in[2] (position) stays: same bytes, longer view */
    while (ok && ret != 0) {
        if (n == cap) { uint8_t *nb = cap < (1u << 30) ? realloc(buf, cap * 2) : NULL; if (!nb) { ok = 0; break; } buf = nb; cap *= 2; }
        out[0] = (uint64_t)(uintptr_t)(buf + n); out[1] = cap - n; out[2] = 0;
        uint64_t before = in[2];
        c->x[0] = d; c->x[1] = (uint64_t)(uintptr_t)out; c->x[2] = (uint64_t)(uintptr_t)in;
        call_guest(c, VKS_ZSTD_DECOMPRESS);
        ret = c->x[0];
        if (ret >= 0xffffffffffffff89ULL) { InterlockedIncrement64(&VKS.bad); ok = 0; break; }
        n += (size_t)out[2];
        if (out[2] == 0 && in[2] == before && ++calls > 4) { ok = 0; break; }       /* no progress: the archive ends inside the frame */
        if (in[2] >= in[1] && ret != 0 && out[2] == 0) { ok = 0; break; }
    }
    if (buf && n >= 0x60) { InterlockedExchangeAdd64(&VKS.bytes_dec, (LONG64)n); vks_containers_in(buf, n); }     /* like the Python tool: what was decoded is searched even when the frame broke */
    if (ok) used = in[2];
    free(buf);
    vks_dctx_free(c, d);
    return used;
}
static DWORD WINAPI vks_thread(void *arg) {
    Ctx *c = calloc(1, sizeof *c);
    size_t stack_size = 4u << 20;
    uint8_t *stack = _aligned_malloc(stack_size, 16);
    (void)arg;
    if (!c || !stack) { InterlockedIncrement(&VKS.fail); return 1; }
    c->x[31] = (uint64_t)(uintptr_t)(stack + stack_size - 256);
    uint64_t nslice = (VKS.total + VKS_SLICE - 1) / VKS_SLICE;
    for (;;) {
        uint64_t s = (uint64_t)InterlockedIncrement64(&VKS.next_slice) - 1;
        if (s >= nslice || g_m4_quit) break;
        uint64_t lo = s * VKS_SLICE, hi = lo + VKS_SLICE < VKS.total ? lo + VKS_SLICE : VKS.total;
        uint64_t vlen = hi + VKS_ROOM < VKS.total ? hi + VKS_ROOM - lo : VKS.total - lo;
        const uint8_t *v = MapViewOfFile(VKS.map, FILE_MAP_READ, (DWORD)(lo >> 32), (DWORD)lo, (SIZE_T)vlen);
        if (!v) { InterlockedIncrement(&VKS.fail); continue; }
        uint64_t end = hi - lo, lim = vlen >= 4 ? vlen - 3 : 0;
        if (end > lim) end = lim;
        for (uint64_t p = 0; p < end; p++) {
            const uint8_t *q = v + p;
            if (*q != 0x28 && *q != 'B') continue;
            uint32_t m;
            memcpy(&m, q, 4);
            if (m == 0xFD2FB528u) {
                InterlockedIncrement64(&VKS.frames);
                /* the far view: the frame from its start to (at most) 1 GB behind it, for a file decoded whole */
                uint64_t fo = (lo + p) & ~0xffffull, flen = VKS.total - fo < (1ull << 30) ? VKS.total - fo : (1ull << 30);
                uint64_t used;
                /* the far view is mapped only when the first bytes say the file is wanted: vks_frame gets it lazily */
                const uint8_t *fv = NULL;
                {   /* cheap first look without the far view */
                    uint8_t head[16];
                    uint64_t d = vks_dctx_new(), in[3], out[3], ret;
                    int want = 0;
                    if (d) {
                        in[0] = (uint64_t)(uintptr_t)q; in[1] = vlen - p < 0x22000 ? vlen - p : 0x22000; in[2] = 0;
                        out[0] = (uint64_t)(uintptr_t)head; out[1] = sizeof head; out[2] = 0;
                        c->x[0] = d; c->x[1] = (uint64_t)(uintptr_t)out; c->x[2] = (uint64_t)(uintptr_t)in;
                        call_guest(c, VKS_ZSTD_DECOMPRESS);
                        ret = c->x[0];
                        if (ret >= 0xffffffffffffff89ULL) InterlockedIncrement64(&VKS.bad);
                        else {
                            InterlockedIncrement64(&VKS.opened);
                            if (out[2] >= 4) { memcpy(&m, head, 4); want = m == 0x4E464645u || m == 0x42584656u || m == 0x48534E42u || m == 0x43524153u; }
                        }
                        vks_dctx_free(c, d);
                    }
                    if (!want) continue;
                }
                fv = MapViewOfFile(VKS.map, FILE_MAP_READ, (DWORD)(fo >> 32), (DWORD)fo, (SIZE_T)flen);
                if (!fv) { InterlockedIncrement(&VKS.fail); continue; }
                InterlockedDecrement64(&VKS.opened);                                 /* vks_frame counts it again */
                used = vks_frame(c, fv + (lo + p - fo), flen - (lo + p - fo), fv + (lo + p - fo), flen - (lo + p - fo));
                UnmapViewOfFile(fv);
                if (used > 4) p += used - 1;                                          /* the frame is done: go on behind it */
            } else if (m == 0x48534E42u && p + 0x60 <= vlen && q[0xC] == 0xff && q[0xD] == 0xfe) {       /* a container stored without compression */
                uint32_t size;
                int grsc = 0;
                memcpy(&size, q + 0x1C, 4);
                for (int k = 0x20; k + 4 <= 0x60; k++) if (!memcmp(q + k, "grsc", 4)) { grsc = 1; break; }
                if (grsc && size >= 0x60 && size <= (64u << 20) && lo + p + size <= VKS.total) {
                    uint64_t fo = (lo + p) & ~0xffffull, flen = lo + p + size - fo;
                    const uint8_t *fv = MapViewOfFile(VKS.map, FILE_MAP_READ, (DWORD)(fo >> 32), (DWORD)fo, (SIZE_T)flen);
                    if (fv) { vks_keep(fv + (lo + p - fo), size, 1); UnmapViewOfFile(fv); }
                }
            }
        }
        UnmapViewOfFile(v);
        InterlockedExchangeAdd64(&VKS.scanned, (LONG64)(hi - lo));
    }
    _aligned_free(stack);
    free(c);
    return 0;
}
/* Builds the store in `dir`. Returns the number of containers written (0: no archive, or nothing found). Blocks the
 * caller (the preparation thread) until done; the helpers run at normal priority: it is the first thing a first start
 * needs and it is seconds. */
static int vks_build(const char *dir) {
    char arc[400], mark[400];
    const char *rom = getenv("SSBU_ROMFS");
    LARGE_INTEGER fq, sz;
    QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk(), last = t0;
    snprintf(arc, sizeof arc, "%s\\data.arc", rom && rom[0] ? rom : "..\\romfs");
    HANDLE fh = CreateFileA(arc, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (fh == INVALID_HANDLE_VALUE || !GetFileSizeEx(fh, &sz)) { printf("nvn_vk_store: %s cannot be opened: no container store\n", arc); if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh); return 0; }
    memset(&VKS, 0, sizeof VKS);
    VKS.total = (uint64_t)sz.QuadPart;
    VKS.map = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!VKS.map) { printf("nvn_vk_store: %s cannot be mapped: no container store\n", arc); CloseHandle(fh); return 0; }
    snprintf(VKS.dir, sizeof VKS.dir, "%s", dir);
    CreateDirectoryA("../cache", NULL); CreateDirectoryA(dir, NULL);
    snprintf(mark, sizeof mark, "%s/store_building.txt", dir);
    { FILE *f = fopen(mark, "wb"); if (f) { fputs("the container store is being written\n", f); fclose(f); } else { printf("nvn_vk_store: the folder %s cannot be written: no container store\n", dir); CloseHandle(VKS.map); CloseHandle(fh); return 0; } }
    InitializeCriticalSection(&VKS.lock);
    VKS.fn_alloc = hostfn_address("vk_store_zstd_alloc", vks_h_alloc);
    VKS.fn_free = hostfn_address("vk_store_zstd_free", vks_h_free);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    VKS.nthread = (int)si.dwNumberOfProcessors / 2;
    if (VKS.nthread > 12) VKS.nthread = 12;
    if (VKS.nthread < 2) VKS.nthread = 2;
    if (getenv("SSBU_VK_STORE_THREADS") && atoi(getenv("SSBU_VK_STORE_THREADS")) > 0 && atoi(getenv("SSBU_VK_STORE_THREADS")) <= 32) VKS.nthread = atoi(getenv("SSBU_VK_STORE_THREADS"));
    printf("nvn_vk_store: no container store in %s: reading %s (%.1f GB) with the game's own zstd decoder on %d threads (frame %lld)\n", dir, arc, (double)VKS.total / 1073741824.0, VKS.nthread, (long long)nvn_front_frame_next());
    fflush(stdout);
    HANDLE th[32];
    for (int i = 0; i < VKS.nthread; i++) th[i] = CreateThread(NULL, 0, vks_thread, NULL, 0, NULL);
    for (;;) {
        DWORD w = WaitForMultipleObjects((DWORD)VKS.nthread, th, TRUE, 500);
        if (w != WAIT_TIMEOUT) break;
        g_store_pct = (int)(100 * (uint64_t)VKS.scanned / (VKS.total ? VKS.total : 1));
        if (tk() - last > 5 * fq.QuadPart) {
            last = tk();
            printf("nvn_vk_store: %d %% of data.arc read, %lld zstd frames, %lld files that can hold containers, %d containers (%.1f s, frame %lld)\n", g_store_pct, (long long)VKS.frames, (long long)VKS.wanted, (int)VKS.ncont, (double)(last - t0) / (double)fq.QuadPart, (long long)nvn_front_frame_next());
            fflush(stdout);
        }
    }
    for (int i = 0; i < VKS.nthread; i++) CloseHandle(th[i]);
    CloseHandle(VKS.map); CloseHandle(fh);
    {   /* the containers built into the game's EXECUTABLE ("TextureShader", "IconShader", "ui2d_BuildinShader_Nvn_Binary",
         * "font_BuildinShader_Nvn_Binary": 4 in 13.0.4, two of them in its data segment, where the game changes them in place
         * when it starts): read from the file the runtime was started with, not from memory */
        const char *elf = __argc > 1 && __argv && __argv[1] && __argv[1][0] != '-' ? __argv[1] : "..\\binaries\\13.0.4\\main.elf";
        FILE *ef = fopen(elf, "rb");
        LONG before = VKS.ncont;
        if (ef) {
            _fseeki64(ef, 0, SEEK_END); long long en = _ftelli64(ef); _fseeki64(ef, 0, SEEK_SET);
            uint8_t *eb = en > 0x60 && en < (1ll << 30) ? malloc((size_t)en) : NULL;
            if (eb && fread(eb, 1, (size_t)en, ef) == (size_t)en) vks_containers_in(eb, (size_t)en);
            free(eb);
            fclose(ef);
        }
        printf("nvn_vk_store: %d container(s) built into the executable %s%s\n", (int)(VKS.ncont - before), elf, ef ? "" : " (the file cannot be opened)");
    }
    g_store_pct = 100;
    int whole = !g_m4_quit && !VKS.fail && (uint64_t)VKS.scanned == VKS.total;
    printf("nvn_vk_store: data.arc read in %.1f s (frame %lld): %lld zstd frames (%lld not decodable), %lld effect / container / layout files decoded whole (%lld MB), %d distinct containers (%lld of them stored uncompressed, %lld repeats), %.1f MB written to %s%s\n",
           (double)(tk() - t0) / (double)fq.QuadPart, (long long)nvn_front_frame_next(), (long long)VKS.frames, (long long)VKS.bad, (long long)VKS.wanted, (long long)(VKS.bytes_dec >> 20), (int)VKS.ncont, (long long)VKS.raw, (long long)VKS.dup, (double)VKS.bytes_out / 1048576.0, dir,
           whole ? "" : g_m4_quit ? ": STOPPED (the game is closing); the next start builds it again" : ": NOT COMPLETE (a file could not be mapped or written); the next start builds it again");
    fflush(stdout);
    if (whole) DeleteFileA(mark);
    DeleteCriticalSection(&VKS.lock);
    return whole ? (int)VKS.ncont : 0;
}
