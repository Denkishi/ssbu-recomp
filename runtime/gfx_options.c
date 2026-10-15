/* Track O (options), first set (track V session 4): config\graphics.ini.
 *
 * Path: SSBU_GRAPHICS_CONFIG=<file> when set; else graphics.ini beside the file SSBU_CONFIG names (SSBU_CONFIG is the
 * path of controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\graphics.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (SSBU_RENDERER, SSBU_VK_DEVICE, SSBU_VK_PRESENT) win over the file.
 * The controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"

static const char *const k_default_ini =
"; graphics.ini: display and rendering options of the native build (read once at start by gfx_options.c).\r\n"
"; Delete the file to get it written again with these defaults. A line starting with ';' is a comment.\r\n"
"; Every value below is the default; the defaults draw exactly what the game draws on the console.\r\n"
"\r\n"
"; renderer = vulkan | opengl\r\n"
";   vulkan: the default since 2026-10-09. opengl: the first renderer (NVIDIA only), kept as a fallback.\r\n"
"renderer = vulkan\r\n"
"\r\n"
"; window = windowed | borderless | fullscreen\r\n"
";   windowed:   a normal window with the client size below.\r\n"
";   borderless: no frame, covers the monitor the window is on (the desktop keeps its resolution).\r\n"
";   fullscreen: as borderless, and always on top. (Exclusive fullscreen with a mode change is not implemented.)\r\n"
"window = windowed\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"\r\n"
"; vsync = clock | on | off | mailbox      (Vulkan)\r\n"
";   clock:   frames are shown at once (mailbox when the GPU offers it, else immediate) and the game is paced by the\r\n"
";            program's own 60 Hz clock. Lowest latency; can tear only when mailbox is missing.\r\n"
";   on:      the display's own vertical sync (FIFO). No tearing; on a display that is not 60 Hz the two clocks drift.\r\n"
";   off:     immediate presentation, tearing allowed. The game still runs at 60 frames per second.\r\n"
";   mailbox: mailbox or nothing (falls back to clock's choice with a message when the GPU does not offer it).\r\n"
"vsync = clock\r\n"
"\r\n"
"; gpu = auto | <number> | <part of the name>      (Vulkan)\r\n"
";   auto: a discrete GPU first, then an integrated one. The log lists the devices with their numbers:\r\n"
";   'nvn_vk: device 0: ...'. Examples: gpu = 1      gpu = Intel      gpu = RTX\r\n"
"gpu = auto\r\n"
"\r\n"
"; console_mode = docked | handheld\r\n"
";   What nn::oe::GetOperationMode answers. MEASURED on 13.0.4 (2026-10-09): with handheld the game still makes its\r\n"
";   1920x1080 window texture, so this alone does not lower the render size; kept for study, leave it at docked.\r\n"
"console_mode = docked\r\n"
"\r\n"
"; resolution_scale = 1.0\r\n"
";   Internal resolution scale, 0.25 to 1.0 (Vulkan only; OpenGL ignores it). 1.0 = the game's 1920x1080. 0.75 = 1440x810,\r\n"
";   0.5 = 960x540: the main switch for a weak GPU. The whole picture is drawn smaller, the 2D interface too (it gets\r\n"
";   softer), and is scaled to the window size. Details and limits: plan\\phases\\O1_options.md.\r\n"
"resolution_scale = 1.0\r\n"
"\r\n"
"; anisotropic_filtering = game | 2 | 4 | 8 | 16      (Vulkan)\r\n"
";   game: each texture sampler keeps the value the game gave it. A number: at least that much on every sampler that\r\n"
";   filters between mip levels (sharper textures at flat angles; changes the picture, costs little on any GPU).\r\n"
"anisotropic_filtering = game\r\n"
"\r\n"
"; shader_wait = hold | skip      (Vulkan)\r\n"
";   What happens when a draw needs a shader or pipeline that is not ready yet (mostly the first time something is shown\r\n"
";   on this PC). hold: the frame waits for it (a late frame, a correct picture). skip: in and around a fight the draw is\r\n"
";   left out until it is ready (no late frame; the object or effect is missing for some frames). The log counts both.\r\n"
"shader_wait = hold\r\n"
"\r\n"
"; profile = custom | low\r\n"
";   custom: every key of this file as written. low: the set measured for a weak or integrated GPU (2026-10-12, Intel\r\n"
";   integrated: 4.2 ms of GPU time per fight frame instead of 9.0): resolution_scale = 0.5, anisotropic_filtering = game,\r\n"
";   vsync = clock; those three keys of the file are then not used. The log says which profile is active.\r\n"
"profile = custom\r\n"
"\r\n"
"; frame_time_overlay = off | on\r\n"
";   on: the average frame time of the last second in the window title ('.. fps - 16.67 ms per frame') and as a\r\n"
";   'frame-time:' line in the log. (Bars drawn into the picture are not implemented yet.)\r\n"
"frame_time_overlay = off\r\n"
"\r\n"
"; frame_rate = 60 | 120\r\n"
";   60: the console's. 120: battles at 120 frames per second, every second one the engine's own picture between two\r\n"
";   of its 60 steps (the sub-frame drawing of its slow motion, as at training mode's 1/2 speed); the game logic keeps\r\n"
";   its 60 steps per second. Menus stay at 60. Needs a display of 120 Hz or more to be seen, and a CPU that draws a\r\n"
";   fight in under 8.3 ms per frame (else the game shows what it can and keeps its speed). Details: runtime\\hfr.c.\r\n"
"frame_rate = 60\r\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("SSBU_GRAPHICS_CONFIG"), *c = getenv("SSBU_CONFIG");
    char tmp[MAX_PATH], exe[MAX_PATH];
    if (e && e[0]) { snprintf(out, MAX_PATH, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '\\'), *s2 = strrchr(tmp, '/');
        if (s2 > s) s = s2;
        if (s) { *s = 0; snprintf(out, MAX_PATH, "%s\\graphics.ini", tmp); } else snprintf(out, MAX_PATH, "graphics.ini");
        return;
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\graphics.ini", exe);
    if (!_fullpath(out, tmp, MAX_PATH)) snprintf(out, MAX_PATH, "%s", tmp);
}
static BOOL CALLBACK opt_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0;
    memset(o, 0, sizeof *o);
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->frame_rate = 60;
    opt_path(o->path);
    FILE *f = fopen(o->path, "rb");
    if (!f) {
        char b[MAX_PATH];
        snprintf(b, sizeof b, "%s", o->path);
        for (char *p = b + 3; *p; p++) if (*p == '\\' || *p == '/') { *p = 0; CreateDirectoryA(b, NULL); *p = '\\'; }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!_stricmp(key, "renderer")) o->renderer = !_stricmp(val, "opengl") || !_stricmp(val, "gl");
        else if (!_stricmp(key, "window")) o->window_mode = !_stricmp(val, "borderless") ? 1 : !_stricmp(val, "fullscreen") ? 2 : 0;
        else if (!_stricmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!_stricmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!_stricmp(key, "vsync")) o->present = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 2 : !_stricmp(val, "mailbox") ? 3 : 0;
        else if (!_stricmp(key, "gpu")) { if (_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!_stricmp(key, "console_mode")) o->handheld = !_stricmp(val, "handheld");
        else if (!_stricmp(key, "resolution_scale")) { float v = (float)atof(val); if (v > 0) o->scale = v; }
        else if (!_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!_stricmp(key, "frame_time_overlay")) o->overlay = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "profile")) o->profile = !_stricmp(val, "low");
        else if (!_stricmp(key, "shader_wait")) o->shader_skip = !_stricmp(val, "skip");
        else if (!_stricmp(key, "frame_rate")) o->frame_rate = atoi(val) == 120 ? 120 : 60;
        else unknown++;
    }
    if (f) fclose(f);
    { const char *e = getenv("SSBU_FRAME_RATE"); if (e && e[0]) o->frame_rate = atoi(e) == 120 ? 120 : 60; }
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file (values: O1_options.md, session 9) */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[3] = {"windowed", "borderless", "fullscreen"}, *const k_vs[4] = {"clock", "on", "off", "mailbox"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s frame_rate=%d%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height, k_vs[o->present],
           o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off", o->frame_rate, unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 1.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 1.0: the nearest of the two is used\n", o->scale);
    fflush(stdout);
    return TRUE;
}
const GfxOptions *gfx_options(void) {
    InitOnceExecuteOnce(&g_once, opt_load, NULL, NULL);
    return &g_opt;
}
