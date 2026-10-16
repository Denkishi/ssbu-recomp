/* nn::fs on host directories, plus the minimal nn::account and nn::nifm
 * answers the game needs to start: one local user, no network.
 *
 * MOUNT TABLE. A path is "name:/dir/file"; the name must have been mounted, as in the SDK's own mount table
 * ([sdk 0x1f4960] MountTable::Find: an unknown name is fs NotMounted 0x35f202; mounting a name twice is
 * MountNameAlreadyExists 0x7802). Mount kinds and their host folders:
 *   MountRom           -> %SSBU_ROMFS% or ..\romfs                  (the game's extracted romfs, update applied; read-only)
 *   MountSaveData      -> <userdata>\save                           (journaled, see below)
 *   MountCacheStorage  -> <userdata>\cache                          (journaled like save data)
 * <userdata> is %SSBU_USERDATA% or ..\userdata (tests point SSBU_USERDATA at a copy of the user's save).
 *
 * SAVE DATA JOURNAL. On the console, writes to save data are not permanent until nn::fs::Commit: they go to a journal,
 * Commit applies all of them together, and uncommitted writes are gone at the next mount. Here:
 *   - <userdata>\save is the COMMITTED state and is never written outside Commit.
 *   - Mounting creates the working folder <userdata>\save.work.<pid>. A file is copied into it the first time it is
 *     opened for writing (copy-on-write) or created there; deletions are recorded (tombstones); every read, existence
 *     test and directory listing sees the working folder over the committed one, minus the tombstones.
 *   - Commit ([sdk 0x1f4490] FileSystemAccessor::Commit: fails with 0x327202 while a file of the mount is open with the
 *     write mode) is two-phase: (1) the changed files are flushed and the list of changes is written to
 *     <userdata>\save.journal.<pid>, flushed, and renamed to <userdata>\save.commit -- from this rename on the commit
 *     is decided; (2) the changes are applied (files moved over the committed ones, deletions done) and the marker is
 *     removed. A process killed in phase 1 leaves the committed state untouched; one killed in phase 2 leaves the
 *     marker, and the next mount finishes the apply (roll forward) before anything is read.
 *   - Unmount, or a later mount by any process, discards the working folder of a process that no longer runs.
 * Not reproduced: the console's limit on the journal SIZE (a Commit is needed before the journal fills; the game's own
 * save code already commits within it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincrypt.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* nn::fs results (module 2), value = description << 9 | 2. Descriptions as published (switchbrew "Error codes",
 * the same numbers the suyu reference uses in file_sys/errors.h): 1 PathNotFound, 2 PathAlreadyExists,
 * 1002 TargetNotFound (no such save data), 3005 OutOfRange (read past the end of the file),
 * 6201 FileExtensionWithoutOpenModeAllowAppend (write past the end of a file opened without the append mode),
 * 6202 ReadNotPermitted, 6203 WriteNotPermitted (the handle was opened without that mode).
 * Read in sdk.elf: 60 MountNameAlreadyExists, 6905 NotMounted [0x1f498c], 6457 "a file is still open for writing" [0x1f44f4].
 * 7 DirectoryNotEmpty, 6400 PermissionDenied (suyu file_sys/errors.h; write to the read-only rom). */
#define RESULT_PATH_NOT_FOUND 0x202u
#define RESULT_PATH_ALREADY_EXISTS 0x402u
#define RESULT_DIRECTORY_NOT_EMPTY 0xe02u
#define RESULT_MOUNT_NAME_EXISTS 0x7802u
#define RESULT_TARGET_NOT_FOUND 0x7d402u
#define RESULT_OUT_OF_RANGE 0x177a02u
#define RESULT_NO_APPEND 0x307202u
#define RESULT_READ_NOT_PERMITTED 0x307402u
#define RESULT_WRITE_NOT_PERMITTED 0x307602u
#define RESULT_PERMISSION_DENIED 0x320002u
#define RESULT_WRITE_FILE_OPEN 0x327202u
#define RESULT_NOT_MOUNTED 0x35f202u
#define RESULT_GENERIC_FAILURE 0x3e802u      /* a Windows failure with no console counterpart (disk error, sharing violation) */

enum { MK_ROM = 1, MK_SAVE, MK_CACHE, MK_PLAIN /* self-tests: a writable host folder without a journal */ };
typedef struct {
    char name[16];
    int kind;
    char root[MAX_PATH];                     /* committed state (or the rom folder) */
    char work[MAX_PATH];                     /* journal working folder ("" for the rom) */
    char **tomb;                             /* relative paths deleted since the last commit, '\\' separators */
    int tomb_n;
} Mount;
#define MOUNT_MAX 8
static Mount g_mounts[MOUNT_MAX];
static char g_rom_root[MAX_PATH], g_user_root[MAX_PATH];
static volatile LONG g_open_log;
static CRITICAL_SECTION g_fs_lock;
static volatile LONG g_fs_lock_ready;

static void fs_lock(void) {
    if (InterlockedCompareExchange(&g_fs_lock_ready, 1, 0) == 0) { InitializeCriticalSection(&g_fs_lock); g_fs_lock_ready = 2; }
    while (g_fs_lock_ready != 2) SwitchToThread();
    EnterCriticalSection(&g_fs_lock);
}
static void fs_unlock(void) { LeaveCriticalSection(&g_fs_lock); }

static void fs_roots(void) {
    if (g_rom_root[0]) return;
    const char *env = getenv("SSBU_ROMFS"), *user = getenv("SSBU_USERDATA");
    strncpy(g_rom_root, env && env[0] ? env : "..\\romfs", MAX_PATH - 1);
    strncpy(g_user_root, user && user[0] ? user : "..\\userdata", MAX_PATH - 1);
    CreateDirectoryA(g_user_root, NULL);
}
const char *fs_user_root(void) { fs_roots(); return g_user_root; }

static int is_dir(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void make_parents(const char *path) {             /* creates every missing folder above `path` */
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
}
static void remove_tree(const char *dir) {
    char pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(sub);
            else { SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL); DeleteFileA(sub); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* ---- tombstones */
static int tomb_find(Mount *m, const char *rel) {
    for (int i = 0; i < m->tomb_n; i++) if (_stricmp(m->tomb[i], rel) == 0) return i;
    return -1;
}
static int tomb_hides(Mount *m, const char *rel) {       /* rel itself or one of its parent folders was deleted */
    for (int i = 0; i < m->tomb_n; i++) {
        size_t n = strlen(m->tomb[i]);
        if (_strnicmp(m->tomb[i], rel, n) == 0 && (rel[n] == 0 || rel[n] == '\\')) return 1;
    }
    return 0;
}
static void tomb_add(Mount *m, const char *rel) {
    if (tomb_find(m, rel) >= 0) return;
    m->tomb = realloc(m->tomb, sizeof(char *) * (size_t)(m->tomb_n + 1));
    m->tomb[m->tomb_n++] = _strdup(rel);
}
static void tomb_remove(Mount *m, const char *rel) {
    int i = tomb_find(m, rel);
    if (i >= 0) { free(m->tomb[i]); m->tomb[i] = m->tomb[--m->tomb_n]; }
}
static void tomb_clear(Mount *m) { for (int i = 0; i < m->tomb_n; i++) free(m->tomb[i]); free(m->tomb); m->tomb = NULL; m->tomb_n = 0; }

/* ---- path resolution */
static Mount *mount_find(const char *name, size_t n) {
    for (int i = 0; i < MOUNT_MAX; i++) if (g_mounts[i].kind && strlen(g_mounts[i].name) == n && strncmp(g_mounts[i].name, name, n) == 0) return &g_mounts[i];
    return NULL;
}
/* "mount:/dir/file" -> mount and relative path with '\\' separators (no leading or trailing separator). Result code. */
static uint32_t split_path(const char *guest, Mount **mount, char *rel) {
    const char *colon = guest ? strchr(guest, ':') : NULL;
    if (!colon) return RESULT_NOT_MOUNTED;
    fs_roots();
    Mount *m = mount_find(guest, (size_t)(colon - guest));
    if (!m) return RESULT_NOT_MOUNTED;
    const char *rest = colon + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    snprintf(rel, MAX_PATH, "%s", rest);
    for (char *p = rel; *p; p++) if (*p == '/') *p = '\\';
    size_t len = strlen(rel);
    while (len > 0 && rel[len - 1] == '\\') rel[--len] = 0;
    *mount = m;
    return 0;
}
static void join(char *out, const char *root, const char *rel) { if (rel[0]) snprintf(out, MAX_PATH, "%s\\%s", root, rel); else snprintf(out, MAX_PATH, "%s", root); }
/* The host path that currently holds `rel` in the mount's view; returns 0 when it does not exist in the view. */
static int view_path(Mount *m, const char *rel, char *out) {
    if (m->work[0]) {
        if (tomb_hides(m, rel)) { join(out, m->work, rel); return exists(out); }      /* re-created after a deletion: only the working copy counts */
        join(out, m->work, rel);
        if (exists(out)) return 1;
    }
    join(out, m->root, rel);
    return exists(out);
}
static int view_parent_is_dir(Mount *m, const char *rel) {
    char parent[MAX_PATH], host[MAX_PATH];
    snprintf(parent, sizeof parent, "%s", rel);
    char *s = strrchr(parent, '\\');
    if (!s) return 1;                                     /* directly under the root */
    *s = 0;
    return view_path(m, parent, host) && is_dir(host);
}
/* Used by nnfsdir.c: the host folders to list for a guest directory (working folder first), 0 = no such directory. */
int fs_dir_roots(const char *guest, char roots[2][MAX_PATH], uint32_t *result) {
    Mount *m; char rel[MAX_PATH], host[MAX_PATH];
    int n = 0;
    fs_lock();
    *result = split_path(guest, &m, rel);
    if (!*result) {
        if (!view_path(m, rel, host) || !is_dir(host)) *result = RESULT_PATH_NOT_FOUND;
        else {
            if (m->work[0]) { join(roots[n], m->work, rel); if (is_dir(roots[n])) n++; }
            if (!m->work[0] || !tomb_hides(m, rel)) { join(roots[n], m->root, rel); if (is_dir(roots[n])) n++; }
        }
    }
    fs_unlock();
    return n;
}
int fs_dir_hidden(const char *guest_dir, const char *name) {      /* 1 = this entry of the committed folder was deleted in the working state */
    Mount *m; char rel[MAX_PATH], full[MAX_PATH];
    int hidden = 0;
    fs_lock();
    if (!split_path(guest_dir, &m, rel) && m->work[0]) { join(full, rel, name); hidden = tomb_hides(m, rel[0] ? full : name); }
    fs_unlock();
    return hidden;
}
uint32_t fs_delete_directory(const char *guest) {
    Mount *m; char rel[MAX_PATH], host[MAX_PATH], roots[2][MAX_PATH];
    uint32_t r;
    fs_lock();
    r = split_path(guest, &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, host) || !is_dir(host))) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    if (r) return r;
    uint32_t r2;                                          /* empty in the view? */
    int n = fs_dir_roots(guest, roots, &r2);
    for (int i = 0; i < n; i++) {
        char pat[MAX_PATH + 4];
        WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof pat, "%s\\*", roots[i]);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            int from_committed = !m->work[0] || _strnicmp(roots[i], m->work, strlen(m->work)) != 0;
            if (from_committed && m->work[0] && fs_dir_hidden(guest, fd.cFileName)) continue;       /* deleted in the working state */
            FindClose(h);
            return RESULT_DIRECTORY_NOT_EMPTY;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    fs_lock();
    if (m->work[0]) {
        char w[MAX_PATH], cm[MAX_PATH];
        join(w, m->work, rel); join(cm, m->root, rel);
        if (is_dir(w)) RemoveDirectoryA(w);
        if (is_dir(cm)) tomb_add(m, rel);
    } else r = RemoveDirectoryA(host) ? 0 : RESULT_GENERIC_FAILURE;
    fs_unlock();
    return r;
}

/* Open files: nn::fs::FileHandle is the Windows handle itself; the open mode (1 read, 2 write, 4 allow append) and the
 * mount are kept here so reads, writes, growth and Commit behave exactly as the mode says. */
#define OPEN_MAX 1024
static struct { HANDLE h; int mode; Mount *mount; } g_open[OPEN_MAX];
static void open_remember(HANDLE h, int mode, Mount *m) {
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (!g_open[i].h) { g_open[i].h = h; g_open[i].mode = mode; g_open[i].mount = m; break; }
    fs_unlock();
}
static int open_mode(HANDLE h, int forget) {             /* mode of an open handle, 7 (everything) if unknown */
    int mode = 7;
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].h == h) { mode = g_open[i].mode; if (forget) g_open[i].h = NULL; break; }
    fs_unlock();
    return mode;
}

/* ---- journal: commit and recovery */
static HANDLE commit_mutex(void) {
    HANDLE mx = CreateMutexA(NULL, FALSE, "ssbu_native_save_commit");
    if (mx) WaitForSingleObject(mx, INFINITE);
    return mx;
}
static void commit_done(HANDLE mx) { if (mx) { ReleaseMutex(mx); CloseHandle(mx); } }
static void journal_list(FILE *j, const char *work, const char *rel) {     /* every file of the working folder, depth first */
    char dir[MAX_PATH], pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    join(dir, work, rel);
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (rel[0]) snprintf(sub, sizeof sub, "%s\\%s", rel, fd.cFileName); else snprintf(sub, sizeof sub, "%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { fprintf(j, "M\t%s\n", sub); journal_list(j, work, sub); }
        else {
            char full[MAX_PATH];
            join(full, work, sub);
            HANDLE f = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) { FlushFileBuffers(f); CloseHandle(f); }
            fprintf(j, "F\t%s\n", sub);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
/* Applies a decided commit: marker lines are "W <working folder>", "R <committed folder>", then "D rel" (delete),
 * "M rel" (make folder), "F rel" (move file). Every step can be repeated, so a commit interrupted here is finished by
 * running it again. */
static int journal_apply(const char *marker) {
    char line[MAX_PATH + 8], work[MAX_PATH] = "", root[MAX_PATH] = "", a[MAX_PATH], b[MAX_PATH];
    FILE *j = fopen(marker, "r");
    int n = 0;
    if (!j) return -1;
    while (fgets(line, sizeof line, j)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len < 3) continue;
        const char *arg = line + 2;
        if (line[0] == 'W') snprintf(work, sizeof work, "%s", arg);
        else if (line[0] == 'R') snprintf(root, sizeof root, "%s", arg);
        else if (!root[0] || !work[0]) continue;
        else if (line[0] == 'D') { join(a, root, arg); if (is_dir(a)) remove_tree(a); else DeleteFileA(a); n++; }
        else if (line[0] == 'M') { join(a, root, arg); make_parents(a); CreateDirectoryA(a, NULL); }
        else if (line[0] == 'F') {
            join(a, work, arg); join(b, root, arg);
            if (exists(a)) { make_parents(b); if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { fclose(j); return -2; } }
            n++;
        }
    }
    fclose(j);
    DeleteFileA(marker);
    return n;
}
static int pid_alive(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    int alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}
/* Before a save-type folder is mounted: finish a commit that was decided but not applied, and drop the working
 * folders and half-written journals of processes that no longer run. */
static void journal_recover(const char *root) {
    char marker[MAX_PATH], pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE mx = commit_mutex();
    snprintf(marker, sizeof marker, "%s.commit", root);
    if (exists(marker)) {
        int n = journal_apply(marker);
        printf("nn::fs: a commit of %s was interrupted after it was decided: finished now (%d changes)\n", root, n);
        trace_printf("     fs journal: interrupted commit of %s rolled forward (%d changes)\n", root, n);
    }
    snprintf(pat, sizeof pat, "%s.*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *ext = strrchr(fd.cFileName, '.');
            const char *kind = ext && ext - 5 >= fd.cFileName && !strncmp(ext - 5, ".work", 5) ? "work" : ext && ext - 8 >= fd.cFileName && !strncmp(ext - 8, ".journal", 8) ? "journal" : NULL;
            if (!kind) continue;
            DWORD pid = (DWORD)strtoul(ext + 1, NULL, 10);
            if (!pid || pid == GetCurrentProcessId() || pid_alive(pid)) continue;
            snprintf(path, sizeof path, "%s\\..\\%s", root, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path); else DeleteFileA(path);
            trace_printf("     fs journal: uncommitted %s of process %lu discarded (%s)\n", kind, (unsigned long)pid, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    commit_done(mx);
}
static uint32_t mount_add(const char *name, int kind, const char *root) {
    Mount *m = NULL;
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= sizeof m->name) return RESULT_GENERIC_FAILURE;
    fs_lock();
    if (mount_find(name, n)) { fs_unlock(); return RESULT_MOUNT_NAME_EXISTS; }
    for (int i = 0; i < MOUNT_MAX && !m; i++) if (!g_mounts[i].kind) m = &g_mounts[i];
    if (!m) { fs_unlock(); return RESULT_GENERIC_FAILURE; }
    memset(m, 0, sizeof *m);
    strcpy(m->name, name);
    snprintf(m->root, MAX_PATH, "%s", root);
    if (kind != MK_ROM) {
        snprintf(m->work, MAX_PATH, "%s.work.%lu", root, (unsigned long)GetCurrentProcessId());
        remove_tree(m->work);                              /* a second mount in this process starts from the committed state */
        CreateDirectoryA(m->work, NULL);
    }
    m->kind = kind;
    fs_unlock();
    return 0;
}

static void save_dir(char *dir, const char *leaf) { fs_roots(); snprintf(dir, MAX_PATH, "%s\\%s", g_user_root, leaf); }
/* Save data of the one local user lives in <userdata>\save. It exists once EnsureSaveData has created it;
 * MountSaveData of save data that does not exist fails as on the console. */
static int save_exists(void) { char dir[MAX_PATH]; save_dir(dir, "save"); return is_dir(dir); }
static void fs_MountRom(Ctx *c) {                        /* (name, cache buffer, cache size): the buffer holds the RomFS metadata cache of the SDK's reader; the host reads the extracted tree */
    fs_roots();
    uint32_t r = mount_add(STR(0), MK_ROM, g_rom_root);
    trace_printf("     fs mount rom \"%s\" -> %s (%x)\n", STR(0), g_rom_root, r);
    RET(r);
}
static void fs_MountSaveData(Ctx *c) {                   /* (const char *name, const Uid &) */
    char dir[MAX_PATH];
    save_dir(dir, "save");
    if (!save_exists()) { trace_printf("     fs mount save data \"%s\": no save data (EnsureSaveData was not called)\n", STR(0)); RET(RESULT_TARGET_NOT_FOUND); return; }
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_SAVE, dir);
    trace_printf("     fs mount save data \"%s\" -> %s, journal working folder %s.work.%lu (%x)\n", STR(0), dir, dir, (unsigned long)GetCurrentProcessId(), r);
    RET(r);
}
static void fs_MountCacheStorage(Ctx *c) {               /* (const char *name): the application's cache storage, created on first use */
    char dir[MAX_PATH];
    save_dir(dir, "cache");
    CreateDirectoryA(dir, NULL);
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_CACHE, dir);
    trace_printf("     fs mount cache storage \"%s\" -> %s (%x)\n", STR(0), dir, r);
    RET(r);
}
static void fs_Unmount(Ctx *c) {                         /* (const char *name): uncommitted changes are dropped */
    const char *name = STR(0);
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (m) {
        for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].h && g_open[i].mount == m) { CloseHandle(g_open[i].h); g_open[i].h = NULL; }   /* [sdk 0x1f4870] the accessor closes what is left open */
        if (m->work[0]) remove_tree(m->work);
        tomb_clear(m);
        trace_printf("     fs unmount \"%s\"\n", name);
        m->kind = 0;
    } else trace_printf("     fs unmount \"%s\": not mounted\n", name ? name : "(null)");
    fs_unlock();
}
static void fs_EnsureSaveData(Ctx *c) {                  /* (const Uid &): creates the user's save data if there is none */
    char dir[MAX_PATH];
    save_dir(dir, "save");
    RET(CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_IsSaveDataExisting(Ctx *c) { RET(save_exists()); }
static void fs_Commit(Ctx *c) {                          /* (const char *mount name) */
    const char *name = STR(0);
    char journal[MAX_PATH], marker[MAX_PATH];
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (!m) { fs_unlock(); RET(RESULT_NOT_MOUNTED); return; }
    if (m->kind == MK_ROM) { fs_unlock(); RET(0); return; }                 /* nothing to commit on a read-only file system */
    for (int i = 0; i < OPEN_MAX; i++)
        if (g_open[i].h && g_open[i].mount == m && (g_open[i].mode & 2)) { fs_unlock(); trace_printf("     fs commit \"%s\": refused, a file is open for writing\n", name); RET(RESULT_WRITE_FILE_OPEN); return; }
    HANDLE mx = commit_mutex();
    snprintf(journal, sizeof journal, "%s.journal.%lu", m->root, (unsigned long)GetCurrentProcessId());
    snprintf(marker, sizeof marker, "%s.commit", m->root);
    FILE *j = fopen(journal, "w");
    int ok = j != NULL, changes = -1;
    if (ok) {
        fprintf(j, "W\t%s\nR\t%s\n", m->work, m->root);
        for (int i = 0; i < m->tomb_n; i++) fprintf(j, "D\t%s\n", m->tomb[i]);
        journal_list(j, m->work, "");
        ok = fflush(j) == 0 && FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(j)));
        fclose(j);
    }
    if (ok) ok = MoveFileExA(journal, marker, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);      /* the commit is decided here */
    if (ok) { changes = journal_apply(marker); ok = changes >= 0; }
    if (ok) tomb_clear(m);
    commit_done(mx);
    fs_unlock();
    trace_printf("     fs commit \"%s\": %s (%d changes)\n", name, ok ? "ok" : "FAILED", changes);
    if (!ok) printf("nn::fs::Commit(\"%s\") failed on the host (Windows error %lu); the committed save data is unchanged or will be completed at the next mount\n", name, GetLastError());
    RET(ok ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_ok(Ctx *c) { RET(0); }

/* [sdk 0x2175e0] QueryMountRomCacheSize asks the RomFS reader for the working memory its metadata needs: the sizes of the
 * four tables of the RomFS image (directory hash table, directory entries, file hash table, file entries). The image
 * itself is not on this machine (only the extracted tree is), so the tables are rebuilt from the tree with the format's
 * rules: a directory entry is 0x18 bytes + its name padded to 4, a file entry 0x20 + name padded to 4, a hash table has
 * 4 bytes per bucket and the bucket count the RomFS builder derives from the entry count. NOT VERIFIED against the
 * image's own header (it would need the original romfs image). */
static uint32_t romfs_buckets(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}
static void romfs_walk(const char *dir, uint32_t *dirs, uint32_t *files, uint64_t *dir_bytes, uint64_t *file_bytes) {
    wchar_t wpat[MAX_PATH + 4];
    char pat[MAX_PATH + 4], sub[MAX_PATH], utf8[1024];
    WIN32_FIND_DATAW fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    MultiByteToWideChar(CP_ACP, 0, pat, -1, wpat, MAX_PATH + 4);
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, utf8, sizeof utf8, NULL, NULL) - 1;
        if (len < 0) len = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            (*dirs)++; *dir_bytes += 0x18 + (uint64_t)((len + 3) & ~3);
            WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, utf8, sizeof utf8, NULL, NULL);
            snprintf(sub, sizeof sub, "%s\\%s", dir, utf8);
            romfs_walk(sub, dirs, files, dir_bytes, file_bytes);
        } else { (*files)++; *file_bytes += 0x20 + (uint64_t)((len + 3) & ~3); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void fs_QueryMountRomCacheSize(Ctx *c) {          /* (size_t *out) */
    uint32_t dirs = 1, files = 0;
    uint64_t dir_bytes = 0x18, file_bytes = 0;
    fs_roots();
    romfs_walk(g_rom_root, &dirs, &files, &dir_bytes, &file_bytes);
    uint64_t size = 4ull * romfs_buckets(dirs) + dir_bytes + 4ull * romfs_buckets(files) + file_bytes;
    trace_printf("     fs QueryMountRomCacheSize: %u directories, %u files -> %llu bytes\n", dirs, files, (unsigned long long)size);
    wr64(A0, size);
    RET(0);
}

static void fs_OpenFile(Ctx *c) {                     /* (FileHandle *out, const char *path, int mode) */
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    int write = (A2 & 2) != 0;
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && write && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r) {
        int found = view_path(m, rel, path) && !is_dir(path);
        if (!found) r = RESULT_PATH_NOT_FOUND;
        else if (write && m->work[0]) {                    /* copy-on-write: the committed file is never opened for writing */
            char w[MAX_PATH];
            join(w, m->work, rel);
            if (_stricmp(w, path) != 0) { make_parents(w); if (!CopyFileA(path, w, TRUE)) r = RESULT_GENERIC_FAILURE; }
            strcpy(path, w);
        }
    }
    fs_unlock();
    if (!r) {
        h = CreateFileA(path, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) r = RESULT_PATH_NOT_FOUND;
    }
    if (InterlockedIncrement(&g_open_log) <= 200 || r)     /* D2: every failed open is logged, not only the first 200 */
        trace_printf("     fs open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r == RESULT_NOT_MOUNTED ? "NOT MOUNTED" : r ? "NOT FOUND" : "ok");
    {   static int on = -1; if (on < 0) on = getenv("SSBU_FSLOG") != NULL;       /* D2 debug: every open on stdout */
        if (on) printf("fslog: open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r ? "NOT FOUND" : "ok"); }
    if (r) { wr64(A0, 0); RET(r); return; }
    open_remember(h, (int)(A2 & 7), m);
    wr64(A0, (uint64_t)h);
    RET(0);
}
static void fs_CloseFile(Ctx *c) { if (A0) { open_mode((HANDLE)A0, 1); CloseHandle((HANDLE)A0); } }
extern int g_hitch_ms; void hitch_note(const char *s);      /* hitch.c: SSBU_HITCH */
static uint64_t read_at(HANDLE h, int64_t offset, void *buf, uint64_t size) {
    uint64_t total = 0;
    if (g_hitch_ms) { char note[80]; snprintf(note, sizeof note, "read of %llu KB", (unsigned long long)(size >> 10)); hitch_note(note); }
    { void r1_wrote(uint64_t, uint64_t); r1_wrote((uint64_t)(uintptr_t)buf, size); }   /* R1 (r1_state.c): the kernel's write into guest memory is marked (nothing without SSBU_R1_MARK) */
    while (total < size) {
        OVERLAPPED ov = {0};
        uint64_t pos = (uint64_t)offset + total;
        ov.Offset = (DWORD)pos;
        ov.OffsetHigh = (DWORD)(pos >> 32);
        DWORD chunk = size - total > 0x40000000 ? 0x40000000 : (DWORD)(size - total), got = 0;
        if (!ReadFile(h, (char *)buf + total, chunk, &got, &ov) || got == 0) break;
        total += got;
    }
    if (g_hitch_ms) hitch_note(NULL);
    return total;
}
static volatile LONG g_read_log;
static void fs_ReadFile(Ctx *c) {                     /* (handle, offset, buffer, size) */
    uint64_t got = read_at((HANDLE)A0, (int64_t)A1, (void *)(uintptr_t)A2, A3);
    if (InterlockedIncrement(&g_read_log) <= 60)
        trace_printf("     fs read offset %llx size %llx -> %llx bytes  from=%llx\n", (unsigned long long)A1,
                     (unsigned long long)A3, (unsigned long long)got, (unsigned long long)c->x[30] - 4);
    if (got != A3 && getenv("SSBU_FSLOG")) printf("fslog: ReadFile SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A0, (unsigned long long)A1, (unsigned long long)A3, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(got == A3 ? 0 : RESULT_OUT_OF_RANGE);            /* this overload must read exactly `size` bytes */
}
static void fs_ReadFileCount(Ctx *c) {                /* (size_t *outRead, handle, offset, buffer, size) */
    uint64_t got = read_at((HANDLE)A1, (int64_t)A2, (void *)(uintptr_t)A3, A4);
    wr64(A0, got);
    if (got != A4 && getenv("SSBU_FSLOG")) printf("fslog: ReadFile(count) SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A4, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(0);
}
static void fs_WriteFile(Ctx *c) {                    /* (handle, offset, buffer, size, option) */
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)A1;
    ov.OffsetHigh = (DWORD)(A1 >> 32);
    DWORD put = 0;
    int mode = open_mode((HANDLE)A0, 0);
    LARGE_INTEGER size;
    if (!(mode & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
    if (!(mode & 4) && GetFileSizeEx((HANDLE)A0, &size) && (int64_t)(A1 + A3) > size.QuadPart) { RET(RESULT_NO_APPEND); return; }   /* a file only grows in append mode */
    if (A3 && !WriteFile((HANDLE)A0, (void *)(uintptr_t)A2, (DWORD)A3, &put, &ov)) { RET(RESULT_GENERIC_FAILURE); return; }
    if (A4 && (rd32(A4) & 1)) FlushFileBuffers((HANDLE)A0);                  /* WriteOption::Flush */
    RET(0);
}
static void fs_GetFileSize(Ctx *c) {                  /* (s64 *out, handle) */
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)A1, &size)) { RET(RESULT_GENERIC_FAILURE); return; }
    wr64(A0, (uint64_t)size.QuadPart);
    RET(0);
}
/* Where a new entry goes: the working folder of a journaled mount. Result code; `path` = host path to create. */
static uint32_t create_target(const char *guest, Mount **mount, char *rel, char *path) {
    char host[MAX_PATH];
    uint32_t r = split_path(guest, mount, rel);
    if (r) return r;
    if ((*mount)->kind == MK_ROM) return RESULT_PERMISSION_DENIED;
    if (!rel[0] || view_path(*mount, rel, host)) return RESULT_PATH_ALREADY_EXISTS;
    if (!view_parent_is_dir(*mount, rel)) return RESULT_PATH_NOT_FOUND;
    join(path, (*mount)->work, rel);
    make_parents(path);
    return 0;
}
static void fs_CreateFile(Ctx *c) {                   /* (path, size) */
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) r = RESULT_GENERIC_FAILURE;
        else {
            LARGE_INTEGER size;
            size.QuadPart = (LONGLONG)A1;
            SetFilePointerEx(h, size, NULL, FILE_BEGIN);
            SetEndOfFile(h);
            CloseHandle(h);
            /* the deletion of an older file of this name stays recorded: the commit deletes it, then moves the new one in */
        }
    }
    fs_unlock();
    RET(r);
}
static void fs_DeleteFile(Ctx *c) {
    char rel[MAX_PATH], path[MAX_PATH], w[MAX_PATH], cm[MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = split_path(STR(0), &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, path) || is_dir(path))) r = RESULT_PATH_NOT_FOUND;
    if (!r) {
        join(w, m->work, rel); join(cm, m->root, rel);
        if (exists(w)) DeleteFileA(w);
        if (exists(cm)) tomb_add(m, rel);
    }
    fs_unlock();
    RET(r);
}
static void fs_CreateDirectory(Ctx *c) {
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r && !CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) r = RESULT_GENERIC_FAILURE;
    fs_unlock();
    RET(r);
}
static void fs_GetEntryType(Ctx *c) {                 /* (DirectoryEntryType *out, path): 0 directory, 1 file */
    char rel[MAX_PATH], path[MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && !view_path(m, rel, path)) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    if (r) { RET(r); return; }
    wr32(A0, is_dir(path) ? 0 : 1);
    RET(0);
}
static void fs_OpenDirectory(Ctx *c) { wr64(A0, 0); RET(RESULT_PATH_NOT_FOUND); }   /* replaced by nnfsdir.c */
static void fs_false(Ctx *c) { RET(0); }

/* ---- self-test of the mount table and of the save data journal, in a scratch folder under %TEMP% (SSBU_SDK_SELFTEST) */
uint32_t fs_mount_plain(const char *name, const char *host_dir) { fs_roots(); return mount_add(name, MK_PLAIN, host_dir); }
static int st_host_is(const char *path, const char *text) {
    char buf[64] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return text == NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return text && strcmp(buf, text) == 0;
}
static uint32_t st_write(Ctx *c, const char *guest, const char *text, uint64_t *keep_open) {
    uint64_t h = 0;
    uint32_t option = 1;
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 7; fs_OpenFile(c);
    if (c->x[0]) return (uint32_t)c->x[0];
    c->x[0] = h; c->x[1] = 0; c->x[2] = (uint64_t)(uintptr_t)text; c->x[3] = strlen(text); c->x[4] = (uint64_t)(uintptr_t)&option; fs_WriteFile(c);
    uint32_t r = (uint32_t)c->x[0];
    if (keep_open) *keep_open = h; else { c->x[0] = h; fs_CloseFile(c); }
    return r;
}
static int st_read_is(Ctx *c, const char *guest, const char *text) {
    uint64_t h = 0, got = 0;
    char buf[64] = "";
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 1; fs_OpenFile(c);
    if (c->x[0]) return text == NULL;
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = h; c->x[2] = 0; c->x[3] = (uint64_t)(uintptr_t)buf; c->x[4] = sizeof buf - 1; fs_ReadFileCount(c);
    c->x[0] = h; fs_CloseFile(c);
    buf[got] = 0;
    return text && strcmp(buf, text) == 0;
}
int nnfs_selftest(Ctx *c) {
    char saved_root[MAX_PATH], t[MAX_PATH], save[MAX_PATH], a[MAX_PATH], n[MAX_PATH], work[MAX_PATH], marker[MAX_PATH], journal[MAX_PATH];
    int bad = 0;
    uint64_t uid[2] = {1, 0}, h = 0;
    #define FSCHECK(cond, text) do { if (!(cond)) { printf("  FAIL fs journal: %s\n", text); bad++; } } while (0)
    fs_roots();
    strcpy(saved_root, g_user_root);
    GetTempPathA(MAX_PATH - 64, t);
    snprintf(t + strlen(t), 64, "ssbu_fs_selftest_%lu", (unsigned long)GetCurrentProcessId());
    remove_tree(t);
    CreateDirectoryA(t, NULL);
    strcpy(g_user_root, t);
    snprintf(save, sizeof save, "%s\\save", t); CreateDirectoryA(save, NULL);
    snprintf(a, sizeof a, "%s\\a.bin", save); snprintf(n, sizeof n, "%s\\sub\\new.bin", save);
    snprintf(work, sizeof work, "%s.work.%lu", save, (unsigned long)GetCurrentProcessId());
    snprintf(marker, sizeof marker, "%s.commit", save); snprintf(journal, sizeof journal, "%s.journal.%lu", save, (unsigned long)GetCurrentProcessId());
    FILE *f = fopen(a, "wb"); if (f) { fputs("committed", f); fclose(f); }

    c->x[0] = (uint64_t)(uintptr_t)"nosuch:/a.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == RESULT_NOT_MOUNTED, "a path with an unmounted name is not NotMounted");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == 0, "MountSaveData failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == RESULT_MOUNT_NAME_EXISTS, "mounting the same name twice is not MountNameAlreadyExists");
    /* 1. a write is visible to the game and NOT in the committed folder */
    FSCHECK(st_write(c, "st:/a.bin", "CHANGED!!", NULL) == 0, "write failed");
    FSCHECK(st_read_is(c, "st:/a.bin", "CHANGED!!"), "the game does not read back what it wrote");
    FSCHECK(st_host_is(a, "committed"), "an uncommitted write reached the committed folder");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a new file went straight to the committed folder");
    /* 2. unmount without commit: everything is rolled back */
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    FSCHECK(!exists(work), "the working folder survived Unmount");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(st_read_is(c, "st:/a.bin", "committed") && st_read_is(c, "st:/sub/new.bin", NULL), "uncommitted changes survived a remount");
    /* 3. commit is refused while a file is open for writing, then applies everything */
    FSCHECK(st_write(c, "st:/a.bin", "SECOND!!!", &h) == 0, "write failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == RESULT_WRITE_FILE_OPEN && st_host_is(a, "committed"), "Commit with a file open for writing was not refused");
    c->x[0] = h; fs_CloseFile(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(st_write(c, "st:/sub/new.bin", "", NULL) == 0, "open of a created file failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && st_host_is(a, "SECOND!!!") && exists(n) && !exists(marker), "Commit did not apply the changes");
    FSCHECK(st_read_is(c, "st:/a.bin", "SECOND!!!"), "the committed file is not read back after Commit");
    /* 4. deletion: hidden at once, done on the host at Commit */
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == 0 && exists(n) && st_read_is(c, "st:/sub/new.bin", NULL), "DeleteFile is not journaled");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == RESULT_PATH_NOT_FOUND, "deleting a deleted file is not PathNotFound");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a committed deletion did not reach the host");
    /* 5. a process killed after the commit was decided: the next mount finishes it */
    FSCHECK(st_write(c, "st:/a.bin", "THIRD!!!!", NULL) == 0, "write failed");
    f = fopen(journal, "w");
    if (f) { fprintf(f, "W\t%s\nR\t%s\n", work, save); journal_list(f, work, ""); fclose(f); }
    MoveFileExA(journal, marker, MOVEFILE_REPLACE_EXISTING);
    FSCHECK(st_host_is(a, "SECOND!!!") && exists(marker), "test setup: decided commit");
    journal_recover(save);
    FSCHECK(st_host_is(a, "THIRD!!!!") && !exists(marker), "a decided commit was not rolled forward at the next mount");
    /* 6. a process killed before the commit was decided: its working folder is dropped (pid 4 is the System process: use a dead pid) */
    {
        char dead[MAX_PATH];
        snprintf(dead, sizeof dead, "%s.work.4294967294", save);
        CreateDirectoryA(dead, NULL);
        snprintf(dead + strlen(dead), 32, "\\a.bin");
        f = fopen(dead, "wb"); if (f) { fputs("LOST", f); fclose(f); }
        journal_recover(save);
        dead[strlen(dead) - 6] = 0;
        FSCHECK(!exists(dead) && st_host_is(a, "THIRD!!!!"), "the working folder of a dead process was not discarded");
    }
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    remove_tree(t);
    strcpy(g_user_root, saved_root);
    printf("  fs: mount table (NotMounted, MountNameAlreadyExists) and save data journal (copy-on-write, rollback at Unmount, Commit refused with a\n"
           "      writer open, two-phase Commit, journaled deletion, roll-forward of a decided commit, discard of a dead process's work): %s\n", bad ? "FAILED" : "ok");
    #undef FSCHECK
    return bad;
}

/* ---- nn::account: one local user, id {1, 0}.
 * In a build with local wireless over the internet (SSBU_LDN_ONLINE, ldn_online.inc) and while it is configured,
 * players must be told apart as on consoles, where every user has a random 128-bit id (with the same id on both sides
 * the host never offered the newcomer a place): the id is then read from config\account.txt (32 hex digits), written
 * there once from random bytes; SSBU_ACCOUNT_UID=<32 hex digits> overrides it (two copies of the game on one PC). */
static uint64_t g_uid[2] = {1, 0};
#ifdef SSBU_LDN_ONLINE
const char *ldn_online_nickname(void);                     /* ldn_online.inc: non-NULL while online is configured */
static void account_uid_init(void) {
    static volatile LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) { while (done != 2) SwitchToThread(); return; }
    char exe[MAX_PATH], path[MAX_PATH + 64], text[80] = "";
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    const char *env = getenv("SSBU_ACCOUNT_UID");
    if (!ldn_online_nickname()) { done = 2; return; }
    if (env && strlen(env) >= 32) snprintf(text, sizeof text, "%s", env);
    else {
        snprintf(path, sizeof path, "%s\\..\\config\\account.txt", exe);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(text, sizeof text, f)) text[0] = 0; fclose(f); }
        else {
            uint8_t r[16] = {0};
            HCRYPTPROV prov;
            if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) { CryptGenRandom(prov, 16, r); CryptReleaseContext(prov, 0); }
            for (int i = 0; i < 16; i++) sprintf(text + i * 2, "%02x", r[i]);
            if ((f = fopen(path, "w"))) { fprintf(f, "%s\n", text); fclose(f); }
        }
    }
    if (strlen(text) >= 32) {
        char lo[17], hi[17];
        memcpy(hi, text, 16); hi[16] = 0; memcpy(lo, text + 16, 16); lo[16] = 0;
        uint64_t h = _strtoui64(hi, NULL, 16), l = _strtoui64(lo, NULL, 16);
        if (h || l) { g_uid[0] = l; g_uid[1] = h; }
        printf("nn::account: user id from %s (local wireless over the internet)\n", env && strlen(env) >= 32 ? "SSBU_ACCOUNT_UID" : "config\\account.txt");
    }
    done = 2;
}
#else
static void account_uid_init(void) {}
#endif
void account_uid(uint64_t *lo, uint64_t *hi) { account_uid_init(); *lo = g_uid[0]; *hi = g_uid[1]; }   /* also nnmisc.c */
static void acc_ListAllUsers(Ctx *c) { account_uid_init(); wr32(A0, 1); if ((int)A2 > 0) { wr64(A1, g_uid[0]); wr64(A1 + 8, g_uid[1]); } RET(0); }
static volatile LONG g_acc_user_open;                    /* the one local user has been opened (there is no CloseUser import) */
int account_user_is_open(uint64_t lo, uint64_t hi) { account_uid_init(); return g_acc_user_open && lo == g_uid[0] && hi == g_uid[1]; }
static void acc_TryOpenPreselectedUser(Ctx *c) { account_uid_init(); wr64(A0, g_uid[0]); wr64(A0 + 8, g_uid[1]); wr64(A0 + 16, 0); InterlockedExchange(&g_acc_user_open, 1); RET(1); }
static void acc_GetUserId(Ctx *c) { account_uid_init(); wr64(A0, g_uid[0]); wr64(A0 + 8, g_uid[1]); RET(0); }
/* The nickname of the console user is the name of the Windows user (SSBU_NICKNAME overrides): UTF-8, at most 32
 * bytes, NUL-terminated, in the 33-byte Nickname. Local wireless over the internet uses its own, required nickname. */
static void acc_GetNickname(Ctx *c) {
    static char name[33];
    static volatile LONG ready;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        const char *env = getenv("SSBU_NICKNAME");
        char utf8[512] = "";
#ifdef SSBU_LDN_ONLINE
        const char *online = ldn_online_nickname();       /* online: the explicit nickname, never the Windows user name */
        if (online) env = online;
#endif
        if (env && env[0]) snprintf(utf8, sizeof utf8, "%s", env);
        else {
            wchar_t wname[257] = L"";
            DWORD n = 257;
            if (GetUserNameW(wname, &n)) WideCharToMultiByte(CP_UTF8, 0, wname, -1, utf8, sizeof utf8, NULL, NULL);
        }
        if (!utf8[0]) strcpy(utf8, "Player");
        size_t len = strlen(utf8);
        if (len > 32) { len = 32; while (len > 0 && ((unsigned char)utf8[len] & 0xc0) == 0x80) len--; }   /* cut between characters */
        memcpy(name, utf8, len);
        name[len] = 0;
        ready = 2;
    }
    while (ready != 2) SwitchToThread();
    memset((void *)(uintptr_t)A0, 0, 33);
    strcpy((char *)(uintptr_t)A0, name);
    RET(0);
}
static void acc_out_true(Ctx *c) { wr8(A0, 1); RET(0); }
static void acc_out_false(Ctx *c) { wr8(A0, 0); RET(0); }
/* [sdk] 0x4b3a8 (EnsureNetworkServiceAccountAvailable): the SDK treats the account results 124-0200 .. 124-0269 from
 * the service as "no usable network service account" (it then offers the account-linking applet and returns the
 * result). An offline machine has no linked account: 124-0200 = 0x1907c, the first code of that range, is returned
 * (NOT VERIFIED: which code of the range a console gives; the linking applet is not shown, it needs the network). */
#define RESULT_ACCOUNT_NSA_UNAVAILABLE 0x1907cull
static void acc_unavailable(Ctx *c) { RET(RESULT_ACCOUNT_NSA_UNAVAILABLE); }
static void keep(Ctx *c) { (void)c; }

/* ---- nn::nifm: no network */
static void nifm_false(Ctx *c) { RET(0); }

#define FS(n) "_ZN2nn2fs" n
#define AC(n) "_ZN2nn7account" n
#define NI(n) "_ZN2nn4nifm" n
static const ImportDef fs_imports[] = {
    {FS("8MountRomEPKcPvm"), fs_MountRom}, {FS("13MountSaveDataEPKcRKNS_7account3UidE"), fs_MountSaveData}, {FS("17MountCacheStorageEPKc"), fs_MountCacheStorage},
    {FS("7UnmountEPKc"), fs_Unmount}, {FS("22QueryMountRomCacheSizeEPm"), fs_QueryMountRomCacheSize}, {FS("6CommitEPKc"), fs_Commit}, {FS("14EnsureSaveDataERKNS_7account3UidE"), fs_EnsureSaveData},
    {FS("18IsSaveDataExistingERKNS_7account3UidE"), fs_IsSaveDataExisting},
    {FS("8OpenFileEPNS0_10FileHandleEPKci"), fs_OpenFile}, {FS("9CloseFileENS0_10FileHandleE"), fs_CloseFile},
    {FS("8ReadFileENS0_10FileHandleElPvm"), fs_ReadFile}, {FS("8ReadFileEPmNS0_10FileHandleElPvm"), fs_ReadFileCount},
    {FS("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"), fs_WriteFile}, {FS("11GetFileSizeEPlNS0_10FileHandleE"), fs_GetFileSize},
    {FS("10CreateFileEPKcl"), fs_CreateFile}, {FS("10DeleteFileEPKc"), fs_DeleteFile}, {FS("15CreateDirectoryEPKc"), fs_CreateDirectory},
    {FS("12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"), fs_GetEntryType}, {FS("13OpenDirectoryEPNS0_15DirectoryHandleEPKci"), fs_OpenDirectory},
    {AC("10InitializeEv"), fs_ok}, {AC("12ListAllUsersEPiPNS0_3UidEi"), acc_ListAllUsers},
    {AC("22TryOpenPreselectedUserEPNS0_10UserHandleE"), acc_TryOpenPreselectedUser},
    {AC("9GetUserIdEPNS0_3UidERKNS0_10UserHandleE"), acc_GetUserId}, {AC("11GetNicknameEPNS0_8NicknameERKNS0_3UidE"), acc_GetNickname},
    {AC("16GetUserExistenceEPbRKNS0_3UidE"), acc_out_true},
    {AC("32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE"), acc_out_false},
    {AC("36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE"), acc_unavailable},
    {AC("25CachedNintendoAccountInfoC1Ev"), keep}, {AC("12AsyncContextC1Ev"), keep}, {AC("12AsyncContextD1Ev"), keep},
    {AC("12AsyncContext7HasDoneEPb"), acc_out_true}, {AC("12AsyncContext9GetResultEv"), acc_unavailable},
    {NI("10InitializeEv"), fs_ok}, {NI("18IsNetworkAvailableEv"), nifm_false}, {NI("22IsNetworkRequestOnHoldEv"), nifm_false},
    {NI("20SubmitNetworkRequestEv"), keep}, {NI("27SubmitNetworkRequestAndWaitEv"), keep}, {NI("20CancelNetworkRequestEv"), keep},
};

void register_fs_imports(void) {
    register_imports(fs_imports, (int)(sizeof fs_imports / sizeof fs_imports[0]));
}
