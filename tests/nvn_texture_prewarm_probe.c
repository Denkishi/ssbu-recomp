/* Exercise the real Vulkan prewarmer without a device or window. */
#include "../runtime/nvn_vk.c"

void resource_test_prewarm(void) {
    Tex *t = nvn_front_tex_first();
    void *saved = t->vk;
    t->vk = (void *)1;
    tex_ahead(10);
    t->vk = saved;
}
