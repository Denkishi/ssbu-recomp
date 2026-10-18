/* Library applets and small device services, as an OFFLINE console answers them:
 * nn::web (offline HTML page, web page, YouTube, lobby), nn::swkbd::ShowKeyboard, nn::ec shop pages,
 * the amiibo settings applets, nn::nfp (NFC reader with no amiibo ever presented), nn::oe volume balance.
 *
 * Everything that only fills a caller structure (nn::web *Arg constructors and setters, *ReturnValue
 * constructors and getters, nn::swkbd MakePreset / Set*Text / SetInitialText / SetTextCheckCallback /
 * GetRequired*Size, InitializeKeyboardConfig) is NOT here: it is the SDK's own code, lifted (sdklift_gen.c).
 *
 * Tags: [sdk] address in binaries\13.0.4\sdk.elf, [game] address in main.elf, [doc] suyu source / public headers.
 *
 * ---- nn::web
 * [sdk] 0x3bd330 ShowOfflineHtmlPage: creates library applet 0x17, pushes the argument, waits for it, then
 * pops 0x1010 bytes into the OfflineHtmlPageReturnValue; Result 0 when that storage exists, 0x2d2 (module
 * 210, description 1) when the applet ended without one. Layout ([sdk] 0x3be5a0 constructor, [doc] suyu
 * WebCommonReturnValue): u32 exitReason @0 (constructor: 0x14 = "none"), char lastUrl[0x1000] @8,
 * u64 lastUrlSize @0x1008. ShowLobbyPage ([sdk] 0x2c41d8) is the same shape.
 * [sdk] 0x3bd154 ShowWebPage / 0x3bd2c4 ShowYouTubeVideo: the output is 0x2000 bytes of TLVs:
 * u16 count @0, u32 shim kind @4, then entries { u16 type, u16 size, u32 pad, data[size] } packed without
 * alignment ([sdk] 0x3c0ef8 walks them as offset += size + 8); type 1 = u32 exit reason, 2 = last URL,
 * 3 = u64 URL size, 9 = u8 "media player closed by completion".
 * [doc] suyu WebExitReason: 0 EndButtonPressed, 1 BackButtonPressed, 2 ExitRequested, 3 CallbackURL,
 * 4 WindowClosed, 7 ErrorDialog.
 * What happens here: no page can be shown (no browser applet; the game's offline HTML documents are in
 * its manual/legal NCA, not in the extracted RomFS), so each call answers what the applet returns when
 * the user closes the page at once: exit reason 0 (EndButtonPressed), last URL "http://localhost/" (what
 * suyu's applet without a browser frontend reports). [game] 0x710375535c ignores the Result and the value
 * of ShowOfflineHtmlPage; [game] 0x7103341e50 only acts on ShowWebPage when the exit reason is 3
 * (callback URL), which cannot happen without a network.
 *
 * ---- nn::swkbd::ShowKeyboard(String *out, const ShowKeyboardArg &arg)
 * [sdk] 0x38a390. ShowKeyboardArg: KeyboardConfig (0x4d0 bytes) @0; work buffer @0x4d0, size @0x4d8;
 * text-check work buffer @0x4e0 (the applet's dialog message goes back from there, 0x7d4 bytes); a
 * second buffer @0x4f0 / size @0x4f8 (user dictionary). KeyboardConfig fields used here ([sdk] MakePreset
 * 0x38a810, SetInitialText 0x38b130, SetTextCheckCallback 0x38b340, [doc] nnheaders swkbd): header text
 * (char16[65]) @0x24, sub text (char16[129]) @0xa6, guide text (char16[257]) @0x1a8, textMaxLength @0x3ac,
 * textMinLength @0x3b0, passwordMode @0x3b4, isUseUtf8 @0x3bd, initial text offset in the work buffer
 * @0x3c0 and length in characters @0x3c4, text-check enabled @0x3d0, callback @0x3d8.
 * String: pointer @0, buffer size in bytes @8.
 * Results: 0 with the text (UTF-16, or UTF-8 when isUseUtf8) when confirmed; 0x29f (module 159,
 * description 1) when cancelled ([sdk] 0x38a75c; [game] 0x7103300584 compares with 0x29f).
 * Text check ([sdk] 0x389e40): the callback is called as cb(textCheckWorkBuf, size_t *outMessageSize,
 * String *text) with text->bufSize temporarily set to the size of the entered text; it returns
 * [doc] TextCheckResult 0 Success, 1 ShowFailureDialog, 2 ShowConfirmDialog, with the message in
 * textCheckWorkBuf.
 * Here the applet is a small Win32 dialog (edit box, OK, Cancel) run on the calling guest thread, which
 * blocks exactly like on the console. Scripted runs must not block: SSBU_SWKBD=cancel answers "cancelled",
 * SSBU_SWKBD=text:<utf-8 text> answers that text, SSBU_SWKBD=dialog forces the dialog; without the
 * variable the dialog is used unless an input script (SSBU_SEQ / SSBU_PRESS / SSBU_SCRIPT) is active.
 *
 * ---- nn::nfp
 * Console modelled: the player-1 controller has an NFC reader (as a Pro Controller / right Joy-Con does)
 * and no amiibo is ever held to it. [sdk] 0x309900 GetState: 0 before Initialize, the service's state
 * after (1 = initialised); [sdk] 0x309a84 GetDeviceState: 5 before Initialize. [doc] suyu nfc_types.h
 * DeviceState 0 Initialized, 1 SearchingForTag, 2 TagFound, 3 TagRemoved, 4 TagMounted, 5 Unavailable;
 * nfp_result.h (module 115): DeviceNotFound 64, InvalidArgument 65, WrongDeviceState 73.
 * [sdk] 0x308814 amiibo settings applet closed without a change: Result 0x1a073 (module 115, description 208).
 * NOT VERIFIED on a console: the DeviceHandle value (8 bytes; the npad id 0 is used), and the Result of
 * calls made before Initialize (WrongDeviceState is returned). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);

/* ================================================================ nn::web */
#define WEB_RESULT_TERMINATED 0x2d2u
#define WEB_EXIT_END_BUTTON 0u
static const char g_last_url[] = "http://localhost/";

/* Input TLVs of a Show*Arg ([doc] switchbrew "Internet Browser": header u16 count, u16 pad, u32 shim kind; entries
 * u16 type, u16 size, u32 pad; type 1 initial URL, 6 document path). Only used to say in the log what was asked. */
static const char *web_arg_string(uint64_t arg, int type) {
    uint32_t count = rd16(arg), off = 8;
    for (uint32_t i = 0; i < count && off + 8 < 0x2000; i++) {
        uint32_t t = rd16(arg + off), size = rd16(arg + off + 2);
        if ((int)t == type) return (const char *)(uintptr_t)(arg + off + 8);
        off += size + 8;
    }
    return NULL;
}
static void web_log(Ctx *c, const char *what, uint64_t arg) {
    const char *url = web_arg_string(arg, 1), *doc = web_arg_string(arg, 6);
    printf("%s: \"%.200s\" cannot be shown here; answered as closed by the user (caller 0x%llx)\n", what, doc ? doc : url ? url : "",
           (unsigned long long)c->x[30] - 4);
    trace_printf("     %s \"%.200s\" -> closed (EndButtonPressed)\n", what, doc ? doc : url ? url : "");
}
static void web_fill_common(uint64_t ret) {                /* 0x1010 bytes */
    memset((void *)(uintptr_t)ret, 0, 0x1010);
    wr32(ret, WEB_EXIT_END_BUTTON);
    memcpy((void *)(uintptr_t)(ret + 8), g_last_url, sizeof g_last_url);
    wr64(ret + 0x1008, sizeof g_last_url - 1);
}
static void web_fill_tlv(uint64_t ret, uint32_t shim_kind) {   /* 0x2000 bytes */
    uint8_t *p = (uint8_t *)(uintptr_t)ret;
    uint32_t off = 8;
    memset(p, 0, 0x2000);
    wr16(ret + off, 1); wr16(ret + off + 2, 4); wr32(ret + off + 8, WEB_EXIT_END_BUTTON); off += 8 + 4;
    wr16(ret + off, 2); wr16(ret + off + 2, (uint16_t)sizeof g_last_url); memcpy(p + off + 8, g_last_url, sizeof g_last_url); off += 8 + (uint32_t)sizeof g_last_url;
    wr16(ret + off, 3); wr16(ret + off + 2, 8); wr64(ret + off + 8, sizeof g_last_url - 1);
    wr16(ret, 3);
    wr32(ret + 4, shim_kind);
}
/* Read in close4 (ShowWebPage / ShowYouTubeVideo / ShowLobbyPage stay OPEN):
 * [sdk] 0x3bd0c0 ShowWebPage and 0x3bd210 ShowYouTubeVideo: library applet 0x13, common arguments (version 0x80000) + the
 * 0x2000-byte argument, start (a failure aborts), then CommonApi::JoinAndFinishLibraryApplet reads 0x2000 bytes: Result 0,
 * or 0x2d2 when the applet left no storage.  [sdk] 0x2c4160 ShowLobbyPage: library applet 0x18, 0x1010 bytes back, same
 * Results.  The SDK makes NO connection check of its own: what comes back on a console without a network is decided
 * inside the browser applet (it shows its own error screen and returns some exit reason), and neither the SDK nor
 * [doc] suyu am/frontend/applet_web_browser.cpp shows which: suyu opens a host browser for Web (ExecuteWeb) and ends
 * Lobby at once with exit reason 0 because "Lobby Applet is not implemented".
 * What the game does with the answers: [game] 0x7103341e50 ShowWebPage: only exit reason 3 (callback URL) is acted on,
 * every other answer and every failure take the same path; [game] 0x71002ab9fc ShowLobbyPage: stores 0 when the exit
 * reason is 0x14 or 0xc and 1 for anything else -- so the unknown exit reason IS visible to the game there;
 * [game] 0x71017634cc / 0x7103341bac ShowYouTubeVideo: reads IsMediaPlayerAutoClosedByCompletion (TLV 9, absent here =
 * false). */
static void web_ShowOfflineHtmlPage(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowOfflineHtmlPage", A1); web_fill_common(ret); RET(0); }
static void web_ShowLobbyPage(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowLobbyPage", A1); web_fill_common(ret); RET(0); }
static void web_ShowWebPage(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowWebPage", A1); web_fill_tlv(ret, 5); RET(0); }     /* [doc] ShimKind::Web = 5 */
static void web_ShowYouTubeVideo(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowYouTubeVideo", A1); web_fill_tlv(ret, 5); RET(0); }

/* ================================================================ nn::ec shop pages
 * [sdk] 0x18cad0 ShowShopAddOnContentList(ApplicationId), 0x18cb10 (+ UserHandle), 0x18d250 ShowShopProductDetails,
 * 0x18d130 ShowShopProductList: void functions.  The UserHandle forms turn the handle into a Uid (account::GetUserId,
 * zero Uid on failure, 0x18cb2c); the Impl (0x1907a0, 0x191330, 0x190ff0) builds the page path and calls
 * detail::ShowShopPage (0x1933a0): library applet 0x14 with the common arguments and the page argument, start, wait,
 * 0x1010 bytes of return value read into an SDK-internal buffer (0xc860c0).  The Result is only inspected for an abort
 * (anything but success and 0x690, 0x190898); neither it nor the return value is handed to the caller.
 * So whatever the shop applet does on a console without a connection (it is the web applet in shop mode; [doc] suyu
 * am/frontend/applet_web_browser.cpp ExecuteShop: ends at once with exit reason 0), the GAME sees a function that
 * returns nothing and writes nothing: that is what the handler does.  Not modelled: the applet's own screen and the
 * focus messages a library applet in front causes. */
static void ec_ShowShop(Ctx *c) {
    printf("nn::ec: the game asked for a Nintendo eShop page (caller 0x%llx); there is no shop here, returning as if it was closed\n",
           (unsigned long long)c->x[30] - 4);
    trace_printf("     nn::ec::ShowShop* -> closed\n");
}

/* ================================================================ nn::oe volume balance
 * [sdk] 0x32c5b0 SetExpectedVolumeBalance(float mainApplet, float libraryApplet): one request to the applet
 * manager's audio controller (object from 0x5368a0, slot +0x20 = command 0 SetExpectedMasterVolume); a failure
 * aborts. [sdk] 0x32c620 GetExpectedVolumeBalance(float *mainApplet, float *libraryApplet): two requests
 * (slot +0x28 = command 1 GetMainAppletExpectedMasterVolume, slot +0x30 = command 2
 * GetLibraryAppletExpectedMasterVolume), each failure aborts. Nothing else happens on the SDK side.
 * The applet manager's side (not in the dump), after [doc] suyu am/service/audio_controller.cpp/.h: it keeps
 * the two values clamped to [0, 1]; initial values main 0.25, library 1.0; the reference does nothing else
 * with them. What the console's applet manager does to the SOUND with them while a library applet is in front
 * is not in any source available here: NOT implemented, NOT verified. [game] 0x71037552fc saves them around
 * the offline HTML page and restores them afterwards. */
static float g_volume_main = 0.25f, g_volume_library = 1.0f;
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static void oe_GetExpectedVolumeBalance(Ctx *c) {
    memcpy((void *)(uintptr_t)A0, &g_volume_main, 4);
    memcpy((void *)(uintptr_t)A1, &g_volume_library, 4);
}
static void oe_SetExpectedVolumeBalance(Ctx *c) { g_volume_main = clamp01(c->v[0].f[0]); g_volume_library = clamp01(c->v[1].f[0]); }

/* ================================================================ nn::swkbd::ShowKeyboard */
#define SWKBD_RESULT_CANCELED 0x29fu
#define KC_HEADER 0x24
#define KC_SUB 0xa6
#define KC_GUIDE 0x1a8
#define KC_MAXLEN 0x3ac
#define KC_MINLEN 0x3b0
#define KC_PASSWORD 0x3b4
#define KC_UTF8 0x3bd
#define KC_INITIAL_OFFSET 0x3c0
#define KC_INITIAL_LENGTH 0x3c4
#define KC_CHECK_ENABLED 0x3d0
#define KC_CHECK_CALLBACK 0x3d8
#define ARG_WORKBUF 0x4d0
#define ARG_CHECKBUF 0x4e0
#define SWKBD_MAX_CHARS 500                               /* [sdk] 0x38b150: SetInitialText stops at 0x1f4 characters */

typedef struct {
    const wchar_t *header, *sub, *guide, *initial;
    int max_len, min_len, password;
    wchar_t text[SWKBD_MAX_CHARS + 2];
} KbdDialog;

#define IDC_EDIT 1001
#define IDC_INFO 1002
static INT_PTR CALLBACK kbd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    KbdDialog *k = (KbdDialog *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        k = (KbdDialog *)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)k);
        wchar_t info[600];
        _snwprintf(info, 599, L"%ls%ls%ls%ls%ls", k->header, k->header[0] && k->sub[0] ? L"\r\n" : L"", k->sub,
                   (k->header[0] || k->sub[0]) && k->guide[0] ? L"\r\n" : L"", k->guide);
        info[599] = 0;
        SetDlgItemTextW(dlg, IDC_INFO, info[0] ? info : L"Enter text");
        SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETLIMITTEXT, (WPARAM)k->max_len, 0);
        if (k->password) SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
        SetDlgItemTextW(dlg, IDC_EDIT, k->initial);
        SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETSEL, 0, -1);
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_EDIT, k->text, SWKBD_MAX_CHARS + 1);
            if ((int)wcslen(k->text) < k->min_len) { MessageBeep(MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    case WM_CLOSE: EndDialog(dlg, 0); return TRUE;
    }
    return FALSE;
}
static uint8_t *tmpl_align(uint8_t *p, int a) { while ((uintptr_t)p & (uintptr_t)(a - 1)) *p++ = 0; return p; }
static uint8_t *tmpl_wstr(uint8_t *p, const wchar_t *s) { size_t n = (wcslen(s) + 1) * sizeof(wchar_t); memcpy(p, s, n); return p + n; }
static uint8_t *tmpl_item(uint8_t *p, DWORD style, int x, int y, int w, int h, int id, WORD cls, const wchar_t *text) {
    p = tmpl_align(p, 4);
    DLGITEMTEMPLATE it = {style | WS_CHILD | WS_VISIBLE, 0, (short)x, (short)y, (short)w, (short)h, (WORD)id};
    memcpy(p, &it, sizeof it); p += sizeof it;
    WORD cl[2] = {0xffff, cls};
    memcpy(p, cl, 4); p += 4;
    p = tmpl_wstr(p, text);
    *p++ = 0; *p++ = 0;                                   /* no creation data */
    return p;
}
static int kbd_run_dialog(KbdDialog *k) {
    uint8_t buf[1024], *p = buf;
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE dt = {DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFOREGROUND, WS_EX_TOPMOST, 4, 0, 0, 260, 96};
    memcpy(p, &dt, sizeof dt); p += sizeof dt;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;               /* no menu, default class */
    p = tmpl_wstr(p, L"Software keyboard (SSBU native)");
    *p++ = 9; *p++ = 0;
    p = tmpl_wstr(p, L"Segoe UI");
    p = tmpl_item(p, SS_LEFT, 8, 6, 244, 34, IDC_INFO, 0x0082, L"");
    p = tmpl_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 44, 244, 14, IDC_EDIT, 0x0081, L"");
    p = tmpl_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 144, 72, 50, 16, IDOK, 0x0080, L"OK");
    p = tmpl_item(p, BS_PUSHBUTTON | WS_TABSTOP, 202, 72, 50, 16, IDCANCEL, 0x0080, L"Cancel");
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (DLGTEMPLATE *)buf, NULL, kbd_proc, (LPARAM)k);
    if (r == -1) { printf("nn::swkbd: the text dialog could not be created (error %lu); answered as cancelled\n", GetLastError()); return 0; }
    return (int)r;
}
static void copy_w(wchar_t *dst, uint64_t src, int max) {
    int i = 0;
    for (; i < max; i++) { uint16_t ch = rd16(src + (uint64_t)i * 2); if (!ch) break; dst[i] = ch; }
    dst[i] = 0;
}
/* How this call is answered: 0 dialog, 1 cancel, 2 fixed text. */
static int kbd_mode(wchar_t *fixed) {
    const char *v = getenv("SSBU_SWKBD");
    if (v && strncmp(v, "text:", 5) == 0) { MultiByteToWideChar(CP_UTF8, 0, v + 5, -1, fixed, SWKBD_MAX_CHARS + 1); fixed[SWKBD_MAX_CHARS] = 0; return 2; }
    if (v && strcmp(v, "cancel") == 0) return 1;
    if (v && strcmp(v, "dialog") == 0) return 0;
    if (getenv("SSBU_SEQ") || getenv("SSBU_PRESS") || getenv("SSBU_SCRIPT")) return 1;
    return 0;
}
/* Writes `text` into the caller's String the way the applet's storage is copied ([sdk] 0x389ff4):
 * at most bufSize bytes, always terminated. Returns the number of bytes used including the terminator. */
static uint64_t kbd_store(uint64_t string, const wchar_t *text, int utf8) {
    uint64_t ptr = rd64(string), cap = rd64(string + 8);
    if (!ptr || !cap) return 0;
    if (utf8) {
        char tmp[SWKBD_MAX_CHARS * 4 + 4];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, tmp, (int)sizeof tmp, NULL, NULL);
        if (n <= 0) { n = 1; tmp[0] = 0; }
        while ((uint64_t)n > cap) { n--; while (n > 1 && ((uint8_t)tmp[n - 1] & 0xc0) == 0x80) n--; if (n > 0) n--; tmp[n] = 0; n++; }
        memcpy((void *)(uintptr_t)ptr, tmp, (size_t)n);
        return (uint64_t)n;
    }
    uint64_t chars = wcslen(text);
    if ((chars + 1) * 2 > cap) chars = cap / 2 ? cap / 2 - 1 : 0;
    if (cap < 2) return 0;
    for (uint64_t i = 0; i < chars; i++) wr16(ptr + i * 2, (uint16_t)text[i]);
    wr16(ptr + chars * 2, 0);
    return (chars + 1) * 2;
}
static void swkbd_ShowKeyboard(Ctx *c) {
    uint64_t string = A0, arg = A1;
    static KbdDialog k;                                    /* one keyboard at a time, as on the console */
    static volatile LONG busy;
    static wchar_t header[70], sub[140], guide[260], initial[SWKBD_MAX_CHARS + 2], fixed[SWKBD_MAX_CHARS + 2];
    if (InterlockedExchange(&busy, 1)) { RET(SWKBD_RESULT_CANCELED); return; }
    copy_w(header, arg + KC_HEADER, 64);
    copy_w(sub, arg + KC_SUB, 128);
    copy_w(guide, arg + KC_GUIDE, 256);
    initial[0] = 0;
    uint64_t work = rd64(arg + ARG_WORKBUF);
    int init_len = (int)rd32(arg + KC_INITIAL_LENGTH), init_off = (int)rd32(arg + KC_INITIAL_OFFSET);
    if (work && init_len > 0 && init_len <= SWKBD_MAX_CHARS && init_off >= 0) copy_w(initial, work + (uint64_t)init_off, init_len);
    int utf8 = rd8(arg + KC_UTF8) != 0;
    int max_len = (int)rd32(arg + KC_MAXLEN), min_len = (int)rd32(arg + KC_MINLEN);
    if (max_len <= 0 || max_len > SWKBD_MAX_CHARS) max_len = SWKBD_MAX_CHARS;
    if (min_len < 0 || min_len > max_len) min_len = 0;
    uint64_t callback = rd8(arg + KC_CHECK_ENABLED) ? rd64(arg + KC_CHECK_CALLBACK) : 0, check_buf = rd64(arg + ARG_CHECKBUF);
    int mode = kbd_mode(fixed);
    uint32_t result = SWKBD_RESULT_CANCELED;
    trace_printf("     nn::swkbd::ShowKeyboard header \"%ls\" guide \"%ls\" max %d min %d%s\n", header, guide, max_len, min_len, callback ? " (text check)" : "");
    for (int attempt = 0; attempt < 16; attempt++) {
        int confirmed;
        if (mode == 1) confirmed = 0;
        else if (mode == 2) { wcsncpy(k.text, fixed, (size_t)max_len); k.text[max_len] = 0; confirmed = attempt == 0; }
        else {
            k.header = header; k.sub = sub; k.guide = guide; k.initial = initial;
            k.max_len = max_len; k.min_len = min_len; k.password = rd32(arg + KC_PASSWORD) == 1;
            confirmed = kbd_run_dialog(&k);
        }
        if (!confirmed) break;
        uint64_t bytes = kbd_store(string, k.text, utf8);
        int verdict = 0;
        if (callback && check_buf) {                       /* [sdk] 0x389ed8..0x389ef8 */
            uint64_t message_size = 0, saved_cap = rd64(string + 8), saved[12];
            for (int i = 0; i < 12; i++) saved[i] = c->x[19 + i];
            wr64(string + 8, bytes);
            c->x[0] = check_buf; c->x[1] = P(&message_size); c->x[2] = string;
            call_guest(c, callback);
            verdict = (int)(uint32_t)c->x[0];
            for (int i = 0; i < 12; i++) c->x[19 + i] = saved[i];
            wr64(string + 8, saved_cap);
            if (verdict) {
                wchar_t message[1024];
                if (utf8) { MultiByteToWideChar(CP_UTF8, 0, (const char *)(uintptr_t)check_buf, -1, message, 1023); message[1023] = 0; }
                else copy_w(message, check_buf, 1000);
                trace_printf("     nn::swkbd text check -> %d \"%ls\"\n", verdict, message);
                if (mode != 0) break;                      /* a scripted answer that is refused counts as cancelled */
                if (verdict == 2 && MessageBoxW(NULL, message, L"Software keyboard", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) verdict = 0;
                else if (verdict != 2) MessageBoxW(NULL, message, L"Software keyboard", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                if (verdict) { wcsncpy(initial, k.text, SWKBD_MAX_CHARS); initial[SWKBD_MAX_CHARS] = 0; continue; }
            }
        }
        result = 0;
        break;
    }
    if (result) trace_printf("     nn::swkbd::ShowKeyboard -> cancelled\n");
    else trace_printf("     nn::swkbd::ShowKeyboard -> \"%ls\"\n", k.text);
    InterlockedExchange(&busy, 0);
    RET(result);
}

/* ================================================================ nn::nfp */
#define NFP_RESULT(desc) ((uint32_t)(115 | ((desc) << 9)))
#define NFP_WRONG_STATE NFP_RESULT(73)
#define NFP_INVALID_ARGUMENT NFP_RESULT(65)
#define NFP_SETTINGS_CLOSED 0x1a073u
static volatile LONG g_nfp_init, g_nfp_searching;
static void nfp_Initialize(Ctx *c) { (void)c; g_nfp_init = 1; g_nfp_searching = 0; }
static void nfp_Finalize(Ctx *c) { (void)c; g_nfp_init = 0; g_nfp_searching = 0; }
static void nfp_GetState(Ctx *c) { RET(g_nfp_init ? 1 : 0); }
static void nfp_GetDeviceState(Ctx *c) { RET(!g_nfp_init ? 5 : g_nfp_searching ? 1 : 0); }
static void nfp_ListDevices(Ctx *c) {                      /* (DeviceHandle *out, int *outCount, int max) */
    if (A1) wr32(A1, 0);
    if (!g_nfp_init) { RET(NFP_WRONG_STATE); return; }
    if (!A0 || (int)A2 < 1) { RET(NFP_INVALID_ARGUMENT); return; }
    wr64(A0, 0);                                           /* the reader of npad 0 */
    if (A1) wr32(A1, 1);
    RET(0);
}
static void nfp_GetNpadId(Ctx *c) { if (!g_nfp_init) { RET(NFP_WRONG_STATE); return; } wr32(A0, 0); RET(0); }   /* (uint32_t *out, handle) */
static void nfp_AttachEvent(Ctx *c) { os_init_event(A0, 0); RET(g_nfp_init ? 0 : NFP_WRONG_STATE); }            /* never signalled: no tag ever appears */
static void nfp_AttachAvailabilityChangeEvent(Ctx *c) { os_init_event(A0, 0); RET(0); }
static void nfp_StartDetection(Ctx *c) {
    if (!g_nfp_init || g_nfp_searching) { RET(NFP_WRONG_STATE); return; }
    g_nfp_searching = 1;
    RET(0);
}
static void nfp_StopDetection(Ctx *c) {
    if (!g_nfp_init || !g_nfp_searching) { RET(NFP_WRONG_STATE); return; }
    g_nfp_searching = 0;
    RET(0);
}
static void nfp_needs_tag(Ctx *c) { RET(NFP_WRONG_STATE); }                 /* every call that needs a found or mounted tag */
static void nfp_GetApplicationArea(Ctx *c) { if (A1) wr64(A1, 0); RET(NFP_WRONG_STATE); }   /* (void *out, size_t *outSize, handle, size) */
/* StartNicknameAndOwnerSettings (2 forms, [sdk] 0x308970 / 0x308af0), StartGameDataEraser ([sdk] 0x308c50), StartRestorer
 * ([sdk] 0x308d90).  The SDK checks NOTHING before the applet (no device state, no tag presence): it fills the 0x1a8-byte
 * StartParamForAmiiboSettings (mode byte @1: 0 / 1 / 2, flags @2, the caller's device handle @4, TagInfo @0xc, RegisterInfo
 * @0x64) and runs library applet 0xb with StartLibraryAppletEasy ([sdk] 0x3087f0).  The Result is 0x1a073 when the applet
 * could not run or when the first byte of its 0x188-byte answer is 0, and only otherwise are the outputs (device handle from
 * answer+4, the "registered" flag = bit 2, RegisterInfo) written ([sdk] 0x308844..0x308858, 0x308a84..0x308ab8).
 * The applet's answer is the system's: [suyu] am/frontend/applet_cabinet.cpp DisplayCompleted -> Cancel() when the reader's
 * state is neither TagFound nor TagMounted: answer byte 0 (CabinetResult::Cancel).  No amiibo is ever on the reader here, so
 * that is the path: 0x1a073 and no output written. */
static void nfp_settings_applet(Ctx *c) {
    trace_printf("     nn::nfp amiibo settings applet -> closed without a change (0x1a073)\n");
    RET(NFP_SETTINGS_CLOSED);
}

/* ================================================================ nn::web for the Skyline plugins (skyline-web)
 * Names the game never imports: the plugins build ShowOfflineHtmlPageArg with the C2 constructor and two more setters,
 * read the return value back, and ARCropolis / hdr-launcher drive OfflineWebSession (a page that talks to the plugin
 * through content messages). All of it answers "the page was opened and the user closed it at once"; every call logs
 * the document path so the log shows what a plugin wanted to show.
 * Layouts read in sdk.elf:
 *   [sdk] 0x3bd980 ShowOfflineHtmlPageArg(const char *) (C1 == C2, same address): the default argument, then the
 *   document path as TLV 6 of 0xc00 bytes. The argument TLV setters ([sdk] 0x3bdaf0 SetFooterEnabled, 0x3be1a0
 *   SetBootLoadingIconEnabled, 0x3bfb90 SetBootMode) look for an entry of their type: same size -> overwritten, else a new
 *   entry { u16 type, u16 size, u32 0, data } is appended at the end and the count at +0 incremented, provided it ends
 *   within the 0x2000 bytes. SetBootLoadingIconEnabled = TLV 0x35 (u8), SetBootMode = TLV 0x41 (u32 WebSessionBootMode).
 *   [sdk] 0x3be5e0 OfflineHtmlPageReturnValue::GetLastUrl = this + 8; 0x3be5f0 GetLastUrlSize = *(u64 *)(this + 0x1008);
 *   0x3be5d0 GetOfflineExitReason = *(u32 *)this.
 *   [sdk] 0x3bfc40 OfflineWebSession: one word, the work buffer; 0x3bfc60 GetWorkBufferSize = 0xd0; 0x3bfc70 Initialize
 *   stores the buffer; 0x3bfca0 Start(SystemEventType **out, const ShowOfflineHtmlPageArg &) -> Result (library applet
 *   0x17); 0x3bfe70 WaitForExit pops 0x1010 bytes into the return value -> Result (0x2d2 without one); Appear,
 *   RequestExit, TrySendContentMessage, TryReceiveContentMessage are forwarded to the session (bool results).
 * Here the session's applet ends at once: Start succeeds and hands out an exit event that is already signalled (key
 * work + 0x40: the work buffer is only ever read by SDK code, so its contents are ours), sent messages are accepted and
 * dropped (logged), and every receive returns an EMPTY message (size 1, just the NUL): skyline-web's recv() then yields
 * "", recv_json() fails, and the plugins' `while let Ok(msg) = session.recv_json()` menu loops end; WaitForExit fills the
 * return value as ShowOfflineHtmlPage does. NOT the console's behaviour for a page nobody closes; a plugin that waits
 * for one specific message (hdr-launcher's request loop) would keep receiving empty ones. */
static void web_tlv_set(uint64_t arg, uint16_t type, const void *data, uint16_t size) {
    uint32_t count = rd16(arg), off = 8;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t t = rd16(arg + off), s = rd16(arg + off + 2);
        if (t == type) { if (s == size) memcpy((void *)(uintptr_t)(arg + off + 8), data, size); return; }
        off += 8u + s;
    }
    if (off + 8u + size > 0x2000) return;
    wr16(arg + off, type); wr16(arg + off + 2, size); wr32(arg + off + 4, 0);
    memcpy((void *)(uintptr_t)(arg + off + 8), data, size);
    wr16(arg, (uint16_t)(count + 1));
}
static void web_OfflineArg_ctor(Ctx *c) {                  /* (ShowOfflineHtmlPageArg *this, const char *documentPath) */
    ImportFn lifted = sdk_lifted("_ZN2nn3web22ShowOfflineHtmlPageArgC1EPKc");
    if (lifted) { lifted(c); return; }                      /* the SDK's own constructor (C1 and C2 are one function) */
    uint64_t arg = A0;                                      /* no SDK image: the shape the log reads (shim kind 3 = offline, document path) */
    char path[0xc00];
    memset(path, 0, sizeof path);
    if (A1) strncpy(path, (const char *)(uintptr_t)A1, sizeof path - 1);
    memset((void *)(uintptr_t)arg, 0, 0x2000);
    wr32(arg + 4, 3);
    web_tlv_set(arg, 6, path, (uint16_t)sizeof path);
}
static void web_SetBootLoadingIconEnabled(Ctx *c) { uint8_t v = (uint8_t)(A1 & 1); web_tlv_set(A0, 0x35, &v, 1); }
static void web_SetBootMode(Ctx *c) { uint32_t v = (uint32_t)A1; web_tlv_set(A0, 0x41, &v, 4); }
static void web_ret_GetLastUrl(Ctx *c) { RET(A0 + 8); }
static void web_ret_GetLastUrlSize(Ctx *c) { RET(rd64(A0 + 0x1008)); }
static void web_ret_GetOfflineExitReason(Ctx *c) { RET(rd32(A0)); }

#define WEB_WORK_SIZE 0xd0
static uint8_t g_web_fallback_work[WEB_WORK_SIZE];         /* a session Start()ed without Initialize */
static uint64_t web_session_work(uint64_t session) { uint64_t w = session ? rd64(session) : 0; return w ? w : P(g_web_fallback_work); }
static void web_session_ctor(Ctx *c) { wr64(A0, 0); }
static void web_session_GetWorkBufferSize(Ctx *c) { RET(WEB_WORK_SIZE); }
static void web_session_Initialize(Ctx *c) {               /* (this, void *workBuffer, size_t size) */
    wr64(A0, A1);
    if (A1) memset((void *)(uintptr_t)A1, 0, A2 < WEB_WORK_SIZE ? (size_t)A2 : WEB_WORK_SIZE);
}
static void web_session_Start(Ctx *c) {                    /* (this, SystemEventType **outExitEvent, const ShowOfflineHtmlPageArg &) -> Result */
    uint64_t work = web_session_work(A0), out = A1, ev = work + 0x40;
    web_log(c, "nn::web::OfflineWebSession::Start", A2);
    os_init_event(ev, 0);
    os_signal_event(ev);                                    /* the applet has already ended */
    wr32(work, 0);                                          /* receives logged for this session */
    if (out) wr64(out, ev);
    RET(0);
}
static void web_session_Appear(Ctx *c) { trace_printf("     nn::web::OfflineWebSession::Appear\n"); RET(1); }
static void web_session_RequestExit(Ctx *c) { trace_printf("     nn::web::OfflineWebSession::RequestExit\n"); RET(1); }
static volatile LONG g_web_sent;
static void web_session_TrySend(Ctx *c) {                  /* (this, const char *message, size_t size) -> bool */
    if (InterlockedIncrement(&g_web_sent) <= 40)
        printf("nn::web::OfflineWebSession: message to the (closed) page dropped: \"%.*s\"\n", (int)(A2 > 200 ? 200 : A2), A1 ? (const char *)(uintptr_t)A1 : "");
    RET(1);
}
static void web_session_TryReceive(Ctx *c) {               /* (this, size_t *outSize, char *buffer, size_t size) -> bool */
    uint64_t work = web_session_work(A0);
    if (!A2 || !A3) { RET(0); return; }
    wr8(A2, 0);
    if (A1) wr64(A1, 1);
    uint32_t n = rd32(work) + 1;
    wr32(work, n);
    if (n <= 2) printf("nn::web::OfflineWebSession: the page is closed: empty message %u handed to the plugin (caller 0x%llx)\n", n, (unsigned long long)c->x[30] - 4);
    RET(1);
}
static void web_session_WaitForExit(Ctx *c) {              /* (this, OfflineHtmlPageReturnValue *) -> Result */
    trace_printf("     nn::web::OfflineWebSession::WaitForExit -> closed (EndButtonPressed)\n");
    if (A1) web_fill_common(A1);
    RET(0);
}
static void web_session_NeedsWaitForExit(Ctx *c) { RET(1); }
static void web_session_Finalize(Ctx *c) { (void)c; }

#define WEB(n) "_ZN2nn3web" n
static const ImportDef applet_plugin_imports[] = {
    {WEB("22ShowOfflineHtmlPageArgC2EPKc"), web_OfflineArg_ctor},
    {WEB("22ShowOfflineHtmlPageArg25SetBootLoadingIconEnabledEb"), web_SetBootLoadingIconEnabled},
    {WEB("11SetBootModeEPNS0_22ShowOfflineHtmlPageArgENS0_18WebSessionBootModeE"), web_SetBootMode},
    {"_ZNK2nn3web26OfflineHtmlPageReturnValue10GetLastUrlEv", web_ret_GetLastUrl},
    {"_ZNK2nn3web26OfflineHtmlPageReturnValue14GetLastUrlSizeEv", web_ret_GetLastUrlSize},
    {"_ZNK2nn3web26OfflineHtmlPageReturnValue20GetOfflineExitReasonEv", web_ret_GetOfflineExitReason},
    {WEB("17OfflineWebSessionC1Ev"), web_session_ctor}, {WEB("17OfflineWebSessionC2Ev"), web_session_ctor},
    {WEB("17OfflineWebSession17GetWorkBufferSizeEv"), web_session_GetWorkBufferSize},
    {WEB("17OfflineWebSession10InitializeEPvm"), web_session_Initialize},
    {WEB("17OfflineWebSession5StartEPPNS_2os15SystemEventTypeERKNS0_22ShowOfflineHtmlPageArgE"), web_session_Start},
    {WEB("17OfflineWebSession6AppearEv"), web_session_Appear}, {WEB("17OfflineWebSession11RequestExitEv"), web_session_RequestExit},
    {WEB("17OfflineWebSession21TrySendContentMessageEPKcm"), web_session_TrySend},
    {WEB("17OfflineWebSession24TryReceiveContentMessageEPmPcm"), web_session_TryReceive},
    {WEB("17OfflineWebSession11WaitForExitEPNS0_26OfflineHtmlPageReturnValueE"), web_session_WaitForExit},
    {"_ZNK2nn3web17OfflineWebSession16NeedsWaitForExitEv", web_session_NeedsWaitForExit},
    {WEB("17OfflineWebSession8FinalizeEv"), web_session_Finalize},
};

/* ================================================================ registration */
#define NFP(n) "_ZN2nn3nfp" n
/* These replace the placeholders nnmisc.c used to hold for the same names (removed there). */
static const ImportDef applet_imports[] = {
    {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", web_ShowOfflineHtmlPage},
    {"_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", web_ShowLobbyPage},
    {"_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", web_ShowWebPage},
    {"_ZN2nn3web16ShowYouTubeVideoEPNS0_18WebPageReturnValueERKNS0_19ShowYouTubeVideoArgE", web_ShowYouTubeVideo},
    {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE", ec_ShowShop},
    {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", ec_ShowShop},
    {"_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE", ec_ShowShop},
    {"_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKcRKNS_7account10UserHandleE", ec_ShowShop},
    {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_GetExpectedVolumeBalance},
    {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", oe_SetExpectedVolumeBalance},
    {"_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_ShowKeyboard},
    {NFP("10InitializeEv"), nfp_Initialize}, {NFP("8FinalizeEv"), nfp_Finalize}, {NFP("8GetStateEv"), nfp_GetState},
    {NFP("14GetDeviceStateERKNS0_12DeviceHandleE"), nfp_GetDeviceState}, {NFP("11ListDevicesEPNS0_12DeviceHandleEPii"), nfp_ListDevices},
    {NFP("9GetNpadIdEPjRKNS0_12DeviceHandleE"), nfp_GetNpadId},
    {NFP("29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE"), nfp_AttachAvailabilityChangeEvent},
    {NFP("19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE"), nfp_AttachEvent},
    {NFP("21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE"), nfp_AttachEvent},
    {NFP("14StartDetectionERKNS0_12DeviceHandleE"), nfp_StartDetection}, {NFP("13StopDetectionERKNS0_12DeviceHandleE"), nfp_StopDetection},
    {NFP("5MountERKNS0_12DeviceHandleENS0_9ModelTypeE"), nfp_needs_tag}, {NFP("7UnmountERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("19OpenApplicationAreaERKNS0_12DeviceHandleEj"), nfp_needs_tag}, {NFP("5FlushERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("7RestoreERKNS0_12DeviceHandleE"), nfp_needs_tag}, {NFP("10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm"), nfp_GetApplicationArea},
    {NFP("18SetApplicationAreaERKNS0_12DeviceHandleEPKvm"), nfp_needs_tag},
    {NFP("21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE"), nfp_needs_tag},
    {NFP("23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE"), nfp_needs_tag},
    {NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"), nfp_settings_applet},
    {NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_"), nfp_settings_applet},
    {NFP("13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"), nfp_settings_applet},
    {NFP("19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"), nfp_settings_applet},
};

void register_nnapplet_imports(void) {
    int n = (int)(sizeof applet_imports / sizeof applet_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(applet_imports[i].name)) printf("nnapplet: \"%s\" is not an import of the game (typo?)\n", applet_imports[i].name);
    register_imports(applet_imports, n);
    register_imports(applet_plugin_imports, (int)(sizeof applet_plugin_imports / sizeof applet_plugin_imports[0]));   /* not imports of the game: kept for nnro.c */
}

/* ================================================================ self-test */
static ImportFn h(const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("  no handler registered for %s\n", name); report_and_exit(3); }
    return f;
}
static int guard_ok(const uint8_t *p, size_t from, size_t to, uint8_t v) { for (size_t i = from; i < to; i++) if (p[i] != v) return 0; return 1; }
static volatile int g_kbd_test_initial_seen;
static DWORD WINAPI kbd_test_driver(LPVOID arg) {
    (void)arg;
    for (int round = 0; round < 2; round++) {
        HWND w = NULL;
        for (int i = 0; i < 500 && !w; i++) { Sleep(10); w = FindWindowW(NULL, L"Software keyboard (SSBU native)"); }
        if (!w) return 1;
        Sleep(100);
        if (round == 0) {
            wchar_t now[64] = L"";
            GetDlgItemTextW(w, IDC_EDIT, now, 63);
            g_kbd_test_initial_seen = wcscmp(now, L"Mario") == 0;
            static const wchar_t typed[] = {'Z', 0x30bc, 0};
            SetDlgItemTextW(w, IDC_EDIT, typed);
            PostMessageW(w, WM_COMMAND, IDOK, 0);
        } else PostMessageW(w, WM_COMMAND, IDCANCEL, 0);
        for (int i = 0; i < 500 && IsWindow(w); i++) Sleep(10);
    }
    return 0;
}
int nnapplet_selftest(Ctx *c) {
    int bad = 0, ok;
    uint8_t *buf = malloc(0x8000), *buf2 = malloc(0x8000);
    /* --- nn::web: argument objects (lifted) stay inside 0x2000 bytes; return values have the documented shape */
    memset(buf, 0xee, 0x8000);
    c->x[0] = P(buf); c->x[1] = P("help/index.html"); h("_ZN2nn3web22ShowOfflineHtmlPageArgC1EPKc")(c);
    c->x[0] = P(buf); c->x[1] = 1; h("_ZN2nn3web22ShowOfflineHtmlPageArg16SetFooterEnabledEb")(c);
    c->x[0] = P(buf); c->x[1] = 1; h("_ZN2nn3web22ShowOfflineHtmlPageArg17SetPointerEnabledEb")(c);
    c->x[0] = P(buf); c->x[1] = 12; c->x[2] = 4; h("_ZN2nn3web22ShowOfflineHtmlPageArg17SetKeyRepeatFrameEii")(c);
    const char *doc = web_arg_string(P(buf), 6);
    ok = guard_ok(buf, 0x2000, 0x8000, 0xee) && doc && strstr(doc, "help/index.html") != NULL;
    printf("  web: ShowOfflineHtmlPageArg (lifted): document path TLV \"%s\", %d TLVs, nothing written past 0x2000: %s\n", doc ? doc : "(none)", rd16(P(buf)), ok ? "ok" : "FAILED");
    bad += !ok;
    memset(buf2, 0xee, 0x8000);
    c->x[0] = P(buf2); h("_ZN2nn3web26OfflineHtmlPageReturnValueC1Ev")(c);
    ok = rd64(P(buf2)) == 0x14 && guard_ok(buf2, 0x1010, 0x8000, 0xee);
    c->x[0] = P(buf2); c->x[1] = P(buf); web_ShowOfflineHtmlPage(c);
    ok = ok && c->x[0] == 0 && rd32(P(buf2)) == 0 && strcmp((char *)buf2 + 8, "http://localhost/") == 0 && rd64(P(buf2) + 0x1008) == 17 && guard_ok(buf2, 0x1010, 0x8000, 0xee);
    printf("  web: OfflineHtmlPageReturnValue: constructor 0x14, after the page: exit reason 0, last URL set, 0x1010 bytes: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    memset(buf, 0xee, 0x8000);
    c->x[0] = P(buf); c->x[1] = P("https://example.invalid/"); h("_ZN2nn3web14ShowWebPageArgC1EPKc")(c);
    c->x[0] = P(buf); c->x[1] = P("https://example.invalid/cb"); h("_ZN2nn3web14ShowWebPageArg14SetCallbackUrlEPKc")(c);
    ok = guard_ok(buf, 0x2000, 0x8000, 0xee);
    memset(buf2, 0xee, 0x8000);
    c->x[0] = P(buf2); h("_ZN2nn3web18WebPageReturnValueC1Ev")(c);
    c->x[0] = P(buf2); h("_ZNK2nn3web18WebPageReturnValue16GetWebExitReasonEv")(c);
    ok = ok && c->x[0] == 0x14;                            /* no TLV yet */
    c->x[0] = P(buf2); c->x[1] = P(buf); web_ShowWebPage(c);
    ok = ok && c->x[0] == 0 && guard_ok(buf2, 0x2000, 0x8000, 0xee);
    c->x[0] = P(buf2); h("_ZNK2nn3web18WebPageReturnValue16GetWebExitReasonEv")(c);      /* the SDK's own getters read what was written */
    ok = ok && c->x[0] == 0;
    c->x[0] = P(buf2); h("_ZNK2nn3web18WebPageReturnValue10GetLastUrlEv")(c);
    ok = ok && c->x[0] && strcmp((const char *)(uintptr_t)c->x[0], "http://localhost/") == 0;
    c->x[0] = P(buf2); h("_ZN2nn3web35IsMediaPlayerAutoClosedByCompletionERKNS0_18WebPageReturnValueE")(c);
    ok = ok && (c->x[0] & 1) == 0;
    printf("  web: WebPageReturnValue TLVs read back by the SDK's getters (exit reason 0, last URL, not auto-closed): %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    /* --- nn::swkbd: config (lifted) is 0x4d0 bytes; a scripted answer goes through ShowKeyboard */
    memset(buf, 0xee, 0x8000);
    c->x[0] = P(buf); c->x[1] = 0; h("_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE")(c);
    static const uint16_t guide[] = {'N', 'a', 'm', 'e', 0}, initial_text[] = {'M', 'a', 'r', 'i', 'o', 0};
    c->x[0] = P(buf); c->x[1] = P(guide); h("_ZN2nn5swkbd12SetGuideTextEPNS0_14KeyboardConfigEPKDs")(c);
    c->x[0] = P(buf); c->x[1] = P(guide); h("_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs")(c);
    c->x[0] = P(buf); c->x[1] = P(guide); h("_ZN2nn5swkbd10SetSubTextEPNS0_14KeyboardConfigEPKDs")(c);
    ok = guard_ok(buf, 0x4d0, 0x8000, 0xee) && rd16(P(buf) + KC_GUIDE) == 'N' && rd16(P(buf) + KC_HEADER) == 'N';
    printf("  swkbd: MakePreset + Set*Text (lifted) stay inside 0x4d0 bytes, texts at 0x24 / 0x1a8: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    c->x[0] = 0; h("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb")(c);
    uint64_t work_size = c->x[0];
    h("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv")(c);
    uint64_t string_size = c->x[0];
    uint8_t *work = calloc(1, work_size + 16), *text = malloc(string_size + 16);
    memset(buf + 0x4d0, 0, 0x30);                          /* the rest of ShowKeyboardArg */
    wr64(P(buf) + ARG_WORKBUF, P(work)); wr64(P(buf) + ARG_WORKBUF + 8, work_size);
    c->x[0] = P(buf); c->x[1] = P(initial_text); h("_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs")(c);
    ok = rd32(P(buf) + KC_INITIAL_LENGTH) == 5 && rd16(P(work) + rd32(P(buf) + KC_INITIAL_OFFSET)) == 'M';
    uint64_t string[2] = {P(text), string_size};
    memset(text, 0xee, string_size + 16);
    _putenv("SSBU_SWKBD=text:Kirby");
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    ok = ok && c->x[0] == 0 && rd16(P(text)) == 'K' && rd16(P(text) + 8) == 'y' && rd16(P(text) + 10) == 0 && guard_ok(text, string_size, string_size + 16, 0xee);
    _putenv("SSBU_SWKBD=cancel");
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    ok = ok && c->x[0] == SWKBD_RESULT_CANCELED;
    /* the real dialog: a helper thread types into it and presses OK, then a second run is cancelled */
    _putenv("SSBU_SWKBD=dialog");
    HANDLE helper = CreateThread(NULL, 0, kbd_test_driver, NULL, 0, NULL);
    memset(text, 0xee, string_size + 16);
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    int dialog_ok = c->x[0] == 0 && rd16(P(text)) == 'Z' && rd16(P(text) + 2) == 0x30bc && rd16(P(text) + 4) == 0 && g_kbd_test_initial_seen;
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    dialog_ok = dialog_ok && c->x[0] == SWKBD_RESULT_CANCELED;
    WaitForSingleObject(helper, 5000);
    CloseHandle(helper);
    printf("  swkbd: Win32 dialog shows the initial text \"Mario\", returns typed text \"Z\" + U+30BC on OK and 0x29f on Cancel: %s\n", dialog_ok ? "ok" : "FAILED");
    bad += !dialog_ok;
    _putenv("SSBU_SWKBD=");
    printf("  swkbd: sizes work 0x%llx string 0x%llx; SetInitialText in the work buffer; scripted answer \"Kirby\" (UTF-16) and cancel (0x29f): %s\n",
           (unsigned long long)work_size, (unsigned long long)string_size, ok ? "ok" : "FAILED");
    bad += !ok;
    free(work); free(text);
    /* --- nn::nfp state machine */
    uint64_t handle = 0xeeeeeeeeeeeeeeeeull, ev[8];
    int count = -1;
    nfp_Finalize(c);
    nfp_GetState(c); ok = c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == 5;
    nfp_Initialize(c);
    nfp_GetState(c); ok = ok && c->x[0] == 1;
    c->x[0] = P(&handle); c->x[1] = P(&count); c->x[2] = 1; nfp_ListDevices(c); ok = ok && c->x[0] == 0 && count == 1 && handle == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_StartDetection(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == 1;
    c->x[0] = P(&handle); c->x[1] = 0; nfp_needs_tag(c); ok = ok && c->x[0] == NFP_WRONG_STATE;
    c->x[0] = P(&handle); nfp_StopDetection(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(ev); c->x[1] = P(&handle); nfp_AttachEvent(c); ok = ok && c->x[0] == 0;
    nfp_Finalize(c);
    printf("  nfp: Initialize / ListDevices (1 reader) / StartDetection / StopDetection state machine, tag calls refused: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    {   /* amiibo settings applets: 0x1a073 and no output written ([sdk] 0x308844..0x308858, 0x308a84) */
        static const char *const names[4] = {
            NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"),
            NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_"),
            NFP("13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"),
            NFP("19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE")};
        uint8_t out_handle[8], out_flag[1], out_reg[0x100], in_a[0x100], in_b[0x100], in_c[0x100];
        ok = 1;
        for (int i = 0; i < 4; i++) {
            ImportFn f = import_lookup(names[i]);
            memset(out_handle, 0x5a, sizeof out_handle); memset(out_flag, 0x5a, sizeof out_flag); memset(out_reg, 0x5a, sizeof out_reg);
            memset(in_a, 0, sizeof in_a); memset(in_b, 0, sizeof in_b); memset(in_c, 0, sizeof in_c);
            c->x[0] = P(out_handle);
            if (i < 2) { c->x[1] = P(out_flag); c->x[2] = P(out_reg); c->x[3] = P(in_a); c->x[4] = P(in_b); c->x[5] = P(in_c); }
            else { c->x[1] = P(in_a); c->x[2] = P(in_b); }
            if (!f) { ok = 0; continue; }
            f(c);
            ok = ok && c->x[0] == 0x1a073 && out_flag[0] == 0x5a;
            for (int k = 0; k < 8; k++) ok = ok && out_handle[k] == 0x5a;
            for (int k = 0; k < 0x100; k++) ok = ok && out_reg[k] == 0x5a;
        }
        printf("  nfp: the 4 amiibo settings applets with no amiibo: 0x1a073, device handle / flag / RegisterInfo untouched: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* nn::ec shop pages: void, nothing written through any argument ([sdk] 0x18cad0, 0x1907a0, 0x1933a0) */
        uint8_t arg[0x40], uid[0x20];
        memset(arg, 0x5a, sizeof arg); memset(uid, 0x5a, sizeof uid);
        c->x[0] = P(arg); c->x[1] = 2; c->x[2] = P(arg + 0x20); c->x[3] = P(uid); ec_ShowShop(c);
        ok = 1;
        for (int k = 0; k < 0x40; k++) ok = ok && arg[k] == 0x5a;
        for (int k = 0; k < 0x20; k++) ok = ok && uid[k] == 0x5a;
        printf("  ec: ShowShop* returns and writes nothing: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* nn::mii::AppendMii cancelled: 0xe7e, *outIndex untouched ([sdk] 0x300bc4, 0x301040) */
        ImportFn f = import_lookup("_ZN2nn3mii9AppendMiiEPi");
        int32_t index = 0x5a5a5a5a;
        c->x[0] = P(&index);
        if (f) f(c);
        ok = f && c->x[0] == 0xe7e && index == 0x5a5a5a5a;
        printf("  mii: AppendMii answered as cancelled: 0xe7e, index untouched: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    /* --- nn::oe */
    float a = -1, b = -1, a0 = g_volume_main, b0 = g_volume_library;
    c->v[0].f[0] = 0.5f; c->v[1].f[0] = 3.0f; oe_SetExpectedVolumeBalance(c);
    c->x[0] = P(&a); c->x[1] = P(&b); oe_GetExpectedVolumeBalance(c);
    ok = a == 0.5f && b == 1.0f && a0 == 0.25f && b0 == 1.0f;
    c->v[0].f[0] = a0; c->v[1].f[0] = b0; oe_SetExpectedVolumeBalance(c);
    printf("  oe: volume balance initial 0.25 / 1.0, set / get (clamped to 1.0): %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    free(buf); free(buf2);
    return bad;
}
