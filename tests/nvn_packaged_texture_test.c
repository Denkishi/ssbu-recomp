/* Real texture initialization/hash/deswizzle paths, without a window or GPU.
 * The dump shows an unreadable packaged source before the first upload; it
 * does not establish how the source became unavailable. This regression
 * deliberately releases a source to exercise the deferred-read hazard. */
#include "../runtime/nvn_gl.c"
#include <assert.h>

/* The companion TU includes the actual Vulkan renderer. No GPU calls
 * are taken: an existing backend record selects the prewarmer's metadata path. */
void resource_test_prewarm(void);
static void check_prewarm(Tex *t) {
    uint8_t *copy = t->packaged_copy;
    resource_test_prewarm();
    if (t->packaged_copy != copy || t->ahead_seen != -1) {
        fputs("Vulkan prewarmer overwrote the packaged pointer: inconsistent Tex layout\n", stderr);
        exit(86);
    }
}
static int levels_seen;
static size_t texels_seen;
static void collect(Tex *t, int level, int layer, int w, int h, int d,
                    const void *data, size_t size, int rgba, int64_t stride) {
    (void)t; (void)layer; (void)w; (void)h; (void)d; (void)rgba; (void)stride;
    assert(level == levels_seen++);
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++) assert(p[i] == 0x35);
    texels_seen += size;
}
static Tex *make_texture(unsigned char *source, uint64_t object[32], int *result) {
    TexBuilder b = {0}; Ctx c = {0};
    b.target = 1; b.format = 0x4d; b.width = b.height = 512; b.depth = 1; b.levels = 10;
    b.packaged = (uint64_t)(uintptr_t)source;
    c.x[0] = (uint64_t)(uintptr_t)object; c.x[1] = (uint64_t)(uintptr_t)&b;
    tex_initialize(&c); *result = (int)c.x[0];
    return hobj((uint64_t)(uintptr_t)object, K_TEX);
}
int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    unsigned char *source = VirtualAlloc(NULL, 0x56000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(source); memset(source, 0x35, 0x56000);
    uint64_t object[32] = {0}; int result;
    Tex *t = make_texture(source, object, &result);
    assert(result == 1 && t && t->data_size == 0x55e00 && t->packaged_copy && t->packaged_copy != source);
    check_prewarm(t);
    if (argc > 1 && !strcmp(argv[1], "--reproduce-borrowed-source")) {
        free(t->packaged_copy); t->packaged_copy = source;
    }
    assert(VirtualFree(source, 0, MEM_RELEASE));
    g_serial = 1;
    assert(nvn_front_tex_changed(t) == 1);  /* deliberately freed borrowed source faults in the reported hash path */
    assert(nvn_front_tex_read(t, 0, collect));
    assert(levels_seen == 10 && texels_seen > 0 && !t->packaged_copy);
    g_serial++;
    assert(nvn_front_tex_changed(t) == 0);  /* already uploaded; no read of the freed original/snapshot */
    puts("PASS all ten mip levels survive decommit before the initial upload; snapshot released afterwards");

    source = VirtualAlloc(NULL, 0x56000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(source); memset(source, 0x35, 0x56000);
    uint64_t unused[32] = {0}; Tex *never_used = make_texture(source, unused, &result);
    assert(result && never_used->packaged_copy);
    assert(VirtualFree(source, 0, MEM_RELEASE));
    tex_free_gl(never_used); assert(!never_used->packaged_copy);
    puts("PASS unused texture cleanup releases the initialization snapshot without reading guest memory");

    source = VirtualAlloc(NULL, 0x56000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(source); memset(source, 0x35, 0x56000);
    uint64_t ordinary[32] = {0};
    Tex *pool_tex = make_texture(NULL, ordinary, &result);
    assert(result && pool_tex && !pool_tex->packaged_copy);
    pool_tex->mem = source; pool_tex->limit = source + 0x56000;
    check_prewarm(pool_tex);
    levels_seen = 0; texels_seen = 0;
    assert(nvn_front_tex_read(pool_tex, 0, collect));
    assert(levels_seen == 10 && !pool_tex->packaged_copy);
    assert(VirtualFree(source, 0, MEM_RELEASE));
    puts("PASS actual Vulkan prewarmer preserves both packaged and ordinary texture ownership");

    Tex *head = g_all_tex; uint64_t invalid[32] = {0};
    assert(!make_texture((unsigned char *)0x1234, invalid, &result));
    assert(!result && !invalid[0] && !invalid[1] && g_all_tex == head);
    puts("PASS unreadable initialization source fails explicitly and is never published");
    return 0;
}
