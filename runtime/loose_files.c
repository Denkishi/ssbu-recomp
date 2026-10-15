// loose_files.c - LOOSE-FILE LAYER, first stage (plan\phases\N4_own_fighter.md, section 3). Part of OUR runtime.
// Serves files from a folder in place of the copies in the game's archive:
//     <root>\<name>\<archive path>        e.g.  characters\n3test\fighter\mario\param\vl.prc
// <root> = SSBU_CHARACTERS, default "<exe folder>\..\characters". Every folder directly under <root> is one
// fighter / mod; below it the tree mirrors the archive's paths. No folder = nothing installed.
//
// This file contains no game content. It is written to be included: the includer defines Ctx, HookFn, HookOrig
// (fighter_sdk\include\hook.h for a mod DLL; the host's own declarations for the exe) and calls
//     loose_files_start(install, log)
// with the hook table's install function. Today it is built as the mod mods_n4_src\n4_loose.cpp; moving it into
// the exe is a wrapper of a few lines in a host source (boot calls loose_files_start(hook_install, log)).
//
// STAGE 1 (this file): a path the ARCHIVE KNOWS. Hook point: the per-file decompress 0x71039a4040
// (zstd stream step: x0 context, x1 out {pointer, size, position}, x2 in {...}; returns 0 when the file is complete),
// called from the resource service's inflate thread 0x71035444c0. The reference tool for the original platform
// needs an INLINE hook at 0x7103544678 to learn WHICH file is being inflated (register w27 = index in the batch).
// We do not: a replacement in our hook table sees the whole guest register context of the caller, and x19 (the
// resource service) and x27 (the loop index) are callee-saved registers of the inflate thread that are still intact
// at the entry of the callee. So: file info index = [service + 0x230] + x27, at the moment the callee returns 0.
// Then the loose file's bytes replace the content of the game's own buffer (loaded data[file info indice index]).
// So that the buffer is large enough, the decompressed size in the archive's in-memory table is raised to the
// loose file's size when the tables are first seen (before the file is requested).
// NOT in stage 1: a path the archive does NOT know (new fighter name): it needs new rows in the archive's
// in-memory tables (file paths, hash -> index, file infos, directory lists); such files are listed in the log as
// "not in the archive: NOT served". Files that the game reads without the inflate thread (uncompressed, streams).
//
// Layouts (13.0.4; names as in reference\smash-arc and reference\ARCropolis, offsets checked against the code of
// 0x710353e4e0, 0x7103540450 and 0x71035444c0):
//   resource service  *(0x7105331f28): +0xd0 filesystem info, +0x228 batch index, +0x22c batch count, +0x230 batch start
//   filesystem info   +0x08 loaded file paths (8 bytes: data index, loaded), +0x10 loaded datas (0x18 bytes: +0 buffer),
//                     +0x78 -> { archive tables, search tables }
//   archive tables    +0x40 header (+4 = number of file paths), +0x60 file paths (0x20 bytes: +0 hash40 | index << 40),
//                     +0x68 file info indices (8 bytes: +4 file info index), +0x90 file infos (16 bytes: path index,
//                     indice index, info-to-data index, flags), +0x98 info to data (12 bytes: +4 file data index),
//                     +0xa0 file datas (16 bytes: +4 compressed size, +8 decompressed size)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>

#define LF_MAX 4096
#define LF_MASK40 0xffffffffffull
typedef struct {
    uint64_t hash;                  // hash40 of the archive path
    char *arc_path, *disk_path;
    uint32_t size;
    int32_t path_index;             // file path index in the archive, -1 = the archive has no such path
    uint32_t indice_index, info_index, data_index, arc_size;
    long served;
} LooseFile;
static LooseFile g_lf[LF_MAX];
static int g_lf_count, g_lf_resolved;
static void (*g_lf_log)(const char *);
static HookOrig g_lf_orig_inflate;
static CRITICAL_SECTION g_lf_lock;

static void lf_logf(const char *fmt, ...) {
    char line[700]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (g_lf_log) g_lf_log(line);
}
static uint32_t lf_crc32(const char *s) {
    uint32_t c = 0xffffffffu;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}
static int lf_readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static uint64_t lf_r64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static uint32_t lf_r32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }

// walk <dir>; `rel` = the archive path so far ("" at a fighter folder's top)
static void lf_scan(const char *dir, const char *rel) {
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH * 2], r[MAX_PATH * 2];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { lf_scan(sub, r); continue; }
        if (g_lf_count >= LF_MAX) continue;
        size_t n = strlen(r);
        if (n > 5 && strcmp(r + n - 5, ".json") == 0 && strchr(r, '/') == NULL) continue;      // the fighter's manifest
        for (char *p = r; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        LooseFile *f = &g_lf[g_lf_count++];
        f->arc_path = _strdup(r); f->disk_path = _strdup(sub);
        f->hash = ((uint64_t)n << 32) | lf_crc32(r);
        f->size = fd.nFileSizeLow; f->path_index = -1;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// the archive's in-memory tables, from the resource service
static int lf_tables(uint64_t service, uint64_t *fsinfo, uint64_t *arc) {
    if (!lf_readable(service + 0xd0, 8)) return 0;
    *fsinfo = lf_r64(service + 0xd0);
    if (!lf_readable(*fsinfo + 0x78, 8)) return 0;
    uint64_t pi = lf_r64(*fsinfo + 0x78);
    if (!lf_readable(pi, 8)) return 0;
    *arc = lf_r64(pi);
    return lf_readable(*arc + 0xa0, 8);
}
// once: find every loose file in the archive's tables and make room for it
static void lf_resolve(uint64_t arc) {
    uint64_t header = lf_r64(arc + 0x40), paths = lf_r64(arc + 0x60), indices = lf_r64(arc + 0x68);
    uint64_t infos = lf_r64(arc + 0x90), to_data = lf_r64(arc + 0x98), datas = lf_r64(arc + 0xa0);
    uint32_t n = lf_r32(header + 4);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t q = lf_r64(paths + 0x20ull * i), h = q & LF_MASK40;
        for (int k = 0; k < g_lf_count; k++) {
            LooseFile *f = &g_lf[k];
            if (f->hash != h || f->path_index >= 0) continue;
            f->path_index = (int32_t)i;
            f->indice_index = (uint32_t)(q >> 40);
            f->info_index = lf_r32(indices + 8ull * f->indice_index + 4);
            uint32_t itd = lf_r32(infos + 16ull * f->info_index + 8);
            f->data_index = lf_r32(to_data + 12ull * itd + 4);
            uint64_t d = datas + 16ull * f->data_index;
            f->arc_size = lf_r32(d + 8);
            if (f->size > f->arc_size) memcpy((void *)(uintptr_t)(d + 8), &f->size, 4);
            found++;
            lf_logf("[loose] %s: file path index 0x%x, file info 0x%x, data 0x%x: archive size %u (compressed %u), loose size %u%s",
                    f->arc_path, i, f->info_index, f->data_index, f->arc_size, lf_r32(d + 4), f->size, f->size > f->arc_size ? " (size in the table raised)" : "");
        }
    }
    for (int k = 0; k < g_lf_count; k++)
        if (g_lf[k].path_index < 0) lf_logf("[loose] %s (%u bytes): not in the archive: NOT served (new paths need the table extension)", g_lf[k].arc_path, g_lf[k].size);
    lf_logf("[loose] %d loose files, %d found among the archive's %u file paths", g_lf_count, found, n);
}
static void lf_serve(uint64_t service) {
    uint64_t fsinfo, arc;
    if (!lf_tables(service, &fsinfo, &arc)) return;
    EnterCriticalSection(&g_lf_lock);
    if (!g_lf_resolved) { g_lf_resolved = 1; lf_resolve(arc); }
    LeaveCriticalSection(&g_lf_lock);
    return;
}
// 0x71039a4040: one step of the decompression of the file the inflate thread is working on
static void lf_inflate(Ctx *c, void *user) {
    (void)user;
    uint64_t lr = c->x[30], service = c->x[19], index = c->x[27];
    int from_thread = lr >= GA(0x71035444c0ull) && lr < GA(0x71035456d8ull);
    if (from_thread && !g_lf_resolved) lf_serve(service);
    g_lf_orig_inflate(c);
    if (!from_thread || c->x[0] != 0) return;
    uint64_t fsinfo, arc;
    if (!lf_tables(service, &fsinfo, &arc)) return;
    uint32_t info_index = lf_r32(service + 0x230) + (uint32_t)index;
    for (int k = 0; k < g_lf_count; k++) {
        LooseFile *f = &g_lf[k];
        if (f->path_index < 0 || f->info_index != info_index) continue;
        uint64_t datas = lf_r64(fsinfo + 0x10);
        uint64_t buf = lf_r64(datas + 0x18ull * f->indice_index);
        if (!lf_readable(buf, f->size)) { lf_logf("[loose] %s: the game's buffer %llx is not there or too small: NOT served", f->arc_path, (unsigned long long)buf); return; }
        FILE *fp = fopen(f->disk_path, "rb");
        if (!fp) { lf_logf("[loose] %s: cannot open %s", f->arc_path, f->disk_path); return; }
        size_t got = fread((void *)(uintptr_t)buf, 1, f->size, fp);
        fclose(fp);
        f->served++;
        lf_logf("[loose] served %s: %u bytes from %s (archive copy: %u bytes; file info 0x%x; time number %ld)", f->arc_path, (unsigned)got, f->disk_path, f->arc_size, info_index, f->served);
        return;
    }
}

// two steps for a caller that may have to wait for the hook table (the exe): scan once, then install until it works
static int loose_files_scan(void (*log)(const char *)) {
    g_lf_log = log;
    char root[MAX_PATH * 2];
    const char *e = getenv("SSBU_CHARACTERS");
    if (e && e[0]) snprintf(root, sizeof root, "%s", e);
    else {
        DWORD n = GetModuleFileNameA(NULL, root, MAX_PATH);
        while (n && root[n - 1] != '\\' && root[n - 1] != '/') n--;
        snprintf(root + n, sizeof root - n, "..\\characters");
    }
    if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) { lf_logf("[loose] no folder %s: nothing installed", root); return 0; }
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char sub[MAX_PATH * 2]; snprintf(sub, sizeof sub, "%s\\%s", root, fd.cFileName);
            int before = g_lf_count;
            lf_scan(sub, "");
            lf_logf("[loose] folder %s: %d files", sub, g_lf_count - before);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!g_lf_count) { lf_logf("[loose] %s holds no files: nothing installed", root); return 0; }
    InitializeCriticalSection(&g_lf_lock);
    lf_logf("[loose] loose-file layer: %d files under %s", g_lf_count, root);
    return g_lf_count;
}
static int loose_files_install(int (*install)(uint64_t, HookFn, void *, HookOrig *), int quiet) {
    int ok = install(GA(0x71039a4040ull), lf_inflate, NULL, &g_lf_orig_inflate);
    if (ok || !quiet) lf_logf("[loose] hook on the per-file decompress %#llx: %s", (unsigned long long)GA(0x71039a4040ull), ok ? "installed" : "FAILED");
    return ok;
}
static int loose_files_start(int (*install)(uint64_t, HookFn, void *, HookOrig *), void (*log)(const char *)) {
    if (!loose_files_scan(log)) return 0;
    return loose_files_install(install, 0);
}
