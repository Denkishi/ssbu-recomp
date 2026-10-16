/* NVN front end: the types both render back ends share (plan\phases\V1_vulkan.md, section 1, seam steps S1 and S2).
 * Moved here from nvn_gl.c without renames. The front end itself (entry points, command recording, the job queue, the
 * texture source logic) still lives in nvn_gl.c until step S3; what the Vulkan back end needs of it is declared at the
 * end of this file (the nvn_front_* functions, defined at the end of nvn_gl.c). */
#ifndef NVN_FRONT_H
#define NVN_FRONT_H
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <GL/gl.h>
#include "mx_translate.h"

typedef struct { uint16_t internal, format, type; uint8_t bpb, bw, bh, kind; } Fmt;   /* kind: 0 colour, 1 depth, 2 stencil, 3 depth+stencil, 4 compressed */

/* Small state objects and builders keep their contents in the guest's own memory (the game copies them
 * around freely); textures, programs, command buffers, pools and windows keep a pointer to a host struct
 * at +0 and a tag at +8. Host structs of textures and programs are never freed (recorded command lists
 * and pool entries may still name them); they are only marked dead. */
enum { K_TEX = 0x5854564e, K_PROG = 0x5250564e, K_CMD = 0x4443564e, K_TPOOL = 0x5054564e, K_SPOOL = 0x5053564e, K_WIN = 0x4e57564e };
enum { K_SYNC = 0x5953564e, K_QUEUE = 0x5551564e };

typedef struct { uint8_t min, mag, ws, wt, wr, cmode, cfunc, reduction; float lodmin, lodmax, bias, aniso, border[4]; uint8_t border_kind, pad[3]; float lod_snap; } SamplerParams;   /* 48 bytes = NVNsampler (0x30); border_kind: 0 float, 1 unsigned, 2 signed integers in border[] */
typedef struct { uint8_t set, target, dsmode, pad; uint16_t format; int16_t lvl0, nlvl, lay0, nlay; uint8_t swz[4]; } View;               /* fits NVNtextureView (0x28) */
typedef struct {                                                                                                                       /* fits NVNtextureBuilder (0x80) */
    int32_t flags, target, format, width, height, depth, levels, samples, dsmode, has_layout;
    uint8_t swz[4], layout[8];
    uint64_t pool, offset, packaged;
    int64_t stride;
    uint32_t glname;                       /* nvnTextureBuilderSetGLTextureName (OpenGL interop of the Windows NVN; only stored) */
} TexBuilder;

typedef struct { GLuint gl; int nh, alias; struct { GLuint smp; uint64_t h; } *h; } TexObj;
typedef struct { int target, format, lvl0, nlvl, lay0, nlay, dsmode; uint8_t swz[4]; } EView;
typedef struct VNode { EView e; TexObj o; struct VNode *next; } VNode;
typedef struct Tex {
    TexBuilder b;
    volatile LONG dead;
    uint8_t *mem, *limit;                  /* storage in the memory pool and the end of that pool */
    uint64_t guest;                        /* the NVNtexture */
    size_t storage, data_size;
    int glevels, dirty, uploaded, gpu_written, label_dirty;
    struct Tex *all_next;                  /* every texture ever made (g_all_tex) */
    int live_i;                            /* its place in g_live_tex (nvn_gl.c) while it is not finalised */
    uint64_t hash;
    uint64_t ww_seen; int ww_hashed;       /* write-watch stamp seen at the last look; hash is a full hash (see ww_newest) */
    uint32_t checked;
    TexObj o;
    VNode *views;
    void *vk;                              /* the Vulkan back end's record of this texture (nvn_vk_draw.c: VTex), NULL until first used */
    long long ahead_seen;                  /* session 8 (nvn_vk_draw.c tex_ahead): 1 + the frame the render thread first saw this record; -1 = looked at, not taken */
} Tex;
Tex *nvn_front_tex_first(void);            /* the newest texture record; ->all_next = the one made before it (records are never freed) */
typedef struct { Tex *tex; View view; } TexPoolEnt;
typedef struct { int count; TexPoolEnt *e; uint64_t mempool, offset; } TexPool;
typedef struct { int count; SamplerParams *e; uint8_t *used; uint64_t mempool, offset; } SamplerPool;
typedef struct { char *text; int len, state, nbuf; GLuint gl; uint8_t map[16]; uint32_t storage, storage_w, glsl_ssbo_w, local[3], smem_bytes, lmem_bytes, glsl_images, glsl_images_w, glsl_images_r; uint16_t glsl_img_key[8], glsl_img_fmt[8]; const uint8_t *code; int code_len, dumped, glsl_state, glsl_alpha; GLuint glsl; uint32_t glsl_cbufs, glsl_ssbos, glsl_dep, glsl_tex, glsl_key[4], ctl[8]; void *vk; } Stage;   /* state: 0 none, 1 text ready, 2 compiled, 3 rejected, 4 machine code only; vk: the Vulkan back end's last variant of this stage */
typedef struct { Stage st[6]; int serial, sample_shading, label_dirty; volatile LONG dead; uint64_t guest; } Prog;
typedef struct { uint32_t op, size; uint64_t a[7]; } Cmd;                                                    /* a blob may follow */
typedef struct {
    uint8_t *data; size_t len, cap;
    uint64_t cmd_size, ctrl_size, cmd_used, ctrl_used;     /* the memory the game added, and how much of it the commands recorded since would take (a model: see rec) */
    uint64_t callback, callback_data, rec_cmd, rec_ctrl;   /* out-of-memory callback; bytes of the current recording */
    int64_t row_stride, image_stride;
    int recording, callback_off, in_callback, dispatches;
} CmdBuf;
typedef struct { int count, interval, nactive; Tex *tex[8]; int crop[4]; uint64_t native; } Window;
typedef struct { void *s; volatile LONG dead; } SyncObj;       /* s: GL sync object; Vulkan: the number of the submission the fence follows */

/* ---------------------------------------------------------------- command lists */
enum {
    OP_NOP, OP_TEXPOOL, OP_SMPPOOL, OP_PROGRAM, OP_UBO, OP_SSBO, OP_TEXTURES, OP_VBUF, OP_VATTR, OP_VSTREAM, OP_BLEND, OP_COLOR, OP_CHANMASK,
    OP_MULTISAMPLE, OP_POLYGON, OP_DEPTHSTENCIL, OP_VIEWPORTS, OP_SCISSORS, OP_DEPTHRANGES, OP_TARGETS, OP_STENCIL_REF, OP_STENCIL_VMASK,
    OP_STENCIL_MASK, OP_BLENDCOLOR, OP_ALPHAREF, OP_POINTSIZE, OP_LINEWIDTH, OP_POLYOFFSET, OP_SAMPLEMASK, OP_RASTDISCARD, OP_DEPTHCLAMP,
    OP_PRIMRESTART, OP_PATCHSIZE, OP_CLEARCOLOR, OP_CLEARDS, OP_DRAW_ARRAYS, OP_DRAW_ELEMENTS, OP_COPY_BUF_TEX, OP_COPY_TEX_TEX, OP_DOWNSAMPLE,
    OP_COPY_BUF_BUF, OP_UPDATE_UBO, OP_CLEAR_BUF, OP_CALL, OP_ROWSTRIDE, OP_COPY_TEX_BUF, OP_DISPATCH, OP_IMAGES, OP_IMAGESTRIDE, OP_DEPTHBOUNDS, OP_CONSRASTER, OP_CONSDILATE, OP_SUBPIXEL, OP_VPSWIZZLE, OP_TESSLEVELS, OP_BARRIER, OP_DISCARD,
    OP_CLEARTEX, OP_DOWNSAMPLE_VIEW, OP_DRAW_INDIRECT, OP_DISPATCH_INDIRECT, OP_RENDER_ENABLE, OP_COUNTER_RESET, OP_COUNTER_REPORT, OP_REPORT_VALUE, OP_FENCE, OP_WAITSYNC,
    OP_EVENT_SIGNAL, OP_EVENT_WAIT, OP_COVMODTABLE, OP_DEBUG, OP_DRAW_TEXTURE, OP_COUNT
};
#define BLOB(m) ((uint8_t *)((m) + 1))
typedef struct { uint64_t addr, size; } Range;
typedef struct { Tex *tex; View view; } TargetRef;

/* ---------------------------------------------------------------- render-thread jobs (arguments of RBackend members) */
typedef struct { int count; const uint64_t *handles; } SubmitJob;
typedef struct { Window *win; int index; int shot_fight; } PresentJob;      /* shot_fight (session 13, test): fight frame whose picture this present is, 0 = none; set when the game queues the present */
typedef struct { SyncObj *o; int flags; uint64_t timeout; int result; } SyncJob;
typedef struct { Tex *tex; View view; int has_view; int region[6]; const void *data; int64_t row_stride; } WriteJob;
typedef struct { Tex *tex; View view; int has_view, to_pool; int region[6]; void *out; int64_t row_stride, image_stride; } ReadJob;      /* to_pool: nvnTextureInvalidateTexels */

/* Seam step S2: what a render back end is. Every member runs on the render thread with the argument the entry point
 * built (the type is named beside it); the entry points call rb_run / rb_post (g_rb-><member>, argument). A member a
 * back end does not implement stops the game with the NVN operation's name (nvn_vk_missing). */
typedef struct RBackend {
    const char *name;                       /* "gl" | "vk" */
    void (*submit)(void *);                 /* SubmitJob: replay the command lists */
    void (*submit_async)(void *);           /* SubmitJob followed by its handles (posted, nobody waits) */
    void (*present)(void *);                /* PresentJob */
    void (*present_movie)(void *);          /* PresentJob to the movie maker's window */
    void (*fence)(void *);                  /* SyncJob */
    void (*sync_wait)(void *);              /* SyncJob: result 0 already signaled, 1 satisfied, 2 timeout, 3 failed */
    void (*server_wait)(void *);            /* SyncJob */
    void (*sync_drop)(void *);              /* SyncObj */
    void (*flush)(void *);
    void (*finish)(void *);
    void (*write_texels)(void *);           /* WriteJob (nvn_gl.c) */
    void (*read_texels)(void *);            /* ReadJob (nvn_gl.c) */
    void (*housekeeping)(void *);           /* deferred deletion of what finalized objects held */
    void (*reset_status)(void *);           /* int * */
    void (*sample_grid)(void *);            /* int[2] */
    void (*idle)(void);                     /* before every job (GL: deferred deletion); may be NULL */
} RBackend;
extern const RBackend rb_gl, rb_vk;
extern const RBackend *g_rb;
void nvn_vk_container_seen(uint64_t control, uint64_t prog);      /* Vulkan, GUEST thread, from nvnProgramSetShaders: the "BNSH" container around this control section / variation program is scanned when new (nvn_vk_m4.c) */
/* Session 8: where one nvnProgramSetShaders call spent its time, in performance-counter ticks, per calling thread:
 * 0 gl_barrier, 1 container_seen (all), 2 container search, 3 container copy / walk, 4 program_set (all), 5 hashing + job copy,
 * 6 waiting for the shader lock, 7 caller held for the workers. Printed by nvn_gl.c prog_set_shaders ("nvn_setsh:"). */
extern __declspec(thread) long long nvn_setsh_part[8];
void nvn_vk_program_set(void *prog);        /* Vulkan, GUEST thread, from nvnProgramSetShaders: the program's stages go to the workers (nvn_vk_m4.c) */

/* ---------------------------------------------------------------- front-end services for the Vulkan back end (end of nvn_gl.c) */
int nvn_front_fmt(int nvn_format, Fmt *f);                                  /* fmt_get */
uint8_t *nvn_front_list(uint64_t handle, size_t *len);                      /* list_lookup */
void nvn_front_batch_begin(void);                                           /* a new submitted batch: texture memory is looked at again once */
void nvn_front_barrier_texmem(void);                                        /* nvnCommandBufferBarrier bit 0x10 */
int nvn_front_tex_layers(const Tex *t);
int nvn_front_tex_levels(const Tex *t);                                     /* levels the host image has (builder levels clamped to the size) */
/* 1 when the guest memory of the texture must be read (again): first use, or written since the last look. `commit` = the
 * caller uploads now (the texture's hash and stamps are then updated). */
int nvn_front_tex_changed(Tex *t);
/* Calls put() once per (level, layer) with tightly packed texels de-swizzled from the texture's memory; decode != 0 turns
 * every level into RGBA8 with tex_decode_rgba8 (rgba8 = 1 in the call). Returns 0 when the memory is not readable. */
typedef void nvn_front_put_fn(Tex *t, int level, int layer, int w, int h, int d, const void *data, size_t size, int rgba8, int64_t row_stride);
int nvn_front_tex_read(Tex *t, int decode, nvn_front_put_fn *put);
/* Texture pool / sampler pool lookup of an NVN texture handle: returns bit 0 = live texture (*t, *v), bit 1 = sampler (*p). */
int nvn_front_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, Tex **t, View *v, SamplerParams *p);
/* Present bookkeeping shared by the back ends: counts the frame, prints the frame-rate line, feeds the scripted input,
 * collects old command lists. Returns the number of the frame just presented. */
long long nvn_front_frame_end(long long draws, long long skipped, long long clears);
long long nvn_front_frame_next(void);                                       /* number the frame being presented will get */
/* SSBU_COMPUTE_CMP=<folder> (V1 session 5): a dispatch named by the hash of its inputs; its written ranges saved once landed. */
void nvn_front_ccmp_dispatch(const Range *ubo, const Range *ssbo, uint32_t written, const uint8_t *code, int code_len, unsigned gx, unsigned gy, unsigned gz);
void nvn_front_ccmp_landed(void);
void nvn_front_take_dead(void (*drop)(int kind, void *host));               /* deferred deletion list: drop(K_TEX | K_PROG, host struct) */
void nvn_front_count(int what, long long n);                                /* 0 draws, 1 skipped draws, 2 clears, 3 submits, 4 texture uploads, 5 textures created */
int nvn_front_clip_modes(void);                                             /* origin mode | depth mode << 8 (1 | 1 << 8 = upper left, 0..1) */
const char *nvn_front_op_name(unsigned op);
void *nvn_front_hwnd(void);
void nvn_front_shot_step(int step);                                         /* scripted input: a key step begins (SSBU_SHOT_STEPS) */
int nvn_front_take_step_shot(void);                                         /* present: step + 1 when a step screenshot is due, else 0 */
#endif
