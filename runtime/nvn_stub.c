/* Graphics entry points: hands out a host function for every NVN name the game asks for.
 * Device information, memory pools and buffers are here; everything else is in nvn_gl.c (OpenGL).
 * A name that neither implements is MISSING: it is announced when the game fetches it, and calling it stops the game with
 * the function's name (SSBU_NVN_MISSING=warn: report the call, answer 0 and go on, for exploration). The audit of all names
 * is plan\phases\G1_nvn_audit.md (python nvn_audit.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))

static void log_call(Ctx *c) {
    if (hostfn_current_calls() <= 3)
        trace_printf("     nvn %s  x0=%llx x1=%llx x2=%llx x3=%llx  from=%llx\n", hostfn_current_name(),
                     (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                     (unsigned long long)c->x[3], (unsigned long long)c->x[30] - 4);
}
/* An entry point with no implementation. */
static int missing_warn_only(void) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("SSBU_NVN_MISSING"); mode = e && !strcmp(e, "warn"); }
    return mode;
}
static void nvn_generic(Ctx *c) {
    log_call(c);
    printf("nvn: MISSING function %s was called (x0=%llx x1=%llx x2=%llx x3=%llx)%c", hostfn_current_name(), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
           (unsigned long long)c->x[2], (unsigned long long)c->x[3], 10);
    fflush(stdout);
    if (!missing_warn_only()) {
        printf("nvn: stopping: %s has no implementation (SSBU_NVN_MISSING=warn to continue with a zero answer)%c", hostfn_current_name(), 10);
        fflush(stdout);
        report_and_exit(6);
    }
    RET(0);
}
/* nvnDeviceGetInteger(device, pname, int *out): device capabilities.
 * Only the values the boot has been seen to depend on are filled in; everything else
 * answers 0 and is counted, so the list of parameters the game really queries is known.
 * The numbers are provisional and must be checked against a reference implementation. */
/* NVNdeviceInfo values of a real Switch, indexed by pname (enum order from the SDK's nvn.h
 * as published in the BotW decomp; values from an NVN device-info dump of a development unit). */
static const int g_devinfo[0x62] = {
    53, 312, 14, 65536, 256, 8, 16, 4, 16, 32, 16, 4, 4, 4, 16, 32,             /* 0x00..0x0f */
    32, 256, 256, 4, 8, 512, 256, 2, 256, 4096, 131072, 16, 1536, 1024, 64, 1536, /* 0x10..0x1f */
    65535, 65535, 65535, 8, 1048576, 4096, 16, 4096, 1, 1, 1, 262144, 15, 2048, 1, 1, /* 0x20..0x2f */
    15, 5, 1, 8, 8, 4, 32, 128, 32, 128, 65536, 1, 4, 16384, 134217728, 2048,  /* 0x30..0x3f */
    16384, 16384, 1, 1, 1, 1, 5, 1, 1, 1, 1, 1, 1, 16, 1024, 32,                /* 0x40..0x4f */
    4096, 65536, 65536, 256, 16384, 262144, 4096, 1, 4, 2, 1, 4096, 16384, 4096, 128, 32, /* 0x50..0x5f */
    65535, 1,                                                                    /* 0x60..0x61 */
};
static volatile LONG g_devinfo_queries[256];
static void nvn_get_integer(Ctx *c) {
    uint32_t pname = (uint32_t)A1;
    int value = pname < 0x62 ? g_devinfo[pname] : 0;
    if (pname < 256 && InterlockedIncrement(&g_devinfo_queries[pname]) == 1)
        trace_printf("     nvnDeviceGetInteger pname=0x%x -> %d  from=%llx\n", pname, value, (unsigned long long)c->x[30] - 4);
    if (A2) wr32(A2, (uint32_t)value);
}
void print_devinfo_summary(FILE *out) {
    fprintf(out, "\nnvnDeviceGetInteger parameters queried (pname:count):");
    for (int i = 0; i < 256; i++) if (g_devinfo_queries[i]) fprintf(out, " %x:%ld", i, (long)g_devinfo_queries[i]);
    fprintf(out, "\n");
}

/* ---- memory pools and buffers: the game supplies the memory, so mapping must work for real.
 * Guest objects are opaque; what each one refers to is kept in a side table keyed by address. */
typedef struct NvnObj { uint64_t key, memory, size, pool, offset, flags; struct NvnObj *next; } NvnObj;
static NvnObj *g_objs[4096];
static SRWLOCK g_objs_lock = SRWLOCK_INIT;
static NvnObj *nobj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & 4095;
    AcquireSRWLockExclusive(&g_objs_lock);
    NvnObj *o = g_objs[h];
    while (o && o->key != key) o = o->next;
    if (!o) { o = calloc(1, sizeof *o); o->key = key; o->next = g_objs[h]; g_objs[h] = o; }
    ReleaseSRWLockExclusive(&g_objs_lock);
    return o;
}
static void nobj_drop(uint64_t key) {                                   /* the object is finalized: forget what it referred to */
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & 4095;
    AcquireSRWLockExclusive(&g_objs_lock);
    for (NvnObj **pp = &g_objs[h]; *pp; pp = &(*pp)->next)
        if ((*pp)->key == key) { NvnObj *o = *pp; *pp = o->next; free(o); break; }
    ReleaseSRWLockExclusive(&g_objs_lock);
}
void nvn_label_set(uint64_t obj, const char *text);                    /* nvn_gl.c */
void nvn_label_drop(uint64_t obj);
void nvn_gl_memory_written(uint64_t addr, uint64_t size);
static uint64_t g_device;
static void builder_defaults(Ctx *c) { NvnObj *b = nobj(A0); b->memory = b->size = b->pool = b->offset = b->flags = 0; }      /* nvnMemoryPoolBuilderSetDefaults / nvnBufferBuilderSetDefaults */
static void builder_set_device(Ctx *c) { if (A1) g_device = A1; }      /* (builder, device): there is one device */
static void builder_get_device(Ctx *c) { RET(g_device); }
static void pool_builder_get_memory(Ctx *c) { RET(nobj(A0)->memory); }
static void pool_builder_get_size(Ctx *c) { RET(nobj(A0)->size); }
static void pool_builder_get_flags(Ctx *c) { RET(nobj(A0)->flags); }
void nvn_front_memory_retire(uint64_t addr, uint64_t size);      /* finish deferred reads and compute copy-back before the guest releases this storage */
static void pool_finalize(Ctx *c) {
    NvnObj *o = nobj(A0);
    nvn_front_memory_retire(o->memory, o->size);
    nvn_label_drop(A0); nobj_drop(A0);
}
static void obj_set_label(Ctx *c) { nvn_label_set(A0, A1 ? (const char *)(uintptr_t)A1 : ""); }
/* FlushMappedRange(object, offset, size): the CPU wrote this range and the GPU must see it. Pool memory is ordinary host memory here,
 * so what has to be told is the backend, which keeps copies of textures. */
static void obj_flush_range(Ctx *c) { NvnObj *o = nobj(A0); if (o->memory) nvn_gl_memory_written(o->memory + A1, A2); }
static void buffer_builder_get_pool(Ctx *c) { RET(nobj(A0)->pool); }
static void buffer_builder_get_offset(Ctx *c) { RET(nobj(A0)->offset); }
static void buffer_builder_get_size(Ctx *c) { RET(nobj(A0)->size); }
static void buffer_get_pool(Ctx *c) { RET(nobj(A0)->pool); }
static void buffer_get_offset(Ctx *c) { RET(nobj(A0)->offset); }
static void buffer_finalize(Ctx *c) {
    NvnObj *o = nobj(A0);
    nvn_front_memory_retire(o->memory, o->size);
    nvn_label_drop(A0); nobj_drop(A0);
}
static void obj_debug_id(Ctx *c) { RET(A0); }
static void pool_builder_storage(Ctx *c) { log_call(c); NvnObj *b = nobj(A0); b->memory = A1; b->size = A2; }   /* (builder, memory, size) */
static void pool_builder_flags(Ctx *c) { nobj(A0)->flags = A1; }                                               /* (builder, flags) */
void nvn_front_pool_made(uint64_t memory, uint64_t size);      /* nvn_gl.c: V1 session 6, a pool laid over the code section of a shader container announces the container */
static void pool_initialize(Ctx *c) { log_call(c); NvnObj *p = nobj(A0), *b = nobj(A1); p->memory = b->memory; p->size = b->size; p->flags = b->flags; nvn_front_pool_made((uint64_t)p->memory, (uint64_t)p->size); RET(1); }
static void pool_flags(Ctx *c) { RET(nobj(A0)->flags); }
/* nvnMemoryPoolMapVirtual(pool, numRequests, const NVNmappingRequest *requests) [sdk 0x41ec70]: takes the NVN lock and calls the
 * pool's method +0x60. Which class a pool is, is decided at nvnMemoryPoolInitialize by flag bit 9 (0x200, "virtual")
 * [0x41eae0 .. 0x41eb00 -> 0x41eda0]:
 *   ordinary pool (class [0xc13f30]): method +0x60 = [0x42acd0] = `return 0`: that is the whole function there.
 *   virtual pool (class [0xc13fc0]): [0x42b490] turns each 0x28-byte request {+0 physical pool, +8 offset in it, +0x10 offset in
 *     this pool, +0x18 size, +0x20 storage class} into an entry {nvmap handle of the physical pool (its method +0x50), class &
 *     ~0x100, both offsets and the size in pages of the device's page size} and gives them, at most (pool +0x50) at a time, to
 *     NvRmGpuAddressSpaceAllocationRemap: the GPU address range of the virtual pool then shows the physical pool's pages
 *     (a request without a pool unmaps). 1 when every batch succeeded.
 * NOT IMPLEMENTED for a virtual pool: here a pool is the game's own host memory and every texture reads its texels through one
 * contiguous pointer (nvn_gl.c Tex.mem); a virtual pool has no memory of its own (its builder must pass none [0x42b208]) and its
 * pages alias pages of other pools, which needs either operating-system aliasing of memory the game allocated (not possible
 * for memory that was not created as a section) or a page table consulted by every texture read / write-back in nvn_gl.c.
 * So the name stays without a handler (MISSING: a call stops with the function's name), also for ordinary pools, where the
 * answer would be 0. */
/* For nvn_gl.c: where a memory pool's storage is. */
int nvn_pool_range(uint64_t pool, uint64_t *mem, uint64_t *size) {
    NvnObj *p = nobj(pool);
    *mem = p->memory; *size = p->size;
    return p->memory != 0;
}
static void pool_map(Ctx *c) { log_call(c); RET(nobj(A0)->memory); }
static void pool_size(Ctx *c) { log_call(c); RET(nobj(A0)->size); }
static void buffer_builder_storage(Ctx *c) {                         /* (builder, pool, offset, size) */
    log_call(c);
    NvnObj *b = nobj(A0);
    b->pool = A1; b->offset = A2; b->size = c->x[3];
}
static void buffer_initialize(Ctx *c) {                              /* (buffer, builder) */
    log_call(c);
    NvnObj *buf = nobj(A0), *b = nobj(A1);
    buf->memory = nobj(b->pool)->memory + b->offset;
    buf->size = b->size; buf->pool = b->pool; buf->offset = b->offset;
    RET(1);
}
static void buffer_map(Ctx *c) { log_call(c); RET(nobj(A0)->memory); }
static void buffer_address(Ctx *c) { log_call(c); RET(nobj(A0)->memory); }   /* GPU address: the same value for now */
static void buffer_size(Ctx *c) { log_call(c); RET(nobj(A0)->size); }

ImportFn nvn_gl_pick(const char *name);     /* nvn_gl.c: the OpenGL backend */
static ImportFn pick(const char *name);
static void nvn_get_proc(Ctx *c) {                                   /* (device, const char *name) */
    const char *name = (const char *)(uintptr_t)A1;
    RET(hostfn_address(name, pick(name)));
}
static void nvn_bootstrap(Ctx *c) {                                  /* nvnBootstrapLoader(const char *name) */
    const char *name = (const char *)(uintptr_t)A0;
    RET(hostfn_address(name, pick(name)));
}

static ImportFn pick(const char *name) {
    size_t n = strlen(name);
    if (strcmp(name, "nvnDeviceGetProcAddress") == 0) return nvn_get_proc;
    if (strcmp(name, "nvnDeviceGetInteger") == 0) return nvn_get_integer;
    static const struct { const char *name; ImportFn fn; } special[] = {
        {"nvnMemoryPoolBuilderSetStorage", pool_builder_storage}, {"nvnMemoryPoolInitialize", pool_initialize},
        {"nvnMemoryPoolMap", pool_map}, {"nvnMemoryPoolGetSize", pool_size},
        {"nvnBufferBuilderSetStorage", buffer_builder_storage}, {"nvnBufferInitialize", buffer_initialize},
        {"nvnBufferMap", buffer_map}, {"nvnBufferGetAddress", buffer_address}, {"nvnBufferGetSize", buffer_size},
        {"nvnMemoryPoolGetBufferAddress", pool_map},         /* GPU address of the pool: same as its memory for now */
        {"nvnMemoryPoolBuilderSetFlags", pool_builder_flags}, {"nvnMemoryPoolGetFlags", pool_flags},
        {"nvnMemoryPoolBuilderSetDefaults", builder_defaults}, {"nvnMemoryPoolBuilderSetDevice", builder_set_device}, {"nvnMemoryPoolBuilderGetDevice", builder_get_device},
        {"nvnMemoryPoolBuilderGetMemory", pool_builder_get_memory}, {"nvnMemoryPoolBuilderGetSize", pool_builder_get_size}, {"nvnMemoryPoolBuilderGetFlags", pool_builder_get_flags},
        {"nvnMemoryPoolFinalize", pool_finalize}, {"nvnMemoryPoolSetDebugLabel", obj_set_label}, {"nvnMemoryPoolFlushMappedRange", obj_flush_range},
        {"nvnBufferBuilderSetDefaults", builder_defaults}, {"nvnBufferBuilderSetDevice", builder_set_device}, {"nvnBufferBuilderGetDevice", builder_get_device},
        {"nvnBufferBuilderGetMemoryPool", buffer_builder_get_pool}, {"nvnBufferBuilderGetMemoryOffset", buffer_builder_get_offset}, {"nvnBufferBuilderGetSize", buffer_builder_get_size},
        {"nvnBufferGetMemoryPool", buffer_get_pool}, {"nvnBufferGetMemoryOffset", buffer_get_offset}, {"nvnBufferFinalize", buffer_finalize},
        {"nvnBufferSetDebugLabel", obj_set_label}, {"nvnBufferFlushMappedRange", obj_flush_range}, {"nvnBufferGetDebugID", obj_debug_id},
    };
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++)
        if (strcmp(name, special[i].name) == 0) return special[i].fn;
    ImportFn gl = nvn_gl_pick(name);
    if (gl) return gl;
    (void)n;
    printf("nvn: MISSING at fetch: the game obtained %s, which has no implementation (calling it stops the game)%c", name, 10);
    fflush(stdout);
    return nvn_generic;
}

static void keep(Ctx *c) { (void)c; }
static const ImportDef nvn_imports[] = {
    {"nvnBootstrapLoader", nvn_bootstrap},
    {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", keep},
    {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", keep},
    {"_ZN2nv18InitializeGraphicsEPvm", keep},
};

void register_nvn_imports(void) {
    register_imports(nvn_imports, (int)(sizeof nvn_imports / sizeof nvn_imports[0]));
}
