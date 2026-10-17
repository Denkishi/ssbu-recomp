/* Included by nnnet.c (round "close5", helper A): nn::account network-service-account functions and AsyncContext,
 * nn::friends requests, nn::bcat DeliveryCacheDirectory / DeliveryCacheFile, nn::ngc::ProfanityFilter object state,
 * nn::pdm::QueryApplicationPlayStatistics, nn::pl::BeginVrMode / EndVrMode.
 * Registered by c5a_register() from nnnet_register_final(), i.e. after every other table: these REPLACE the handlers of
 * the same names in nnfs.c, nnmisc.c, nnsdk.c and nnpl.c (whose table entries are now dead).
 *
 * Sources: [sdk] = binaries\13.0.4\sdk.elf (addresses quoted), [suyu] = reference\suyu\src\core\hle\service (file:line),
 * [game] = main, read with Ghidra. Console state modelled: one local user, no network connection (nn::nifm never accepts
 * an internet request), no Nintendo Account linked to the user.
 *
 * ------------------------------------------------------------------ nn::account
 * UserHandle +0x10 is the IManagerForApplication of the opened user; its commands as the SDK calls them:
 *   +0x20 CheckAvailability, +0x28 GetAccountId, +0x30 EnsureIdTokenCacheAsync, +0x38 LoadIdTokenCache, +0x40 (cmd 130).
 * [sdk 0x4b450] IsNetworkServiceAccountAvailable: CheckAvailability; 0 -> true; a result of module 124 with description
 *     200..269 -> success + false; anything else is returned. So "the user has no usable network service account" is
 *     the range 124-0200..0269. WHICH code of the range the account module gives for a user without a linked Nintendo
 *     Account is NOT in the reference ([suyu] acc/acc.cpp:713 CheckAvailability is a stub that answers success, i.e. it
 *     models a linked account) and no such constant exists in sdk.elf (tools_audit/patches/close5/c5a_scan124.py: none).
 * [sdk 0x4b380] EnsureNetworkServiceAccountAvailable: CheckAvailability; 0 -> 0; a code outside that range is returned;
 *     a code INSIDE it is discarded: GetUserId, then detail::StartPselApplet [sdk 0x3afe0 -> 0x3b010] with UiSettings
 *     {mode 2, uid} ([suyu] am/frontend/applet_profile_select.h:29 mode 2 = EnsureNetworkServiceAccountAvailable):
 *     CreateLibraryApplet(0x10); StartLibraryApplet failing -> 0x27c (124-0001); else the first u32 of the applet's 0x18
 *     byte answer is the result. The applet can only link an account over the network; offline the user can only leave
 *     it, and the applet then answers 124-0001 "cancelled by user" ([suyu] applet_profile_select.cpp:106-107). So the
 *     function gives 0x27c whichever code of the range the module used. The applet is not shown here.
 *     [game] 0x71037558a0: (result & 0x3fffff) == 0x27c -> returns 2 without an error message.
 * [sdk 0x4b500] EnsureNetworkServiceAccountIdTokenCacheAsync: FIRST detail::CheckInternetAvailability [sdk 0x3afb0]:
 *     nn::nifm::IsAnyInternetRequestAccepted(GetClientId()) false -> 0x767c (124-0059), returned before the account
 *     module is asked and before the AsyncContext is touched. [game] 0x7100249cd0 only tests it for 0.
 * AsyncContext [sdk 0x494f0 C1, 0x493b0 move C1, 0x493e0 move =, 0x49510 D1]: {vtable, 8 + 8 bytes, IAsyncContext * @0x18}.
 *     C1(): vtable (_ZTVN2nn7account12AsyncContextE + 0x10), the other three words 0.
 *     C1(&&): vtable; words @8, @0x10, @0x18 copied; the source's three words zeroed.
 *     operator=(&&): takes the source's three words (source zeroed), stores them; if the OLD pointer @0x18 was set: HasDone
 *         on it must succeed and be true (abort otherwise) and it is released. Returns this.
 *     D1: vtable; if the pointer is set: HasDone must succeed, release, pointer = 0, abort when it had not finished.
 *     HasDone [0x49750] / GetResult [0x49780] / Cancel [0x49720] / GetSystemEvent [0x49670] call the IAsyncContext
 *         (+0x30, +0x38, +0x28, +0x20) WITHOUT a null check. In this model no IAsyncContext ever exists (the only
 *         function of the game that makes one fails first, see above), so the pointer is always null and those four
 *         would fault on a console; here they stop the game with a message instead of answering anything.
 * GetNetworkServiceAccountId [0x4b4d0] (+0x28), LoadNetworkServiceAccountIdTokenCache [0x4b5d0] (+0x38),
 *     LoadCachedNintendoAccountInfo [0x4b7c0] (CheckAvailability, then +0x40): the account module's answer for a user
 *     without a network service account, returned unchanged. STILL OPEN: 0x1907c = 124-0200, the first code of the
 *     range above, is used; the exact code is not established (see IsNetworkServiceAccountAvailable).
 *
 * ------------------------------------------------------------------ nn::friends
 * detail::CheckUserStatus [sdk 0x1e0350]: fr_check_user() of nnnet.c (0x479 invalid Uid, 0x679 user not open).
 * AsyncContext [sdk 0x1de230 C1 (lifted)]: {nn::os::MutexType @0, detail::AsyncContextInternal * @0x20 = null}.
 *     GetSystemEvent(ev) [0x1de290]: lock; internal null -> 0xe79 (121-0007); else its completion event's readable handle
 *         is attached to ev ([0x1e0e80], manual clear; abort when ev is already initialised: byte ev+0x28); unlock.
 *     GetResult [0x1de390]: lock; internal null -> 0xe79; else the internal's result word (+0x610); unlock.
 *     D1 [0x1de260]: internal -> DeleteInstance; FinalizeMutex(this).
 * GetProfileList(ctx, profiles, uid, ids, count) [0x1da300]: CheckUserStatus; detail::CallAsync [0x1e0810]: an async
 *     session to the friends module (exists offline: [suyu] friend/friend.cpp CreateFriendService), an
 *     AsyncContextInternal, a thread [0x1e0c80] that sets the result to 0x1079 (121-0008) and runs the request
 *     [0x1da9d0], whose FIRST step is detail::CheckInternetRequestAccepted [0x1e0440]: nifm not accepted -> 0xc79
 *     (121-0006), the module is never asked; the context is attached (SetInternal [0x1de3e0]) and 0 returned.
 *     Here the request is finished before GetProfileList returns (no thread): result 0xc79, event signalled. A console
 *     reports 0x1079 for the few moments the thread needs.
 * GetBlockedUserList(outCount, ids, uid, offset, count) [0x1da1b0]: CheckUserStatus; module command +0x60;
 *     [suyu] friend.cpp:226-233 success with 0 entries; the count is stored only on success.
 * NotificationQueue::Pop(info) [0x1dee90]: lock; service @0x30 null -> 0x279 (121-0001); else the notification service's
 *     Pop: [suyu] friend.cpp:417-425 an empty queue answers Account::ResultNoNotifications = {module 124, 15} = 0x1e7c.
 *     NOT VERIFIED on a console: the reference uses the ACCOUNT module number for this friends result; a console may
 *     answer 121-0015 (0x1e79). Nothing is ever queued offline.
 * NotificationQueue D1 [0x1deb90]: lock; service: release, @0x30 = 0, DestroySystemEvent(this+0x38); unlock; byte @0x60
 *     set -> DestroySystemEvent(this+0x38); FinalizeMutex(this).
 * STILL OPEN (the friends module's answer is not in the reference):
 *     UpdateFriendInfo [0x1da070] (+0x40) and AddPlayHistory [0x1da870] (+0x98; two NormalizeInAppScreenName [0x1e0470]
 *         calls before it are NOT reproduced): [suyu] friend.cpp:31 and :48 have no handler. After the SDK's user check 0
 *         is returned and nothing is written: that is the reference's generic answer for an unhandled command
 *         ([suyu] service.cpp:80-83), not a known answer of the module.
 *     GetPlayHistoryRegistrationKey [0x1da700] (+0x88): [suyu] friend.cpp:260-270 is a stub: success, key not written.
 *         Same here (key untouched); a console writes a 0x40-byte key or fails for a user without an account.
 *     ShowUserDetailInfo [0x1e80c0 -> 0x1e8000]: library applet 0x1a; when it cannot be started the SDK returns 0x3c79
 *         (121-0030) [0x1e8078]. There is no such applet on a PC: 0x3c79. What the applet answers on a console when the
 *         user closes it is not established.
 *
 * ------------------------------------------------------------------ nn::bcat DeliveryCacheDirectory / DeliveryCacheFile
 * Both objects are one pointer to the module's directory / file service ([sdk 0xc3830, 0xc40c0] C1: null; lifted).
 * Directory::Open(name) [0xc3be0 -> 0xc3f80]: DirectoryName::IsValid false -> 0x27a (122-0001); already open -> 0xc7a
 *     (122-0006); ShimLibraryGlobal::CreateDirectoryService [0xc5610]: storage not mounted -> 0xa7a (122-0005); the
 *     service's Open: [suyu] bcat/delivery_cache_directory_service.cpp:55-56 a directory that does not exist is
 *     ResultFailedOpenEntity = 122-0002 = 0x47a; detail::HandleError [0xc51b0] passes it through (it aborts only for
 *     fs results 2-4000..4999); the service is released and the object stays closed.
 *     The delivery cache of a console that never synchronised is empty ([sdk] nnsdk.c: 0 directories), so every valid
 *     name gives 0x47a.
 * Directory::Read [0xc3dd0]: not open -> 0xe7a (122-0007), count not written. Close [0xc3c80] / D1 [0xc3840]: nothing
 *     when not open.
 * File::Open(dir, file) [0xc4520 -> 0xc4960]: DirectoryName or FileName [0xc1b60: 1..31 characters of A-Z a-z 0-9 _ . ,
 *     not ending in '.'] invalid -> 0x27a; already open -> 0xc7a; CreateFileService: not mounted -> 0xa7a; the service's
 *     Open: [suyu] bcat/delivery_cache_file_service.cpp:42-43 -> 0x47a. Close [0xc45d0] / D1 [0xc40d0]: nothing when
 *     not open.
 *
 * ------------------------------------------------------------------ nn::ngc::ProfanityFilter
 * The filter runs inside the SDK ([0x30cc50], [0x30d9b0]) on word lists that ContentsReader::MountContents [0x30b7e0]
 * loads from a system data archive. That archive is firmware content and NOT in the dump.
 *     C1 [0x30b6a0]: vtable, byte @9 (initialised) = 0, u32 @0xc (skip mode) = 0, u64 @0x20 = 0, u32 @0x28 (mask mode) = 0.
 *     SetMaskMode [0x30b9e0]: u32 @0x28 = mode. SkipAtSignCheck [0x30b9d0]: u32 @0xc = mode.
 *     Initialize(buf, size) [0x30b9b0 -> 0x30b700 with flag 1]: buf null -> 0x692 (146-0003); size < 0x20000 -> 0x892
 *         (146-0004); already initialised -> 0x492 (146-0002); modes reset to 0; work buffer set up; contents mounted;
 *         byte @8 = flag, byte @9 = 1.
 *     MaskProfanityWordsInText(outCount, text, patterns) [0x30d5b0]: not initialised -> 0x292 (146-0001); text null ->
 *         0x692; then the masking.
 *     D1 [0x30b840]: vtable; initialised -> contents reference dropped, byte @9 = 0.
 * Here: the object state and every argument check are the SDK's. STILL OPEN: Initialize succeeds without mounting
 * anything and MaskProfanityWordsInText masks nothing (0 words): the faithful answer needs the word lists.
 *
 * ------------------------------------------------------------------ nn::pdm, nn::pl
 * pdm::QueryApplicationPlayStatistics(out, ids, count) [0x341240]: IApplicationFunctions command +0x128; abort on
 *     failure; returns the number of entries. [suyu] am/service/application_functions.cpp:413-420: 0 entries. A console
 *     that has run the queried applications answers with their play counts / times.
 * pl::BeginVrMode [0x341e90]: nn::pctl::CheckStereoVisionPermission(false) false -> 0xd08e (142-0104); else
 *     ICommonStateGetter +0xd0 (BeginVrModeEx; abort on failure) and 0. pl::EndVrMode [0x341f00]: +0xd8 (EndVrModeEx).
 *     [suyu] am/service/common_state_getter.cpp:225-237: the module only keeps a flag. The console also changes the
 *     panel's brightness handling in VR mode; nothing of that exists on a PC. */
void os_signal_event(uint64_t key);                       /* os_threads.c */

/* ---------------------------------------------------------------- nn::account */
#define ACC_CANCELLED 0x27cu                               /* 124-0001 */
#define ACC_NO_INTERNET 0x767cu                            /* 124-0059 */
#define ACC_NSA_UNAVAILABLE_UNVERIFIED 0x1907cu            /* 124-0200: first code of the SDK's range; exact code open */
static uint64_t c5a_acc_vtable(void) { uint64_t v = sdk_symbol("_ZTVN2nn7account12AsyncContextE"); return v ? v + 0x10 : 0; }
static void c5a_acc_no_context(Ctx *c, const char *what) {
    printf("\nnn::account::AsyncContext::%s on a context that holds no request ([sdk] null pointer at +0x18: a console faults here).\n", what);
    guest_trap("nn::account::AsyncContext used without a request", c->x[30] - 4);
}
static void c5a_acc_release_old(Ctx *c, uint64_t old) {
    if (old) c5a_acc_no_context(c, "destructor / move assignment (a request pointer exists, which this model never creates)");
}
static void c5a_acc_Async_ctor(Ctx *c) { uint64_t s = A0; wr64(s, c5a_acc_vtable()); wr64(s + 8, 0); wr64(s + 0x10, 0); wr64(s + 0x18, 0); }
static void c5a_acc_Async_move_ctor(Ctx *c) {
    uint64_t s = A0, o = A1;
    wr64(s, c5a_acc_vtable());
    wr64(s + 8, rd64(o + 8)); wr64(s + 0x10, rd64(o + 0x10)); wr64(s + 0x18, rd64(o + 0x18));
    wr64(o + 0x10, 0); wr64(o + 0x18, 0); wr64(o + 8, 0);
}
static void c5a_acc_Async_move_assign(Ctx *c) {
    uint64_t s = A0, o = A1;
    uint64_t a = rd64(o + 8), b = rd64(o + 0x10), p = rd64(o + 0x18);
    wr64(o + 8, 0); wr64(o + 0x10, 0); wr64(o + 0x18, 0);
    uint64_t old = rd64(s + 0x18);
    wr64(s + 8, a); wr64(s + 0x10, b); wr64(s + 0x18, p);
    c5a_acc_release_old(c, old);
    RET(s);
}
static void c5a_acc_Async_dtor(Ctx *c) {
    uint64_t s = A0;
    wr64(s, c5a_acc_vtable());
    c5a_acc_release_old(c, rd64(s + 0x18));
    RET(s);
}
static void c5a_acc_Async_HasDone(Ctx *c) { c5a_acc_no_context(c, "HasDone"); }
static void c5a_acc_Async_GetResult(Ctx *c) { c5a_acc_no_context(c, "GetResult"); }
static void c5a_acc_Async_Cancel(Ctx *c) { c5a_acc_no_context(c, "Cancel"); }
static void c5a_acc_Async_GetSystemEvent(Ctx *c) { c5a_acc_no_context(c, "GetSystemEvent"); }
static void c5a_acc_EnsureAvailable(Ctx *c) { RET(ACC_CANCELLED); }
static void c5a_acc_EnsureIdTokenCacheAsync(Ctx *c) { RET(ACC_NO_INTERNET); }
static void c5a_acc_nsa_unavailable(Ctx *c) { RET(ACC_NSA_UNAVAILABLE_UNVERIFIED); }

/* ---------------------------------------------------------------- nn::friends */
#define FR_NOT_CALLED 0xe79u                               /* 121-0007 */
#define FR_NO_INTERNET 0xc79u                              /* 121-0006 */
#define FR_QUEUE_NOT_INITIALIZED 0x279u                    /* 121-0001 */
#define FR_NO_NOTIFICATION 0x1e7cu                         /* [suyu] {124, 15} */
#define FR_APPLET_NOT_STARTED 0x3c79u                      /* 121-0030 */
typedef struct { uint32_t magic, result; } C5aFriendsAsync;   /* stands for detail::AsyncContextInternal: a finished request */
#define C5A_FR_MAGIC 0x63356166u
static C5aFriendsAsync *c5a_fr_internal(uint64_t ctx) {
    C5aFriendsAsync *a = (C5aFriendsAsync *)(uintptr_t)rd64(ctx + 0x20);
    if (a && a->magic != C5A_FR_MAGIC) guest_trap("nn::friends::AsyncContext: the pointer at +0x20 is not a request of this model", ctx);
    return a;
}
static void c5a_fr_Async_GetSystemEvent(Ctx *c) {         /* (this, SystemEvent *) */
    uint64_t self = A0, ev = A1;
    uint32_t r = FR_NOT_CALLED;
    fr_lock(c, self, 1);
    if (c5a_fr_internal(self)) { os_init_event(ev, 0); os_signal_event(ev); r = 0; }
    fr_lock(c, self, 0);
    RET(r);
}
static void c5a_fr_Async_GetResult(Ctx *c) {
    uint64_t self = A0;
    fr_lock(c, self, 1);
    C5aFriendsAsync *a = c5a_fr_internal(self);
    uint32_t r = a ? a->result : FR_NOT_CALLED;
    fr_lock(c, self, 0);
    RET(r);
}
static void c5a_fr_Async_dtor(Ctx *c) {
    uint64_t self = A0;
    C5aFriendsAsync *a = c5a_fr_internal(self);
    if (a) { a->magic = 0; free(a); }
    ImportFn fin = import_lookup("_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE");
    c->x[0] = self;
    if (fin) fin(c);
    RET(self);
}
static void c5a_fr_GetProfileList(Ctx *c) {                /* (AsyncContext *, Profile *, const Uid &, const NetworkServiceAccountId *, int) */
    uint64_t ctx = A0;
    uint32_t r = fr_check_user(c, A2);
    if (r) { RET(r); return; }
    C5aFriendsAsync *a = malloc(sizeof *a);
    a->magic = C5A_FR_MAGIC;
    a->result = FR_NO_INTERNET;                             /* [sdk 0x1da9e8] the request stops at CheckInternetRequestAccepted */
    fr_lock(c, ctx, 1);                                     /* SetInternal [sdk 0x1de3e0]: the previous request is deleted */
    C5aFriendsAsync *old = c5a_fr_internal(ctx);
    if (old) { old->magic = 0; free(old); }
    wr64(ctx + 0x20, P(a));
    fr_lock(c, ctx, 0);
    RET(0);
}
static void c5a_fr_GetBlockedUserList(Ctx *c) {            /* (int *outCount, NetworkServiceAccountId *, const Uid &, int offset, int count) */
    uint64_t out = A0;
    uint32_t r = fr_check_user(c, A2);
    if (!r) wr32(out, 0);
    RET(r);
}
static void c5a_fr_UpdateFriendInfo(Ctx *c) { RET(fr_check_user(c, A1)); }               /* (Friend *, const Uid &, ids, count): module answer open */
static void c5a_fr_AddPlayHistory(Ctx *c) { RET(fr_check_user(c, A0)); }                 /* (const Uid &, key, name, myName): module answer open */
static void c5a_fr_GetPlayHistoryRegistrationKey(Ctx *c) { RET(fr_check_user(c, A1)); }  /* (key *, const Uid &, bool): module answer open */
static void c5a_fr_ShowUserDetailInfo(Ctx *c) { RET(FR_APPLET_NOT_STARTED); }
static void c5a_fr_NQ_Pop(Ctx *c) {                        /* (this, NotificationInfo *) */
    uint64_t self = A0;
    fr_lock(c, self, 1);
    uint32_t r = rd64(self + 0x30) ? FR_NO_NOTIFICATION : FR_QUEUE_NOT_INITIALIZED;
    fr_lock(c, self, 0);
    RET(r);
}
static void c5a_fr_NQ_dtor(Ctx *c) {
    uint64_t self = A0;
    ImportFn destroy = import_lookup("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    ImportFn fin = import_lookup("_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE");
    fr_lock(c, self, 1);
    if (rd64(self + 0x30)) {
        wr64(self + 0x30, 0);
        c->x[0] = self + 0x38;
        if (destroy) destroy(c);
    }
    fr_lock(c, self, 0);
    if (rd8(self + 0x60)) { c->x[0] = self + 0x38; if (destroy) destroy(c); }
    c->x[0] = self;
    if (fin) fin(c);
    RET(self);
}

/* ---------------------------------------------------------------- nn::bcat */
#define BCAT_INVALID_ARG 0x27au
#define BCAT_NOT_FOUND 0x47au
#define BCAT_NOT_MOUNTED 0xa7au
#define BCAT_ALREADY_OPEN 0xc7au
#define BCAT_NOT_OPEN 0xe7au
extern volatile LONG g_bcat_mounted;                      /* nnsdk.c */
int bcat_dirname_valid(const char *s);                    /* nnsdk.c: [sdk 0xc1be0] */
static int c5a_bcat_dir_ok(uint64_t name) { const char *s = (const char *)(uintptr_t)name; return strnlen(s, 32) < 32 && bcat_dirname_valid(s); }
static int c5a_bcat_file_ok(uint64_t name) {              /* [sdk 0xc1b60] */
    const unsigned char *s = (const unsigned char *)(uintptr_t)name;
    unsigned n = 0;
    for (; n < 32; n++) {
        unsigned ch = s[n];
        if (!ch) break;
        if (!((ch >= '0' && ch <= '9') || ((ch & 0xdf) >= 'A' && (ch & 0xdf) <= 'Z') || ch == '_' || ch == '.')) return 0;
    }
    if (n == 0 || n == 32) return 0;
    return s[n - 1] != '.';
}
static uint32_t c5a_bcat_open(uint64_t self, int names_ok) {
    if (!names_ok) return BCAT_INVALID_ARG;
    if (rd64(self)) return BCAT_ALREADY_OPEN;
    if (!g_bcat_mounted) return BCAT_NOT_MOUNTED;
    return BCAT_NOT_FOUND;                                  /* the module: nothing was ever delivered */
}
static void c5a_bcat_Dir_Open(Ctx *c) { RET(c5a_bcat_open(A0, c5a_bcat_dir_ok(A1))); }                              /* (this, const DirectoryName &) */
static void c5a_bcat_File_Open(Ctx *c) { RET(c5a_bcat_open(A0, c5a_bcat_dir_ok(A1) && c5a_bcat_file_ok(A2))); }      /* (this, const DirectoryName &, const FileName &) */
static void c5a_bcat_Dir_Read(Ctx *c) {                    /* (this, int *outCount, DeliveryCacheDirectoryEntry *, int) */
    if (rd64(A0)) guest_trap("nn::bcat::DeliveryCacheDirectory::Read on an open directory (this model never opens one)", c->x[30] - 4);
    RET(BCAT_NOT_OPEN);
}
static void c5a_bcat_close(Ctx *c) {                       /* Close and the destructors: nothing when not open */
    if (rd64(A0)) guest_trap("nn::bcat delivery cache object is open (this model never opens one)", c->x[30] - 4);
}

/* ---------------------------------------------------------------- nn::ngc */
#define NGC_NOT_INITIALIZED 0x292u
#define NGC_ALREADY_INITIALIZED 0x492u
#define NGC_NULL_POINTER 0x692u
#define NGC_SMALL_BUFFER 0x892u
static void c5a_ngc_ctor(Ctx *c) {
    uint64_t s = A0, vt = sdk_symbol("_ZTVN2nn3ngc15ProfanityFilterE");
    wr8(s + 9, 0); wr32(s + 0xc, 0); wr64(s + 0x20, 0); wr64(s, vt ? vt + 0x10 : 0); wr32(s + 0x28, 0);
}
static void c5a_ngc_dtor(Ctx *c) {
    uint64_t s = A0, vt = sdk_symbol("_ZTVN2nn3ngc15ProfanityFilterE");
    wr64(s, vt ? vt + 0x10 : 0);
    if (rd8(s + 9)) wr8(s + 9, 0);
    RET(s);
}
static void c5a_ngc_SetMaskMode(Ctx *c) { wr32(A0 + 0x28, (uint32_t)A1); }
static void c5a_ngc_SkipAtSignCheck(Ctx *c) { wr32(A0 + 0xc, (uint32_t)A1); }
static void c5a_ngc_Initialize(Ctx *c) {                   /* (this, void *workBuffer, size_t size) */
    uint64_t s = A0;
    if (!A1) { RET(NGC_NULL_POINTER); return; }
    if (A2 < 0x20000) { RET(NGC_SMALL_BUFFER); return; }
    if (rd8(s + 9)) { RET(NGC_ALREADY_INITIALIZED); return; }
    wr32(s + 0xc, 0); wr32(s + 0x28, 0);
    wr8(s + 8, 1); wr8(s + 9, 1);                           /* OPEN: no contents are mounted (the word lists are not in the dump) */
    RET(0);
}
static void c5a_ngc_Mask(Ctx *c) {                         /* (this, int *outCount, char16_t *text, Bit32 patterns) */
    uint32_t r = !A2 ? NGC_NULL_POINTER : 0;
    if (!rd8(A0 + 9)) r = NGC_NOT_INITIALIZED;
    if (!r && A1) wr32(A1, 0);                              /* OPEN: nothing is masked */
    RET(r);
}

/* ---------------------------------------------------------------- nn::pdm, nn::pl */
static void c5a_pdm_Query(Ctx *c) { RET(0); }              /* number of entries written */
static volatile LONG g_c5a_vr_mode;                        /* the flag the am module keeps ([suyu] common_state_getter.cpp:225) */
static void c5a_pl_BeginVrMode(Ctx *c) {
    ImportFn permit = import_lookup("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    c->x[0] = 0;
    if (permit) permit(c);
    if (!permit || !(c->x[0] & 1)) { RET(0xd08eu); return; }
    InterlockedExchange(&g_c5a_vr_mode, 1);
    RET(0);
}
static void c5a_pl_EndVrMode(Ctx *c) { InterlockedExchange(&g_c5a_vr_mode, 0); RET(0); }

static const ImportDef c5a_defs[] = {
    {"_ZN2nn7account12AsyncContextC1Ev", c5a_acc_Async_ctor}, {"_ZN2nn7account12AsyncContextC1EOS1_", c5a_acc_Async_move_ctor},
    {"_ZN2nn7account12AsyncContextaSEOS1_", c5a_acc_Async_move_assign}, {"_ZN2nn7account12AsyncContextD1Ev", c5a_acc_Async_dtor},
    {"_ZN2nn7account12AsyncContext7HasDoneEPb", c5a_acc_Async_HasDone}, {"_ZN2nn7account12AsyncContext9GetResultEv", c5a_acc_Async_GetResult},
    {"_ZN2nn7account12AsyncContext6CancelEv", c5a_acc_Async_Cancel},
    {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", c5a_acc_Async_GetSystemEvent},
    {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", c5a_acc_EnsureAvailable},
    {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", c5a_acc_EnsureIdTokenCacheAsync},
    {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", c5a_acc_nsa_unavailable},
    {"_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm", c5a_acc_nsa_unavailable},
    {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", c5a_acc_nsa_unavailable},
    {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", c5a_fr_Async_GetSystemEvent},
    {"_ZN2nn7friends12AsyncContextD1Ev", c5a_fr_Async_dtor}, {"_ZNK2nn7friends12AsyncContext9GetResultEv", c5a_fr_Async_GetResult},
    {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", c5a_fr_GetProfileList},
    {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", c5a_fr_GetBlockedUserList},
    {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", c5a_fr_UpdateFriendInfo},
    {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", c5a_fr_AddPlayHistory},
    {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", c5a_fr_GetPlayHistoryRegistrationKey},
    {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", c5a_fr_ShowUserDetailInfo},
    {"_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE", c5a_fr_NQ_Pop}, {"_ZN2nn7friends17NotificationQueueD1Ev", c5a_fr_NQ_dtor},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", c5a_bcat_Dir_Open},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", c5a_bcat_Dir_Read},
    {"_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv", c5a_bcat_close}, {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", c5a_bcat_close},
    {"_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", c5a_bcat_File_Open},
    {"_ZN2nn4bcat17DeliveryCacheFile5CloseEv", c5a_bcat_close}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", c5a_bcat_close},
    {"_ZN2nn3ngc15ProfanityFilterC1Ev", c5a_ngc_ctor}, {"_ZN2nn3ngc15ProfanityFilterD1Ev", c5a_ngc_dtor},
    {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", c5a_ngc_Initialize},
    {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", c5a_ngc_SetMaskMode},
    {"_ZN2nn3ngc15ProfanityFilter15SkipAtSignCheckENS1_8SkipModeE", c5a_ngc_SkipAtSignCheck},
    {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", c5a_ngc_Mask},
    {"_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsEPKNS_13ApplicationIdEi", c5a_pdm_Query},
    {"_ZN2nn2pl11BeginVrModeEv", c5a_pl_BeginVrMode}, {"_ZN2nn2pl9EndVrModeEv", c5a_pl_EndVrMode},
};
static void c5a_register(void) {
    int n = (int)(sizeof c5a_defs / sizeof c5a_defs[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(c5a_defs[i].name)) printf("nnsvc_c5a: \"%s\" is not an import of the game (typo?)\n", c5a_defs[i].name);
    register_imports(c5a_defs, n);
}

/* ---------------------------------------------------------------- self-test */
#define C5A_CHECK(cond, what) do { cases++; if (!(cond)) { printf("    c5a FAILED: %s\n", what); bad++; } } while (0)
static int c5a_selftest(Ctx *c) {
    int bad = 0, cases = 0;
    /* every name is answered by the handler of this file */
    for (int i = 0; i < (int)(sizeof c5a_defs / sizeof c5a_defs[0]); i++)
        if (import_lookup(c5a_defs[i].name) != c5a_defs[i].fn) { printf("    c5a FAILED: %s is answered by another handler\n", c5a_defs[i].name); bad++; }
    cases++;

    /* nn::account */
    uint64_t a[4], b[4], handle[3] = {1, 0, 0};
    memset(a, 0xee, sizeof a); memset(b, 0xee, sizeof b);
    c->x[0] = P(a); h("_ZN2nn7account12AsyncContextC1Ev")(c);
    C5A_CHECK(a[0] == c5a_acc_vtable() && a[0] && !a[1] && !a[2] && !a[3], "account AsyncContext(): vtable + three zero words");
    a[1] = 0x11; a[2] = 0x22;
    c->x[0] = P(b); c->x[1] = P(a); h("_ZN2nn7account12AsyncContextC1EOS1_")(c);
    C5A_CHECK(b[0] == a[0] && b[1] == 0x11 && b[2] == 0x22 && !b[3] && !a[1] && !a[2] && !a[3], "account AsyncContext(AsyncContext &&): words moved, source zeroed");
    c->x[0] = P(a); c->x[1] = P(b); h("_ZN2nn7account12AsyncContextaSEOS1_")(c);
    C5A_CHECK(c->x[0] == P(a) && a[1] == 0x11 && a[2] == 0x22 && !a[3] && !b[1] && !b[2] && !b[3], "account AsyncContext::operator=(&&): words moved, returns this");
    c->x[0] = P(a); h("_ZN2nn7account12AsyncContextD1Ev")(c);
    C5A_CHECK(a[0] == c5a_acc_vtable() && !a[3], "account ~AsyncContext without a request");
    c->x[0] = P(handle); h("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x27c && (c->x[0] & 0x1ff) == 124 && ((c->x[0] >> 9) & 0x1fff) == 1, "EnsureNetworkServiceAccountAvailable = 124-0001");
    c->x[0] = P(b); c->x[1] = P(handle); h("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x767c && ((c->x[0] >> 9) & 0x1fff) == 59 && !b[3], "EnsureNetworkServiceAccountIdTokenCacheAsync = 124-0059, context untouched");
    {
        uint8_t avail = 0xee;
        uint64_t id = 0xeeeeeeeeeeeeeeeeull;
        c->x[0] = P(&avail); c->x[1] = P(handle); h("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE")(c);
        C5A_CHECK(c->x[0] == 0 && avail == 0, "IsNetworkServiceAccountAvailable = success, false");
        c->x[0] = P(&id); c->x[1] = P(handle); h("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE")(c);
        uint32_t r = (uint32_t)c->x[0], desc = (r >> 9) & 0x1fff;
        C5A_CHECK((r & 0x1ff) == 124 && desc >= 200 && desc < 270 && id == 0xeeeeeeeeeeeeeeeeull, "GetNetworkServiceAccountId: a result of the SDK's 'unavailable' range, id untouched");
    }

    /* nn::friends */
    uint8_t ctx[0x40], ev[0x40], queue[0x80];
    uint64_t uid_bad[2] = {0, 0}, uid[2] = {1, 0}, open_handle[3];
    int count = 77;
    { void account_uid(uint64_t *lo, uint64_t *hi); account_uid(&uid[0], &uid[1]); }     /* nnfs.c: {1, 0}, or the online id (ldn_online.inc) */
    c->x[0] = P(open_handle); h("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE")(c);      /* the user must be open for CheckUserStatus */
    memset(ctx, 0xee, sizeof ctx);
    c->x[0] = P(ctx); h("_ZN2nn7friends12AsyncContextC1Ev")(c);
    C5A_CHECK(rd64(P(ctx) + 0x20) == 0, "friends AsyncContext(): no request");
    c->x[0] = P(ctx); h("_ZNK2nn7friends12AsyncContext9GetResultEv")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0xe79, "friends AsyncContext::GetResult without a request = 121-0007");
    c->x[0] = P(ctx); c->x[1] = P(ev); h("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0xe79, "friends AsyncContext::GetSystemEvent without a request = 121-0007");
    c->x[0] = P(ctx); c->x[1] = 0; c->x[2] = P(uid_bad); c->x[3] = 0; c->x[4] = 0;
    h("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x479 && rd64(P(ctx) + 0x20) == 0, "GetProfileList with an invalid Uid = 121-0002, no request");
    c->x[0] = P(ctx); c->x[1] = 0; c->x[2] = P(uid); c->x[3] = 0; c->x[4] = 0;
    h("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi")(c);
    C5A_CHECK(c->x[0] == 0 && rd64(P(ctx) + 0x20) != 0, "GetProfileList starts a request");
    c->x[0] = P(ctx); h("_ZNK2nn7friends12AsyncContext9GetResultEv")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0xc79, "the request ends with 121-0006 (no internet request accepted)");
    c->x[0] = P(ctx); c->x[1] = P(ev); h("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE")(c);
    C5A_CHECK(c->x[0] == 0, "friends AsyncContext::GetSystemEvent with a request");
    c->x[0] = P(ev); h("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE")(c);
    C5A_CHECK((c->x[0] & 1) == 1, "the completion event is signalled");
    c->x[0] = P(ctx); h("_ZN2nn7friends12AsyncContextD1Ev")(c);
    c->x[0] = P(&count); c->x[1] = 0; c->x[2] = P(uid); c->x[3] = 0; c->x[4] = 10;
    h("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii")(c);
    C5A_CHECK(c->x[0] == 0 && count == 0, "GetBlockedUserList = success, 0");
    count = 77;
    c->x[0] = P(&count); c->x[1] = 0; c->x[2] = P(uid_bad); c->x[3] = 0; c->x[4] = 10;
    h("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x479 && count == 77, "GetBlockedUserList with an invalid Uid = 121-0002, count untouched");
    memset(queue, 0xee, sizeof queue);
    c->x[0] = P(queue); h("_ZN2nn7friends17NotificationQueueC1Ev")(c);
    c->x[0] = P(queue); c->x[1] = P(ev); h("_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x279, "NotificationQueue::Pop before Initialize = 121-0001");
    c->x[0] = P(queue); c->x[1] = P(uid); h("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE")(c);
    C5A_CHECK(c->x[0] == 0, "NotificationQueue::Initialize");
    c->x[0] = P(queue); c->x[1] = P(ev); h("_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x1e7c, "NotificationQueue::Pop on an empty queue = the reference's 'no notifications'");
    c->x[0] = P(queue); h("_ZN2nn7friends17NotificationQueueD1Ev")(c);
    C5A_CHECK(rd64(P(queue) + 0x30) == 0, "~NotificationQueue releases the service");
    for (int i = 0; i < (int)(sizeof c5a_defs / sizeof c5a_defs[0]); i++) {
        ImportFn f = c5a_defs[i].fn;
        if (f != c5a_acc_EnsureAvailable && f != c5a_acc_EnsureIdTokenCacheAsync && f != c5a_acc_nsa_unavailable && f != c5a_fr_ShowUserDetailInfo
            && f != c5a_fr_UpdateFriendInfo && f != c5a_fr_AddPlayHistory && f != c5a_fr_GetPlayHistoryRegistrationKey) continue;
        c->x[0] = P(uid); c->x[1] = P(uid); c->x[2] = P(uid); c->x[3] = 0;
        f(c);
        if ((uint32_t)c->x[0] == 0x3e87a) { printf("    c5a FAILED: %s still answers 0x3e87a\n", c5a_defs[i].name); bad++; }
    }
    cases++;

    /* nn::bcat */
    uint64_t dir = 0xeeeeeeeeeeeeeeeeull, file = 0xeeeeeeeeeeeeeeeeull;
    char dname[32], fname[32];
    int was_mounted = g_bcat_mounted;
    c->x[0] = P(&dir); h("_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev")(c);
    c->x[0] = P(&file); h("_ZN2nn4bcat17DeliveryCacheFileC1Ev")(c);
    C5A_CHECK(dir == 0 && file == 0, "bcat constructors: closed objects");
    memset(dname, 0, sizeof dname); memset(fname, 0, sizeof fname);
    strcpy(dname, "bad name");
    c->x[0] = P(&dir); c->x[1] = P(dname); h("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x27a, "Directory::Open with an invalid name = 122-0001");
    strcpy(dname, "spirits");
    if (!was_mounted) {
        c->x[0] = P(&dir); c->x[1] = P(dname); h("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE")(c);
        C5A_CHECK((uint32_t)c->x[0] == 0xa7a, "Directory::Open without a mounted storage = 122-0005");
        h("_ZN2nn4bcat25MountDeliveryCacheStorageEv")(c);
    }
    c->x[0] = P(&dir); c->x[1] = P(dname); h("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x47a && dir == 0, "Directory::Open in an empty delivery cache = 122-0002, still closed");
    count = 77;
    c->x[0] = P(&dir); c->x[1] = P(&count); c->x[2] = 0; c->x[3] = 4; h("_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0xe7a && count == 77, "Directory::Read when not open = 122-0007, count untouched");
    static const struct { const char *name; int ok; } fn[] = {{"a.bin", 1}, {"A_9.msbt", 1}, {"", 0}, {"end.", 0}, {"sp ace", 0}, {"dash-x", 0},
                                                              {"0123456789012345678901234567890", 1}, {"01234567890123456789012345678901", 0}};
    int names_ok = 1;
    for (int i = 0; i < (int)(sizeof fn / sizeof fn[0]); i++) {
        char buf[40];
        memset(buf, 0, sizeof buf);
        memcpy(buf, fn[i].name, strlen(fn[i].name));
        if (c5a_bcat_file_ok(P(buf)) != fn[i].ok) names_ok = 0;
    }
    C5A_CHECK(names_ok, "FileName rule of sdk 0xc1b60 (8 names)");
    strcpy(fname, "end.");
    c->x[0] = P(&file); c->x[1] = P(dname); c->x[2] = P(fname); h("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x27a, "File::Open with an invalid file name = 122-0001");
    strcpy(fname, "a.bin");
    c->x[0] = P(&file); c->x[1] = P(dname); c->x[2] = P(fname); h("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x47a && file == 0, "File::Open in an empty delivery cache = 122-0002, still closed");
    c->x[0] = P(&dir); h("_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv")(c);
    c->x[0] = P(&file); h("_ZN2nn4bcat17DeliveryCacheFile5CloseEv")(c);
    c->x[0] = P(&dir); h("_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev")(c);
    c->x[0] = P(&file); h("_ZN2nn4bcat17DeliveryCacheFileD1Ev")(c);
    C5A_CHECK(dir == 0 && file == 0, "Close / destructors of closed objects do nothing");
    if (!was_mounted) h("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv")(c);

    /* nn::ngc */
    uint8_t filter[0x40];
    static uint16_t text[] = {'h', 'e', 'l', 'l', 'o', 0};
    static uint8_t work[16];
    memset(filter, 0xee, sizeof filter);
    c->x[0] = P(filter); h("_ZN2nn3ngc15ProfanityFilterC1Ev")(c);
    C5A_CHECK(filter[9] == 0 && rd32(P(filter) + 0xc) == 0 && rd64(P(filter) + 0x20) == 0 && rd32(P(filter) + 0x28) == 0 && filter[8] == 0xee && rd64(P(filter)) != 0,
              "ProfanityFilter(): fields of sdk 0x30b6a0");
    count = 77;
    c->x[0] = P(filter); c->x[1] = P(&count); c->x[2] = P(text); c->x[3] = 0; h("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x292 && count == 77, "MaskProfanityWordsInText before Initialize = 146-0001");
    c->x[0] = P(filter); c->x[1] = 0; c->x[2] = 0x20000; h("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x692, "Initialize(null) = 146-0003");
    c->x[0] = P(filter); c->x[1] = P(work); c->x[2] = 0x1ffff; h("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x892 && filter[9] == 0, "Initialize with a buffer under 0x20000 = 146-0004");
    c->x[0] = P(filter); c->x[1] = 3; h("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE")(c);
    c->x[0] = P(filter); c->x[1] = 1; h("_ZN2nn3ngc15ProfanityFilter15SkipAtSignCheckENS1_8SkipModeE")(c);
    C5A_CHECK(rd32(P(filter) + 0x28) == 3 && rd32(P(filter) + 0xc) == 1, "SetMaskMode / SkipAtSignCheck store their argument");
    c->x[0] = P(filter); c->x[1] = P(work); c->x[2] = 0x20000; h("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm")(c);
    C5A_CHECK(c->x[0] == 0 && filter[9] == 1 && rd32(P(filter) + 0x28) == 0 && rd32(P(filter) + 0xc) == 0, "Initialize: initialised, modes reset");
    c->x[0] = P(filter); c->x[1] = P(work); c->x[2] = 0x20000; h("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x492, "second Initialize = 146-0002");
    c->x[0] = P(filter); c->x[1] = P(&count); c->x[2] = 0; c->x[3] = 0; h("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj")(c);
    C5A_CHECK((uint32_t)c->x[0] == 0x692, "MaskProfanityWordsInText(null text) = 146-0003");
    c->x[0] = P(filter); h("_ZN2nn3ngc15ProfanityFilterD1Ev")(c);
    C5A_CHECK(filter[9] == 0, "~ProfanityFilter: no longer initialised");

    /* nn::pdm, nn::pl */
    uint64_t stats[4] = {0xee, 0xee, 0xee, 0xee}, ids[1] = {0x0100000000010000ull};
    c->x[0] = P(stats); c->x[1] = P(ids); c->x[2] = 1; h("_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsEPKNS_13ApplicationIdEi")(c);
    C5A_CHECK((int)c->x[0] == 0 && stats[0] == 0xee, "QueryApplicationPlayStatistics = 0 entries, buffer untouched");
    c->x[0] = 0; h("_ZN2nn4pctl27CheckStereoVisionPermissionEb")(c);
    int permitted = (int)(c->x[0] & 1);
    h("_ZN2nn2pl11BeginVrModeEv")(c);
    C5A_CHECK(permitted ? (c->x[0] == 0 && g_c5a_vr_mode == 1) : ((uint32_t)c->x[0] == 0xd08e && g_c5a_vr_mode == 0), "BeginVrMode follows CheckStereoVisionPermission (0xd08e when refused)");
    h("_ZN2nn2pl9EndVrModeEv")(c);
    C5A_CHECK(g_c5a_vr_mode == 0, "EndVrMode clears the flag");

    printf("  c5a: account AsyncContext + network service account answers, friends requests, bcat directory / file, ngc filter state,\n"
           "       pdm, pl VR mode: %d cases: %s\n", cases, bad ? "FAILED" : "ok");
    return bad;
}
