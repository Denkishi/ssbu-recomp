/* nn::fs directory functions on host folders: OpenDirectory, GetDirectoryEntryCount, ReadDirectory,
 * CloseDirectory, DeleteDirectory.
 *
 * nnfs.c (not edited: another worker owns it) has OpenDirectory as "path not found, not needed yet" and
 * does not have the other four. A directory that is never found is wrong for save data the game has
 * written itself, so OpenDirectory is REPLACED here (this file registers after nnfs.c) and the rest added.
 * The mount-name mapping is the one documented at the top of nnfs.c and must stay identical to it:
 *   rom   -> %SSBU_ROMFS% or ..\romfs,   any other mount -> ..\userdata\<mount name>
 *
 * [game] 0x71037c4360: OpenDirectory(&handle, path, 3); GetDirectoryEntryCount(&count, handle);
 * allocates count * 0x310 bytes; ReadDirectory(&read, entries, handle, count); so DirectoryHandle is one
 * 8-byte value passed in a register and sizeof(DirectoryEntry) = 0x310.
 * [doc] nnheaders nn/fs: DirectoryEntry { char name[0x301]; char pad[3]; s8 type @0x304 (0 directory,
 * 1 file); s64 fileSize @0x308 }; OpenDirectoryMode 1 = directories, 2 = files, 3 = all.
 * Results (module 2): [doc] switchbrew: PathNotFound 2002-0001 (0x202), DirectoryNotEmpty... the SDK code
 * for a non-empty directory is NOT ESTABLISHED here; 0x202 is returned when the folder is missing and the
 * same generic failure nnfs.c uses (0x3e802) when Windows refuses to remove it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

#define RESULT_PATH_NOT_FOUND 0x202u
#define RESULT_GENERIC_FAILURE 0x3e802u
#define ENTRY_SIZE 0x310
#define DIR_MAGIC 0x52494453u                             /* "SDIR" */

typedef struct { char name[0x301]; int is_file; int64_t size; } HostEntry;
typedef struct { uint32_t magic; int count, pos; HostEntry *entries; } HostDir;

int fs_dir_roots(const char *guest, char roots[2][MAX_PATH], uint32_t *result);   /* nnfs.c: mount table + save data journal view */
int fs_dir_hidden(const char *guest_dir, const char *name);
uint32_t fs_delete_directory(const char *guest);
/* nnfs.c: directories of guest file systems (nn::fs::fsa::Register, ARCropolis's "mods:" / "arc:"); their handles are
 * tagged values, never HostDir pointers, so they are recognised before dir_of() looks at memory */
int fs_fsa_dir_handle(uint64_t h);
uint32_t fs_fsa_open_directory(Ctx *c, const char *guest, int mode, uint64_t out, int *handled);
uint32_t fs_fsa_dir_count(Ctx *c, uint64_t h, int64_t *count);
uint32_t fs_fsa_dir_read(Ctx *c, uint64_t h, uint64_t entries, int64_t max, int64_t *got);
void fs_fsa_dir_close(Ctx *c, uint64_t h);
uint32_t fs_fsa_delete_directory(Ctx *c, const char *guest, int *handled);
static int dir_host_path(const char *guest, char *out) {  /* superseded by fs_dir_roots (kept for the self-test's scratch folder) */
    const char *env = getenv("SSBU_ROMFS");
    const char *colon = guest ? strchr(guest, ':') : NULL;
    if (!colon) return 0;
    size_t n = (size_t)(colon - guest);
    const char *rest = colon + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    if (n == 3 && strncmp(guest, "rom", 3) == 0) snprintf(out, MAX_PATH, "%s\\%s", env && env[0] ? env : "..\\romfs", rest);
    else snprintf(out, MAX_PATH, "..\\userdata\\%.*s\\%s", (int)n, guest, rest);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '\\') out[--len] = 0;
    return 1;
}

static void fs_OpenDirectory(Ctx *c) {                    /* (DirectoryHandle *out, const char *path, int mode) -> Result */
    uint64_t out = A0;
    int mode = (int)A2;
    char roots[2][MAX_PATH], pattern[MAX_PATH + 4];
    uint32_t result = 0;
    if (out) wr64(out, 0);
    {   int handled = 0;
        uint32_t r = fs_fsa_open_directory(c, (const char *)(uintptr_t)A1, mode, out, &handled);
        if (handled) { RET(r); return; } }
    /* the folders that make up this directory: for journaled save data the working folder, then the committed one */
    int nroots = fs_dir_roots((const char *)(uintptr_t)A1, roots, &result);
    if (result) { RET(result); return; }
    HostDir *d = calloc(1, sizeof *d);
    int cap = 0;
    d->magic = DIR_MAGIC;
    for (int r = 0; r < nroots; r++) {
        snprintf(pattern, sizeof pattern, "%s\\*", roots[r]);
        WIN32_FIND_DATAW fd;
        wchar_t wpattern[MAX_PATH + 4];
        MultiByteToWideChar(CP_ACP, 0, pattern, -1, wpattern, MAX_PATH + 4);
        HANDLE h = FindFirstFileW(wpattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, dup = 0;
            char name[0x301];
            memset(name, 0, sizeof name);
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, 0x300, NULL, NULL)) continue;
            if (r == nroots - 1 && nroots == 2) {            /* committed folder under a working folder: skip what the working state replaced or deleted */
                char ansi[MAX_PATH];
                WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, ansi, sizeof ansi, NULL, NULL);
                if (fs_dir_hidden((const char *)(uintptr_t)A1, ansi)) continue;
            }
            for (int k = 0; k < d->count && !dup; k++) dup = _stricmp(d->entries[k].name, name) == 0;
            if (dup) continue;
            if (is_dir ? !(mode & 1) : !(mode & 2)) {
                if (nroots < 2) continue;                    /* still remembered (below) so the committed twin is not listed either */
            }
            if (d->count == cap) { cap = cap ? cap * 2 : 32; d->entries = realloc(d->entries, sizeof(HostEntry) * (size_t)cap); }
            HostEntry *e = &d->entries[d->count];
            memset(e, 0, sizeof *e);
            memcpy(e->name, name, sizeof e->name);
            e->is_file = !is_dir;
            e->size = is_dir ? 0 : (int64_t)(((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            d->count++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (nroots == 2) {                                       /* drop the entries the open mode does not ask for */
        int n = 0;
        for (int k = 0; k < d->count; k++) if (d->entries[k].is_file ? (mode & 2) : (mode & 1)) d->entries[n++] = d->entries[k];
        d->count = n;
    }

    trace_printf("     fs OpenDirectory \"%s\" mode %d -> %d entries\n", (const char *)(uintptr_t)A1, mode, d->count);
    wr64(out, P(d));
    RET(0);
}
static HostDir *dir_of(uint64_t handle) {
    HostDir *d = (HostDir *)(uintptr_t)handle;
    return handle > 0x10000 && d->magic == DIR_MAGIC ? d : NULL;
}
static void fs_GetDirectoryEntryCount(Ctx *c) {           /* (int64_t *out, DirectoryHandle) -> Result */
    if (fs_fsa_dir_handle(A1)) { int64_t n = 0; uint64_t out = A0; uint32_t r = fs_fsa_dir_count(c, A1, &n); if (out) wr64(out, (uint64_t)n); RET(r); return; }
    HostDir *d = dir_of(A1);
    if (A0) wr64(A0, d ? (uint64_t)d->count : 0);
    RET(d ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_ReadDirectory(Ctx *c) {                    /* (int64_t *outCount, DirectoryEntry *entries, DirectoryHandle, int64_t max) -> Result */
    if (fs_fsa_dir_handle(A2)) { int64_t n = 0; uint64_t out = A0; uint32_t r = fs_fsa_dir_read(c, A2, A1, (int64_t)A3, &n); if (out) wr64(out, (uint64_t)n); RET(r); return; }
    HostDir *d = dir_of(A2);
    uint64_t out = A1;
    int64_t max = (int64_t)A3, n = 0;
    if (!d) { if (A0) wr64(A0, 0); RET(RESULT_GENERIC_FAILURE); return; }
    while (n < max && d->pos < d->count) {
        HostEntry *e = &d->entries[d->pos++];
        uint8_t *g = (uint8_t *)(uintptr_t)(out + (uint64_t)n * ENTRY_SIZE);
        memset(g, 0, ENTRY_SIZE);
        memcpy(g, e->name, 0x301);
        g[0x304] = (uint8_t)e->is_file;
        memcpy(g + 0x308, &e->size, 8);
        n++;
    }
    if (A0) wr64(A0, (uint64_t)n);
    RET(0);
}
static void fs_CloseDirectory(Ctx *c) {                   /* (DirectoryHandle) */
    if (fs_fsa_dir_handle(A0)) { fs_fsa_dir_close(c, A0); return; }
    HostDir *d = dir_of(A0);
    if (d) { d->magic = 0; free(d->entries); free(d); }
}
static void fs_DeleteDirectory(Ctx *c) {                  /* (const char *path) -> Result; the directory must be empty */
    int handled = 0;
    uint32_t r = fs_fsa_delete_directory(c, (const char *)(uintptr_t)A0, &handled);
    if (handled) { RET(r); return; }
    RET(fs_delete_directory((const char *)(uintptr_t)A0));
}

#define FS(n) "_ZN2nn2fs" n
static const ImportDef fsdir_imports[] = {
    {FS("13OpenDirectoryEPNS0_15DirectoryHandleEPKci"), fs_OpenDirectory},
    {FS("22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE"), fs_GetDirectoryEntryCount},
    {FS("13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"), fs_ReadDirectory},
    {FS("14CloseDirectoryENS0_15DirectoryHandleE"), fs_CloseDirectory},
    {FS("15DeleteDirectoryEPKc"), fs_DeleteDirectory},
};
void register_nnfsdir_imports(void) {
    int n = (int)(sizeof fsdir_imports / sizeof fsdir_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(fsdir_imports[i].name)) printf("nnfsdir: \"%s\" is not an import of the game (typo?)\n", fsdir_imports[i].name);
    register_imports(fsdir_imports, n);
}

int nnfsdir_selftest(Ctx *c) {
    /* a scratch mount under %TEMP% (never the user's save data): two files and one folder, listed with each mode */
    int ok = 1;
    char base[MAX_PATH], p[MAX_PATH];
    uint32_t fs_mount_plain(const char *name, const char *host_dir);
    GetTempPathA(MAX_PATH - 64, base);
    snprintf(base + strlen(base), 64, "ssbu_fsdir_selftest_%lu", (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(base, NULL);
    snprintf(p, sizeof p, "%s\\d", base); CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\d\\sub", base); CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\d\\a.bin", base);
    FILE *f = fopen(p, "wb"); if (f) { fwrite("12345", 1, 5, f); fclose(f); }
    snprintf(p, sizeof p, "%s\\d\\b.bin", base);
    f = fopen(p, "wb"); if (f) fclose(f);
    ok = fs_mount_plain("sdkselftest", base) == 0;
    static const int expect[4] = {0, 1, 2, 3};
    for (int mode = 1; mode <= 3; mode++) {
        uint64_t handle = 0;
        int64_t count = -1, got = -1;
        uint8_t *entries = malloc(4 * ENTRY_SIZE + 16);
        memset(entries, 0xee, 4 * ENTRY_SIZE + 16);
        c->x[0] = P(&handle); c->x[1] = P("sdkselftest:/d"); c->x[2] = (uint64_t)mode; fs_OpenDirectory(c);
        ok = ok && c->x[0] == 0 && handle;
        c->x[0] = P(&count); c->x[1] = handle; fs_GetDirectoryEntryCount(c);
        ok = ok && c->x[0] == 0 && count == expect[mode];
        c->x[0] = P(&got); c->x[1] = P(entries); c->x[2] = handle; c->x[3] = 4; fs_ReadDirectory(c);
        ok = ok && c->x[0] == 0 && got == expect[mode];
        for (int i = 0; i < got && ok; i++) {
            const char *name = (const char *)entries + i * ENTRY_SIZE;
            int is_file = entries[i * ENTRY_SIZE + 0x304];
            int64_t size;
            memcpy(&size, entries + i * ENTRY_SIZE + 0x308, 8);
            if (!strcmp(name, "a.bin")) ok = is_file == 1 && size == 5;
            else if (!strcmp(name, "b.bin")) ok = is_file == 1 && size == 0;
            else if (!strcmp(name, "sub")) ok = is_file == 0;
            else ok = 0;
        }
        for (int i = 0; i < 16; i++) if (entries[got * ENTRY_SIZE + i] != 0xee) ok = 0;
        c->x[0] = P(&got); c->x[1] = P(entries); c->x[2] = handle; c->x[3] = 4; fs_ReadDirectory(c);
        ok = ok && got == 0;                               /* the listing is consumed */
        c->x[0] = handle; fs_CloseDirectory(c);
        free(entries);
    }
    uint64_t handle = 1;
    c->x[0] = P(&handle); c->x[1] = P("sdkselftest:/missing"); c->x[2] = 3; fs_OpenDirectory(c);
    ok = ok && c->x[0] == RESULT_PATH_NOT_FOUND && handle == 0;
    c->x[0] = P("sdkselftest:/d"); fs_DeleteDirectory(c);
    ok = ok && c->x[0] != 0;                               /* not empty */
    snprintf(p, sizeof p, "%s\\d\\a.bin", base); DeleteFileA(p);
    snprintf(p, sizeof p, "%s\\d\\b.bin", base); DeleteFileA(p);
    c->x[0] = P("sdkselftest:/d/sub"); fs_DeleteDirectory(c);
    ok = ok && c->x[0] == 0;
    c->x[0] = P("sdkselftest:/d"); fs_DeleteDirectory(c);
    ok = ok && c->x[0] == 0;
    RemoveDirectoryA(base);
    printf("  fs: OpenDirectory / GetDirectoryEntryCount / ReadDirectory (modes 1, 2, 3; 0x310-byte entries) / DeleteDirectory: %s\n", ok ? "ok" : "FAILED");
    return !ok;
}
