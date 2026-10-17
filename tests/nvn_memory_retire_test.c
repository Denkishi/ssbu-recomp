/* Uses the real Vulkan compute-buffer state machine without a GPU. Completed submissions and host mappings
 * reproduce the deferred copy-back that used to outlive a finalized guest memory pool. */
#include "../runtime/nvn_vk.c"
#include <assert.h>

static unsigned char snapshot[67], gpu_bytes[67], landing_bytes[67];
static int waits, submits;
static VkResult VKAPI_PTR test_wait(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void)d; (void)f; assert(n == 1 && all && timeout); waits++; return VK_SUCCESS;
}
static VkResult VKAPI_PTR test_reset(VkDevice d, uint32_t n, const VkFence *f) {
    (void)d; (void)f; assert(n == 1); return VK_SUCCESS;
}
static VkResult VKAPI_PTR test_end(VkCommandBuffer cmd) { (void)cmd; return VK_SUCCESS; }
static VkResult VKAPI_PTR test_submit(VkQueue q, uint32_t n, const VkSubmitInfo *info, VkFence fence) {
    (void)q; (void)info; (void)fence; assert(n == 1); submits++; return VK_SUCCESS;
}
static void reset_buffers(void) {
    memset(g_cb, 0, sizeof g_cb);
    memset(snapshot, 0, sizeof snapshot);
    memset(gpu_bytes, 0x35, sizeof gpu_bytes);
    memset(landing_bytes, 0x46, sizeof landing_bytes);
    g_cb_keep = 1; g_cb_nkept = g_cb_live = g_cb_pending = g_ncdisp = g_kept_n = 0;
    g_kept_lo = g_live_lo = ~0ull; g_kept_hi = g_live_hi = 0;
    g_cb_serial = 17; R.done = R.serial = 17; R.recording = R.in_pass = R.cur = 0;
    memset(R.slot, 0, sizeof R.slot); waits = submits = 0;
    vkWaitForFences = test_wait; vkResetFences = test_reset;
    vkEndCommandBuffer = test_end; vkQueueSubmit = test_submit;
}
static unsigned char *guest_memory(void) {
    unsigned char *p = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(p); return p;
}
static void init_buffer(int i, unsigned char *p, int state) {
    CB *c = &g_cb[i];
    c->addr = (uint64_t)(uintptr_t)p; c->size = sizeof snapshot; c->state = state;
    c->snap = snapshot; c->mem.map = gpu_bytes; c->stg_mem.map = landing_bytes;
    c->stale = state >= 3; c->dirty = state <= 2; c->kserial = c->lserial = 17;
    if (state >= 3) g_cb_nkept++;
    if (state == 2) g_cb_pending++;
    if (state == 1) g_cb_live++;
}
static void retire_and_decommit(int state, int partial, int unfinished) {
    reset_buffers();
    unsigned char *p = guest_memory(), *other = guest_memory();
    init_buffer(0, p, state); init_buffer(1, other, 3);
    if (state == 1 || (state == 4 && unfinished == 2)) {
        R.recording = 1; g_cb[0].lserial = 18;
    } else if (unfinished) {
        R.done = 16; R.slot[0].pending = 1; R.slot[0].serial = 17;
    }
    /* A guest write newer than the fill must survive the merge. */
    p[8] = 0x7b;
    Range r = {(uint64_t)(uintptr_t)p + (partial ? 32 : 0), partial ? 1 : 65536};
    /* Deliberately leave the per-batch kept index empty, as it can be when a pending CB becomes kept. */
    nvn_vk_memory_retire(&r);
    assert(g_cb[0].state == 0 && !g_cb[0].dirty && !g_cb[0].stale);
    assert(g_cb[0].busy == (state == 1 ? 19 : unfinished == 2 ? 18 : 17) && g_cb_nkept == 1);
    assert(waits == ((state == 1 || unfinished) ? 1 : 0));
    assert(submits == ((state == 1 || unfinished == 2) ? 1 : 0));
    assert(g_cb[1].state == 3 && other[0] == 0); /* unrelated pool stays kept */
    assert(p[0] == (state == 4 ? 0x46 : 0x35));
    assert(p[8] == 0x7b && p[9] == 0); /* preserve the entire guest-written word */
    assert(p[66] == (state == 4 ? 0x46 : 0x35)); /* non-word tail */
    assert(VirtualFree(p, 65536, MEM_DECOMMIT));
    cb_landing_poll(); /* used to enter cb_keep_back and fault here */
    assert(g_cb[0].state == 0);
    /* Retired storage can be committed and reused without an old GPU copy overwriting it. */
    assert(VirtualAlloc(p, 65536, MEM_COMMIT, PAGE_READWRITE));
    memset(p, 0x6a, 65536); cb_landing_poll();
    assert(p[0] == 0x6a && p[66] == 0x6a);
    assert(VirtualFree(p, 0, MEM_RELEASE)); assert(VirtualFree(other, 0, MEM_RELEASE));
}
int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc > 1 && !strcmp(argv[1], "--reproduce-old-lifetime")) {
        reset_buffers(); unsigned char *p = guest_memory(); init_buffer(0, p, 4);
        assert(VirtualFree(p, 65536, MEM_DECOMMIT));
        cb_landing_poll(); /* expected access violation: no retirement before decommit */
        return 1;
    }
    for (int state = 1; state <= 4; state++) {
        retire_and_decommit(state, 0, 0); retire_and_decommit(state, 1, 0);
    }
    retire_and_decommit(2, 0, 1); /* pending submission must finish first */
    retire_and_decommit(4, 0, 1); /* staging copy was submitted but has not completed */
    retire_and_decommit(4, 0, 2); /* staging copy is still in the command buffer being recorded */
    reset_buffers();
    unsigned char *p = guest_memory(); init_buffer(0, p, 4);
    Range before = {(uint64_t)(uintptr_t)p - 8, 8}, after = {(uint64_t)(uintptr_t)p + sizeof snapshot, 1};
    nvn_vk_memory_retire(&before); nvn_vk_memory_retire(&after);
    assert(g_cb[0].state == 4 && g_cb_nkept == 1); /* adjacent ranges are not overlaps */
    Range empty = {(uint64_t)(uintptr_t)p, 0}; nvn_vk_memory_retire(&empty);
    assert(g_cb[0].state == 4);
    Range all = {(uint64_t)(uintptr_t)p, 65536}; nvn_vk_memory_retire(&all);
    assert(!g_cb_nkept && g_kept_lo == ~0ull && !g_kept_hi);
    assert(VirtualFree(p, 0, MEM_RELEASE));
    puts("PASS live, pending, kept and asynchronous compute retirement; submission and waits; partial overlap, adjacent and empty ranges; guest writes, tail, decommit and reuse");
    return 0;
}
