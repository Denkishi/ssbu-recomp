/* NVN on Vulkan, the drawing part (session 2; design: plan\phases\V1_vulkan.md). Included at the end of nvn_vk.c: it uses
 * that file's device (V), loader functions and error helpers. Render thread only.
 *
 * What is here: device features, a block allocator, the stream ring (guest memory -> one mapped buffer, at most one copy
 * of a range per batch), images and views made at first use from the front end's texture source, samplers, translated
 * stages -> SPIR-V (ssbu_glslang.dll, cached in ..\cache\vk\spv) -> pipelines made synchronously, state tracking with
 * dynamic state, dynamic rendering, clears, draws, buffer->texture and texture->texture copies, present by blit, the
 * screenshot of the window texture, fences. Anything else stops with its name (nvn_vk_missing).
 *
 * Debug: SSBU_VK_DEBUG=1 turns on the checks that stand in for the validation layer that is not installed (image used
 * after destroy, barrier inside a pass, a view of an image in an unexpected layout) and prints every new pipeline. */
#include <stdarg.h>
#include <math.h>
#include "tex_decode.h"
static int have_targets_live(void);

#define VKD(X) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkGetFenceStatus) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache)
#define X(n) static PFN_##n n;
VKD(X)
#undef X
static PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT;
static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fatal(#call, r_); } while (0)

static struct {
    VkPhysicalDeviceFeatures2 f2; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12; VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids; VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc; VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT iu8; VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr; VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vad;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    const char *exts[16]; uint32_t next;
    int has_vids, has_cbc, has_pv, has_iu8, has_plr, has_vad, has_push, has_eds3, debug;
    int has_gpl, ext_gpl, ext_so;
    VkPhysicalDeviceMemoryProperties mp;
} D;
static void vkd_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("nvn_vk: FATAL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); ExitProcess(6);
}
#define DBG_ASSERT(c, ...) do { if (D.debug && !(c)) vkd_fail(__VA_ARGS__); } while (0)
/* Session 11: the pipeline-library mode. SSBU_VK_GPL wins (0 = whole pipelines, the path before session 9; 1 = a library per
 * stage pair, a measurement mode; 2 = a library per stage + preparation + barrier); without it VK_GPL_DEFAULT. */
#ifndef VK_GPL_DEFAULT
#define VK_GPL_DEFAULT 2                           /* session 11: on by default where the driver has the extension (validation run s11v3 and the first-run table of V1_vulkan.md "Session 11"); -DVK_GPL_DEFAULT=0 or SSBU_VK_GPL=0 for the old path */
#endif
static int vk_gpl_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_VK_GPL"); m = e && e[0] ? atoi(e) : VK_GPL_DEFAULT; } return m; }

/* ---------------------------------------------------------------- device features */
static void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext) {
    void **tail;
    vkGetPhysicalDeviceFeatures2 = vk_sym("vkGetPhysicalDeviceFeatures2");
    vkGetPhysicalDeviceFormatProperties = vk_sym("vkGetPhysicalDeviceFormatProperties");
    vkGetPhysicalDeviceMemoryProperties = vk_sym("vkGetPhysicalDeviceMemoryProperties");
    vkGetDeviceProcAddr = vk_sym("vkGetDeviceProcAddr");
    D.debug = getenv("SSBU_VK_DEBUG") && atoi(getenv("SSBU_VK_DEBUG"));
    D.f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; D.f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    D.f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; D.f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    D.f2.pNext = &D.f11; D.f11.pNext = &D.f12; D.f12.pNext = &D.f13; tail = &D.f13.pNext;
    D.exts[D.next++] = "VK_KHR_swapchain";
#define EXT(flag, name, st, stype) if (has_name(dext, ndext, name)) { D.flag = 1; D.exts[D.next++] = name; D.st.sType = stype; *tail = &D.st; tail = &D.st.pNext; }
    EXT(has_vids, "VK_EXT_vertex_input_dynamic_state", vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT)
    EXT(has_cbc, "VK_EXT_custom_border_color", cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT)
    EXT(has_pv, "VK_EXT_provoking_vertex", pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT)
    EXT(has_iu8, "VK_EXT_index_type_uint8", iu8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT)
    EXT(has_plr, "VK_EXT_primitive_topology_list_restart", plr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT)
    EXT(has_vad, "VK_EXT_vertex_attribute_divisor", vad, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)
    if (!(getenv("SSBU_VK_EDS3") && !atoi(getenv("SSBU_VK_EDS3")))) { EXT(has_eds3, "VK_EXT_extended_dynamic_state3", eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) }
    /* session 9: graphics pipeline libraries (nvn_vk_m4.c "graphics pipeline libraries"): the stage pair is compiled at loading
     * without the pass formats, a draw only LINKS. SSBU_VK_GPL=1 turns it on (needs extended dynamic state 3 as well). */
    D.ext_gpl = has_name(dext, ndext, "VK_EXT_graphics_pipeline_library") && has_name(dext, ndext, "VK_KHR_pipeline_library"); D.ext_so = has_name(dext, ndext, "VK_EXT_shader_object");
    if (D.ext_gpl && vk_gpl_mode()) { D.exts[D.next++] = "VK_KHR_pipeline_library"; EXT(has_gpl, "VK_EXT_graphics_pipeline_library", gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT) }
#undef EXT
    if (has_name(dext, ndext, "VK_KHR_push_descriptor") && !(getenv("SSBU_VK_PUSH") && !atoi(getenv("SSBU_VK_PUSH")))) { D.has_push = 1; D.exts[D.next++] = "VK_KHR_push_descriptor"; }
    vkGetPhysicalDeviceFeatures2(V.gpu, &D.f2);                    /* every supported feature of these structures is enabled */
    D.has_vids = D.has_vids && D.vids.vertexInputDynamicState; D.has_cbc = D.has_cbc && D.cbc.customBorderColors && D.cbc.customBorderColorWithoutFormat;
    D.has_pv = D.has_pv && D.pv.provokingVertexLast; D.has_iu8 = D.has_iu8 && D.iu8.indexTypeUint8; D.has_plr = D.has_plr && D.plr.primitiveTopologyListRestart;
    /* session 4: blend / mask / polygon / multisample state out of the pipeline key, only when the GPU has ALL of these (else all stay in the key) */
    D.has_eds3 = D.has_eds3 && D.eds3.extendedDynamicState3ColorBlendEnable && D.eds3.extendedDynamicState3ColorBlendEquation && D.eds3.extendedDynamicState3ColorWriteMask && D.eds3.extendedDynamicState3PolygonMode &&
                 D.eds3.extendedDynamicState3AlphaToCoverageEnable && D.eds3.extendedDynamicState3SampleMask && (D.eds3.extendedDynamicState3DepthClampEnable || !D.f2.features.depthClamp);
    printf("nvn_vk: extended dynamic state 3 (blend enable / equation / write mask, polygon mode, alpha to coverage, sample mask, depth clamp dynamic = not in the pipeline key): %s\n", D.has_eds3 ? "yes" : "no");
    D.has_gpl = D.has_gpl && D.gpl.graphicsPipelineLibrary && D.has_eds3 && D.has_vids;
    printf("nvn_vk: driver support for pipelines without a hold: VK_EXT_graphics_pipeline_library %s, VK_EXT_shader_object %s, extended dynamic state 3 (all seven states) %s, vertex input dynamic state %s | pipeline libraries in use: %s\n",
           D.ext_gpl ? "yes" : "NO", D.ext_so ? "yes" : "NO", D.has_eds3 ? "yes" : "NO", D.has_vids ? "yes" : "NO", D.has_gpl ? "YES" : "no");
    /* session 11: ONE line that says which path draws and why */
    {   const char *e = getenv("SSBU_VK_GPL");
        int m = vk_gpl_mode();
        printf("nvn_vk_path: %s: %s (SSBU_VK_GPL %s; default %d; SSBU_VK_GPL=0 is the switch back to whole pipelines)\n",
               D.has_gpl && m >= 2 ? "STAGE LIBRARIES + preparation + loading barrier (a draw links, nothing is compiled in a fight)" : D.has_gpl ? "PAIR LIBRARIES (SSBU_VK_GPL=1, a measurement mode)" : "WHOLE PIPELINES (made by the workers from the list of earlier runs; a first use can hold a frame)",
               !m ? (e ? "turned off by SSBU_VK_GPL=0" : "off by default in this build") : D.has_gpl ? (e ? "asked for by SSBU_VK_GPL" : "the default: the driver has VK_EXT_graphics_pipeline_library, extended dynamic state 3 and dynamic vertex input") :
               !D.ext_gpl ? "the driver has NO VK_EXT_graphics_pipeline_library: fallen back" : "the driver lacks extended dynamic state 3 or dynamic vertex input: fallen back", e ? e : "not set", VK_GPL_DEFAULT);
    }
    if (!D.f13.dynamicRendering || !D.f13.synchronization2) vkd_fail("the device lacks dynamicRendering / synchronization2");
    if (!D.has_vids) nvn_vk_missing("a GPU without VK_EXT_vertex_input_dynamic_state (the vertex layout is not yet part of the pipeline key)");
    if (!D.f2.features.textureCompressionBC) nvn_vk_missing("a GPU without BC texture compression");
    di->enabledExtensionCount = D.next; di->ppEnabledExtensionNames = D.exts; di->pNext = &D.f2;
    printf("nvn_vk: features: vertex input dynamic %d, custom border colour %d, provoking vertex last %d, 8-bit indices %d, list restart %d, ASTC %d, depth clamp %d, wide lines %d, robust buffers %d\n",
           D.has_vids, D.has_cbc, D.has_pv, D.has_iu8, D.has_plr, (int)D.f2.features.textureCompressionASTC_LDR, (int)D.f2.features.depthClamp, (int)D.f2.features.wideLines, (int)D.f2.features.robustBufferAccess);
}

/* ---------------------------------------------------------------- memory: 64 MB blocks per memory type and class, first fit */
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
typedef struct { VkDeviceMemory mem; uint64_t size; void *map; uint32_t type; int cls, nfree, used; struct { uint64_t off, size; } fr[512]; } VBlock;
static VBlock *g_blk[2048];
static int g_nblk;
static LONG64 g_mem_total, g_mem_leak;
static VMem vmem_alloc(VkMemoryRequirements rq, VkFlags want, int cls) {
    VMem m = {-1, 0, 0, NULL, NULL};
    uint32_t type = 32;
    for (uint32_t i = 0; i < D.mp.memoryTypeCount && type == 32; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == 32) vkd_fail("no memory type for flags 0x%x (bits 0x%x)", (unsigned)want, (unsigned)rq.memoryTypeBits);
    uint64_t al = rq.alignment < 256 ? 256 : rq.alignment;
    for (int pass = 0; pass < 2; pass++) {
        for (int b = 0; b < g_nblk; b++) {
            VBlock *k = g_blk[b];
            if (!k || k->type != type || k->cls != cls) continue;
            for (int i = 0; i < k->nfree; i++) {
                uint64_t start = (k->fr[i].off + al - 1) / al * al, end = k->fr[i].off + k->fr[i].size;
                if (start + rq.size > end) continue;
                uint64_t head = start - k->fr[i].off, tail = end - (start + rq.size);
                if (head && tail) { if (k->nfree == 512) continue; memmove(&k->fr[i + 2], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree++; k->fr[i].size = head; k->fr[i + 1].off = start + rq.size; k->fr[i + 1].size = tail; }
                else if (head) k->fr[i].size = head;
                else if (tail) { k->fr[i].off = start + rq.size; k->fr[i].size = tail; }
                else { memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; }
                m.block = b; m.off = start; m.size = rq.size; m.mem = k->mem; m.map = k->map ? (uint8_t *)k->map + start : NULL; k->used++;
                return m;
            }
        }
        if (pass) break;
        int b = 0;
        while (b < g_nblk && g_blk[b]) b++;
        if (b == 2048) vkd_fail("out of memory blocks");
        VBlock *k = calloc(1, sizeof *k);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        k->size = rq.size > (16u << 20) ? rq.size : (64u << 20); k->type = type; k->cls = rq.size > (16u << 20) ? 100 + b : cls;      /* large: a block of its own */
        ai.allocationSize = k->size; ai.memoryTypeIndex = type;
        VkResult r = vkAllocateMemory(V.device, &ai, NULL, &k->mem);
        if (r != VK_SUCCESS) vkd_fail("vkAllocateMemory of %llu MB (type %u): %s; %lld MB allocated so far", (unsigned long long)(k->size >> 20), type, vk_result_name(r), (long long)(g_mem_total >> 20));
        if (D.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VKC(vkMapMemory(V.device, k->mem, 0, VK_WHOLE_SIZE, 0, &k->map));
        k->nfree = 1; k->fr[0].off = 0; k->fr[0].size = k->size;
        g_blk[b] = k; if (b == g_nblk) g_nblk++;
        g_mem_total += (LONG64)k->size;
        cls = k->cls;
    }
    vkd_fail("allocation of %llu bytes failed in a fresh block", (unsigned long long)rq.size);
    return m;
}
static void vmem_free(VMem m) {
    if (m.block < 0) return;
    VBlock *k = g_blk[m.block];
    if (--k->used == 0 && k->cls >= 100) { vkFreeMemory(V.device, k->mem, NULL); g_mem_total -= (LONG64)k->size; free(k); g_blk[m.block] = NULL; return; }
    int i = 0;
    while (i < k->nfree && k->fr[i].off < m.off) i++;
    if (i > 0 && k->fr[i - 1].off + k->fr[i - 1].size == m.off) { k->fr[i - 1].size += m.size; if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i - 1].size += k->fr[i].size; memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; } return; }
    if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i].off = m.off; k->fr[i].size += m.size; return; }
    if (k->nfree == 512) { g_mem_leak += (LONG64)m.size; return; }
    memmove(&k->fr[i + 1], &k->fr[i], sizeof k->fr[0] * (size_t)(k->nfree - i)); k->nfree++;
    k->fr[i].off = m.off; k->fr[i].size = m.size;
}
static VkBuffer vbuf_new(uint64_t size, VkFlags usage, VMem *mem) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkBuffer b;
    VkMemoryRequirements rq;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(V.device, &bi, NULL, &b));
    vkGetBufferMemoryRequirements(V.device, b, &rq);
    *mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VKC(vkBindBufferMemory(V.device, b, mem->mem, mem->off));
    return b;
}

/* ---------------------------------------------------------------- command buffers in flight */
#define NSLOT 8
typedef struct { VkCommandBuffer cmd; VkFence fence; VkDescriptorPool pool[16]; int npool, ipool, pending; uint64_t serial; LONG64 ts_frame; } Slot;
typedef struct VTex VTex;
static struct {
    Slot slot[NSLOT]; int cur, recording; uint64_t serial, done;      /* serial: number of the last submission; done: every submission up to here has finished */
    VkCommandBuffer cmd;
    /* stream ring */
    VkBuffer ring; VMem ring_mem; uint64_t ring_pos, ring_size; uint32_t gen;
    struct { uint64_t addr, size, off; uint32_t gen; } acache[8192];
    VkBuffer zero; VMem zero_mem;
    /* pass */
    int in_pass, pass_w, pass_h;
    int pass_rs; float pass_sx, pass_sy;           /* track O: the pass draws into scaled images (pass_w, pass_h are the images'; sx, sy = image / game) */
    /* NVN state as left by the commands executed so far */
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_tex[6][32], c2[6][112];
    uint32_t tex_int[6], tex_sint[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds; uint32_t chanmask, sample_mask;
    int sref[2], svmask[2], swmask[2];
    float alpha_ref; int alpha_func;
    uint8_t blend[8][8], blend_on, logic, poly[4], ms[24];
    float viewport[4], drange[2], blendc[4], point_size, line_width, polyoff[3];
    int scissor[4], have_viewport, have_scissor, rast_discard, depth_clamp, restart, render_off;
    int ncol; TargetRef col[8], depth; int have_targets;
    int64_t row_stride, image_stride;
    int dyn_dirty, vi_dirty, dyn3_dirty, dyn3_samples;      /* dyn3: the extended-dynamic-state-3 group (blend, masks, polygon mode, multisample) */
    void *pipe_bound;
    LONG64 frame_draws, frame_skipped, frame_clears, pipelines, modules, uploads, upload_bytes, images, images_live, views, ring_wraps, feedback, flushes, sets;
    LONG64 sets_reused, tset_collisions, dummy_kind;
    LONG64 dispatches, cb_bytes, cb_syncs, cb_draws, cb_made, cb_waits, cb_wait_ticks, cb_guest_words, cb_conflict_words, readbacks;
} R;

static void pass_end(void) { if (R.in_pass) { vkCmdEndRendering(R.cmd); R.in_pass = 0; } }
static LONG64 g_tm[12];                          /* ticks: 0 wait for a submission, 1 copy-back, 2 dispatch, 3 batch replay, 4 present, 5 pipelines, 6 modules, 7 copy-back wait at batch start, 8 draw preparation */
static LONG64 tk(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
/* Session 5, SSBU_VK_GPUTIME=1: two timestamp queries per command buffer (written at its first and after its last command),
 * read when the buffer has finished; "nvn_vk_gpu:" line every 300 frames. The span of a buffer includes the time the GPU
 * gave to other processes while it ran. */
static int g_ts_on = -1;
static VkQueryPool g_ts_pool;
static double g_ts_ns;
static LONG64 g_ts_n;
/* Session 9: the same per FRAME (the frame a buffer was recorded for), with the time between two presents and whether the frame
 * was a battle frame (compute dispatches and more than 120 draws): median / p99 of the battle frames at exit ("nvn_vk_gpu_fight:"). */
#define FRSTAT_N 40000
static float g_fr_gpu[FRSTAT_N], g_fr_wall[FRSTAT_N];
static uint8_t g_fr_fight[FRSTAT_N];
static LONG64 g_cur_frame, g_battle_frames;
static void slot_wait(Slot *s) {
    if (!s->pending) return;
    LONG64 t0 = tk();
    VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, 20000000000ull);
    g_tm[0] += tk() - t0;
    if (r != VK_SUCCESS) vkd_fail("a submitted command buffer did not finish within 20 s (%s): the GPU is stuck or the device is lost", vk_result_name(r));
    VKC(vkResetFences(V.device, 1, &s->fence));
    if (g_ts_on > 0) {                             /* SSBU_VK_GPUTIME=1: the GPU's own clock at the first and the last command of this buffer */
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(V.device, g_ts_pool, (uint32_t)(s - R.slot) * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) { double ns = (double)(ts[1] - ts[0]) * (double)V.props.limits.timestampPeriod; g_ts_ns += ns; g_ts_n++; if (s->ts_frame >= 0 && s->ts_frame < FRSTAT_N) g_fr_gpu[s->ts_frame] += (float)(ns / 1e6); }
    }
    s->pending = 0;
    if (s->serial > R.done) R.done = s->serial;
}
static void garbage_collect(void);
static uint32_t g_tset_gen = 1;                   /* texture descriptor sets live as long as the command buffer whose pools they come from */
static int g_tset_used, g_tset_list_used;
static void cmd_begin(void) {
    if (R.recording) return;
    g_tset_gen++; g_tset_used = 0; g_tset_list_used = 0;
    R.cur = (R.cur + 1) % NSLOT;
    Slot *s = &R.slot[R.cur];
    slot_wait(s);
    for (int i = 0; i < s->npool; i++) VKC(vkResetDescriptorPool(V.device, s->pool[i], 0));
    s->ipool = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(s->cmd, 0));
    VKC(vkBeginCommandBuffer(s->cmd, &bi));
    R.cmd = s->cmd; R.recording = 1; R.in_pass = 0; R.pipe_bound = NULL; R.dyn_dirty = R.vi_dirty = 1;
    if (g_ts_on < 0) {
        g_ts_on = getenv("SSBU_VK_GPUTIME") && atoi(getenv("SSBU_VK_GPUTIME")) && V.props.limits.timestampComputeAndGraphics;
        if (g_ts_on) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NSLOT * 2; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_ts_pool)); }
    }
    s->ts_frame = g_cur_frame;
    if (g_ts_on > 0) { vkCmdResetQueryPool(s->cmd, g_ts_pool, (uint32_t)R.cur * 2, 2); vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2); }
    garbage_collect();
}
/* Ends the command buffer being recorded and gives it to the queue. Returns its number (the last one when nothing was recorded). */
static uint64_t cmd_submit(VkSemaphore wait, VkSemaphore signal) {
    if (!R.recording) { if (!wait && !signal) return R.serial; cmd_begin(); }
    pass_end();
    Slot *s = &R.slot[R.cur];
    if (g_ts_on > 0) vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2 + 1);
    VKC(vkEndCommandBuffer(s->cmd));
    VkFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo sb = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (wait) { sb.waitSemaphoreCount = 1; sb.pWaitSemaphores = &wait; sb.pWaitDstStageMask = &stage; }
    if (signal) { sb.signalSemaphoreCount = 1; sb.pSignalSemaphores = &signal; }
    sb.commandBufferCount = 1; sb.pCommandBuffers = &s->cmd;
    VKC(vkQueueSubmit(V.queue, 1, &sb, s->fence));
    s->pending = 1; s->serial = ++R.serial;
    R.recording = 0; R.cmd = NULL; R.flushes++;
    return R.serial;
}
static void cmd_wait_all(void) { cmd_submit(NULL, NULL); for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]); R.done = R.serial; }

/* ---------------------------------------------------------------- stream ring */
#define RING_ALIGN 256
static void cb_ring_wrap(void);
static uint64_t ring_raw(const void *p, uint64_t n) {
    R.ring_pos = (R.ring_pos + RING_ALIGN - 1) & ~(uint64_t)(RING_ALIGN - 1);
    if (n > R.ring_size / 4) vkd_fail("a range of %llu bytes does not fit the stream ring (%llu MB)", (unsigned long long)n, (unsigned long long)(R.ring_size >> 20));
    if (R.ring_pos + n > R.ring_size) { cmd_wait_all(); cb_ring_wrap(); R.ring_pos = 0; R.gen++; R.ring_wraps++; }     /* everything that reads the ring has finished */
    uint64_t off = R.ring_pos;
    if (p) memcpy((uint8_t *)R.ring_mem.map + off, p, (size_t)n);
    R.ring_pos += n;
    return off;
}
static uint64_t ring_put(uint64_t addr, uint64_t size) {
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19;
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (R.acache[i].gen == R.gen && R.acache[i].addr == addr && R.acache[i].size == size) return R.acache[i].off;
        if (R.acache[i].gen != R.gen) { h = i; break; }
    }
    uint64_t off = ring_raw(P(addr), size);
    h &= 8191;
    R.acache[h].addr = addr; R.acache[h].size = size; R.acache[h].off = off; R.acache[h].gen = R.gen;
    return off;
}

/* ---------------------------------------------------------------- formats */
static VkFormat comp_fmt(int comps, int i) {
    static const int b8[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}, b16[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
                     b32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    static const struct { uint8_t w, o; } m[12] = {{8, 0}, {8, 1}, {8, 4}, {8, 5}, {16, 6}, {16, 0}, {16, 1}, {16, 4}, {16, 5}, {32, 2}, {32, 0}, {32, 1}};      /* offsets: UNORM 0 SNORM 1 USCALED 2 SSCALED 3 UINT 4 SINT 5 SFLOAT 6; 32-bit: UINT 0 SINT 1 SFLOAT 2 */
    return (VkFormat)((m[i].w == 8 ? b8 : m[i].w == 16 ? b16 : b32)[comps - 1] + m[i].o);
}
static int fmt_supported(VkFormat f, VkFlags need) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(V.gpu, f, &p); return (p.optimalTilingFeatures & need) == need; }
/* NVN texture format -> Vulkan. *expand = 1 + component kind when a 3-component format is stored with 4; *decode = 1 when the texels are decoded to RGBA8 on the CPU. */
static VkFormat tex_vkfmt(int nf, int *expand, int *decode) {
    static int d24 = -1, d24s8, astc;
    if (d24 < 0) { d24 = fmt_supported(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); d24s8 = fmt_supported(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); astc = D.f2.features.textureCompressionASTC_LDR != 0; }
    *expand = 0; *decode = 0;
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (comps == 3) { *expand = 1 + i; comps = 4; }
        return comp_fmt(comps, i);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        if (astc) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * ((nf - 0x79) % 14) + (nf >= 0x87));
        *decode = 1;
        return nf >= 0x87 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    switch (nf) {
    case 0x31: return VK_FORMAT_S8_UINT; case 0x32: return VK_FORMAT_D16_UNORM; case 0x33: return d24 ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT; case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT; case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB; case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16; case 0x3A: case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16; case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK; case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK; case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK; case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK; case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK; case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK; case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK; case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK; case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK; case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16; case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM; case 0x9B: case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}
static VkFlags fmt_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static VkFormat vertex_vkfmt(int nf) {
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) return comp_fmt(nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12);
    if (nf >= 0x51 && nf <= 0x68) {
        int comps = (nf - 0x51) / 6 + 1, t = (nf - 0x51) % 6;
        if (t >= 4) return VK_FORMAT_UNDEFINED;                       /* 32-bit scaled: Vulkan has none */
        return (VkFormat)(comp_fmt(comps, t < 2 ? 0 : 5) + 2 + (t & 1));
    }
    switch (nf) {
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

/* ---------------------------------------------------------------- images */
typedef struct VView { EView e; int vtype, attach; VkImageView view; struct VView *next; } VView;
struct VTex {
    uint32_t magic; Tex *t; VkImage img; VMem mem; VkFormat fmt; VkImageLayout layout; VkFlags aspect;
    int w, h, d, levels, layers, samples, expand, decode, is3d, feedback, bpb, bw, bh;
    int rs; float sx, sy;                          /* track O: rs = 1 when the image is smaller than the game's texture (w, h are the image's; sx, sy = image / game) */
    VView *views; VTex *next, *prev;
};
/* ---- Track O: internal resolution scale (graphics.ini resolution_scale; SSBU_VK_SCALE wins; plan\phases\O1_options.md, session 7).
 * A texture of the screen class (2D, one layer, attachable, the game's size exactly 1920x1080 >> k, k = 0..6) gets an image
 * of round(size * scale); everything the game says in ITS pixels about such an image is converted where it is used:
 * viewport, scissor, clear rectangles, copy / blit / resolve / downsample regions, uploads and read-backs (through a
 * temporary image of the game's size), the crop of the present. The size table of the shaders (c2) and the regions the game
 * reads keep the game's sizes. Fragment stages are translated with MxIn.screen_scale (another hash: ssrc_hb). At 1.0 none
 * of this runs: every branch is behind RS_ON or VTex.rs / R.pass_rs, which are then 0. */
static float g_rs = -1.0f;
static LONG64 g_rs_images, g_rs_stages, g_rs_fcs, g_rs_tf, g_rs_up, g_rs_down, g_rs_blits;
static float rs_scale(void) {
    if (g_rs < 0) {
        const char *e = getenv("SSBU_VK_SCALE");
        float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
        if (v < 0.25f) v = 0.25f;
        if (v > 1.0f) v = 1.0f;                    /* above 1.0 (supersampling) is not offered: not tested */
        g_rs = v;
    }
    return g_rs;
}
#define RS_ON (rs_scale() != 1.0f)
static int rs_screen_class(int w, int h) { for (int k = 0; k < 7; k++) if (w == 1920 >> k && h == 1080 >> k) return 1; return 0; }
/* A coordinate of the game's grid on a scaled image (nearest image texel edge, inside 0..max). */
static int rs_edge(int v, float s, int max) { int r = (int)floorf((float)v * s + 0.5f); return r < 0 ? 0 : r > max ? max : r; }
/* Session 9: the textures of the game's window (nvn_gl.c win_initialize keeps the list; also noted here at every present).
 * SSBU_VK_UI_NATIVE=1 (resolution scale, experiment): a window texture is NOT scaled, so what the game draws straight into it
 * (its last full-screen pass and the 2D interface after it) is drawn at the game's 1920x1080. */
int nvn_front_tex_is_window(const Tex *t);
static Tex *g_wintex[8];
static LONG64 g_rs_mixed;
static int wintex_is(const Tex *t) { if (nvn_front_tex_is_window(t)) return 1; for (int i = 0; i < 8; i++) if (g_wintex[i] == t) return 1; return 0; }
static void wintex_note(Tex *t) { if (!t || wintex_is(t)) return; for (int i = 0; i < 8; i++) if (!g_wintex[i]) { g_wintex[i] = t; return; } }
static int rs_ui_native(void) { static int on = -1; if (on < 0) on = getenv("SSBU_VK_UI_NATIVE") && atoi(getenv("SSBU_VK_UI_NATIVE")); return on; }
static VTex *g_vtex_all;
static struct { VTex *vt; uint64_t serial; } *g_garbage;
static int g_ngarbage, g_garbage_cap;
static VkFlags layout_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT; case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT; case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default: return 0;
    }
}
static void image_barrier(VkImage img, VkFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    DBG_ASSERT(!R.in_pass, "debug: an image layout change inside a render pass");
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange.aspectMask = aspect; b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS; b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    b.srcAccessMask = layout_access(from); b.dstAccessMask = layout_access(to); b.oldLayout = from; b.newLayout = to;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void vt_layout(VTex *vt, VkImageLayout to) {
    if (vt->layout == to) return;
    pass_end();
    cmd_begin();
    image_barrier(vt->img, vt->aspect, vt->layout, to);
    vt->layout = to;
}
static void vt_check(VTex *vt) { if (vt->magic != 0x56544558) vkd_fail("an image is used after it was destroyed (texture record %p)", (void *)vt); }
static void vt_destroy(VTex *vt) {
    for (VView *v = vt->views, *n; v; v = n) { n = v->next; vkDestroyImageView(V.device, v->view, NULL); free(v); }
    vkDestroyImage(V.device, vt->img, NULL);
    vmem_free(vt->mem);
    if (vt->prev) vt->prev->next = vt->next; else g_vtex_all = vt->next;
    if (vt->next) vt->next->prev = vt->prev;
    vt->magic = 0xDEADDEAD; R.images_live--;
    free(vt);
}
static void garbage_collect(void) {
    int n = 0;
    LONG64 t0 = g_ngarbage ? tk() : 0;
    for (int i = 0; i < g_ngarbage; i++) if (g_garbage[i].serial <= R.done) vt_destroy(g_garbage[i].vt); else g_garbage[n++] = g_garbage[i];
    if (t0 && g_ngarbage - n >= 50) {              /* session 8: what destroying many images at once costs (the end of a screen) */
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double ms = (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart;
        static int nl;
        if (ms > 10.0 && nl++ < 40) printf("nvn_vk_tex: frame %lld: %d images destroyed in %.1f ms\n", (long long)nvn_front_frame_next(), g_ngarbage - n, ms);
    }
    g_ngarbage = n;
}
static void vk_drop(int kind, void *host) {
    if (kind != K_TEX) return;
    Tex *t = host;
    VTex *vt = t->vk;
    if (!vt) return;
    t->vk = NULL;
    for (int i = 0; i < 8; i++) if (R.col[i].tex == t) { pass_end(); R.col[i].tex = NULL; }
    if (R.depth.tex == t) { pass_end(); R.depth.tex = NULL; }
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;       /* the command buffer being recorded may still name it */
}
static int lvl_(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Track O. A temporary image of w x h (one level, one layer, the format of `like`) for an upload into, or a read-back from,
 * an image that is smaller than the game's texture. The command buffer is being recorded; the image goes when it has finished. */
static VTex *rs_temp(VTex *like, int w, int h) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    vt->magic = 0x56544558; vt->fmt = like->fmt; vt->aspect = like->aspect; vt->w = w; vt->h = h; vt->d = 1; vt->levels = 1; vt->layers = 1; vt->samples = 1;
    vt->bpb = like->bpb; vt->bw = like->bw; vt->bh = like->bh;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vt->fmt; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;
    return vt;
}
static VkFilter rs_filter(VTex *vt) { return vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
/* Texels of the game's grid (w x h at `off` of the stream ring, for the rectangle x, y of one level) into a scaled image:
 * through a temporary image of the game's size and a filtered blit. The command buffer is being recorded, no pass is open. */
static void rs_upload(VTex *vt, int level, int layer, uint64_t off, uint32_t rowlen, uint32_t rows, int x, int y, int w, int h) {
    int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
    VkBufferImageCopy c;
    VkImageBlit b;
    if (w <= 0 || h <= 0) return;
    if (!fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) { nvn_vk_missing("resolution scale: an upload into a scaled texture whose format cannot be blitted"); return; }
    VTex *tmp = rs_temp(vt, w, h);
    memset(&c, 0, sizeof c); memset(&b, 0, sizeof b);
    c.bufferOffset = off; c.bufferRowLength = rowlen; c.bufferImageHeight = rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(R.cmd, R.ring, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.mipLevel = (uint32_t)level; b.dstSubresource.baseArrayLayer = (uint32_t)layer; b.dstSubresource.layerCount = 1;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = rs_edge(x, vt->sx, lw); b.dstOffsets[0].y = rs_edge(y, vt->sy, lh); b.dstOffsets[1].x = rs_edge(x + w, vt->sx, lw); b.dstOffsets[1].y = rs_edge(y + h, vt->sy, lh); b.dstOffsets[1].z = 1;
    if (b.dstOffsets[1].x <= b.dstOffsets[0].x || b.dstOffsets[1].y <= b.dstOffsets[0].y) return;
    vkCmdBlitImage(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
    g_rs_up++;
}
static VTex *g_put_vt;
static uint32_t g_fmt_hist[256];                   /* images made per NVN format (printed with the totals) */
static void vt_put(Tex *t, int level, int layer, int w, int h, int d, const void *data, size_t size, int rgba8, int64_t row_stride) {
    VTex *vt = g_put_vt;
    uint8_t *tmp = NULL;
    (void)t;
    if (level >= vt->levels) return;
    if (vt->expand && !rgba8) {                    /* 3 components -> 4, the fourth is one */
        static const uint8_t csize[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
        static const uint32_t one[12] = {255, 127, 1, 1, 0x3C00, 65535, 32767, 1, 1, 0x3F800000, 1, 1};
        int cs = csize[vt->expand - 1];
        size_t n = (size_t)w * h * d, stride = row_stride ? (size_t)row_stride : (size_t)w * 3 * cs;
        tmp = malloc(n * 4 * cs);
        for (size_t y = 0; y < (size_t)h * d; y++)
            for (size_t x = 0; x < (size_t)w; x++) { uint8_t *o = tmp + (y * w + x) * 4 * cs; memcpy(o, (const uint8_t *)data + y * stride + x * 3 * cs, 3 * (size_t)cs); memcpy(o + 3 * cs, &one[vt->expand - 1], (size_t)cs); }
        data = tmp; size = n * 4 * cs; row_stride = 0;
    }
    uint64_t off = ring_raw(data, size);           /* may submit what was recorded so far */
    free(tmp);
    pass_end(); cmd_begin();
    if (vt->rs) {                                  /* track O: the texture's own memory holds the game's size */
        if (!vt->is3d && (uint32_t)layer < (uint32_t)vt->layers) rs_upload(vt, level, layer, off, row_stride && vt->bpb ? (uint32_t)(row_stride / vt->bpb) : 0, 0, 0, 0, w, h);
        R.upload_bytes += (LONG64)size;
        return;
    }
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    if (row_stride && vt->bpb) c.bufferRowLength = (uint32_t)(row_stride / vt->bpb);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = (uint32_t)(vt->is3d ? d : 1);
    if (!vt->is3d && (uint32_t)layer >= (uint32_t)vt->layers) return;
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    R.upload_bytes += (LONG64)size;
}
/* The image of a texture: made at first use, and holding what the guest memory holds (unless the GPU wrote it). */
/* Session 8: textures made and read AHEAD of their first draw (tex_ahead, below), and what is still made at a draw. */
static LONG64 g_ta_n, g_ta_bytes, g_ta_tk, g_ta_max_tk, g_late_n, g_late_tk, g_late_fight_n, g_late_fight_tk, g_up_fr_tk, g_up_fr_bytes;
static int g_up_fr_n, g_ta_inside;
static volatile LONG g_in_fight;                   /* nvn_vk_m4.c (included further down) */
static VTex *vt_get2(Tex *t, int sync) {
    VTex *vt = t->vk;
    LONG64 tu0 = 0, ub0 = R.upload_bytes;
    if (!vt) {
        tu0 = tk();
        TexBuilder *b = &t->b;
        Fmt f;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        vt = calloc(1, sizeof *vt);
        vt->magic = 0x56544558; vt->t = t;
        vt->fmt = tex_vkfmt(b->format, &vt->expand, &vt->decode);
        if (!vt->fmt || !nvn_front_fmt(b->format, &f)) { static char msg[80]; snprintf(msg, sizeof msg, "texture format 0x%x", b->format); nvn_vk_missing(msg); vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; nvn_front_fmt(0x25, &f); }
        vt->bpb = vt->decode ? 4 : vt->expand ? f.bpb / 3 * 4 : f.bpb; vt->bw = vt->decode ? 1 : f.bw; vt->bh = vt->decode ? 1 : f.bh;
        vt->aspect = fmt_aspect(vt->fmt);
        vt->w = b->width > 0 ? b->width : 1; vt->h = b->height > 0 ? b->height : 1; vt->d = b->depth > 0 ? b->depth : 1;
        vt->levels = nvn_front_tex_levels(t); vt->layers = nvn_front_tex_layers(t); vt->samples = b->samples > 1 ? b->samples : 1; vt->is3d = b->target == 2;
        if (b->target == 0 || b->target == 3) { ci.imageType = VK_IMAGE_TYPE_1D; vt->h = 1; if (b->target == 3) vt->layers = b->height > 0 && b->depth <= 1 ? b->height : vt->layers; }
        else ci.imageType = vt->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        if (RS_ON && (b->target == 1 || b->target == 5) && vt->layers == 1 && !vt->decode && rs_screen_class(vt->w, vt->h) && !(rs_ui_native() && wintex_is(t)) &&
            (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (f.kind != 4 && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)))) {      /* track O: a screen-class target */
            int gw = vt->w, gh = vt->h, lv = 1;
            vt->w = (int)((float)gw * rs_scale() + 0.5f); vt->h = (int)((float)gh * rs_scale() + 0.5f);
            if (vt->w < 1) vt->w = 1; if (vt->h < 1) vt->h = 1;
            vt->rs = 1; vt->sx = (float)vt->w / (float)gw; vt->sy = (float)vt->h / (float)gh;
            while ((vt->w >> lv) || (vt->h >> lv)) lv++;
            if (vt->levels > lv) vt->levels = lv;
            g_rs_images++;
        }
        ci.format = vt->fmt; ci.extent.width = (uint32_t)vt->w; ci.extent.height = (uint32_t)vt->h; ci.extent.depth = (uint32_t)(vt->is3d ? vt->d : 1);
        ci.mipLevels = (uint32_t)vt->levels; ci.arrayLayers = (uint32_t)(vt->is3d ? 1 : vt->layers); ci.samples = (VkSampleCountFlagBits)vt->samples; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        int can_color = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && f.kind != 4 && !vt->decode && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        if (can_color) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (ci.imageType == VK_IMAGE_TYPE_2D && vt->w == vt->h && vt->layers >= 6 && vt->samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(V.device, &ci, NULL, &vt->img);
        if (r != VK_SUCCESS) vkd_fail("vkCreateImage (NVN format 0x%x -> %d, target %d, %dx%dx%d, %d levels, %d layers, %d samples): %s", b->format, (int)vt->fmt, b->target, vt->w, vt->h, vt->d, vt->levels, vt->layers, vt->samples, vk_result_name(r));
        VkMemoryRequirements rq;
        vkGetImageMemoryRequirements(V.device, vt->img, &rq);
        vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
        vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
        t->vk = vt;
        R.images++; R.images_live++; nvn_front_count(5, 1);
        g_fmt_hist[b->format & 255]++;
        if (can_color || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {      /* an image nothing was written to reads as zero (the OpenGL driver's behaviour, which the comparison saw) */
            VkImageSubresourceRange sr = {vt->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (vt->samples == 1) {
                if (can_color) { VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
                else { VkClearDepthStencilValue z = {0.0f, 0}; vkCmdClearDepthStencilImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
            }
        }
    }
    vt_check(vt);
    if (sync && nvn_front_tex_changed(t)) {
        if (!tu0) tu0 = tk();
        g_put_vt = vt;
        cmd_begin();
        if (!nvn_front_tex_read(t, vt->decode, vt_put)) { static int n; if (n++ < 20) printf("nvn_vk: texture %dx%d format 0x%x: its memory is not readable; not uploaded\n", t->b.width, t->b.height, t->b.format); }
        R.uploads++;
    }
    if (tu0 && !g_ta_inside) {                     /* made or read again at its use (a draw, a copy, a target): the frame pays */
        LONG64 d = tk() - tu0;
        g_up_fr_tk += d; g_up_fr_n++; g_up_fr_bytes += R.upload_bytes - ub0;
        g_late_n++; g_late_tk += d;
        if (g_in_fight) { g_late_fight_n++; g_late_fight_tk += d; }
    }
    return vt;
}
static VTex *vt_get(Tex *t) { return vt_get2(t, 1); }
/* Session 8, render thread, after every present. A texture used to get its image, and its texels read out of the game's
 * memory (hash, de-swizzle, copy into the stream ring), at the first draw that sampled it: the first frame of a fight paid
 * 37-50 ms for the stage's and the fighters' textures (runs s7fn2 / s7gn2, frames 1804-1823). Here every block-compressed
 * texture the game has made (nvnTextureInitialize) and not yet drawn with is given its image two frames after the render
 * thread first saw it, under a time budget per frame (4 ms outside a fight, 1 ms in one; one texture is always taken, so a
 * very large one can exceed it): the loading screen absorbs the work. Only compressed formats: they cannot be render
 * targets, so nothing the GPU is about to write is touched; if the game fills the memory later, the draw sees the change
 * (nvn_front_tex_changed) and reads it again, as before. SSBU_VK_TEXAHEAD=0 = as before (everything at the first draw). */
static int fight_recent(void);                     /* nvn_vk_m4.c */
static void present_progress(float frac);          /* session 11: the held loading screen presented again with the progress bar (defined above vk_present) */
static LONG64 g_ovl_presents, g_ovl_frames;        /* extra presents of a held loading screen; normal frames drawn with the thin bar */
static int holdlog_on(void);
static void tex_ahead(LONG64 frame) {
    static int on = -1;
    static Tex *done_head;
    static LONG64 fq;
    if (on < 0) { const char *e = getenv("SSBU_VK_TEXAHEAD"); LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; on = !(e && !atoi(e)); }
    if (!on) return;
    Tex *head = nvn_front_tex_first();
    if (head == done_head) return;
    int fr = fight_recent();
    LONG64 t0 = tk(), budget = fq * (fr ? 1 : 4) / 1000;
    int pending = 0, taken = 0;
    g_ta_inside = 1;
    for (Tex *t = head; t && t != done_head; t = t->all_next) {
        Fmt f;
        if (t->ahead_seen < 0) continue;
        if (t->vk || t->dead) { t->ahead_seen = -1; continue; }
        if (!t->ahead_seen) { t->ahead_seen = frame + 1; pending++; continue; }
        if (frame + 1 - t->ahead_seen < 2) { pending++; continue; }
        if (t->gpu_written || (!t->mem && !t->b.packaged) || t->b.samples > 1 || t->b.width <= 0 || t->b.height <= 0 || !nvn_front_fmt(t->b.format, &f) || f.kind != 4) { t->ahead_seen = -1; continue; }
        if (taken && tk() - t0 > budget) { pending++; continue; }
        if (fr) {
            /* in and around a fight (a frame of the last 600 had fighters) only what is certainly cheap: at most 512 KB, no
             * decoding on the CPU, no wrap of the stream ring (a wrap waits for the GPU). Measured without this rule (runs s8b
             * frame 3549, s8d 4297, s8f 3840: the result screen's textures arriving while the fight ends, in frames without
             * a dispatch): 15-22 ms after one present and a frame of 34-35 ms. The others wait for a frame far from a fight,
             * or are made at their use as before. */
            VTex probe;
            if (t->data_size > (512u << 10) || R.ring_pos + 2 * (uint64_t)t->data_size + (1u << 20) > R.ring_size) { pending++; continue; }
            tex_vkfmt(t->b.format, &probe.expand, &probe.decode);
            if (probe.decode) { pending++; continue; }
        }
        LONG64 b0 = R.upload_bytes;
        vt_get(t);
        t->ahead_seen = -1; taken++;
        g_ta_n++; g_ta_bytes += R.upload_bytes - b0;
    }
    g_ta_inside = 0;
    LONG64 d = tk() - t0;
    g_ta_tk += d; if (d > g_ta_max_tk) g_ta_max_tk = d;
    if (d * 1000 > 6 * fq && holdlog_on()) { static int nl; if (nl++ < 100) printf("nvn_vk_tex: frame %lld (%s): %d textures made ahead after the present in %.1f ms\n", (long long)frame, fr ? "in or near a fight" : "far from a fight", taken, (double)d * 1000.0 / (double)fq); }
    if (!pending) done_head = head;
}
/* tex_view_obj's rules: the effective view of `t` through `v`. */
static void eview_of(Tex *t, VTex *vt, const View *v, EView *e) {
    EView base;
    memset(&base, 0, sizeof base);
    base.target = t->b.target; base.format = t->b.format; base.nlvl = vt->levels; base.nlay = nvn_front_tex_layers(t); base.dsmode = t->b.dsmode;
    memcpy(base.swz, t->b.swz, 4);
    *e = base;
    if (!v || !(v->set & 0x80)) return;
    if (v->set & 1) { e->lvl0 = v->lvl0; e->nlvl = v->nlvl; }
    if (v->set & 2) { e->lay0 = v->lay0; e->nlay = v->nlay; }
    if ((v->set & 4) && v->format) e->format = v->format;
    if (v->set & 8) memcpy(e->swz, v->swz, 4);
    if (v->set & 16) e->dsmode = v->dsmode;
    if (v->set & 32) e->target = v->target;
    if (e->lvl0 < 0 || e->lvl0 >= base.nlvl) e->lvl0 = 0;
    if (e->nlvl <= 0 || e->lvl0 + e->nlvl > base.nlvl) e->nlvl = base.nlvl - e->lvl0;
    if (e->lay0 < 0 || e->lay0 >= base.nlay) e->lay0 = 0;
    if (e->nlay <= 0 || e->lay0 + e->nlay > base.nlay) e->nlay = base.nlay - e->lay0;
    if (t->b.target == 2 || t->b.samples > 1) { e->target = base.target; e->lay0 = 0; e->nlay = base.nlay; }
    if ((e->target == 1 || e->target == 7 || e->target == 0) && e->nlay > 1) e->nlay = 1;
    if (e->target == 8 && e->nlay != 6) e->target = e->nlay == 1 ? 1 : 4;
    if (e->target == 9 && e->nlay % 6) e->target = 4;
}
enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1DA, DIM_2DA, DIM_CUBEA };
static const VkImageViewType k_vtype[7] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
static int target_dim(int target) { static const int m[10] = {DIM_1D, DIM_2D, DIM_3D, DIM_1DA, DIM_2DA, DIM_2D, DIM_2DA, DIM_2D, DIM_CUBE, DIM_CUBEA}; return target >= 0 && target < 10 ? m[target] : DIM_2D; }
/* dim < 0: an attachment view (one level, one layer, both aspects, no swizzle). */
static VkImageView vt_view(VTex *vt, const EView *e, int dim, int attach) {
    for (VView *n = vt->views; n; n = n->next) if (n->vtype == dim && n->attach == attach && !memcmp(&n->e, e, sizeof *e)) return n->view;
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    static const VkComponentSwizzle sw[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    int ex, dec;
    n->e = *e; n->vtype = dim; n->attach = attach;
    ci.image = vt->img; ci.format = vt->fmt;
    if (e->format != vt->t->b.format && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !vt->decode && !vt->expand) { VkFormat f = tex_vkfmt(e->format, &ex, &dec); if (f && !ex && !dec && fmt_aspect(f) == VK_IMAGE_ASPECT_COLOR_BIT) ci.format = f; }
    if (attach) {
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.subresourceRange.aspectMask = vt->aspect;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = 1; ci.subresourceRange.baseArrayLayer = (uint32_t)(vt->is3d ? 0 : e->lay0); ci.subresourceRange.layerCount = 1;
        if (vt->is3d) nvn_vk_missing("a 3D texture as render target");
    } else {
        int lay = e->nlay;
        if (dim == DIM_1D || dim == DIM_2D || dim == DIM_3D) lay = 1; else if (dim == DIM_CUBE) lay = 6; else if (dim == DIM_CUBEA) lay -= lay % 6;
        ci.viewType = k_vtype[dim];
        ci.subresourceRange.aspectMask = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (e->dsmode == 1 || vt->aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = (uint32_t)e->nlvl; ci.subresourceRange.baseArrayLayer = (uint32_t)(vt->is3d ? 0 : e->lay0); ci.subresourceRange.layerCount = (uint32_t)(vt->is3d ? 1 : lay);
        ci.components.r = sw[e->swz[0] < 6 ? e->swz[0] : 0]; ci.components.g = sw[e->swz[1] < 6 ? e->swz[1] : 0]; ci.components.b = sw[e->swz[2] < 6 ? e->swz[2] : 0]; ci.components.a = sw[e->swz[3] < 6 ? e->swz[3] : 0];
    }
    VkResult r = vkCreateImageView(V.device, &ci, NULL, &n->view);
    if (r != VK_SUCCESS) vkd_fail("vkCreateImageView (type %d, format %d of image format %d, levels %d+%d, layers %d+%d of %d, attach %d): %s", (int)ci.viewType, (int)ci.format, (int)vt->fmt, e->lvl0, e->nlvl, e->lay0, e->nlay, vt->layers, attach, vk_result_name(r));
    n->next = vt->views; vt->views = n; R.views++;
    return n->view;
}

/* ---------------------------------------------------------------- samplers (shared by value, never deleted before shutdown) */
static struct { SamplerParams p; VkSampler s; } g_smp[2048];
static int g_nsmp;
static VkSampler g_smp_default;
static VkSampler sampler_get(const SamplerParams *p) {
    static const VkSamplerAddressMode wrap[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (int i = 0; i < g_nsmp; i++) if (!memcmp(&g_smp[i].p, p, sizeof *p)) return g_smp[i].s;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo red = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    int minf = p->min < 6 ? p->min : 1;          /* nearest, linear, nearest-mip-nearest, linear-mip-nearest, nearest-mip-linear, linear-mip-linear */
    ci.minFilter = (minf & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.magFilter = p->mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap[p->ws & 7]; ci.addressModeV = wrap[p->wt & 7]; ci.addressModeW = wrap[p->wr & 7];
    ci.mipLodBias = p->bias; ci.minLod = p->lodmin < 0 ? 0 : p->lodmin; ci.maxLod = p->lodmax;
    if (minf < 2) { ci.minLod = 0; ci.maxLod = 0.25f; }      /* no mip maps: level 0 only (the Vulkan specification's own mapping of GL_NEAREST / GL_LINEAR) */
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    float aniso = p->aniso;
    {   /* track O: anisotropic_filtering in graphics.ini = at least that on samplers that filter linearly between mip levels (not shadow-compare samplers) */
        static LONG64 raised;
        float want = (float)gfx_options()->aniso;
        if (want > aniso && minf >= 4 && p->cmode != 1) { aniso = want; raised++; if (raised == 1 || raised % 100 == 0) printf("nvn_vk: anisotropic_filtering=%d: %lld samplers raised so far (this one from %.0f)\n", gfx_options()->aniso, (long long)raised, p->aniso); }
    }
    if (aniso > 1.0f && D.f2.features.samplerAnisotropy) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = aniso > V.props.limits.maxSamplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : aniso; }
    if (p->cmode == 1) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)(p->cfunc ? (p->cfunc - 1) & 7 : 7); }
    {   /* the OpenGL path can only use four border colours (bindless handles: sampler_gl); the same rounding here */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]}, mx = b[0] > b[1] ? b[0] : b[1];
        int rgb, al;
        if (b[2] > mx) mx = b[2];
        rgb = mx >= 0.5f; al = b[3] >= 0.5f;
        if (p->border_kind) { int32_t bi[4]; memcpy(bi, p->border, 16); rgb = bi[0] || bi[1] || bi[2]; al = bi[3] != 0; }
        if (!rgb) ci.borderColor = p->border_kind ? (al ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK) : (al ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        else if (al) ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else {
            int uses = (p->ws & 7) == 5 || (p->wt & 7) == 5 || (p->wr & 7) == 5;
            if (uses && !D.has_cbc) nvn_vk_missing("a sampler border colour (1, 1, 1, 0) on a GPU without VK_EXT_custom_border_color");
            if (D.has_cbc) {
                ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
                if (p->border_kind) { cb.customBorderColor.int32[0] = cb.customBorderColor.int32[1] = cb.customBorderColor.int32[2] = 1; }
                else { cb.customBorderColor.float32[0] = cb.customBorderColor.float32[1] = cb.customBorderColor.float32[2] = 1.0f; }
                cb.pNext = ci.pNext; ci.pNext = &cb;
            }
        }
    }
    if (p->reduction == 1 || p->reduction == 2) {
        if (!D.f12.samplerFilterMinmax) nvn_vk_missing("a min / max reduction sampler on a GPU without samplerFilterMinmax");
        else { red.reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX; red.pNext = ci.pNext; ci.pNext = &red; }
    }
    if (p->lod_snap != 0.0f) nvn_vk_missing("a sampler with LOD snap");
    VkSampler s;
    VKC(vkCreateSampler(V.device, &ci, NULL, &s));
    if (g_nsmp == 2048) vkd_fail("more than 2048 distinct samplers");
    g_smp[g_nsmp].p = *p; g_smp[g_nsmp++].s = s;
    return s;
}

#include "nvn_vk_m4.c"                             /* session 4: stages, pipelines, workers (M4) */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's stages and pipelines made at the draw); kept only until someone deletes the block */
/* ---------------------------------------------------------------- stages: machine code -> Vulkan GLSL -> SPIR-V -> module */
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h; int stage, code_len, ok; uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, nsmp;
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next;
} VStage;
/* M4 (design section 5): what a stage needs to become a module again without the translator and glslang, in
 * ..\cache\vk\stage\<stage>_<hash of machine code, constants, variant, translator version>.st */
#define MX_VK_VERSION 3u                           /* part of the stage hash: change it when mx_translate's Vulkan text changes */
typedef struct { uint32_t magic; int32_t stage, code_len; uint32_t key[4], fo; uint64_t spv; uint32_t cbufs, ssbos, ssbo_w, images, dep, tex_units, in_attr, out_attr; int32_t uses_alpha, nsmp; VSmp smp[64]; } StageRec;
static CRITICAL_SECTION g_sh_lock;                 /* the stage and pipeline tables while the start-up workers run */
static VkPipelineCache g_pcache;
static LONG64 g_pw_stages, g_pw_pipes, g_pw_failed, g_pipe_miss;
static VStage *g_vstage[4096];
static uint32_t g_vstage_ids;
static int (*g_glsl_to_spirv)(int, const char *, uint32_t **, size_t *, char *, size_t);
static void (*g_spirv_free)(uint32_t *);
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static uint64_t g_spv_unused;
static VkShaderModule module_from_spv(uint64_t h) {              /* any thread; NULL when the file is not there */
    char path[128];
    VkShaderModule mod = NULL;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t *words = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, fp) == (size_t)n && words[0] == 0x07230203) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = (size_t)n; ci.pCode = words;
        if (vkCreateShaderModule(V.device, &ci, NULL, &mod) != VK_SUCCESS) mod = NULL;
    }
    free(words); fclose(fp);
    return mod;
}
static void stage_rec_path(char *path, size_t n, int s, uint64_t h) { snprintf(path, n, "../cache/vk/stage/%d_%016llx.st", s, (unsigned long long)h); }
/* A stage from its record on disk (the caller holds g_sh_lock when workers run). NULL when there is none. */
static VStage *vstage_from_rec(int s, uint64_t h) {
    char path[128];
    StageRec r;
    stage_rec_path(path, sizeof path, s, h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t got = fread(&r, 1, sizeof r, fp);
    fclose(fp);
    if (got != sizeof r || r.magic != 0x52545356 + MX_VK_VERSION || r.stage != s || r.nsmp < 0 || r.nsmp > 64) return NULL;
    VkShaderModule mod = module_from_spv(r.spv);
    if (!mod) return NULL;
    VStage *v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = r.code_len; memcpy(v->key, r.key, 16); v->fo = r.fo; v->id = ++g_vstage_ids;
    v->cbufs = r.cbufs; v->ssbos = r.ssbos; v->ssbo_w = r.ssbo_w; v->images = r.images; v->dep = r.dep; v->tex_units = r.tex_units; v->in_attr = r.in_attr; v->out_attr = r.out_attr;
    v->uses_alpha = r.uses_alpha; v->nsmp = r.nsmp; memcpy(v->smp, r.smp, sizeof v->smp);
    v->mod = mod; v->ok = 1;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    InterlockedIncrement64(&R.modules);
    return v;
}
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash) {
    char path[128], log[2000];
    uint32_t *words = NULL;
    size_t count = 0;
    int from_dll = 0;
    VkShaderModule mod = NULL;
    LONG64 t_mod = tk();
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601, glsl, strlen(glsl));
    *spv_hash = h;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n > 20 && n % 4 == 0) { words = malloc((size_t)n); count = (size_t)n / 4; if (fread(words, 1, (size_t)n, fp) != (size_t)n || words[0] != 0x07230203) { free(words); words = NULL; } }
        fclose(fp);
    }
    if (!words) {
        if (!g_glsl_to_spirv) {
            HMODULE m = LoadLibraryA("ssbu_glslang.dll");
            g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "ssbu_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "ssbu_spirv_free") : NULL;
            if (!g_glsl_to_spirv || !g_spirv_free) vkd_fail("ssbu_glslang.dll cannot be loaded (build_glslang.bat makes it)");
        }
        if (!g_glsl_to_spirv(stage, glsl, &words, &count, log, sizeof log)) { printf("nvn_vk: %s: glslang rejected the translated text: %s\n", what, log); fflush(stdout); return NULL; }
        from_dll = 1;
        CreateDirectoryA("../cache/vk", NULL); CreateDirectoryA("../cache/vk/spv", NULL);
        fp = fopen(path, "wb");
        if (fp) { fwrite(words, 4, count, fp); fclose(fp); }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &mod));
    if (from_dll) g_spirv_free(words); else free(words);
    InterlockedIncrement64(&R.modules);
    g_tm[6] += tk() - t_mod;
    return mod;
}
static VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo) {
    Stage *st = &p->st[s];
    VStage *v = st->vk;
    if (!st->code) return NULL;
    uint32_t dep_a = v && s == 0 ? v->dep : 0;
    if (v && ((key[0] ^ v->key[0]) & dep_a) == 0 && ((key[1] ^ v->key[1]) & dep_a) == 0 && ((key[2] ^ v->key[2]) & v->tex_units) == 0 && ((key[3] ^ v->key[3]) & v->tex_units) == 0 && v->fo == fo) return v->ok ? v : NULL;
    const uint8_t *cdata = st->ctl[3] && st->ctl[4] ? st->code - (s == 5 ? 0 : 0x30) + st->ctl[4] : NULL;       /* a compute stage has no program header */
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
    if (s == 5) { h = fnv(h, st->local, sizeof st->local); h = fnv(h, &st->smem_bytes, 4); h = fnv(h, &st->lmem_bytes, 4); }
    if (cdata) h = fnv(h ^ st->ctl[3], cdata, st->ctl[3]);
    h = fnv(h, key, 16); h = fnv(h, &fo, 4);
    { uint32_t ver = MX_VK_VERSION; h = fnv(h, &ver, 4); }
    for (v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s && v->code_len == st->code_len && v->fo == fo && !memcmp(v->key, key, 16)) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v->ok ? v : NULL; }
    {   LONG64 t_mod = tk();
        v = vstage_from_rec(s, h);                 /* met in an earlier run: no translator, no glslang */
        g_tm[6] += tk() - t_mod;
        if (v) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v; }
    }
    MxIn in;
    MxOut out;
    char what[64];
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = s; in.code = st->code; in.code_len = st->code_len; in.cdata = cdata; in.cdata_size = cdata ? st->ctl[3] : 0;
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3]; in.force_out = fo;
    in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s; in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
    if (s == 5) {                                  /* compute has a layout of its own (design, section 4): the bindings of stage 0 */
        in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
        memcpy(in.local_size, st->local, sizeof in.local_size); in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    }
    v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = st->code_len; memcpy(v->key, key, 16); v->fo = fo; v->id = ++g_vstage_ids;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    st->vk = v;
    snprintf(what, sizeof what, "program #%d %s stage", p->serial, s == 5 ? "compute" : s ? "fragment" : "vertex");
    if (!mx_translate(&in, &out)) { printf("nvn_vk: %s: machine code not translated: %s\n", what, out.why); fflush(stdout); return NULL; }
    uint64_t spv_hash = 0;
    v->mod = module_from_glsl(s, out.glsl, what, &spv_hash);
    memcpy(v->local, st->local, sizeof v->local);
    v->cbufs = out.cbufs; v->ssbos = out.ssbos; v->ssbo_w = out.ssbo_written; v->images = out.images; v->in_attr = out.in_attr; v->out_attr = out.out_attr; v->dep = s == 0 ? out.int_dep : 0; v->tex_units = out.tex_units; v->uses_alpha = out.uses_alpha; v->nsmp = out.nsmp;
    if (s == 0) v->dep = out.int_dep; else v->tex_units = out.tex_units | out.int_dep;
    v->tex_units = out.tex_units;
    for (int i = 0; i < out.nsmp && i < 64; i++) {
        const char *ty = out.smp[i].type;
        int arr = strstr(ty, "Array") != NULL, dim = strstr(ty, "Cube") ? (arr ? DIM_CUBEA : DIM_CUBE) : strstr(ty, "3D") ? DIM_3D : strstr(ty, "1D") ? (arr ? DIM_1DA : DIM_1D) : (arr ? DIM_2DA : DIM_2D);
        if (strstr(ty, "MS") || strstr(ty, "Rect") || strstr(ty, "Buffer")) { static char msg[96]; snprintf(msg, sizeof msg, "a shader sampler of type %s", ty); nvn_vk_missing(msg); }
        v->smp[i].unit = out.smp[i].unit; v->smp[i].dim = (uint8_t)dim; v->smp[i].shadow = out.smp[i].shadow; v->smp[i].binding = out.smp[i].binding;
    }
    free(out.glsl);
    v->ok = v->mod != NULL;
    if (v->ok) {
        char path[128];
        StageRec r;
        memset(&r, 0, sizeof r);
        r.magic = 0x52545356 + MX_VK_VERSION; r.stage = s; r.code_len = v->code_len; memcpy(r.key, v->key, 16); r.fo = v->fo; r.spv = spv_hash;
        r.cbufs = v->cbufs; r.ssbos = v->ssbos; r.ssbo_w = v->ssbo_w; r.images = v->images; r.dep = v->dep; r.tex_units = v->tex_units; r.in_attr = v->in_attr; r.out_attr = v->out_attr;
        r.uses_alpha = v->uses_alpha; r.nsmp = v->nsmp; memcpy(r.smp, v->smp, sizeof r.smp);
        CreateDirectoryA("../cache/vk", NULL); CreateDirectoryA("../cache/vk/stage", NULL);
        stage_rec_path(path, sizeof path, s, v->h);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
    }
    return v->ok ? v : NULL;
}

/* ---------------------------------------------------------------- pipelines (made at the draw that first needs them: session 2; workers are M4) */
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; } PKey;
typedef struct Pipe { PKey k; VkPipeline p; VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs; struct Pipe *next; } Pipe;
static Pipe *g_pipes[1024];
static void pkey_targets(PKey *k);
static VkBlendFactor blend_factor(unsigned f) {
    static const uint8_t m[9] = {VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                 VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (f >= 3 && f <= 0xB) return (VkBlendFactor)m[f - 3];
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
                 case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                 case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned e) { static const uint8_t m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX}; return (VkBlendOp)m[e < 6 ? e : 1]; }
static void layout_bindings(VStage *v, int s, VkDescriptorSetLayoutBinding *b0, int *n0, VkDescriptorSetLayoutBinding *b1, int *n1) {
    VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    for (unsigned n = 1; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[(*n1)++] = b; }
}
/* Makes the pipeline of a key (any thread: the device calls are thread-safe, the pipeline cache is synchronised by the driver). */
static Pipe *pipe_make(const PKey *k, VStage *vs, VStage *fs, double *took) {
    Pipe *p = calloc(1, sizeof *p);
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    p->k = *k; p->vs = vs; p->fs = fs;
    VkDescriptorSetLayoutBinding b0[80], b1[140];
    layout_bindings(vs, 0, b0, &p->nb[0], b1, &p->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &p->nb[0], b1, &p->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)p->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && p->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; p->push0 = 1; }      /* 32: the smallest maxPushDescriptors a device may have */
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &p->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = p->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &p->layout));

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs->mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs ? fs->mod : NULL; st[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k->poly == 0 ? VK_POLYGON_MODE_POINT : k->poly == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.depthClampEnable = k->dclamp; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkSampleMask smask = k->sample_mask;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1); ms.alphaToCoverageEnable = k->a2c; ms.pSampleMask = &smask;
    if (k->sshade == 1 && D.f2.features.sampleRateShading) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[8];
    memset(ba, 0, sizeof ba);
    for (int i = 0; i < k->ncolor; i++) {
        const uint8_t *b = k->blend[i];
        ba[i].blendEnable = k->blend_on >> i & 1; ba[i].colorWriteMask = (k->chanmask >> (i * 4)) & 15;
        ba[i].srcColorBlendFactor = blend_factor(b[0]); ba[i].dstColorBlendFactor = blend_factor(b[1]); ba[i].srcAlphaBlendFactor = blend_factor(b[2]); ba[i].dstAlphaBlendFactor = blend_factor(b[3]);
        ba[i].colorBlendOp = blend_op(b[4]); ba[i].alphaBlendOp = blend_op(b[5]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor; cb.pAttachments = ba;
    if (k->logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic & 15); }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof dyn / sizeof *dyn; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat cf[8];
    for (int i = 0; i < 8; i++) cf[i] = (VkFormat)k->cfmt[i];
    ri.colorAttachmentCount = k->ncolor; ri.pColorAttachmentFormats = cf;
    if (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    if (k->dfmt && (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    *took = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    InterlockedIncrement64(&R.pipelines);
    return p;
}
/* ..\cache\vk\pipes.bin: every pipeline key this PC ever needed, with its stages named by their hashes (design section 5:
 * "pipes.txt"). Read at start by the workers; a key missing at a draw is made there and appended (once per key per PC). */
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC (0x31455049u + MX_VK_VERSION)
static void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute) {
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC; r.compute = (uint32_t)compute; r.vs_h = vs->h; r.fs_h = fs ? fs->h : 0;
    if (k) { r.k = *k; r.k.vs = r.k.fs = 0; }
    CreateDirectoryA("../cache/vk", NULL);
    FILE *fp = fopen("../cache/vk/pipes.bin", "ab");
    if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
}
static Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, k, sizeof *k);
    for (Pipe *p = g_pipes[h & 1023]; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p;
    double ms = 0;
    LONG64 t0 = tk();
    Pipe *p = pipe_make(k, vs, fs, &ms);           /* never met on this PC: made here, the frame is late once (counted) */
    g_tm[5] += tk() - t0;
    g_pipe_miss++;
    if (D.debug) printf("nvn_vk: pipeline miss %lld: stages %u/%u, class %d, %d colour (format %d), depth %d, samples %d: %.1f ms\n", (long long)g_pipe_miss, k->vs, k->fs, k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
    piperec_append(k, vs, fs, 0);
    return p;
}

#endif
/* ---------------------------------------------------------------- passes */
static int target_level(const TargetRef *r) { return (r->view.set & 0x81) == 0x81 ? r->view.lvl0 : 0; }
static VkImageView target_view(const TargetRef *r, VTex *vt) {
    EView e;
    eview_of(r->tex, vt, &r->view, &e);
    memset(e.swz, 0, 4); e.nlvl = 1; e.nlay = 1; e.format = r->tex->b.format; e.target = 1; e.dsmode = 0;
    if ((r->view.set & 0x84) == 0x84 && r->view.format) e.format = r->view.format;
    return vt_view(vt, &e, -1, 1);
}
static int pass_begin(void) {
    VkRenderingAttachmentInfo ca[8], da;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    int w = 0, h = 0;
    if (R.in_pass) return 1;
    cmd_begin();
    R.pass_rs = 0;
    memset(ca, 0, sizeof ca); memset(&da, 0, sizeof da);
    for (int i = 0; i < R.ncol; i++) {
        Tex *t = R.col[i].tex;
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!t || t->dead) continue;
        VTex *vt = vt_get(t);
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("a depth format bound as colour target"); continue; }
        t->gpu_written = 1;
        vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca[i].imageView = target_view(&R.col[i], vt); ca[i].imageLayout = vt->layout;
        if (!w) { w = lvl_(vt->w, target_level(&R.col[i])); h = lvl_(vt->h, target_level(&R.col[i])); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
    }
    if (R.depth.tex && !R.depth.tex->dead) {
        VTex *vt = vt_get(R.depth.tex);
        R.depth.tex->gpu_written = 1;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) nvn_vk_missing("a colour format bound as depth target");
        else if (RS_ON && rs_ui_native() && vt->rs && !R.pass_rs && w && (lvl_(vt->w, target_level(&R.depth)) < w || lvl_(vt->h, target_level(&R.depth)) < h)) g_rs_mixed++;      /* session 9 (SSBU_VK_UI_NATIVE): an unscaled colour target with a scaled depth target: the pass runs WITHOUT depth (counted, reported at exit) */
        else {
            vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.imageView = target_view(&R.depth, vt); da.imageLayout = vt->layout;
            if (vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
            if (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
            if (!w) { w = lvl_(vt->w, target_level(&R.depth)); h = lvl_(vt->h, target_level(&R.depth)); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
        }
    }
    if (!w) return 0;
    {   /* session 9, SSBU_VK_UILOG=<n>: every pass begun in the n-th battle frame ("nvn_vk_ui:" lines): its targets, whether the
         * colour target is a texture of the window, and the draws of the frame so far (the next line's number minus this one = its draws) */
        static int uil = -2, np;
        if (uil == -2) uil = getenv("SSBU_VK_UILOG") ? atoi(getenv("SSBU_VK_UILOG")) : -1;
        if (uil > 0 && g_battle_frames == uil) {
            Tex *c0 = R.ncol ? R.col[0].tex : NULL, *dt = R.depth.tex && !R.depth.tex->dead ? R.depth.tex : NULL;
            printf("nvn_vk_ui: pass %d: %d colour, first %dx%d format 0x%x level %d%s%s | depth %dx%d format 0x%x%s | image size of the pass %dx%d | %lld draws so far, %lld dispatches so far\n", np++, R.ncol,
                   c0 ? c0->b.width : 0, c0 ? c0->b.height : 0, c0 ? (int)c0->b.format : 0, c0 ? target_level(&R.col[0]) : 0, c0 && wintex_is(c0) ? " WINDOW TEXTURE" : "", c0 && c0->vk && ((VTex *)c0->vk)->rs ? " (scaled)" : "",
                   dt ? dt->b.width : 0, dt ? dt->b.height : 0, dt ? (int)dt->b.format : 0, dt && dt->vk && ((VTex *)dt->vk)->rs ? " (scaled)" : "", w, h, (long long)R.frame_draws, (long long)R.dispatches);
        }
    }
    {   /* SSBU_VK_RTLOG=1 (track O, resolution scale survey): every distinct render-target size a pass is begun with, with
         * the colour / depth formats and how many passes used it (printed at exit: "nvn_vk_rt:" lines) */
        static int on = -1;
        if (on < 0) on = getenv("SSBU_VK_RTLOG") && atoi(getenv("SSBU_VK_RTLOG"));
        if (on) {
            int cf = R.ncol && R.col[0].tex ? (int)R.col[0].tex->b.format : 0, df = R.depth.tex ? (int)R.depth.tex->b.format : 0, lv = R.ncol && R.col[0].tex ? target_level(&R.col[0]) : R.depth.tex ? target_level(&R.depth) : 0, i;
            for (i = 0; i < g_rtlog_n; i++) if (g_rtlog[i].w == w && g_rtlog[i].h == h && g_rtlog[i].ncol == R.ncol && g_rtlog[i].cf == cf && g_rtlog[i].df == df && g_rtlog[i].lv == lv) break;
            if (i == g_rtlog_n && g_rtlog_n < 256) { g_rtlog[i].w = w; g_rtlog[i].h = h; g_rtlog[i].ncol = R.ncol; g_rtlog[i].cf = cf; g_rtlog[i].df = df; g_rtlog[i].lv = lv; g_rtlog[i].first = nvn_front_frame_next(); g_rtlog_n++; }
            if (i < 256) { g_rtlog[i].n++; if (g_in_fight) g_rtlog[i].fight++; }
        }
    }
    cmd_begin();                                   /* vt_get may have submitted (ring wrap) */
    ri.renderArea.extent.width = (uint32_t)w; ri.renderArea.extent.height = (uint32_t)h; ri.layerCount = 1; ri.colorAttachmentCount = (uint32_t)R.ncol; ri.pColorAttachments = ca;
    vkCmdBeginRendering(R.cmd, &ri);
    R.in_pass = 1; R.pass_w = w; R.pass_h = h;
    return 1;
}

/* ---------------------------------------------------------------- draws */
static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    Slot *s = &R.slot[R.cur];
    VkDescriptorSet set;
    for (;;) {
        if (s->ipool < s->npool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s->pool[s->ipool]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
            VkResult r = vkAllocateDescriptorSets(V.device, &ai, &set);
            if (r == VK_SUCCESS) { R.sets++; return set; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) vk_fatal("vkAllocateDescriptorSets", r);
            s->ipool++;
            continue;
        }
        if (s->npool == 16) vkd_fail("more than 16 descriptor pools in one command buffer");
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 8192; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        VKC(vkCreateDescriptorPool(V.device, &pi, NULL, &s->pool[s->npool++]));
    }
}
static struct { uint64_t key; VkDescriptorSet set; uint32_t gen; VkDescriptorSetLayout dl; int n, off; } g_tset[4096];      /* texture sets of the command buffer being recorded */
static struct { VTex *d2, *d1, *d3; VkImageView v[7]; } g_dummy;
static struct { VkImageView view; VkSampler smp; VkImageLayout layout; } g_dtex[140], g_tset_list[32768];      /* g_tset_list: the lists of the cached sets, for the full comparison */
static int g_ndtex;
/* The image view and sampler of one declared sampler of stage s. May end the pass (upload, layout change). */
static void resolve_sampler(int s, VStage *v, int i) {
    unsigned u = v->smp[i].unit;
    Tex *t = NULL;
    View view;
    SamplerParams sp;
    int r = u < 32 ? nvn_front_handle(R.tpool, R.spool, R.nvn_tex[s][u], &t, &view, &sp) : 0;
    g_dtex[g_ndtex].view = g_dummy.v[v->smp[i].dim]; g_dtex[g_ndtex].smp = g_smp_default; g_dtex[g_ndtex].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (r & 1) {
        VTex *vt = vt_get(t);
        EView e;
        int attached = R.depth.tex == t, dim = v->smp[i].dim;
        for (int k = 0; k < R.ncol; k++) attached |= R.col[k].tex == t;
        eview_of(t, vt, &view, &e);
        int have = target_dim(e.target);
        int ok = have == dim || (dim == DIM_2D && (have == DIM_2DA || have == DIM_CUBE || have == DIM_CUBEA)) || (dim == DIM_2DA && (have == DIM_2D || have == DIM_CUBE || have == DIM_CUBEA)) ||
                 (dim == DIM_CUBE && have == DIM_CUBEA) || (dim == DIM_CUBEA && have == DIM_CUBE) || (dim == DIM_1D && have == DIM_1DA) || (dim == DIM_1DA && have == DIM_1D) ||
                 ((dim == DIM_CUBE || dim == DIM_CUBEA) && (have == DIM_2DA) && e.nlay >= 6 && vt->w == vt->h);
        if (ok && vt->samples == 1) {
            if (attached && have_targets_live()) { if (!vt->feedback) { vt->feedback = 1; R.feedback++; pass_end(); } vt_layout(vt, VK_IMAGE_LAYOUT_GENERAL); }
            else vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_dtex[g_ndtex].view = vt_view(vt, &e, dim, 0); g_dtex[g_ndtex].layout = vt->layout;
        } else { static int n; R.dummy_kind++;
            if (n < 3) printf("nvn_vk: (detail of the next line) frame %lld, program #%d, texture format 0x%x, %d x %d x %d, %d levels, draws so far with a dummy for a view-kind mismatch: %lld\n", (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, t->b.format, t->b.width, t->b.height, t->b.depth, nvn_front_tex_levels(t), (long long)R.dummy_kind);
            if (n++ < 20) printf("nvn_vk: texture unit %u of the %s stage: the shader declares view kind %d, the texture is kind %d (target %d, %d samples): a dummy is bound\n", u, s ? "fragment" : "vertex", dim, have, e.target, vt->samples); }
        if (r & 2) g_dtex[g_ndtex].smp = sampler_get(&sp);
    }
    g_ndtex++;
}
static int have_targets_live(void);
/* Track O: a rectangle of the game's pixels (scissor, clear) on the scaled images of the pass: every image pixel the
 * rectangle touches (floor of the first edge, ceiling of the last). */
static void rs_rect(int *x, int *y, int *w, int *h) {
    int x0 = (int)floorf((float)*x * R.pass_sx), y0 = (int)floorf((float)*y * R.pass_sy);
    int x1 = (int)ceilf((float)(*x + *w) * R.pass_sx - 0.001f), y1 = (int)ceilf((float)(*y + *h) * R.pass_sy - 0.001f);
    *x = x0; *y = y0; *w = x1 > x0 ? x1 - x0 : 0; *h = y1 > y0 ? y1 - y0 : 0;
}
static void set_dynamic_state(void) {
    VkViewport vp;
    VkRect2D sc;
    uint64_t d = R.ds;
    float x = R.have_viewport ? R.viewport[0] : 0, y = R.have_viewport ? R.viewport[1] : 0, w = R.have_viewport ? R.viewport[2] : (float)R.pass_w, h = R.have_viewport ? R.viewport[3] : (float)R.pass_h;
    if (R.pass_rs && R.have_viewport) { x *= R.pass_sx; w *= R.pass_sx; y *= R.pass_sy; h *= R.pass_sy; }      /* track O: the game's rectangle on the scaled image */
    vp.x = x; vp.y = y + h; vp.width = w; vp.height = -h; vp.minDepth = R.drange[0]; vp.maxDepth = R.drange[1];      /* NVN upper-left origin: clip y = +1 is row y (design, section 3) */
    if (vp.minDepth < 0) vp.minDepth = 0; if (vp.maxDepth > 1) vp.maxDepth = 1;
    vkCmdSetViewport(R.cmd, 0, 1, &vp);
    int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
    if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
    if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
    if (sx > R.pass_w) sx = R.pass_w; if (sy > R.pass_h) sy = R.pass_h;
    if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
    sc.offset.x = sx; sc.offset.y = sy; sc.extent.width = (uint32_t)(sw > 0 ? sw : 0); sc.extent.height = (uint32_t)(sh > 0 ? sh : 0);
    vkCmdSetScissor(R.cmd, 0, 1, &sc);
    if (R.pass_rs && D.f2.features.wideLines) { float lw = R.line_width * R.pass_sx; vkCmdSetLineWidth(R.cmd, lw < V.props.limits.lineWidthRange[0] ? V.props.limits.lineWidthRange[0] : lw); } else
    vkCmdSetLineWidth(R.cmd, D.f2.features.wideLines ? R.line_width : 1.0f);
    vkCmdSetDepthBias(R.cmd, R.polyoff[1], D.f2.features.depthBiasClamp ? R.polyoff[2] : 0.0f, R.polyoff[0]);
    vkCmdSetBlendConstants(R.cmd, R.blendc);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.svmask[0]); vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.svmask[1]);
    vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.swmask[0]); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.swmask[1]);
    vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.sref[0]); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.sref[1]);
    vkCmdSetCullMode(R.cmd, R.poly[0] & 3);
    vkCmdSetFrontFace(R.cmd, R.poly[1] ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(R.cmd, d & 1); vkCmdSetDepthWriteEnable(R.cmd, (d >> 1) & 1);
    { unsigned f = (unsigned)(d >> 4) & 15; vkCmdSetDepthCompareOp(R.cmd, (VkCompareOp)(f >= 1 && f <= 8 ? f - 1 : 7)); }
    vkCmdSetStencilTestEnable(R.cmd, (d >> 2) & 1);
    {   static const uint8_t op[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                                      VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
#define SOP(sh) ((VkStencilOp)op[((d >> (sh)) & 15) < 9 ? ((d >> (sh)) & 15) : 1])
#define SFN(sh) ((VkCompareOp)((((d >> (sh)) & 15) >= 1 && ((d >> (sh)) & 15) <= 8) ? ((d >> (sh)) & 15) - 1 : 7))
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_BIT, SOP(16), SOP(24), SOP(20), SFN(8));       /* (fail, pass, depth fail, compare); NVN order: stencil fail, depth fail, depth pass */
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_BACK_BIT, SOP(28), SOP(36), SOP(32), SFN(12));
#undef SOP
#undef SFN
    }
    vkCmdSetDepthBiasEnable(R.cmd, (R.poly[3] >> (R.poly[2] < 3 ? R.poly[2] : 2)) & 1);          /* offset enable bits: point, line, fill; the polygon mode picks one */
    vkCmdSetRasterizerDiscardEnable(R.cmd, R.rast_discard);
}
static int have_targets_live(void) { return R.in_pass || R.have_targets; }
/* Session 4: the state that left the pipeline key (VK_EXT_extended_dynamic_state3). Set after a pipeline is bound, whenever
 * one of the values changed, the pass changed, or a pipeline of our own (static state) was bound in between. */
static void set_dynamic_state3(int samples) {
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
    for (int i = 0; i < 8; i++) {
        const uint8_t *b = R.blend[i];
        en[i] = R.blend_on >> i & 1; wm[i] = (R.chanmask >> (i * 4)) & 15;
        eq[i].srcColorBlendFactor = blend_factor(b[0]); eq[i].dstColorBlendFactor = blend_factor(b[1]); eq[i].colorBlendOp = blend_op(b[4]);
        eq[i].srcAlphaBlendFactor = blend_factor(b[2]); eq[i].dstAlphaBlendFactor = blend_factor(b[3]); eq[i].alphaBlendOp = blend_op(b[5]);
    }
    vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en);
    vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq);
    vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
    vkCmdSetPolygonModeEXT(R.cmd, R.poly[2] == 0 ? VK_POLYGON_MODE_POINT : R.poly[2] == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetAlphaToCoverageEnableEXT(R.cmd, R.ms[1] != 0);
    { VkSampleMask m = R.sample_mask; vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(samples ? samples : 1), &m); }
    if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, R.depth_clamp != 0);
    R.dyn3_samples = samples;
}

/* ---------------------------------------------------------------- compute (M3; design section 3, "DispatchCompute" and "Storage buffers written by compute")
 * A storage buffer a compute stage may write gets a Vulkan buffer of its own (CB: host-visible memory, filled from guest
 * memory at its first dispatch of the batch). Later commands of the SAME batch whose range lies inside it bind that buffer
 * (the fighter's vertex buffers: no round trip). At the end of the batch the command buffer is submitted and the CBs become
 * "pending"; when that submission has finished the ranges a dispatch REALLY wrote (mx_wm, one word per dispatch in the
 * stream ring) are copied back to guest memory (cb_land): at the latest before the next batch reads guest memory, earlier
 * when the render thread is idle or a sync is waited for. The OpenGL path copies back before the next draw; what guest
 * memory holds once the batch is done is the same. Anything that reads guest memory through a range that only partly lies
 * in a CB forces the copy-back at once (cb_sync: submit, wait; counted). */
typedef struct { uint64_t addr, size, cap, busy; VkBuffer buf; VMem mem; int state, dirty; uint8_t *snap; uint64_t snap_cap; } CB;     /* state: 0 free, 1 live in this batch, 2 pending (batch submitted); snap: guest memory as it was when the CB was filled */
static int g_cb_merge = -1;                        /* SSBU_VK_CBMERGE=0: the whole range is copied back as before session 5 */
typedef struct { uint64_t wm_off; uint32_t written; int16_t cb[16]; } CDisp;
static CB g_cb[256];
static CDisp g_cdisp[2048];
static int g_ncdisp, g_cb_live, g_cb_pending, g_cb_cached = -1;
static uint64_t g_cb_serial;                       /* the submission the pending CBs wait for */
static void cb_masks(void) {                       /* every recorded dispatch has run: which buffers did each one write? */
    for (int i = 0; i < g_ncdisp; i++) {
        uint32_t wm;
        memcpy(&wm, (uint8_t *)R.ring_mem.map + g_cdisp[i].wm_off, 4);
        wm &= g_cdisp[i].written;
        for (int k = 0; k < 16; k++) if ((wm >> k & 1) && g_cdisp[i].cb[k] >= 0) g_cb[g_cdisp[i].cb[k]].dirty = 1;
    }
    g_ncdisp = 0;
}
static uint32_t g_cb_epoch;
/* midbatch: the command being prepared may still name a freed CB's buffer (it is recorded into the next submission), so
 * that buffer is not given out again before that submission has finished. */
static void cb_land(int midbatch) {                /* the GPU has finished everything that wrote a CB */
    LONG64 t0 = tk();
    cb_masks();
    for (int i = 0; i < 256; i++) {
        CB *c = &g_cb[i];
        if (!c->state) continue;
        if (c->dirty && !c->snap) { memcpy(P(c->addr), c->mem.map, (size_t)c->size); R.cb_bytes += (LONG64)c->size; }
        else if (c->dirty) {
            /* Session 5: the copy-back is DEFERRED (up to the next batch), so the guest may have written into the range since
             * the CB was filled. Only what compute changed goes back: a word the guest changed meanwhile (guest != snapshot)
             * is kept and counted; when compute changed that word too it is a real conflict (counted apart; the guest's later
             * write stays, as on the OpenGL path where the copy-back comes before it). */
            uint64_t *g = P(c->addr);
            const uint64_t *s = (const uint64_t *)c->snap, *m = (const uint64_t *)c->mem.map;
            size_t nw = (size_t)(c->size / 8), tail = (size_t)(c->size & 7);
            LONG64 gw = 0, cf = 0;
            for (size_t k = 0; k < nw; k++) {
                uint64_t sv = s[k], mv = m[k];
                if (mv == sv) { if (g[k] != sv) gw++; continue; }
                if (g[k] == sv) g[k] = mv; else { gw++; cf++; }
            }
            if (tail) memcpy((uint8_t *)g + nw * 8, (const uint8_t *)m + nw * 8, tail);
            R.cb_bytes += (LONG64)c->size; R.cb_guest_words += gw; R.cb_conflict_words += cf;
            if (gw) { static int n; if (n++ < 10) printf("nvn_vk: copy-back of %llx+%llu: the guest wrote %lld words of the range while it was pending (%lld of them also written by compute); merged\n", (unsigned long long)c->addr, (unsigned long long)c->size, (long long)gw, (long long)cf); }
        }
        c->state = c->dirty = 0;
        c->busy = midbatch ? R.serial + 1 : 0;
    }
    g_cb_live = g_cb_pending = 0;
    nvn_front_ccmp_landed();                       /* SSBU_COMPUTE_CMP: what the dispatches wrote is in guest memory now */
    g_cb_epoch++;
    g_tm[1] += tk() - t0;
    R.gen++;                                       /* guest memory changed: ranges are streamed again */
}
static void cb_ring_wrap(void) { if (g_cb_pending) cb_land(0); else cb_masks(); }      /* after cmd_wait_all: the mask words in the ring are about to be reused */
static void cb_poll(void) {                        /* pending CBs whose submission has finished are copied back; never waits */
    if (!g_cb_pending) return;
    if (R.done < g_cb_serial) for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && vkGetFenceStatus(V.device, R.slot[i].fence) == VK_SUCCESS) slot_wait(&R.slot[i]);
    if (R.done >= g_cb_serial) cb_land(0);
}
static void cb_sync(void) {                        /* guest memory must hold what compute wrote, now */
    int mid = g_cb_live != 0;
    if (!g_cb_live && !g_cb_pending) return;
    if (g_cb_live) { g_cb_serial = cmd_submit(NULL, NULL); R.cb_syncs++; }
    if (R.done < g_cb_serial) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && R.slot[i].serial <= g_cb_serial) slot_wait(&R.slot[i]);
        if (R.done < g_cb_serial) R.done = g_cb_serial;
        QueryPerformanceCounter(&t1);
        R.cb_waits++; R.cb_wait_ticks += t1.QuadPart - t0.QuadPart;
    }
    cb_land(mid);
}
static void cb_batch_end(void) {
    if (!g_cb_live) return;
    g_cb_serial = cmd_submit(NULL, NULL);
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 1) g_cb[i].state = 2;
    g_cb_pending = g_cb_live; g_cb_live = 0;
}
static CB *cb_inside(uint64_t addr, uint64_t size) {
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 1 && addr >= g_cb[i].addr && addr + size <= g_cb[i].addr + g_cb[i].size) return &g_cb[i];
    return NULL;
}
static int cb_overlap(uint64_t addr, uint64_t size) {
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 1 && addr < g_cb[i].addr + g_cb[i].size && g_cb[i].addr < addr + size) return 1;
    return 0;
}
static CB *cb_new(uint64_t addr, uint64_t size) {
    CB *c = NULL, *empty = NULL, *victim = NULL;
    for (int i = 0; i < 256 && !c; i++) {
        CB *k = &g_cb[i];
        if (k->state || k->busy > R.done) continue;
        if (!k->buf) { if (!empty) empty = k; }
        else if (k->cap >= size && k->cap <= size * 2 + 4096) c = k;
        else if (!victim) victim = k;
    }
    if (!c) {
        c = empty ? empty : victim;
        if (!c) vkd_fail("more than 256 storage buffers written by compute in one batch");
        if (c->buf) { vkDestroyBuffer(V.device, c->buf, NULL); vmem_free(c->mem); c->buf = NULL; }      /* free and not busy: nothing in flight names it */
        if (g_cb_cached < 0) {                     /* read back by the CPU: cached host memory when the device has it */
            g_cb_cached = 0;
            for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((D.mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) g_cb_cached = 1;
        }
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryRequirements rq;
        c->cap = (size + 4095) & ~(uint64_t)4095;
        bi.size = c->cap; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateBuffer(V.device, &bi, NULL, &c->buf));
        vkGetBufferMemoryRequirements(V.device, c->buf, &rq);
        VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (g_cb_cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
        int found = 0;
        for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) found = 1;
        if (!found) want &= ~(VkFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        c->mem = vmem_alloc(rq, want, 2);
        VKC(vkBindBufferMemory(V.device, c->buf, c->mem.mem, c->mem.off));
        R.cb_made++;
    }
    c->addr = addr; c->size = size; c->state = 1; c->dirty = 0;
    if (g_cb_merge < 0) g_cb_merge = !(getenv("SSBU_VK_CBMERGE") && !atoi(getenv("SSBU_VK_CBMERGE")));
    if (g_cb_merge) {                              /* the snapshot the copy-back is compared with (cb_land) */
        if (c->snap_cap < size) { free(c->snap); c->snap_cap = (size + 4095) & ~(uint64_t)4095; c->snap = malloc((size_t)c->snap_cap); }
        memcpy(c->snap, P(addr), (size_t)size);
        memcpy(c->mem.map, c->snap, (size_t)size);
    } else
    memcpy(c->mem.map, P(addr), (size_t)size);
    g_cb_live++;
    return c;
}
typedef struct { uint64_t off, size; int zero; VkBuffer buf; } BufRef;
/* Where a command reads the guest range from: a CB of this batch that contains it, else the stream ring. align: the offset
 * alignment the use needs (storage descriptors), 1 = none. Returns 1 when a CB is bound. */
static int buf_ref(uint64_t addr, uint64_t size, uint64_t align, BufRef *b) {
    b->zero = 0; b->size = size;
    if (g_cb_live) {
        CB *c = cb_inside(addr, size);
        if (c && (addr - c->addr) % align == 0) { b->buf = c->buf; b->off = addr - c->addr; return 1; }
        if (cb_overlap(addr, size)) cb_sync();
    }
    b->off = ring_put(addr, size); b->buf = R.ring;      /* in this order: a ring wrap submits */
    return 0;
}
static void compute_pipe_make(VStage *v);
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz);
static void compute_dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    Prog *p = R.prog[5];
    static int nlog;
    R.dispatches++;
    if (!p) { if (nlog++ < 10) printf("nvn_vk: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    if (!st->code) { nvn_vk_missing("nvnCommandBufferDispatchCompute of a program that has no machine code (assembly text only)"); return; }
    uint32_t key[4] = {0, 0, R.tex_int[5], R.tex_sint[5]};
    VStage *v = vstage_get(p, 5, key, 0);
    if (!v) { nvn_vk_missing("nvnCommandBufferDispatchCompute: the compute stage could not be translated or compiled (the log names the program)"); return; }
    if (v->images) { nvn_vk_missing("a compute program that uses images (nvnCommandBufferBindImage)"); return; }
    if (v->nsmp > 32) { nvn_vk_missing("a compute program with more than 32 samplers"); return; }
    if (!gx || !gy || !gz) return;
    int cp_how = 0;
    if (v->cp_state != 2) { LONG64 t0 = tk(); cp_how = compute_pipe_ensure(v); if (cp_how) { hold_note(cp_how == 1 ? H_PIPE_DRAW : H_PIPE_WAIT, tk() - t0); g_tm[5] += tk() - t0; } }
    if (!v->cp_recorded) { v->cp_recorded = 1; piperec_append(NULL, v, NULL, 1); }
    if (cp_how == 1) {
        g_pipe_miss++;
        printf("nvn_vk: compute program #%d: pipeline made at its first dispatch (%d bytes of machine code, work group %u x %u x %u, constant buffers 0x%x, storage 0x%x written 0x%x, %d samplers)\n", p->serial, st->code_len,
               st->local[0], st->local[1], st->local[2], v->cbufs, v->ssbos, v->ssbo_w, v->nsmp);
    }
    compute_dispatch_run(p, st, v, gx, gy, gz);
}
static void compute_pipe_make(VStage *v) {         /* any thread */
    {
        VkDescriptorSetLayoutBinding b0[40], b1[64];
        int n0 = 0, n1 = 0;
        for (unsigned n = 0; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        if (v->ssbo_w) { VkDescriptorSetLayoutBinding b = {38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b1[n1++] = b; }
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
            VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &v->cdl[i]));
        }
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2; pl.pSetLayouts = v->cdl;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &v->cl));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = v->mod; ci.stage.pName = "main"; ci.layout = v->cl;
        VkPipeline cp = NULL;
        VkResult r = vkCreateComputePipelines(V.device, g_pcache, 1, &ci, NULL, &cp);
        if (r != VK_SUCCESS) vkd_fail("vkCreateComputePipelines (compute stage %u): %s", v->id, vk_result_name(r));
        v->cp = cp;
        InterlockedIncrement64(&R.pipelines);
    }
}
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz) {
    static BufRef bufs[40];
    int nbuf = 0;
    int16_t wcb[16];
    uint64_t al = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    pass_end();
    /* textures, then buffers (both may submit: upload, ring wrap, copy-back), then the mask word, then the commands */
    g_ndtex = 0;
    for (int i = 0; i < v->nsmp; i++) resolve_sampler(5, v, i);
    if (g_ncdisp == 2048 || g_cb_pending) cb_sync();
    uint64_t wm_off = 0;
    for (int attempt = 0; ; attempt++) {           /* a copy-back in here (cb_sync) frees the CBs chosen before it: everything is then chosen again */
        uint32_t epoch = g_cb_epoch;
        nbuf = 0;
        memset(wcb, 0xff, sizeof wcb);
        for (unsigned n = 0; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = ring_raw(R.c2[5], sizeof R.c2[5]); b->size = sizeof R.c2[5]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[5][n - 3].addr && R.ubo[5][n - 3].size) buf_ref(R.ubo[5][n - 3].addr, R.ubo[5][n - 3].size, al, b);
            else b->zero = 1;                      /* c0 outside the storage descriptors, or nothing bound: zeros (OpenGL: nothing bound) */
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            Range r = R.ssbo[5][k];
            b->zero = !(r.addr && r.size); b->buf = R.ring;
            if (b->zero) continue;
            if (!(v->ssbo_w >> k & 1)) { buf_ref(r.addr, r.size, al, b); continue; }
            CB *c = g_cb_live ? cb_inside(r.addr, r.size) : NULL;
            if (c && (r.addr - c->addr) % al) c = NULL;
            if (!c) { if (g_cb_live && cb_overlap(r.addr, r.size)) cb_sync(); c = cb_new(r.addr, r.size); }
            b->buf = c->buf; b->off = r.addr - c->addr; b->size = r.size; wcb[k] = (int16_t)(c - g_cb);
        }
        if (v->ssbo_w) { static const uint32_t zero[4]; wm_off = ring_raw(zero, 16); }
        if (epoch == g_cb_epoch) break;
        if (attempt == 3) vkd_fail("compute program #%d: its buffers cannot be bound without a copy-back in between", p->serial);
    }
    nvn_front_ccmp_dispatch(R.ubo[5], R.ssbo[5], v->ssbo_w, st->code, st->code_len, gx, gy, gz);      /* SSBU_COMPUTE_CMP */
    cmd_begin();
    static VkWriteDescriptorSet w[80];
    static VkDescriptorBufferInfo bi[40];
    static VkDescriptorImageInfo ii[64];
    VkDescriptorSet sets[2];
    int nw = 0, ib = 0;
    sets[0] = set_alloc(v->cdl[0]); sets[1] = set_alloc(v->cdl[1]);
    memset(w, 0, sizeof w);
    for (unsigned n = 0; n < 34; n++) {
        unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
        if (!bit) continue;
        BufRef *b = &bufs[ib];
        bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = n < 18 ? n : 20 + n - 18; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    if (v->ssbo_w) {
        bi[ib].buffer = R.ring; bi[ib].offset = wm_off; bi[ib].range = 16;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 38; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    for (int i = 0; i < v->nsmp; i++) {
        ii[i].sampler = g_dtex[i].smp; ii[i].imageView = g_dtex[i].view; ii[i].imageLayout = g_dtex[i].layout;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[1]; w[nw].dstBinding = v->smp[i].binding; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i]; nw++;
    }
    if (nw) vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cp);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cl, 0, 2, sets, 0, NULL);
    vkCmdDispatch(R.cmd, gx, gy, gz);
    if (v->ssbo_w) {                               /* what the dispatch wrote is read by later dispatches and draws of this command buffer */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CDisp *d = &g_cdisp[g_ncdisp++];
        d->wm_off = wm_off; d->written = v->ssbo_w; memcpy(d->cb, wcb, sizeof wcb);
    }
}
/* Everything of a draw up to the draw call. topo: NVN primitive. Returns 0 when the draw cannot be done (counted as skipped). */
static int draw_prepare(unsigned prim) {
    static const int8_t topo[7] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, -1, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    static const uint8_t tclass[7] = {0, 1, 1, 1, 2, 2, 2};
    if (!R.have_targets || R.render_off) return 0;
    if (prim > 6 || topo[prim] < 0) { static char msg[64]; snprintf(msg, sizeof msg, "a draw with NVN primitive type %u", prim); nvn_vk_missing(msg); return 0; }
    if (!R.prog[0]) return 0;
    if (R.prog[2] || R.prog[3] || R.prog[4]) { nvn_vk_missing("a draw with a geometry or tessellation stage"); return 0; }
    uint32_t ints = 0, sints = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) {
        int nf = (int)(R.vattr[i] & 0xff), k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) { ints |= 1u << i; if (k == 3 || k == 8 || k == 11) sints |= 1u << i; }
        if (nf == 0x3E) ints |= 1u << i;
        if (nf == 0x6A) { ints |= 1u << i; sints |= 1u << i; }
    }
    uint32_t key0[4] = {ints, sints, R.tex_int[0], R.tex_sint[0]}, key1[4] = {0, 0, R.tex_int[1], R.tex_sint[1]};
    int depth_only = !R.prog[1];
    /* fragment inputs the vertex code never writes are declared by a vertex variant of their own, left at (0, 0, 0, 1). Session 4:
     * the variant the program used last is asked for first (session 3 asked for the plain one at every draw of such a pair,
     * which hashed the machine code each time). */
    VStage *cur = R.prog[0]->st[0].vk;
    VStage *fs = depth_only ? NULL : vstage_get(R.prog[1], 1, key1, 0), *vs = vstage_get(R.prog[0], 0, key0, cur ? cur->fo : 0);
    if (vs) { uint32_t need = fs ? fs->in_attr & ~(vs->out_attr & ~vs->fo) : 0; if (need != vs->fo) vs = vstage_get(R.prog[0], 0, key0, need); }
    if (g_skip_flag) { g_skip_flag = 0; if (!vs || (!depth_only && !fs)) return 0; }      /* shader_wait = skip: the draw is left out (counted in vstage_get) */
    if (!vs || (!depth_only && !fs)) { nvn_vk_missing("a draw whose vertex or fragment stage could not be translated or compiled (the log names the stage)"); return 0; }

    /* 1. textures (may upload, change layouts, end the pass) */
    g_ndtex = 0;
    for (int i = 0; i < vs->nsmp; i++) resolve_sampler(0, vs, i);
    if (fs) for (int i = 0; i < fs->nsmp; i++) resolve_sampler(1, fs, i);
    /* 2. buffers: every range the draw reads, into the ring (may wrap the ring = submit) */
    static BufRef bufs[80];
    int nbuf = 0;
    uint64_t sal = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    for (int s = 0; s < (fs ? 2 : 1); s++) {
        VStage *v = s ? fs : vs;
        Stage *st = &R.prog[s]->st[s];
        for (unsigned n = 1; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = ring_raw(R.c2[s], sizeof R.c2[s]); b->size = sizeof R.c2[s]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code - 0x30 + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[s][n - 3].addr && R.ubo[s][n - 3].size) buf_ref(R.ubo[s][n - 3].addr, R.ubo[s][n - 3].size, sal, b);
            else b->zero = 1;
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = !(R.ssbo[s][k].addr && R.ssbo[s][k].size); b->buf = R.ring;
            if (!b->zero) buf_ref(R.ssbo[s][k].addr, R.ssbo[s][k].size, sal, b);
        }
    }
    /* vertex streams: only the ones an attribute names (a stale binding of another mesh is not read, and is not copied) */
    uint64_t voff[16];
    VkBuffer vbufs[16];
    uint32_t streams = 0;
    int from_cb = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) if (R.vattr[i] & 0xff) streams |= 1u << ((R.vattr[i] >> 8) & 31);
    for (int i = 0; i < 16; i++) {
        BufRef b;
        voff[i] = ~0ull; vbufs[i] = R.ring;
        if (!(streams >> i & 1) || !R.vbuf[i].addr || !R.vbuf[i].size) continue;
        from_cb |= buf_ref(R.vbuf[i].addr, R.vbuf[i].size, 1, &b);
        voff[i] = b.off; vbufs[i] = b.buf;
    }
    R.cb_draws += from_cb;
    /* 3. pass and pipeline */
    if (!pass_begin()) return 0;
    PKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.fs = fs ? fs->id : 0; k.topo_class = tclass[prim]; k.logic = R.logic; k.sshade = fs ? (uint8_t)R.prog[1]->sample_shading : 0;
    if (!D.has_eds3) {                             /* without extended dynamic state 3 this state is part of the key */
        k.chanmask = R.chanmask; k.sample_mask = R.sample_mask; k.poly = R.poly[2] < 3 ? R.poly[2] : 2;
        k.a2c = R.ms[1] != 0; k.dclamp = R.depth_clamp && D.f2.features.depthClamp; k.blend_on = R.blend_on;
        memcpy(k.blend, R.blend, sizeof k.blend);
    }
    pkey_targets(&k);
    Pipe *p = pipe_get(&k, vs, fs);
    if (!p) return 0;                              /* shader_wait = skip: the pipeline is being made by a worker, the draw is left out (counted in pipe_get) */
    if (!R.pipe_bound) R.dyn3_dirty = 1;          /* a new command buffer, or a pipeline of our own with static state was bound */
    if (R.pipe_bound != p) { vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p); R.pipe_bound = p; }
    if (R.dyn_dirty) { set_dynamic_state(); R.dyn_dirty = 0; }
    if (D.has_eds3 && (R.dyn3_dirty || R.dyn3_samples != k.samples)) { set_dynamic_state3(k.samples); R.dyn3_dirty = 0; }
    vkCmdSetPrimitiveTopology(R.cmd, (VkPrimitiveTopology)topo[prim]);
    vkCmdSetPrimitiveRestartEnable(R.cmd, R.restart && (D.has_plr || prim == 3 || prim == 5 || prim == 6));
    /* 4. vertex input */
    {
        VkVertexInputBindingDescription2EXT bd[17];
        VkVertexInputAttributeDescription2EXT ad[16];
        VkBuffer vb[17];
        VkDeviceSize vo[17];
        uint32_t nb = 0, na = 0, bmap[32], need_const = 0;
        memset(bmap, 0xff, sizeof bmap);
        for (int i = 0; i < 16; i++) {
            uint32_t a = i < R.nattr ? R.vattr[i] : 0;
            int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
            VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
            if (nf && !f) { static char msg[64]; snprintf(msg, sizeof msg, "vertex attribute format 0x%x", nf); nvn_vk_missing(msg); }
            memset(&ad[na], 0, sizeof ad[na]);
            ad[na].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT; ad[na].location = (uint32_t)i;
            if (f && stream < 16 && voff[stream] != ~0ull) {
                if (bmap[stream] == 0xffffffffu) {
                    bmap[stream] = nb;
                    memset(&bd[nb], 0, sizeof bd[nb]);
                    bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = R.vstream[stream].stride;
                    bd[nb].inputRate = R.vstream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; bd[nb].divisor = R.vstream[stream].divisor ? R.vstream[stream].divisor : 1;
                    vb[nb] = vbufs[stream]; vo[nb] = voff[stream]; nb++;
                }
                ad[na].binding = bmap[stream]; ad[na].format = f; ad[na].offset = a >> 14; na++;
            } else if (vs->dep >> i & 1 || 1) {       /* not supplied: (0, 0, 0, 1), as OpenGL's current attribute value; integer inputs take the integer copy */
                ad[na].binding = 16; ad[na].format = (ints >> i & 1) ? ((sints >> i & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT; ad[na].offset = (ints >> i & 1) ? 16 : 0;
                need_const = 1; na++;
            }
        }
        if (need_const) {
            memset(&bd[nb], 0, sizeof bd[nb]);
            bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = 0; bd[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; bd[nb].divisor = 1;
            for (uint32_t i = 0; i < na; i++) if (ad[i].binding == 16) ad[i].binding = nb;
            vb[nb] = R.zero; vo[nb] = 65536; nb++;
        }
        vkCmdSetVertexInputEXT(R.cmd, nb, bd, na, ad);
        if (nb) vkCmdBindVertexBuffers(R.cmd, 0, nb, vb, vo);
    }
    /* 5. descriptors */
    {
        static VkWriteDescriptorSet w[220];
        static VkDescriptorBufferInfo bi[80];
        static VkDescriptorImageInfo ii[140];
        /* M4: buffers change with every draw: pushed into the command buffer (no set, no pool) when the device can; the
         * textures of a draw repeat: one set per distinct list (layout, views, samplers, image layouts) per command buffer */
        VkDescriptorSet sets[2] = {NULL, NULL};
        int nw = 0, ib = 0, it = 0;
        if (!p->push0 && p->nb[0]) sets[0] = set_alloc(p->dl[0]);
        memset(w, 0, sizeof w[0] * (size_t)(p->nb[0] + p->nb[1]));
        for (int s = 0; s < (fs ? 2 : 1); s++) {
            VStage *v = s ? fs : vs;
            for (unsigned n = 1; n < 34; n++) {
                unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
                if (!bit) continue;
                BufRef *b = &bufs[ib];
                bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
                w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 40u * (unsigned)s + (n < 18 ? n : 20 + n - 18); w[nw].descriptorCount = 1;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
            }
        }
        if (p->push0) { if (nw) vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, (uint32_t)nw, w); }
        else if (sets[0]) { vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL); vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &sets[0], 0, NULL); }
        if (p->nb[1]) {
            uint64_t key = fnv(0xcbf29ce484222325ULL, &p->dl[1], sizeof p->dl[1]);
            unsigned slot;
            if (p->dl[1] == g_udl[1]) for (int s = 0; s < (fs ? 2 : 1); s++) { VStage *v = s ? fs : vs; for (int i = 0; i < v->nsmp; i++) key = fnv(key, &v->smp[i].binding, sizeof v->smp[i].binding); }      /* session 9: one layout for every pipeline: the binding numbers tell the lists apart */
            for (int i = 0; i < g_ndtex; i++) { key = fnv(key, &g_dtex[i].view, sizeof g_dtex[i].view); key = fnv(key, &g_dtex[i].smp, sizeof g_dtex[i].smp); key = fnv(key, &g_dtex[i].layout, sizeof g_dtex[i].layout); }
            if (!key) key = 1;
            for (slot = (unsigned)key & 4095; g_tset[slot].gen == g_tset_gen && g_tset[slot].key != key; slot = (slot + 1) & 4095) {}
            /* session 4: a hash hit is confirmed by comparing the whole list (layout, views, samplers, image layouts); a
             * different list with the same hash gets a set of its own and is counted */
            int hit = g_tset[slot].gen == g_tset_gen && g_tset[slot].key == key;
            if (hit && (g_tset[slot].dl != p->dl[1] || g_tset[slot].n != g_ndtex || memcmp(&g_tset_list[g_tset[slot].off], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex))) { hit = 0; R.tset_collisions++; slot = 4096; }
            if (hit) { sets[1] = g_tset[slot].set; R.sets_reused++; }
            else {
                int n1 = 0;
                sets[1] = set_alloc(p->dl[1]);
                for (int s = 0; s < (fs ? 2 : 1); s++) {
                    VStage *v = s ? fs : vs;
                    for (int i = 0; i < v->nsmp; i++) {
                        ii[it].sampler = g_dtex[it].smp; ii[it].imageView = g_dtex[it].view; ii[it].imageLayout = g_dtex[it].layout;
                        memset(&w[n1], 0, sizeof w[n1]);
                        w[n1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n1].dstSet = sets[1]; w[n1].dstBinding = v->smp[i].binding; w[n1].descriptorCount = 1;
                        w[n1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n1].pImageInfo = &ii[it]; n1++; it++;
                    }
                }
                vkUpdateDescriptorSets(V.device, (uint32_t)n1, w, 0, NULL);
                if (slot < 4096 && g_tset_used < 3000 && g_tset_list_used + g_ndtex <= (int)(sizeof g_tset_list / sizeof g_tset_list[0])) {
                    g_tset[slot].key = key; g_tset[slot].set = sets[1]; g_tset[slot].gen = g_tset_gen; g_tset[slot].dl = p->dl[1]; g_tset[slot].n = g_ndtex; g_tset[slot].off = g_tset_list_used;
                    memcpy(&g_tset_list[g_tset_list_used], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex);
                    g_tset_list_used += g_ndtex; g_tset_used++;
                }
            }
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &sets[1], 0, NULL);
        }
    }
    {   uint32_t pc[4] = {(uint32_t)R.alpha_func, 0, 0, 0};
        memcpy(&pc[1], &R.alpha_ref, 4); memcpy(&pc[2], &R.point_size, 4);
        if (RS_ON) {                               /* track O: mx_fcs = game pixels per image pixel of this pass (fragment stages translated with screen_scale); points shrink with the image */
            float fcs = R.pass_rs ? 1.0f / R.pass_sx : 1.0f, ps = R.pass_rs ? R.point_size * R.pass_sx : R.point_size;
            memcpy(&pc[3], &fcs, 4); memcpy(&pc[2], &ps, 4);
        }
        vkCmdPushConstants(R.cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    }
    return 1;
}

/* The pass formats of a target set as the pipeline key holds them (also used by the look-ahead of nvn_vk_m4.c on targets
 * that are not bound yet). Returns 0 when one of the images does not exist yet. */
static int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth) {
    int all = 1;
    k->ncolor = (uint8_t)ncol; k->samples = 1;
    for (int i = 0; i < ncol; i++) {
        if (!col[i].tex) continue;
        VTex *vt = col[i].tex->vk;
        int ex, dec;
        if (!vt) { all = 0; continue; }
        VkFormat f = vt->fmt;
        if ((col[i].view.set & 0x84) == 0x84 && col[i].view.format) { VkFormat f2 = tex_vkfmt(col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; }
        k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples;
    }
    if (depth->tex) { VTex *vt = depth->tex->vk; if (!vt) all = 0; else { k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; } }
    return all;
}
static void pkey_targets(PKey *k) { pkey_targets_of(k, R.ncol, R.col, &R.depth); }
#if 0   /* session 3's form */
static void pkey_targets(PKey *k) {
    k->ncolor = (uint8_t)R.ncol; k->samples = 1;
    for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) { VTex *vt = R.col[i].tex->vk; int ex, dec; VkFormat f = vt->fmt; if ((R.col[i].view.set & 0x84) == 0x84 && R.col[i].view.format) { VkFormat f2 = tex_vkfmt(R.col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; } k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples; }
    if (R.depth.tex && R.depth.tex->vk) { VTex *vt = R.depth.tex->vk; k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; }
}
#endif
/* ClearDepthStencil with a stencil mask that is not all bits: Vulkan's clears have no mask, so one triangle over the
 * rectangle replaces the masked bits (no colour, no depth). The pass is active. */
static void stencil_clear_masked(uint32_t mask, uint32_t value, int sx, int sy, int sw, int sh) {
    static VStage *vs;
    PKey k;
    if (!vs) {
        vs = calloc(1, sizeof *vs);
        vs->id = ++g_vstage_ids; vs->ok = 1; vs->stage = -1;
        vs->mod = module_from_glsl(0, "#version 460\nvoid main() { gl_Position = vec4(vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0); }\n", "internal stencil clear", &g_spv_unused);
        if (!vs->mod) vkd_fail("the internal stencil-clear vertex shader does not compile");
        vs->next = g_vstage[0]; g_vstage[0] = vs;
    }
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.sample_mask = 0xffffffffu; k.topo_class = 2; k.poly = 2; k.logic = 3;
    pkey_targets(&k);
    Pipe *p = pipe_get(&k, vs, NULL);
    VkViewport vp = {0, 0, (float)R.pass_w, (float)R.pass_h, 0, 1};
    VkRect2D sc = {{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
    float zero4[4] = {0, 0, 0, 0};
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    if (D.has_eds3) {                              /* session 4: what the key held (no colour written, no blending, fill, every sample) is dynamic state now */
        VkBool32 en[8] = {0};
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8] = {0};
        VkSampleMask all = 0xffffffffu;
        uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
        for (int i = 0; i < 8; i++) { eq[i].srcColorBlendFactor = eq[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; eq[i].dstColorBlendFactor = eq[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; eq[i].colorBlendOp = eq[i].alphaBlendOp = VK_BLEND_OP_ADD; }
        vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en); vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq); vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
        vkCmdSetPolygonModeEXT(R.cmd, VK_POLYGON_MODE_FILL); vkCmdSetAlphaToCoverageEnableEXT(R.cmd, VK_FALSE); vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(k.samples ? k.samples : 1), &all);
        if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, VK_FALSE);
        R.dyn3_dirty = 1;
    }
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc); vkCmdSetLineWidth(R.cmd, 1.0f); vkCmdSetDepthBias(R.cmd, 0, 0, 0); vkCmdSetBlendConstants(R.cmd, zero4);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, mask); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, value);
    vkCmdSetCullMode(R.cmd, VK_CULL_MODE_NONE); vkCmdSetFrontFace(R.cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE); vkCmdSetPrimitiveTopology(R.cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(R.cmd, VK_FALSE); vkCmdSetDepthWriteEnable(R.cmd, VK_FALSE); vkCmdSetDepthCompareOp(R.cmd, VK_COMPARE_OP_ALWAYS); vkCmdSetStencilTestEnable(R.cmd, VK_TRUE);
    vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBiasEnable(R.cmd, VK_FALSE); vkCmdSetPrimitiveRestartEnable(R.cmd, VK_FALSE); vkCmdSetRasterizerDiscardEnable(R.cmd, VK_FALSE);
    vkCmdSetVertexInputEXT(R.cmd, 0, NULL, 0, NULL);
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    R.pipe_bound = NULL; R.dyn_dirty = 1;
}

/* ---------------------------------------------------------------- copies */
/* CopyBufferToTexture and WriteTexels: texels from memory into a region of one level. The image is not synchronised
 * with the texture's own memory first (the OpenGL path does not either). */
static void write_region(Tex *t, const View *vp, const int *r, const void *data, int64_t row_stride, int64_t image_stride) {
    View v;
    memset(&v, 0, sizeof v);
    if (vp) v = *vp;
    VTex *vt = vt_get2(t, 0);
    if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || vt->decode || vt->expand) { nvn_vk_missing("CopyBufferToTexture / WriteTexels into a depth, ASTC-decoded or 3-component texture"); return; }
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0; else if (t->b.target != 2) layer += r[2];
    uint64_t rowlen = row_stride && vt->bw == 1 ? (uint64_t)(row_stride / vt->bpb) : 0, rows = image_stride && row_stride && vt->bw == 1 ? (uint64_t)(image_stride / row_stride) : 0;
    uint64_t bw = ((uint64_t)(rowlen ? rowlen : (uint64_t)r[3]) + vt->bw - 1) / vt->bw, bh = ((uint64_t)(rows ? rows : (uint64_t)r[4]) + vt->bh - 1) / vt->bh, size = bw * bh * vt->bpb * (uint64_t)depth;
    uint64_t off = ring_raw(data, size);
    pass_end(); cmd_begin();
    if (vt->rs) { rs_upload(vt, level, layer, off, (uint32_t)rowlen, (uint32_t)rows, r[0], r[1], r[3], r[4]); t->gpu_written = 1; t->uploaded = 1; return; }      /* track O */
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off; c.bufferRowLength = (uint32_t)rowlen; c.bufferImageHeight = (uint32_t)rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    t->gpu_written = 1; t->uploaded = 1;
}
static void copy_buf_tex(const Cmd *m) {
    Tex *t = P(m->a[1]);
    View v;
    int r[6];
    if (!t || t->dead || !m->a[0]) return;
    memcpy(&v, BLOB(m), sizeof v); memcpy(r, BLOB(m) + 24, 24);
    write_region(t, &v, r, P(m->a[0]), R.row_stride, R.image_stride);
}
static void vk_write_texels(void *arg) {
    WriteJob *j = arg;
    if (V.alive && j->tex && !j->tex->dead) write_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->data, j->row_stride, 0);
}
/* Session 5. CopyTextureToBuffer, ReadTexels, the movie maker's present: a region of one level to host memory, as the
 * OpenGL path's glGetTextureSubImage (tight rows, or the given row / image strides). Synchronous: the command buffer is
 * submitted and waited for (these calls are rare: album, replays, screenshots). Returns the bytes written, 0 when it stopped. */
static size_t read_region(Tex *t, const View *vp, const int *r, void *out, int64_t row_stride, int64_t image_stride) {
    VTex *vt = vt_get(t);
    if (vt->decode || vt->expand || vt->samples > 1) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of an ASTC-decoded, 3-component or multisample texture"); return 0; }
    if (vt->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of a combined depth-stencil texture"); return 0; }
    int level = vp && (vp->set & 0x81) == 0x81 ? vp->lvl0 : 0, layer = vp && (vp->set & 0x82) == 0x82 ? vp->lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    uint64_t rowlen = row_stride && vt->bw == 1 ? (uint64_t)(row_stride / vt->bpb) : 0, rows = image_stride && row_stride && vt->bw == 1 ? (uint64_t)(image_stride / row_stride) : 0;
    uint64_t bw = ((uint64_t)(rowlen ? rowlen : (uint64_t)r[3]) + vt->bw - 1) / vt->bw, bh = ((uint64_t)(rows ? rows : (uint64_t)r[4]) + vt->bh - 1) / vt->bh, size = bw * bh * vt->bpb * (uint64_t)depth;
    if (!size) return 0;
    VMem mem;
    VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem);
    VkBufferImageCopy c;
    pass_end(); cmd_begin();
    VkImageLayout was = vt->layout;
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.bufferRowLength = (uint32_t)rowlen; c.bufferImageHeight = (uint32_t)rows;
    c.imageSubresource.aspectMask = vt->aspect; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)(layer + r[2]); c.imageSubresource.layerCount = vt->is3d ? 1 : (uint32_t)depth;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    if (vt->rs) {                                  /* track O: the game reads ITS rectangle: the image's part of it is blitted up to a temporary image of that size */
        int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
        VkImageBlit b;
        if (r[3] <= 0 || r[4] <= 0 || !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            nvn_vk_missing("resolution scale: ReadTexels / CopyTextureToBuffer of a scaled texture whose format cannot be blitted");
            if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
            vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
            return 0;
        }
        VTex *tmp = rs_temp(vt, r[3], r[4]);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = vt->aspect; b.srcSubresource.mipLevel = (uint32_t)level; b.srcSubresource.baseArrayLayer = (uint32_t)(layer + r[2]); b.srcSubresource.layerCount = 1;
        b.dstSubresource.aspectMask = vt->aspect; b.dstSubresource.layerCount = 1;
        b.srcOffsets[0].x = rs_edge(r[0], vt->sx, lw); b.srcOffsets[0].y = rs_edge(r[1], vt->sy, lh); b.srcOffsets[1].x = rs_edge(r[0] + r[3], vt->sx, lw); b.srcOffsets[1].y = rs_edge(r[1] + r[4], vt->sy, lh); b.srcOffsets[1].z = 1;
        if (b.srcOffsets[1].x <= b.srcOffsets[0].x) b.srcOffsets[1].x = b.srcOffsets[0].x + 1; if (b.srcOffsets[1].y <= b.srcOffsets[0].y) b.srcOffsets[1].y = b.srcOffsets[0].y + 1;
        if (b.srcOffsets[1].x > lw) { b.srcOffsets[0].x = lw - 1; b.srcOffsets[1].x = lw; } if (b.srcOffsets[1].y > lh) { b.srcOffsets[0].y = lh - 1; b.srcOffsets[1].y = lh; }
        b.dstOffsets[1].x = r[3]; b.dstOffsets[1].y = r[4]; b.dstOffsets[1].z = 1;
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.imageSubresource.mipLevel = 0; c.imageSubresource.baseArrayLayer = 0; c.imageSubresource.layerCount = 1; c.imageOffset.x = c.imageOffset.y = c.imageOffset.z = 0;
        vkCmdCopyImageToBuffer(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        g_rs_down++;
    } else
    vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
    cmd_wait_all();
    memcpy(out, mem.map, (size_t)size);
    vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
    R.readbacks++;
    return (size_t)size;
}
static void vk_read_texels(void *arg) {
    ReadJob *j = arg;
    if (!V.alive || !j->tex || j->tex->dead) return;
    if (j->to_pool) { nvn_vk_missing("nvnTextureInvalidateTexels (rendered texels written back into the texture's pool memory)"); return; }
    cb_sync();
    read_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->out, j->row_stride, j->image_stride);
}
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);      /* nnalbum.c, as nvn_gl.c job_present_movie */
static void vk_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!V.alive || !t || t->dead || t->b.width <= 0 || t->b.height <= 0) return;
    VTex *vt = vt_get(t);
    int w = vt->rs ? t->b.width : vt->w, h = vt->rs ? t->b.height : vt->h, r[6] = {0, 0, 0, w, h, 1}, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("nvnQueuePresentTexture to the movie maker's window with a window texture that is not 8-bit RGBA"); return; }
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    if (read_region(t, NULL, r, px, 0, 0)) {
        if (bgr) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
        album_movie_push_frame(px, w, h, w * 4, 0);
    }
    free(px);
}
static void blit_tex(Tex *s, const View *sv, const int *sr, Tex *d, const View *dv, const int *dr, int linear) {
    VTex *vs = vt_get(s), *vd = vt_get(d);
    VkImageBlit b;
    d->gpu_written = 1;
    if (vs == vd) { nvn_vk_missing("CopyTextureToTexture within one texture"); return; }
    pass_end(); cmd_begin();
    vt_layout(vs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = vs->aspect; b.srcSubresource.mipLevel = sv && (sv->set & 0x81) == 0x81 ? (uint32_t)sv->lvl0 : 0; b.srcSubresource.baseArrayLayer = sv && (sv->set & 0x82) == 0x82 && !vs->is3d ? (uint32_t)sv->lay0 : 0; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = vd->aspect; b.dstSubresource.mipLevel = dv && (dv->set & 0x81) == 0x81 ? (uint32_t)dv->lvl0 : 0; b.dstSubresource.baseArrayLayer = dv && (dv->set & 0x82) == 0x82 && !vd->is3d ? (uint32_t)dv->lay0 : 0; b.dstSubresource.layerCount = 1;
    b.srcOffsets[0].x = sr[0]; b.srcOffsets[0].y = sr[1]; b.srcOffsets[1].x = sr[0] + sr[3]; b.srcOffsets[1].y = sr[1] + sr[4]; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = dr[0]; b.dstOffsets[0].y = dr[1]; b.dstOffsets[1].x = dr[0] + dr[3]; b.dstOffsets[1].y = dr[1] + dr[4]; b.dstOffsets[1].z = 1;
    int rs_sw = sr[3], rs_sh = sr[4], rs_dw = dr[3], rs_dh = dr[4];
    if (vs->rs || vd->rs) {                        /* track O: the game's rectangles on the scaled images */
        if (vs->rs) {
            int lw = lvl_(vs->w, (int)b.srcSubresource.mipLevel), lh = lvl_(vs->h, (int)b.srcSubresource.mipLevel);
            b.srcOffsets[0].x = rs_edge(sr[0], vs->sx, lw); b.srcOffsets[0].y = rs_edge(sr[1], vs->sy, lh); b.srcOffsets[1].x = rs_edge(sr[0] + sr[3], vs->sx, lw); b.srcOffsets[1].y = rs_edge(sr[1] + sr[4], vs->sy, lh);
            rs_sw = b.srcOffsets[1].x - b.srcOffsets[0].x; rs_sh = b.srcOffsets[1].y - b.srcOffsets[0].y;
        }
        if (vd->rs) {
            int lw = lvl_(vd->w, (int)b.dstSubresource.mipLevel), lh = lvl_(vd->h, (int)b.dstSubresource.mipLevel);
            b.dstOffsets[0].x = rs_edge(dr[0], vd->sx, lw); b.dstOffsets[0].y = rs_edge(dr[1], vd->sy, lh); b.dstOffsets[1].x = rs_edge(dr[0] + dr[3], vd->sx, lw); b.dstOffsets[1].y = rs_edge(dr[1] + dr[4], vd->sy, lh);
            rs_dw = b.dstOffsets[1].x - b.dstOffsets[0].x; rs_dh = b.dstOffsets[1].y - b.dstOffsets[0].y;
        }
        if (!rs_sw || !rs_sh || !rs_dw || !rs_dh) return;
        g_rs_blits++;
    }
    if (vs->aspect != vd->aspect) { nvn_vk_missing("CopyTextureToTexture between a colour and a depth format"); return; }
    if (vs->samples > 1 && vd->samples == 1 && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT && rs_sw == rs_dw && rs_sh == rs_dh) {
        VkImageResolve rv;
        memset(&rv, 0, sizeof rv);
        rv.srcSubresource = b.srcSubresource; rv.dstSubresource = b.dstSubresource; rv.srcOffset = b.srcOffsets[0]; rv.dstOffset = b.dstOffsets[0];
        rv.extent.width = (uint32_t)rs_sw; rv.extent.height = (uint32_t)rs_sh; rv.extent.depth = 1;
        vkCmdResolveImage(R.cmd, vs->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vd->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rv);
        return;
    }
    if (vs->samples > 1 || vd->samples > 1) { nvn_vk_missing("a scaled or depth copy of a multisample texture"); return; }
    /* glBlitFramebuffer with FRAMEBUFFER_SRGB off copies the stored values; vkCmdBlitImage converts between an sRGB and a
     * linear format. Same class on both sides = the same values. */
    vkCmdBlitImage(R.cmd, vs->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vd->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, linear && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

/* ---------------------------------------------------------------- replay */
static void replay(uint64_t handle, int depth);
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    switch (m->op) {
    case OP_NOP: case OP_DEBUG: case OP_DISCARD: case OP_CONSDILATE: case OP_SUBPIXEL: break;
    case OP_TEXPOOL: R.tpool = P(a[0]); break;
    case OP_SMPPOOL: R.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); R.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? R.ubo[stage] : R.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            unsigned u = (unsigned)(first + i);
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            Tex *t;
            View v;
            SamplerParams sp;
            memcpy(&h, blob + i * 8, 8);
            R.nvn_tex[stage][u] = h;
            if (nvn_front_handle(R.tpool, R.spool, h, &t, &v, &sp) & 1) {      /* what TXQ reports, and whether the shader must use an integer sampler (resolve_handle's rules) */
                int glevels = nvn_front_tex_levels(t), l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : glevels - l0, is3d = t->b.target == 2;
                int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : nvn_front_tex_layers(t), nf = (v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
                dims[0] = (uint32_t)lvl_(t->b.width, l0); dims[1] = (uint32_t)lvl_(t->b.height, l0);
                dims[2] = is3d ? (uint32_t)lvl_(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
                dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
                if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) isint = k == 3 || k == 8 || k == 11 ? 2 : 1;
                if (nf == 0x3E) isint = 1;
            }
            memcpy(&R.c2[stage][48 + 2 * u], dims, 16);
            R.tex_int[stage] = (R.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            R.tex_sint[stage] = (R.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        break;
    }
    case OP_VBUF: for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&R.vbuf[a[0] + i], blob + i * 16, 16); break;
    case OP_VATTR: R.nattr = (int)a[0] < 32 ? (int)a[0] : 32; memcpy(R.vattr, blob, (size_t)R.nattr * 4); break;
    case OP_VSTREAM: for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&R.vstream[i], blob + i * 8, 8); break;
    case OP_BLEND: { const uint8_t *b = (const uint8_t *)&a[0]; if (b[7]) nvn_vk_missing("an advanced blend equation (nvnBlendStateSetAdvancedMode)"); memcpy(R.blend[b[0] & 7], b + 1, 6); R.dyn3_dirty = 1; break; }
    case OP_COLOR: { const uint8_t *b = (const uint8_t *)&a[0]; R.blend_on = b[0]; R.logic = b[1]; R.alpha_func = b[2]; R.dyn3_dirty = 1; if (b[1] != 3 && !D.f2.features.logicOp) nvn_vk_missing("a logic op on a GPU without logicOp"); break; }
    case OP_CHANMASK: R.chanmask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_MULTISAMPLE: { const uint8_t *b = (const uint8_t *)&a[0]; memcpy(R.ms, b, 24); R.dyn3_dirty = 1; if (b[3] || b[5]) nvn_vk_missing("a multisample state with coverage-to-colour, coverage modulation, sample locations or raster samples"); break; }
    case OP_POLYGON: memcpy(R.poly, &a[0], 4); R.dyn_dirty = 1; R.dyn3_dirty = 1; break;
    case OP_DEPTHSTENCIL: R.ds = a[0]; R.dyn_dirty = 1; break;
    case OP_VIEWPORTS: if (a[0] == 0 && a[1] > 0) { memcpy(R.viewport, blob, 16); R.have_viewport = 1; R.dyn_dirty = 1; } break;
    case OP_SCISSORS: if (a[0] == 0 && a[1] > 0) { memcpy(R.scissor, blob, 16); if (R.scissor[2] < 0) R.scissor[2] = 0; if (R.scissor[3] < 0) R.scissor[3] = 0; R.have_scissor = 1; R.dyn_dirty = 1; } break;
    case OP_DEPTHRANGES: if (a[0] == 0 && a[1] > 0) { memcpy(R.drange, blob, 8); R.dyn_dirty = 1; } break;
    case OP_TARGETS: {
        const TargetRef *col = (const TargetRef *)blob, *dep = col + 8;
        int n = (int)a[0] < 8 ? (int)a[0] : 8;
        pass_end();
        for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) ((VTex *)R.col[i].tex->vk)->feedback = 0;
        if (R.depth.tex && R.depth.tex->vk) ((VTex *)R.depth.tex->vk)->feedback = 0;
        memset(R.col, 0, sizeof R.col); memset(&R.depth, 0, sizeof R.depth);
        R.ncol = 0; R.have_targets = 0;
        for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { R.col[i] = col[i]; R.ncol = i + 1; R.have_targets = 1; }
        if (dep->tex && !dep->tex->dead) { R.depth = *dep; R.have_targets = 1; }
        R.dyn_dirty = 1;                           /* default viewport / scissor follow the target size */
        break;
    }
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? R.sref : m->op == OP_STENCIL_VMASK ? R.svmask : R.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        R.dyn_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: memcpy(R.blendc, blob, 16); R.dyn_dirty = 1; break;
    case OP_ALPHAREF: memcpy(&R.alpha_ref, &a[0], 4); break;
    case OP_POINTSIZE: memcpy(&R.point_size, &a[0], 4); break;
    case OP_LINEWIDTH: memcpy(&R.line_width, &a[0], 4); R.dyn_dirty = 1; break;
    case OP_POLYOFFSET: memcpy(R.polyoff, &a[0], 12); R.dyn_dirty = 1; break;
    case OP_SAMPLEMASK: R.sample_mask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_RASTDISCARD: R.rast_discard = a[0] & 1; R.dyn_dirty = 1; break;
    case OP_DEPTHCLAMP: R.depth_clamp = a[0] & 1; R.dyn3_dirty = 1; if ((a[0] & 1) && !D.f2.features.depthClamp) nvn_vk_missing("depth clamp on a GPU without depthClamp"); break;
    case OP_PRIMRESTART: R.restart = a[0] & 1; break;
    case OP_PATCHSIZE: break;                      /* only read by tessellation, which stops at the draw */
    case OP_IMAGES: break;                         /* only read by compute programs: DispatchCompute stops with its name until M3 */
    case OP_FENCE: if (a[0]) { SyncObj *o = P(a[0]); if (!o->dead) o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); } break;
    case OP_WAITSYNC: break;                       /* one queue: what was submitted before has run before */
    case OP_CONSRASTER: if (a[0] & 1) nvn_vk_missing("conservative rasterisation (nvnCommandBufferSetConservativeRasterEnable)"); break;
    case OP_DEPTHBOUNDS: if (a[0] & 1) nvn_vk_missing("the depth bounds test (nvnCommandBufferSetDepthBounds)"); break;
    case OP_RENDER_ENABLE: if (!a[1]) R.render_off = !(a[0] & 1); else nvn_vk_missing("conditional rendering (nvnCommandBufferSetRenderEnableConditional)"); break;
    case OP_ROWSTRIDE: R.row_stride = (int64_t)a[0]; break;
    case OP_IMAGESTRIDE: R.image_stride = (int64_t)a[0]; break;
    case OP_BARRIER: if (a[0] & 0x10) nvn_front_barrier_texmem(); break;      /* ordering bits: every draw is recorded in order; feedback is handled at the draw */
    case OP_DISPATCH: { LONG64 t0 = tk(); compute_dispatch((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); g_tm[2] += tk() - t0; break; }
    case OP_COPY_BUF_BUF: cb_sync(); if (a[0] && a[1]) memmove(P(a[1]), P(a[0]), (size_t)a[2]); R.gen++; break;      /* guest memory is read / written: what compute wrote must be there (as OpenGL's compute_flush) */
    case OP_UPDATE_UBO: cb_sync(); if (a[0]) memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); R.gen++; break;
    case OP_CLEAR_BUF: cb_sync(); if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); } R.gen++; break;
    case OP_CLEARCOLOR: {
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2] & 15;
        if (!R.have_targets || R.render_off || (int)i >= R.ncol || !R.col[i].tex) break;
        if (!pass_begin()) break;
        if (mask != 15) {                          /* a partial channel mask only matters for channels the format has */
            Fmt f; int comps = 4, nf = R.col[i].tex->b.format;
            if (nf >= 1 && nf <= 0x30) comps = (nf - 1) / 12 + 1;
            if (!nvn_front_fmt(nf, &f) || (mask & ((1u << comps) - 1)) != (1u << comps) - 1) { if (mask) nvn_vk_missing("ClearColor with a partial channel mask"); break; }
        }
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = i; memcpy(&ca.clearValue.color, blob, 16);
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_CLEARDS: {
        if (!R.have_targets || R.render_off || !R.depth.tex || !((a[0] & 1) || (uint32_t)a[2])) break;
        if (!pass_begin()) break;
        VTex *vt = R.depth.tex->vk;
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0 || !vt) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        ca.aspectMask = ((a[0] & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((uint32_t)a[2] ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if ((ca.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && ((uint32_t)a[2] & 0xff) != 0xff) {
            stencil_clear_masked((uint32_t)a[2] & 0xff, (uint32_t)a[1], sx, sy, sw, sh);
            ca.aspectMask &= ~(VkFlags)VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        ca.aspectMask &= vt->aspect;
        if (!ca.aspectMask) break;
        memcpy(&ca.clearValue.depthStencil.depth, &a[3], 4); ca.clearValue.depthStencil.stencil = (uint32_t)a[1];
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_DRAW_ARRAYS:
        { LONG64 t0 = tk(); int ok = draw_prepare((unsigned)a[0]); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); break; } }
        vkCmdDraw(R.cmd, (uint32_t)a[2], (uint32_t)a[4], (uint32_t)a[1], (uint32_t)a[3]);
        R.frame_draws++; nvn_front_count(0, 1);
        break;
    case OP_DRAW_ELEMENTS: {
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
        int count = (int)a[2];
        if (count <= 0 || !a[3]) { R.frame_skipped++; nvn_front_count(1, 1); break; }
        if (t == 0 && !D.has_iu8) { nvn_vk_missing("8-bit indices on a GPU without VK_EXT_index_type_uint8"); break; }
        BufRef ib;
        buf_ref(a[3], (uint64_t)count << t, (uint64_t)1 << t, &ib);    /* before the draw is prepared: a ring wrap submits */
        { LONG64 t0 = tk(); int ok = draw_prepare((unsigned)a[0]); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); break; } }
        vkCmdBindIndexBuffer(R.cmd, ib.buf, ib.off, t == 0 ? VK_INDEX_TYPE_UINT8_EXT : t == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(R.cmd, (uint32_t)count, (uint32_t)a[6], 0, (int32_t)a[4], (uint32_t)a[5]);
        R.frame_draws++; nvn_front_count(0, 1);
        break;
    }
    case OP_COPY_BUF_TEX: cb_sync(); copy_buf_tex(m); break;
    case OP_COPY_TEX_BUF: {                        /* a0 texture, a3 buffer address; blob: view, region (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        if (!t || t->dead || !a[3]) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        cb_sync();
        read_region(t, &v, r, P(a[3]), R.row_stride, R.image_stride);
        R.gen++;                                   /* guest memory changed: ranges are streamed again */
        break;
    }
    case OP_COPY_TEX_TEX: {
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(sr, blob + 24, 24); memcpy(&dv, blob + 48, sizeof dv); memcpy(dr, blob + 72, 24);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1));
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_CLEARTEX: {                            /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        Fmt f;
        if (!t || t->dead) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        int whole = a[6] || (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] >= lvl_(t->b.width, level) && r[4] >= lvl_(t->b.height, level));
        pass_end();
        VTex *vt = vt_get(t);
        int comps = t->b.format >= 1 && t->b.format <= 0x30 ? (t->b.format - 1) / 12 + 1 : 4;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !nvn_front_fmt(t->b.format, &f) || f.kind == 4 || vt->decode || vt->expand) { nvn_vk_missing("nvnCommandBufferClearTexture of a depth, compressed or 3-component texture"); break; }
        if (!whole || ((unsigned)a[4] & ((1u << comps) - 1)) != (1u << comps) - 1) { nvn_vk_missing("nvnCommandBufferClearTexture of a part of a level or with a partial channel mask"); break; }
        cmd_begin();
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, (uint32_t)(vt->is3d ? 0 : layer), 1};
        VkClearColorValue cv;
        memcpy(&cv, blob + 48, 16);
        vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
        t->gpu_written = 1; t->uploaded = 1;
        break;
    }
    case OP_CALL: for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); } break;
    default: { static char msg[OP_COUNT][64]; snprintf(msg[m->op % OP_COUNT], 64, "nvnCommandBuffer%s", nvn_front_op_name(m->op)); nvn_vk_missing(msg[m->op % OP_COUNT]); break; }
    }
}
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = nvn_front_list(handle, &len);
    if (!data) { static int n; if (n++ < 10) printf("nvn_vk: command handle %llx is unknown or expired\n", (unsigned long long)handle); return; }
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
        off += m->size;
    }
}

/* ---------------------------------------------------------------- the RBackend members */
static void prewarm_wait(void);
static void vk_submit(void *arg) {
    SubmitJob *j = arg;
    if (!V.alive) return;                          /* after the shutdown at exit: the device is gone */
    prewarm_wait();
    LONG64 t0 = tk();
    cb_sync();                                     /* what the last batch's dispatches wrote is in guest memory before this batch reads it */
    LONG64 t1 = tk();
    nvn_front_batch_begin();
    R.gen++;                                       /* guest memory may have changed since the last batch */
    if (g_scan_on) { LONG64 ts = tk(); Scan s; scan_begin(&s); for (int i = 0; i < j->count; i++) scan_list(j->handles[i], 0, &s); g_tm[9] += tk() - ts; }      /* session 4: what this batch will need goes to the workers before it is replayed */
    for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
    cb_batch_end();
    g_tm[7] += t1 - t0; g_tm[3] += tk() - t1;
    nvn_front_count(3, 1);
}
static void vk_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); vk_submit(j); }
static void vk_idle(void) { if (V.alive) { cb_poll(); nvn_front_take_dead(vk_drop); } }
static void vk_housekeeping(void *arg) { (void)arg; if (V.alive) nvn_front_take_dead(vk_drop); }
static void vk_flush(void *arg) { (void)arg; if (V.alive) cmd_submit(NULL, NULL); }
static void vk_finish(void *arg) { (void)arg; if (V.alive) cmd_wait_all(); }
static void vk_fence(void *arg) { SyncJob *j = arg; if (j->o->dead || !V.alive) return; j->o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); }
static void vk_sync_wait(void *arg) {
    SyncJob *j = arg;
    uint64_t want = (uint64_t)(uintptr_t)j->o->s;
    if (!want || !V.alive) { j->result = 3; return; }       /* never fenced */
    want--;
    if (want <= R.done) { j->result = 0; return; }
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (!s->pending || s->serial != want) continue;
        VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, j->timeout);
        if (r == VK_SUCCESS) { if (s->serial > R.done) R.done = s->serial; j->result = 1; cb_poll(); } else j->result = r == VK_TIMEOUT ? 2 : 3;
        return;
    }
    j->result = 0;                                  /* its command buffer was reused: finished long ago */
}
static void vk_server_wait(void *arg) { SyncJob *j = arg; j->timeout = ~0ull; if (j->o->s) vk_sync_wait(j); j->result = j->o->s != NULL; }
static void vk_sync_drop(void *arg) { SyncObj *o = arg; o->s = NULL; }
static void vk_reset_status(void *arg) { *(int *)arg = 0; }       /* NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR: a lost device ends the process with its own message */
static void vk_no_grid(void *arg) { nvn_vk_missing("nvnMultisampleStateGetSampleLocationsGrid"); ((int *)arg)[0] = ((int *)arg)[1] = 1; }

/* The window texture as a 24-bit BMP, the file save_bmp of the OpenGL path writes (row 0 of the texture is the top row). */
static void save_bmp_vk(const char *path, VTex *vt) {
    VMem mem;
    int w = vt->w, h = vt->h, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (vt->rs && getenv("SSBU_VK_SHOT_NATIVE") && atoi(getenv("SSBU_VK_SHOT_NATIVE"))) {      /* track O: the scaled image itself, texel for texel (the default below is the game's size, filtered up as the window shows it) */
        VMem m2;
        uint64_t size = (uint64_t)w * h * 4;
        VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m2);
        VkBufferImageCopy c;
        if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2); return; }
        pass_end(); cmd_begin();
        VkImageLayout was = vt->layout;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
        cmd_wait_all();
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t head[54] = {'B', 'M'};
            uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, fsize = 54 + row * (uint32_t)h, v;
            uint8_t *rowb = calloc(1, row);
            memcpy(head + 2, &fsize, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
            fwrite(head, 1, 54, f);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *s = (const uint8_t *)m2.map + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
                fwrite(rowb, 1, row, f);
            }
            free(rowb); fclose(f);
            printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
        }
        vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2);
        return;
    }
    if (vt->rs) { w = vt->t->b.width; h = vt->t->b.height; }      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("nvn_vk: screenshot: the window texture's format %d is not an 8-bit RGBA one; not saved\n", (int)vt->fmt); return; }
    /* session 5: through read_region, the path of ReadTexels / CopyTextureToBuffer (so every screenshot exercises it) */
    int rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    (void)mem;
    if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); return; }
    uint8_t head[54] = {'B', 'M'}, *rowb;
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    FILE *f = fopen(path, "wb");
    if (f) {
        memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
        fwrite(head, 1, 54, f);
        rowb = calloc(1, row);
        for (int y = h - 1; y >= 0; y--) {
            const uint8_t *s = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
            fwrite(rowb, 1, row, f);
        }
        free(rowb); fclose(f);
        printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);      /* the front end's line, word for word: tests/ref/port_capture.py and the shot tools look for it */
    }
    free(px);
}
static VkSemaphore g_sem[8][2];
static VkSemaphore g_sem_img[16];                  /* session 11: the "rendering finished" semaphore, one per swapchain image */
static int g_isem;
/* Session 11, the PROGRESS BAR: our own overlay, drawn over the swapchain image after the blit of the game's picture (never
 * into the game's textures, so pictures saved by SSBU_SHOT* do not have it). A bar at the bottom of the window:
 *   - while the loading barrier holds a loading screen: the same picture is presented again about ten times a second with
 *     the bar growing (jobs done of jobs queued when the hold began), so the screen is not a still picture;
 *   - while the container store is read from data.arc (first start) and while the preparation runs: a thinner bar on every
 *     presented frame, NOT near a fight.
 * SSBU_VK_PROGRESS=0 turns it off. The image: 8x8, two layers (bar colour, track colour), cleared once; drawn by two blits. */
static struct { VkImage img; VMem mem; int made, on; float frac; int thick; void *bar_arg; } g_ovl = {.on = -1};
static void overlay_draw(uint32_t index) {
    if (g_ovl.on < 0) g_ovl.on = !(getenv("SSBU_VK_PROGRESS") && !atoi(getenv("SSBU_VK_PROGRESS")));
    if (!g_ovl.on || g_ovl.thick <= 0) return;
    if (g_ovl.thick < 8) g_ovl_frames++;
    VkImageSubresourceRange sr2 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!g_ovl.made) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        VkClearColorValue c0 = {{0.92f, 0.92f, 0.92f, 1.0f}}, c1 = {{0.06f, 0.06f, 0.06f, 1.0f}};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent.width = 8; ci.extent.height = 8; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 2;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_ovl.img));
        vkGetImageMemoryRequirements(V.device, g_ovl.img, &rq);
        g_ovl.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_ovl.img, g_ovl.mem.mem, g_ovl.mem.off));
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &sr2);
        sr2.baseArrayLayer = 1;
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c1, 1, &sr2);
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        g_ovl.made = 1;
    }
    int W = (int)V.extent.width, H = (int)V.extent.height, th = g_ovl.thick * H / 720, x0 = W / 10, x1 = W - W / 10, y1 = H - H / 40, y0;
    float f = g_ovl.frac < 0 ? 0 : g_ovl.frac > 1 ? 1 : g_ovl.frac;
    if (th < 2) th = 2;
    y0 = y1 - th;
    if (x1 - x0 < 8 || y0 < 0) return;
    for (int pass = 0; pass < 2; pass++) {          /* the track, then the bar over it */
        VkImageBlit b;
        int xe = pass ? x0 + (int)((float)(x1 - x0) * f) : x1;
        if (xe <= x0) continue;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.srcSubresource.baseArrayLayer = pass ? 0 : 1;
        b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = 8; b.srcOffsets[1].y = 8; b.srcOffsets[1].z = 1;
        b.dstOffsets[0].x = pass ? x0 : x0 - 2; b.dstOffsets[0].y = pass ? y0 : y0 - 2; b.dstOffsets[1].x = pass ? xe : x1 + 2; b.dstOffsets[1].y = pass ? y1 : y1 + 2; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
}
static void vk_present2(void *arg, int progress);
/* nvn_vk_m4.c m4_barrier, render thread, g_sh_lock NOT held: the held loading screen once more, with the bar at `frac` */
static void present_progress(float frac) { if (!g_ovl.bar_arg) return; g_ovl.frac = frac; g_ovl.thick = 8; vk_present2(g_ovl.bar_arg, 1); g_ovl.thick = 0; }
static void vk_present(void *arg) { vk_present2(arg, 0); }
static void vk_present2(void *arg, int progress) {
    PresentJob *j = arg;
    static int shot = -2, every, shot_from;
    if (!V.alive) return;
    LONG64 t_present = tk();
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    LONG64 frame = nvn_front_frame_next();
    {   /* session 12, TEST ONLY: SSBU_VK_WD_TEST=<frame>[,<seconds>] holds the render thread at that frame (default 12 s), to see the watchdog's report */
        static int wt = -2, ws = 12;
        if (wt == -2) { const char *e = getenv("SSBU_VK_WD_TEST"); wt = e ? atoi(e) : -1; if (e && strchr(e, ',')) ws = atoi(strchr(e, ',') + 1); }
        if (wt > 0 && frame == wt && !progress) { printf("nvn_vk_watchdog: TEST: the render thread is held %d s at frame %lld (SSBU_VK_WD_TEST)\n", ws, (long long)frame); fflush(stdout); wt = -1; Sleep((DWORD)ws * 1000); }
    }
    if (progress) goto swap;
    if (j->win) for (int i = 0; i < j->win->count && i < 8; i++) wintex_note(j->win->tex[i]);
    g_ovl.bar_arg = arg;
    g_wd_where = 1;
    m4_barrier(frame, R.frame_draws);
    g_wd_where = 0;
    g_ovl.bar_arg = NULL;
    /* the thin bar of a normal frame: the store being read, or the preparation running, and no fight near */
    g_ovl.thick = 0;
    if ((g_prep_state == 5 || g_prep_state == 1) && !fight_recent()) {
        g_ovl.thick = 4;
        g_ovl.frac = g_prep_state == 5 ? (float)g_store_pct / 100.0f : g_prep_nfiles ? (float)g_pq_done / (float)(g_pq_queued > 0 ? g_pq_queued + (LONG64)(g_prep_nfiles - g_prep_read) * 90 : 1) : 0;      /* about 90 variations a file */
    }
    g_render_tid = GetCurrentThreadId();
    if (g_skip_fr_n) { g_skip_frames++; printf("nvn_vk_skipf: frame %lld (%s): %lld draws left out (shader_wait = skip)\n", (long long)frame, g_in_fight ? "fight" : "no fight", (long long)g_skip_fr_n); g_skip_fr_n = 0; }
    if (shot == -2) { shot = getenv("SSBU_SHOT") ? atoi(getenv("SSBU_SHOT")) : -1; every = getenv("SSBU_SHOT_EVERY") ? atoi(getenv("SSBU_SHOT_EVERY")) : 0; shot_from = getenv("SSBU_SHOT_FROM") ? atoi(getenv("SSBU_SHOT_FROM")) : 0; }
    pass_end();
    { int step = nvn_front_take_step_shot(); if (step && vt) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp_vk(path, vt); } }
    if (vt && (frame == shot || (every > 0 && frame >= shot_from && frame % every == 0))) {
        char path[300];
        const char *dir = getenv("SSBU_SHOT_DIR");
        snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
        save_bmp_vk(path, vt);
    }
    {   /* session 9, SSBU_SHOT_BATTLE=<n>: one picture at the n-th battle frame (dispatches and more than 120 draws), i.e. at the
         * same moment of the fight whatever the loading took: battle_<n>.bmp in SSBU_SHOT_DIR */
        static int bshot = -2, bcount;
        if (bshot == -2) { bshot = getenv("SSBU_SHOT_BATTLE") ? atoi(getenv("SSBU_SHOT_BATTLE")) : -1; bcount = getenv("SSBU_SHOT_BATTLE_N") ? atoi(getenv("SSBU_SHOT_BATTLE_N")) : 1; }
        if (vt && bshot > 0 && g_battle_frames >= bshot) {      /* SSBU_SHOT_BATTLE_N=<k>: k pictures in a row from there (frame stepping, the 120 Hz mode) */
            char path[300];
            const char *dir = getenv("SSBU_SHOT_DIR");
            snprintf(path, sizeof path, "%s%sbattle_%d.bmp", dir ? dir : "", dir ? "/" : "", (int)g_battle_frames);
            save_bmp_vk(path, vt);
            printf("nvn_vk: picture of battle frame %d saved at frame %lld: %s\n", (int)g_battle_frames, (long long)frame, path);
            if (--bcount <= 0) bshot = -1;
        }
    }
swap: ;
    pass_end();
    /* an extra present of a held loading screen: everything submitted so far is waited for first, so that the semaphore pair
     * taken below has no pending operation (validation run s11v2: VUID-vkAcquireNextImageKHR-semaphore-01779, three times,
     * all at such presents: with one submission a present the pair came round before its command buffer had been waited for) */
    if (progress) cmd_wait_all();
    /* The swapchain has the window texture's colour class: a blit between them then keeps the stored values (the OpenGL
     * path blits with FRAMEBUFFER_SRGB off). */
    VkFormat want = vt && (vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    if (want != V.format.format) { cmd_wait_all(); swapchain_destroy(); V.format.format = want; printf("nvn_vk: swapchain format %d (the window texture's colour class)\n", (int)want); }
    uint32_t index = 0;
    VkResult r = VK_SUCCESS;
    /* one pair of semaphores per present, 8 in turn: a semaphore handed to vkAcquireNextImageKHR must not still be waited
     * for by an earlier submission (validation layer: VUID-vkAcquireNextImageKHR-semaphore-01779) */
    g_isem = (g_isem + 1) % 8;
    if (!g_sem[g_isem][0]) { VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][0])); VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][1])); }
    VkSemaphore acq = g_sem[g_isem][0], rend = g_sem[g_isem][1];
    int ok = V.swapchain || swapchain_create();
    InterlockedIncrement64(&g_wd_try);
    if (ok) {
        g_wd_where = 2;
        r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); ok = swapchain_create(); if (ok) r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index); }
        if (ok && r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_ok("vkAcquireNextImageKHR", r); ok = 0; }
    }
    if (!ok) { InterlockedIncrement64(&g_vk_skipped); cmd_submit(NULL, NULL); }
    else {
        int suboptimal = r == VK_SUBOPTIMAL_KHR;
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        {   /* session 11: the semaphore a present waits for is one PER SWAPCHAIN IMAGE (validation run s11v3, twice in 9,783
             * frames: VUID-vkQueueSubmit-pSignalSemaphores-00067, "may still be in use by the swapchain", when the driver
             * hands out the same image many times in a row and the ring of 8 comes round); the acquire semaphore stays in the ring */
            if (index < 16) {
                if (!g_sem_img[index]) { VkSemaphoreCreateInfo smi2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi2, NULL, &g_sem_img[index])); }
                rend = g_sem_img[index];
            }
        }
        cmd_begin();
        if (vt) vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd_begin();
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (vt && vt->samples == 1) {
            VkImageBlit b;
            int s4[4] = {0, 0, vt->w, vt->h};
            if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(s4, j->win->crop, sizeof s4);
            if (vt->rs && j->win->crop[2] > 0 && j->win->crop[3] > 0) {      /* track O: the crop is in the game's pixels */
                int x1 = rs_edge(s4[0] + s4[2], vt->sx, vt->w), y1 = rs_edge(s4[1] + s4[3], vt->sy, vt->h);
                s4[0] = rs_edge(s4[0], vt->sx, vt->w); s4[1] = rs_edge(s4[1], vt->sy, vt->h); s4[2] = x1 - s4[0]; s4[3] = y1 - s4[1];
            }
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = (int32_t)V.extent.width; b.dstOffsets[1].y = (int32_t)V.extent.height; b.dstOffsets[1].z = 1;
            vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        } else { VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f; vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
        overlay_draw(index);
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        cmd_submit(acq, rend);
        VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &rend; pr.swapchainCount = 1; pr.pSwapchains = &V.swapchain; pr.pImageIndices = &index;
        g_wd_where = 4;
        r = vkQueuePresentKHR(V.queue, &pr);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || suboptimal) { cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }
        else vk_ok("vkQueuePresentKHR", r);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { LONG64 nowt = tk(); if (g_gap_prev && nowt - g_gap_prev > g_gap_max) g_gap_max = nowt - g_gap_prev; g_gap_prev = nowt; g_wd_beat = nowt; }      /* session 12: the watchdog's heartbeat = a frame reached the window */
        if (!progress) InterlockedIncrement64(&g_vk_frames);
    }
    g_wd_where = 0;
    if (progress) { g_ovl_presents++; return; }
    g_tm[4] += tk() - t_present;
    nvn_front_frame_end(R.frame_draws, R.frame_skipped, R.frame_clears);
    { static LONG64 disp_prev; g_in_fight = R.dispatches != disp_prev; disp_prev = R.dispatches; if (g_in_fight) { g_fight_last = frame; if (g_prep_state == 1) g_prep_fight_frames++; } }      /* a frame with compute dispatches (skinning): fighters are on screen */
    {   /* session 9: per-frame record for the "nvn_vk_gpu_fight:" line */
        static LONG64 t_prev;
        LONG64 t_now = tk();
        int battle = g_in_fight && R.frame_draws > 120;      /* the fight draws 126..450 a frame (entrance over 205), loading screens 38..45 */
        if (battle) g_battle_frames++;
        {   /* how long the way to the fight took (menus are scripted by polls, so two runs differ by their loading) */
            static LONG64 t_first, t_load; static int said;
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            if (!t_first) t_first = t_now;
            if (!battle && R.frame_draws <= 60 && frame > 600) { if (!t_load) t_load = t_now; } else if (!battle && R.frame_draws > 60) t_load = 0;      /* t_load: the start of the run of loading-screen frames before the battle */
            if (battle && !said) { said = 1; printf("nvn_vk: first battle frame is frame %lld, %.1f s after the first present; the loading screen before it lasted %.1f s\n", (long long)frame, (double)(t_now - t_first) / (double)fq.QuadPart, t_load ? (double)(t_now - t_load) / (double)fq.QuadPart : 0.0); }
        }
        if (frame >= 0 && frame < FRSTAT_N) {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            g_fr_fight[frame] = (uint8_t)battle;
            g_fr_wall[frame] = t_prev ? (float)((double)(t_now - t_prev) * 1000.0 / (double)fq.QuadPart) : 0.0f;
        }
        t_prev = t_now;
        g_cur_frame = frame + 1;
    }
    {   /* session 8: what this frame paid for textures made or read at their use (hold log), then the textures made ahead */
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        if (holdlog_on() && g_up_fr_tk * 1000 > 3 * fq.QuadPart) { static int nl; if (nl++ < 300) printf("nvn_vk_upl: frame %lld (%s): %d textures made or read at their use, %.1f MB, %.1f ms\n", (long long)frame, g_in_fight ? "fight" : "no fight", g_up_fr_n, (double)g_up_fr_bytes / 1048576.0, (double)g_up_fr_tk * 1000.0 / (double)fq.QuadPart); }
        g_up_fr_tk = g_up_fr_bytes = 0; g_up_fr_n = 0;
        tex_ahead(frame);
    }
    if (frame % 300 == 0) {                      /* where the render thread's time went, per frame, in the last 300 frames */
        static LONG64 last[12], last_sets, last_flush, last_reused, last_disp;
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double k = 1000.0 / (double)fq.QuadPart / 300.0;
        if (g_ts_on > 0) { static double ts_last; static LONG64 n_last; printf("nvn_vk_gpu: frames %lld..%lld: GPU time %.2f ms per frame (first to last command of %lld command buffers finished in the window)\n", (long long)frame - 300, (long long)frame, (g_ts_ns - ts_last) / 1e6 / 300.0, (long long)(g_ts_n - n_last)); ts_last = g_ts_ns; n_last = g_ts_n; }
        printf("nvn_vk_perf: frames %lld..%lld ms/frame: batch replay %.2f (of it: dispatch %.2f, draw preparation %.2f, pipelines %.2f, shader modules %.2f), wait for the GPU %.2f, copy-back %.2f (waiting at batch start %.2f), present %.2f | per frame: %.1f dispatches, %.0f descriptor sets allocated, %.0f reused, %.1f submissions | pipelines not made ahead so far: %lld | holds so far: %lld, %.1f ms, longest %.1f ms\n",
               (long long)frame - 300, (long long)frame, (double)(g_tm[3] - last[3]) * k, (double)(g_tm[2] - last[2]) * k, (double)(g_tm[8] - last[8]) * k, (double)(g_tm[5] - last[5]) * k, (double)(g_tm[6] - last[6]) * k,
               (double)(g_tm[0] - last[0]) * k, (double)(g_tm[1] - last[1]) * k, (double)(g_tm[7] - last[7]) * k, (double)(g_tm[4] - last[4]) * k, (double)(R.dispatches - last_disp) / 300.0, (double)(R.sets - last_sets) / 300.0, (double)(R.sets_reused - last_reused) / 300.0, (double)(R.flushes - last_flush) / 300.0, (long long)g_pipe_miss,
               (long long)(g_hold_n[0] + g_hold_n[1] + g_hold_n[2] + g_hold_n[3]), (double)(g_hold_tk[0] + g_hold_tk[1] + g_hold_tk[2] + g_hold_tk[3]) * k * 300.0, (double)g_hold_max * k * 300.0);
        if (frame % 3000 == 0) m4_report();
        memcpy(last, g_tm, sizeof last); last_sets = R.sets; last_flush = R.flushes; last_reused = R.sets_reused; last_disp = R.dispatches;
    }
    if (frame % 600 == 0)
        printf("nvn_vk: totals at frame %lld: %lld images made (%lld live, %lld views), %lld uploads (%lld MB), %lld shader modules, %lld pipelines, %lld descriptor sets, %lld command buffers, %lld ring wraps, %lld feedback passes, %lld MB device memory\n",
               (long long)frame, (long long)R.images, (long long)R.images_live, (long long)R.views, (long long)R.uploads, (long long)(R.upload_bytes >> 20), (long long)R.modules, (long long)R.pipelines, (long long)R.sets,
               (long long)R.flushes, (long long)R.ring_wraps, (long long)R.feedback, (long long)(g_mem_total >> 20));
    if (frame % 600 == 0 && R.dispatches) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        printf("nvn_vk: compute at frame %lld: %lld dispatches, %lld KB copied back to guest memory, draws using a vertex buffer written by compute in the same batch: %lld, copy-backs forced inside a batch: %lld, waits for the GPU before a copy-back: %lld (%.1f ms in all), %lld buffers made; words the guest wrote into a range pending copy-back: %lld (also written by compute: %lld)\n",
               (long long)frame, (long long)R.dispatches, (long long)(R.cb_bytes >> 10), (long long)R.cb_draws, (long long)R.cb_syncs, (long long)R.cb_waits, (double)R.cb_wait_ticks * 1000.0 / (double)fq.QuadPart, (long long)R.cb_made,
               (long long)R.cb_guest_words, (long long)R.cb_conflict_words);
    }
    if (frame % 600 == 0) { printf("nvn_vk: images made per NVN texture format (format:count):"); for (int i = 0; i < 256; i++) if (g_fmt_hist[i]) printf(" 0x%x:%u", i, g_fmt_hist[i]); printf("\n"); }
    R.frame_draws = R.frame_skipped = R.frame_clears = 0;
}
const RBackend rb_vk = {"vk", vk_submit, vk_submit_async, vk_present, vk_present_movie, vk_fence, vk_sync_wait, vk_server_wait, vk_sync_drop, vk_flush, vk_finish, vk_write_texels, vk_read_texels, vk_housekeeping,
                        vk_reset_status, vk_no_grid, vk_idle};

/* ---------------------------------------------------------------- start and end */
static VTex *dummy_image(VkImageType type, uint32_t layers, VkFlags flags) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    VkClearColorValue z;
    VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vt->magic = 0x56544558; vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = vt->h = vt->d = 1; vt->levels = 1; vt->layers = (int)layers; vt->samples = 1;
    ci.imageType = type; ci.format = vt->fmt; ci.extent.width = ci.extent.height = ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.flags = flags;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&z, 0, sizeof z);
    vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    return vt;
}
static VkImageView dummy_view(VTex *vt, VkImageViewType type, uint32_t layers) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VView *n = calloc(1, sizeof *n);
    ci.image = vt->img; ci.viewType = type; ci.format = vt->fmt; ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.levelCount = 1; ci.subresourceRange.layerCount = layers;
    VKC(vkCreateImageView(V.device, &ci, NULL, &n->view));
    n->vtype = 100 + (int)type; n->next = vt->views; vt->views = n;
    return n->view;
}
/* ---------------------------------------------------------------- M4: pipelines made before they are needed (design section 5, step 1)
 * At start the worker threads read ..\cache\vk\pipes.bin and make every pipeline listed there (stages from their records and
 * SPIR-V files, pipelines through the driver's pipeline cache kept in ..\cache\vk\pipelines_<gpu>.bin). The render thread
 * waits for them before it replays its first batch, so the tables are never shared with a running worker. */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's start-up workers) */
static HANDLE g_pw_thread[16];
static int g_pw_nthread, g_pw_n, g_pw_waited;
static PipeRec *g_pw_rec;
static volatile LONG g_pw_next;
static LONG64 g_pw_t0, g_pw_t1;
static char g_pcache_path[160];
static VStage *pw_stage(int s, uint64_t h) {      /* g_sh_lock held */
    for (VStage *v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s) return v;
    VStage *v = vstage_from_rec(s, h);
    if (v) InterlockedIncrement64(&g_pw_stages);
    return v;
}
static DWORD WINAPI pw_main(void *arg) {
    (void)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&g_pw_next) - 1;
        if (i >= g_pw_n) break;
        PipeRec *r = &g_pw_rec[i];
        EnterCriticalSection(&g_sh_lock);
        VStage *vs = pw_stage(r->compute ? 5 : 0, r->vs_h), *fs = r->fs_h ? pw_stage(1, r->fs_h) : NULL;
        LeaveCriticalSection(&g_sh_lock);
        if (!vs || (r->fs_h && !fs)) { InterlockedIncrement64(&g_pw_failed); continue; }      /* a stage record or SPIR-V file is gone: made at the draw as on a first run */
        if (r->compute) { compute_pipe_make(vs); InterlockedIncrement64(&g_pw_pipes); continue; }
        double ms;
        r->k.vs = vs->id; r->k.fs = fs ? fs->id : 0;
        Pipe *p = pipe_make(&r->k, vs, fs, &ms);
        uint64_t h = fnv(0xcbf29ce484222325ULL, &r->k, sizeof r->k);
        EnterCriticalSection(&g_sh_lock);
        p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
        LeaveCriticalSection(&g_sh_lock);
        InterlockedIncrement64(&g_pw_pipes);
    }
    InterlockedExchange64(&g_pw_t1, tk());
    return 0;
}
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
static void prewarm_start(void) {
    InitializeCriticalSection(&g_sh_lock);
    {   /* the driver's pipeline cache, ours on disk: keyed by GPU and driver version (the driver checks its own header too) */
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        void *data = NULL;
        snprintf(g_pcache_path, sizeof g_pcache_path, "../cache/vk/pipelines_%04x_%04x_%08x.bin", (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
        FILE *fp = fopen(g_pcache_path, "rb");
        if (fp) { fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, fp) == (size_t)n) { ci.initialDataSize = (size_t)n; ci.pInitialData = data; } fclose(fp); }
        if (vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; VKC(vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache)); }
        printf("nvn_vk: pipeline cache %s: %llu KB read\n", g_pcache_path, (unsigned long long)(ci.initialDataSize >> 10));
        free(data);
    }
    if (getenv("SSBU_VK_PREWARM") && !atoi(getenv("SSBU_VK_PREWARM"))) { printf("nvn_vk: SSBU_VK_PREWARM=0: no pipeline is made ahead\n"); return; }
    FILE *fp = fopen("../cache/vk/pipes.bin", "rb");
    if (!fp) { printf("nvn_vk: no ../cache/vk/pipes.bin: a first run on this PC, every pipeline is made at the draw that first needs it\n"); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_pw_rec = malloc((size_t)n + 1);
    g_pw_n = (int)(fread(g_pw_rec, sizeof(PipeRec), (size_t)n / sizeof(PipeRec), fp));
    fclose(fp);
    int kept = 0;
    qsort(g_pw_rec, (size_t)g_pw_n, sizeof(PipeRec), piperec_cmp);
    for (int i = 0; i < g_pw_n; i++) if (g_pw_rec[i].magic == PIPEREC_MAGIC && (!kept || memcmp(&g_pw_rec[kept - 1], &g_pw_rec[i], sizeof(PipeRec)))) g_pw_rec[kept++] = g_pw_rec[i];
    g_pw_n = kept;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pw_nthread = (int)si.dwNumberOfProcessors - 2;
    if (g_pw_nthread < 1) g_pw_nthread = 1;
    if (g_pw_nthread > 16) g_pw_nthread = 16;
    if (getenv("SSBU_VK_WORKERS") && atoi(getenv("SSBU_VK_WORKERS")) > 0 && atoi(getenv("SSBU_VK_WORKERS")) <= 16) g_pw_nthread = atoi(getenv("SSBU_VK_WORKERS"));
    g_pw_t0 = tk();
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = CreateThread(NULL, 0, pw_main, NULL, 0, NULL);
    printf("nvn_vk: %d pipelines listed in ../cache/vk/pipes.bin: %d worker threads make them now\n", g_pw_n, g_pw_nthread);
}
static void prewarm_wait(void) {                   /* render thread, before its first batch */
    if (g_pw_waited) return;
    g_pw_waited = 1;
    if (!g_pw_nthread) return;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk();
    WaitForMultipleObjects((DWORD)g_pw_nthread, g_pw_thread, TRUE, INFINITE);
    for (int i = 0; i < g_pw_nthread; i++) CloseHandle(g_pw_thread[i]);
    printf("nvn_vk: pipelines made ahead: %lld of %d listed (%lld stages from their records, %lld entries without their stage files) in %.0f ms on %d threads; the first batch waited %.0f ms for them\n",
           (long long)g_pw_pipes, g_pw_n, (long long)g_pw_stages, (long long)g_pw_failed, (double)(g_pw_t1 - g_pw_t0) * 1000.0 / (double)fq.QuadPart, g_pw_nthread, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    free(g_pw_rec); g_pw_rec = NULL;
}
static void pcache_save(void) {
    size_t n = 0;
    if (!g_pcache || vkGetPipelineCacheData(V.device, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n);
    if (data && vkGetPipelineCacheData(V.device, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[176];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
        FILE *fp = fopen(tmp, "wb");
        if (fp) { fwrite(data, 1, n, fp); fclose(fp); MoveFileExA(tmp, g_pcache_path, MOVEFILE_REPLACE_EXISTING); printf("nvn_vk: pipeline cache saved: %llu KB\n", (unsigned long long)(n >> 10)); }
    }
    free(data);
}
#endif
#include "nvn_vk_wd.c"                             /* session 12: the freeze watchdog (wd_start / wd_stop) */
static void vkd_init(void) {
#define X(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has no " #n);
    VKD(X)
#undef X
    vkCmdSetVertexInputEXT = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(V.device, "vkCmdSetVertexInputEXT");
    if (D.has_push) { vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(V.device, "vkCmdPushDescriptorSetKHR"); if (!vkCmdPushDescriptorSetKHR) D.has_push = 0; }
    if (D.has_eds3) {
#define X3(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has VK_EXT_extended_dynamic_state3 but no " #n);
        X3(vkCmdSetColorBlendEnableEXT) X3(vkCmdSetColorBlendEquationEXT) X3(vkCmdSetColorWriteMaskEXT) X3(vkCmdSetPolygonModeEXT) X3(vkCmdSetAlphaToCoverageEnableEXT) X3(vkCmdSetSampleMaskEXT)
        if (D.f2.features.depthClamp) { X3(vkCmdSetDepthClampEnableEXT) }
#undef X3
    }
    printf("nvn_vk: descriptors: buffers (set 0) %s, textures (set 1) one set per distinct binding list per command buffer\n", D.has_push ? "pushed (VK_KHR_push_descriptor)" : "one allocated set per draw");
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &D.mp);
    for (int i = 0; i < NSLOT; i++) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ai.commandPool = V.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(V.device, &ai, &R.slot[i].cmd));
        VKC(vkCreateFence(V.device, &fi, NULL, &R.slot[i].fence));
    }
    R.ring_size = 256ull << 20;
    R.ring = vbuf_new(R.ring_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &R.ring_mem);
    R.zero = vbuf_new(65536 + 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &R.zero_mem);
    memset(R.zero_mem.map, 0, 65536 + 64);
    { float f1 = 1.0f; uint32_t i1 = 1; memcpy((uint8_t *)R.zero_mem.map + 65536 + 12, &f1, 4); memcpy((uint8_t *)R.zero_mem.map + 65536 + 28, &i1, 4); }
    R.gen = 1; R.point_size = 1.0f; R.line_width = 1.0f; R.drange[1] = 1.0f; R.sample_mask = 0xffffffffu; R.chanmask = 0xffffffffu; R.logic = 3; R.poly[2] = 2; R.poly[1] = 1;
    R.svmask[0] = R.svmask[1] = R.swmask[0] = R.swmask[1] = 0xff; R.ds = 0x10 * 2;
    for (int i = 0; i < 8; i++) { R.blend[i][0] = 2; R.blend[i][1] = 1; R.blend[i][2] = 2; R.blend[i][3] = 1; R.blend[i][4] = 1; R.blend[i][5] = 1; }
    if (nvn_front_clip_modes() != (1 | 1 << 8)) nvn_vk_missing("a window origin other than upper left or a depth range other than 0..1 (nvnDeviceSetWindowOriginMode / SetDepthMode)");
    cmd_begin();
    g_dummy.d2 = dummy_image(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT); g_dummy.d1 = dummy_image(VK_IMAGE_TYPE_1D, 1, 0); g_dummy.d3 = dummy_image(VK_IMAGE_TYPE_3D, 1, 0);
    g_dummy.v[DIM_1D] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D, 1); g_dummy.v[DIM_1DA] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1);
    g_dummy.v[DIM_2D] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D, 1); g_dummy.v[DIM_2DA] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
    g_dummy.v[DIM_CUBE] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE, 6); g_dummy.v[DIM_CUBEA] = D.f2.features.imageCubeArray ? dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6) : g_dummy.v[DIM_CUBE];
    g_dummy.v[DIM_3D] = dummy_view(g_dummy.d3, VK_IMAGE_VIEW_TYPE_3D, 1);
    {   /* a texture bound without a sampler: the texture's own OpenGL parameters (nearest, nearest mip, repeat) */
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.maxLod = 1000.0f; ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VKC(vkCreateSampler(V.device, &ci, NULL, &g_smp_default));
    }
    cmd_wait_all();
    CreateDirectoryA("../cache/vk", NULL);
    prewarm_start();
    wd_start();
    printf("nvn_vk: drawing part ready: %d command buffers in flight, stream ring %llu MB, %u memory types; debug checks %s\n", NSLOT, (unsigned long long)(R.ring_size >> 20), (unsigned)D.mp.memoryTypeCount, D.debug ? "on" : "off (SSBU_VK_DEBUG=1)");
    if (RS_ON) printf("nvn_vk_scale: internal resolution scale %.3f (%s): screen-class targets are made at %d x %d instead of 1920 x 1080; fragment stages are another translation\n",
                      (double)rs_scale(), getenv("SSBU_VK_SCALE") ? "SSBU_VK_SCALE" : "graphics.ini", (int)(1920.0f * rs_scale() + 0.5f), (int)(1080.0f * rs_scale() + 0.5f));
    fflush(stdout);
}
static int frstat_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
/* Session 9: battle frames only (dispatches and more than 120 draws: the fight and the result screen's fighters); the first 30 are left out (their buffers were recorded
 * while the stage was still being set up). GPU ms = sum of the spans of the frame's command buffers (SSBU_VK_GPUTIME=1). */
static void frstat_report(void) {
    static float g[FRSTAT_N], w[FRSTAT_N];
    int n = 0, seen = 0, slow = 0;
    double wsum = 0, gsum = 0;
    for (int f = 0; f < FRSTAT_N; f++) {
        if (!g_fr_fight[f] || seen++ < 30) continue;
        g[n] = g_fr_gpu[f]; w[n] = g_fr_wall[f]; wsum += w[n]; gsum += g[n]; if (w[n] > 33.4f) slow++; n++;
    }
    if (n < 100) return;
    qsort(g, (size_t)n, sizeof g[0], frstat_cmp); qsort(w, (size_t)n, sizeof w[0], frstat_cmp);
    printf("nvn_vk_gpu_fight: %d battle frames: GPU ms per frame median %.2f, p99 %.2f, max %.2f, mean %.2f%s | time between presents: median %.2f ms, p99 %.2f, max %.2f, %d frames over 33.4 ms | %.1f frames per second over the battle\n",
           n, (double)g[n / 2], (double)g[n * 99 / 100], (double)g[n - 1], gsum / n, g_ts_on > 0 ? "" : " (SSBU_VK_GPUTIME not set: no GPU times)",
           (double)w[n / 2], (double)w[n * 99 / 100], (double)w[n - 1], slow, wsum > 0 ? 1000.0 * n / wsum : 0.0);
}
static void vkd_shutdown(void) {
    wd_stop();
    if (!R.ring) return;
    {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double k = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_tex: textures made ahead of their first use: %lld (%.0f MB, %.0f ms in all, at most %.1f ms after one present); made or read at their use: %lld times, %.0f ms (of them in fight frames: %lld, %.0f ms)\n",
               (long long)g_ta_n, (double)g_ta_bytes / 1048576.0, (double)g_ta_tk * k, (double)g_ta_max_tk * k, (long long)g_late_n, (double)g_late_tk * k, (long long)g_late_fight_n, (double)g_late_fight_tk * k);
    }
    frstat_report();
    if (RS_ON && rs_ui_native()) printf("nvn_vk_scale: SSBU_VK_UI_NATIVE=1: the window's textures are kept at the game's size; passes with such a colour target and a SCALED depth target (drawn without depth): %lld\n", (long long)g_rs_mixed);
    if (RS_ON) printf("nvn_vk_scale: resolution scale %.3f: %lld images made scaled, %lld fragment stages translated in this run for it (%lld of them read the pixel position, %lld fetch texels by integer position), %lld uploads into and %lld read-backs from a scaled image, %lld copies / blits touching one\n",
                      (double)rs_scale(), (long long)g_rs_images, (long long)g_rs_stages, (long long)g_rs_fcs, (long long)g_rs_tf, (long long)g_rs_up, (long long)g_rs_down, (long long)g_rs_blits);
    m4_stop();
    pcache_save();
    if (g_pcache) { vkDestroyPipelineCache(V.device, g_pcache, NULL); g_pcache = NULL; }
    for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]);
    R.done = ~0ull; garbage_collect();
    while (g_vtex_all) { VTex *vt = g_vtex_all; if (vt->t) vt->t->vk = NULL; vt_destroy(vt); }
    for (int i = 0; i < 1024; i++) for (Pipe *p = g_pipes[i], *n; p; p = n) { n = p->next; vkDestroyPipeline(V.device, p->p, NULL); if (p->p_fast) vkDestroyPipeline(V.device, p->p_fast, NULL); if (!p->libbed) { vkDestroyPipelineLayout(V.device, p->layout, NULL); vkDestroyDescriptorSetLayout(V.device, p->dl[0], NULL); vkDestroyDescriptorSetLayout(V.device, p->dl[1], NULL); } free(p); }
    gpl_destroy();
    for (int i = 0; i < 8; i++) for (int k = 0; k < 2; k++) if (g_sem[i][k]) { vkDestroySemaphore(V.device, g_sem[i][k], NULL); g_sem[i][k] = NULL; }
    for (int i = 0; i < 16; i++) if (g_sem_img[i]) { vkDestroySemaphore(V.device, g_sem_img[i], NULL); g_sem_img[i] = NULL; }
    if (g_ovl.made) { vkDestroyImage(V.device, g_ovl.img, NULL); g_ovl.made = 0; }      /* the progress bar's image; its memory goes with the blocks */
    for (int i = 0; i < 256; i++) if (g_cb[i].buf) { vkDestroyBuffer(V.device, g_cb[i].buf, NULL); g_cb[i].buf = NULL; g_cb[i].state = 0; }
    g_cb_live = g_cb_pending = g_ncdisp = 0;
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i], *n; v; v = n) {
        n = v->next;
        if (v->cp) { vkDestroyPipeline(V.device, v->cp, NULL); vkDestroyPipelineLayout(V.device, v->cl, NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[0], NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[1], NULL); }
        if (v->mod) vkDestroyShaderModule(V.device, v->mod, NULL);
        free(v);
    }
    for (int i = 0; i < g_nsmp; i++) vkDestroySampler(V.device, g_smp[i].s, NULL);
    vkDestroySampler(V.device, g_smp_default, NULL);
    if (g_ts_pool) { vkDestroyQueryPool(V.device, g_ts_pool, NULL); g_ts_pool = NULL; g_ts_on = 0; }
    for (int i = 0; i < NSLOT; i++) { for (int k = 0; k < R.slot[i].npool; k++) vkDestroyDescriptorPool(V.device, R.slot[i].pool[k], NULL); vkDestroyFence(V.device, R.slot[i].fence, NULL); }
    vkDestroyBuffer(V.device, R.ring, NULL); vkDestroyBuffer(V.device, R.zero, NULL);
    for (int b = 0; b < g_nblk; b++) if (g_blk[b]) { vkFreeMemory(V.device, g_blk[b]->mem, NULL); free(g_blk[b]); g_blk[b] = NULL; }
    R.ring = NULL;
    printf("nvn_vk: drawing part shut down: %lld images, %lld pipelines, %lld shader modules, %d samplers destroyed; %lld bytes of block space lost to fragmentation records\n", (long long)R.images, (long long)R.pipelines, (long long)R.modules, g_nsmp, (long long)g_mem_leak);
}
