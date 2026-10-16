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
#ifndef L1_LATENCY_DEFAULT
#define L1_LATENCY_DEFAULT 0      /* track L1: normal (coordinator 2026-10-14); low is an option */
#endif
static int g_gfx_latency = L1_LATENCY_DEFAULT;      /* track L1: latency = normal 0 | low 1 | lowest 2 (display.c); read with gfx_latency() */
int gfx_latency(void) { gfx_options(); return g_gfx_latency; }
static int g_gfx_runahead;                           /* track R1 round 5: runahead = off 0 | on 1 (r1_state.c); read with gfx_runahead() */
int gfx_runahead(void) { gfx_options(); return g_gfx_runahead; }

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
";   F11 switches between the window and a borderless window over the monitor at any time. The picture keeps its 16:9\r\n"
";   shape: the window resizes in 16:9, and other shapes (maximized, another monitor) get black bars.\r\n"
"window = windowed\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"\r\n"
"; vsync = auto | display | clock | off | on | mailbox      (Vulkan)\r\n"
";   auto:    the refresh rate of the monitor is measured at start. 60, 120, 240 Hz (within 1.1 %: 59.94, 119.88 too):\r\n"
";            as 'display'. Any other rate (75, 100, 144, 165 Hz): as 'clock'. The log says which ('display:' lines).\r\n"
";   display: the game's frame clock IS the display's refresh (every refresh at 60 Hz, every second one at 120 Hz):\r\n"
";            each frame is on screen for exactly the same time. No tearing. The game runs at refresh / k frames per\r\n"
";            second (59.94 on a 119.88 Hz panel). Falls back to 'clock' when the rate is not a multiple of 60.\r\n"
";   clock:   the program's own 60.000 Hz clock; a finished frame is shown at the next tick of that clock (mailbox when\r\n"
";            the GPU offers it, else immediate). On a display that is not 60 / 120 / 240 Hz some frames stay on screen\r\n"
";            one refresh longer than others: that cannot be avoided there.\r\n"
";   off:     immediate presentation as soon as a frame is ready, tearing allowed. Lowest latency. Still 60 frames per second.\r\n"
";   on:      the swapchain's own vertical sync (FIFO) next to the program's clock: two clocks, not recommended.\r\n"
";   mailbox: as clock, mailbox or nothing.\r\n"
"vsync = auto\r\n"
"\r\n"
"; latency = normal | low | lowest      (track L1, input latency; Vulkan)\r\n"
";   normal:  the game is let go at the tick. (The game's picture is shown one frame sooner than before 2026-10-14 in\r\n"
";            every mode: it goes to the window as soon as the game has submitted it.)\r\n"
";   low:     the game is let go late in each frame, just in time for its picture to be ready for the next tick, so it\r\n"
";            reads the controller 2-5 ms later = closer to the screen. Same cadence; with four players a few more late frames.\r\n"
";   lowest:  as low, and the picture goes to the window as soon as it is ready (no wait for the tick): about 4 ms\r\n"
";            less again, but the frames are no longer all on screen for the same time (judder).\r\n"
"latency = normal\r\n"
"\r\n"
"; runahead = off | on      (track R1, input latency; Vulkan; needs the build with dirty-page marks)\r\n"
";   on: in a fight each frame's picture is drawn one game frame earlier: the state is saved after the game's\r\n"
";       submission, the next frame is run ahead only to draw the current one, then the state is restored. One frame\r\n"
";       less delay; costs a second pass of the game's frame (CPU) every frame. Leave it off on a weak PC.\r\n"
"runahead = off\r\n"
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
/* PR #8 (stephenjayakar): the range goes above 1.0 (supersampling); capped at 2.0 here (review 2026-10-16), strong GPUs only */
";   Internal resolution scale, 0.25 to 2.0 (Vulkan only; OpenGL ignores it). 1.0 = the game's 1920x1080. 0.75 = 1440x810,\r\n"
";   0.5 = 960x540: the main switch for a weak GPU. The whole picture is drawn smaller, the 2D interface too (it gets\r\n"
";   softer), and is scaled to the window size. Above 1.0 it is drawn larger (strong GPUs only: GPU time and memory grow\r\n"
";   with the square): 1.5 = 2880x1620, 2.0 = 3840x2160 (a 4K screen pixel for pixel); a picture more than twice the\r\n"
";   window is reduced to it in 2:1 steps (supersampling). Details and limits: plan\\phases\\O1_options.md.\r\n"
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
"; shader_prepare = first_run | background | off      (Vulkan)\r\n"
";   first_run:  the first start on this PC (or after a new graphics driver / new game data) shows 'PREPARING SHADERS NN %'\r\n"
";               and builds every shader of the game before the title: several minutes, once. Closing the window stops it\r\n"
";               cleanly; the next start goes on from there. Later starts go straight to the game.\r\n"
";   background: the game starts at once and the same work runs behind it (first-seen things can make a frame late until it is done).\r\n"
";   off:        nothing is prepared ahead; shaders are built when the game first needs them.\r\n"
"shader_prepare = first_run\r\n"
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
"frame_time_overlay = off\r\n";

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
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->present = 5;      /* vsync = auto when the key is missing (track V session 14) */
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
        else if (!_stricmp(key, "vsync")) o->present = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 2 : !_stricmp(val, "mailbox") ? 3 : !_stricmp(val, "display") ? 4 : !_stricmp(val, "auto") ? 5 : 0;
        else if (!_stricmp(key, "gpu")) { if (_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!_stricmp(key, "console_mode")) o->handheld = !_stricmp(val, "handheld");
        else if (!_stricmp(key, "resolution_scale")) { float v = (float)atof(val); if (v > 0) o->scale = v; }
        else if (!_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!_stricmp(key, "frame_time_overlay")) o->overlay = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "profile")) o->profile = !_stricmp(val, "low");
        else if (!_stricmp(key, "shader_wait")) o->shader_skip = !_stricmp(val, "skip");
        else if (!_stricmp(key, "runahead")) g_gfx_runahead = !_stricmp(val, "on") || !_stricmp(val, "1");      /* track R1 round 5 (r1_state.c) */
        else if (!_stricmp(key, "latency")) g_gfx_latency = !_stricmp(val, "lowest") ? 2 : !_stricmp(val, "low") ? 1 : 0;      /* track L1 (display.c latency_mode) */
        else if (!_stricmp(key, "shader_prepare")) o->shader_prepare = !_stricmp(val, "background") ? 1 : !_stricmp(val, "off") ? 2 : 0;
        else unknown++;
    }
    if (f) fclose(f);
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file (values: O1_options.md, session 9) */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[3] = {"windowed", "borderless", "fullscreen"}, *const k_vs[6] = {"clock", "on", "off", "mailbox", "display", "auto"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height, k_vs[o->present],
           o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off", unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 2.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 2.0: the nearest of the two is used\n", o->scale);      /* PR #8 (stephenjayakar): above 1.0 allowed (nvn_vk_draw.c RS_MAX = 2.0) */
    fflush(stdout);
    return TRUE;
}
const GfxOptions *gfx_options(void) {
    InitOnceExecuteOnce(&g_once, opt_load, NULL, NULL);
    return &g_opt;
}
