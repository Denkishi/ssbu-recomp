/* Track O (options), first set: config\graphics.ini. See gfx_options.c and plan\phases\O1_options.md. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env SSBU_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless (desktop size), 2 fullscreen (borderless, topmost, covering the monitor) */
    int width, height;       /* client size of the windowed mode */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env SSBU_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env SSBU_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for (Vulkan: nvn_vk_draw.c rs_scale, 0.25 .. 1.0; env SSBU_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a fight the draw is left out until they are ready; nvn_vk_m4.c skip_on; env SSBU_VK_NOHOLD wins) */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int frame_rate;          /* 60 (the console's) or 120: battles at 120 Hz (hfr.c, display.c);   env SSBU_FRAME_RATE wins */
} GfxOptions;
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
#endif
