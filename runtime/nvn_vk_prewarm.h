/* Included by nvn_vk_m4.c. Per-worker startup measurements: no shared counter
 * contention, no driver feedback requests in normal gameplay. Vulkan 1.3 core. */
enum { PW_LIBRARY, PW_LINK, PW_WHOLE, PW_COMPUTE, PW_API_N };
typedef struct {
    LONG64 calls, valid, hits, failed, ticks, ns, max_ticks;
} PwApi;
typedef struct {
    LONG64 record_ticks, spv_ticks, module_ticks, lock_wait, lock_hold, stage_wait;
    LONG64 records, spvs, modules, bytes;
    PwApi api[PW_API_N];
} PwProfile;
static PwProfile g_pw_profiles[16];
static __declspec(thread) PwProfile *g_pw_profile;
static int g_pw_profile_on;

static LONG64 pw_lock(void) {
    LONG64 start = g_pw_profile ? tk() : 0;
    EnterCriticalSection(&g_sh_lock);
    LONG64 now = g_pw_profile ? tk() : 0;
    if (g_pw_profile) g_pw_profile->lock_wait += now - start;
    return now;
}
static void pw_unlock(LONG64 start) {
    if (g_pw_profile) g_pw_profile->lock_hold += tk() - start;
    LeaveCriticalSection(&g_sh_lock);
}

static void pw_feedback(int kind, VkResult result, LONG64 start, const VkPipelineCreationFeedback *f) {
    PwApi *a = &g_pw_profile->api[kind];
    LONG64 dt = tk() - start;
    a->calls++; a->ticks += dt;
    if (dt > a->max_ticks) a->max_ticks = dt;
    if (result != VK_SUCCESS) a->failed++;
    if (f->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) {
        a->valid++; a->ns += (LONG64)f->duration;
        if (f->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) a->hits++;
    }
}
/* The creation feedback a call already asks for (nvn_vk_m4.c chains one to stage libraries and links for fb_note): a
 * pNext chain may hold only one structure of a type, so that one is read instead of chaining a second. */
static VkPipelineCreationFeedback *pw_feedback_in(const void *chain) {
    for (const VkBaseInStructure *s = chain; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) return ((const VkPipelineCreationFeedbackCreateInfo *)s)->pPipelineCreationFeedback;
    return NULL;
}
static VkResult pw_graphics(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *info, VkPipeline *out) {
    if (!g_pw_profile) return vkCreateGraphicsPipelines(V.device, cache, 1, info, NULL, out);
    VkPipelineCreationFeedback *have = pw_feedback_in(info->pNext);
    if (have) {
        LONG64 t0 = tk();
        VkResult r = vkCreateGraphicsPipelines(V.device, cache, 1, info, NULL, out);
        pw_feedback((info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) ? PW_LIBRARY : info->stageCount ? PW_WHOLE : PW_LINK, r, t0, have);
        return r;
    }
    VkGraphicsPipelineCreateInfo ci = *info;
    VkPipelineCreationFeedback f = {0};
    VkPipelineCreationFeedbackCreateInfo feedback = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    feedback.pNext = ci.pNext; feedback.pPipelineCreationFeedback = &f; ci.pNext = &feedback;
    LONG64 t0 = tk();
    VkResult r = vkCreateGraphicsPipelines(V.device, cache, 1, &ci, NULL, out);
    pw_feedback((ci.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) ? PW_LIBRARY : ci.stageCount ? PW_WHOLE : PW_LINK, r, t0, &f);
    return r;
}
static VkResult pw_compute(const VkComputePipelineCreateInfo *info, VkPipeline *out) {
    if (!g_pw_profile) return vkCreateComputePipelines(V.device, g_pcache, 1, info, NULL, out);
    VkComputePipelineCreateInfo ci = *info;
    VkPipelineCreationFeedback f = {0};
    VkPipelineCreationFeedbackCreateInfo feedback = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    feedback.pNext = ci.pNext; feedback.pPipelineCreationFeedback = &f; ci.pNext = &feedback;
    LONG64 t0 = tk();
    VkResult r = vkCreateComputePipelines(V.device, g_pcache, 1, &ci, NULL, out);
    pw_feedback(PW_COMPUTE, r, t0, &f);
    return r;
}
/* Call only after all history workers have published their last sample and
 * signalled g_pw_gate. Durations are summed worker time, NOT wall time. */
static void pw_profile_report(int workers) {
    if (!g_pw_profile_on) return;
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    double ms = 1000.0 / (double)freq.QuadPart;
    PwProfile p = {0};
    for (int i = 0; i < workers; i++) {
        PwProfile *s = &g_pw_profiles[i];
        p.record_ticks += s->record_ticks; p.spv_ticks += s->spv_ticks; p.module_ticks += s->module_ticks;
        p.lock_wait += s->lock_wait; p.lock_hold += s->lock_hold;
        p.stage_wait += s->stage_wait;
        p.records += s->records; p.spvs += s->spvs; p.modules += s->modules; p.bytes += s->bytes;
        for (int k = 0; k < PW_API_N; k++) {
            PwApi *a = &p.api[k], *b = &s->api[k];
            a->calls += b->calls; a->valid += b->valid; a->hits += b->hits;
            a->failed += b->failed; a->ticks += b->ticks; a->ns += b->ns;
            if (b->max_ticks > a->max_ticks) a->max_ticks = b->max_ticks;
        }
    }
    printf("nvn_vk_prewarm_profile: summed worker ms (overlapping, not additive wall time): records %.1f (%lld), SPIR-V I/O %.1f (%lld files, %lld bytes), shader modules %.1f (%lld), history table lock wait %.1f hold %.1f, shared stage wait %.1f\n",
           p.record_ticks * ms, p.records, p.spv_ticks * ms, p.spvs, p.bytes, p.module_ticks * ms, p.modules, p.lock_wait * ms, p.lock_hold * ms, p.stage_wait * ms);
    const char *names[] = {"library", "link", "whole", "compute"};
    for (int k = 0; k < PW_API_N; k++) {
        PwApi *a = &p.api[k];
        printf("nvn_vk_prewarm_profile: %s calls %lld failed %lld valid %lld application-cache-hits %lld | API worker %.1f ms driver %.1f ms longest %.1f ms\n",
               names[k], a->calls, a->failed, a->valid, a->hits, a->ticks * ms, (double)a->ns / 1e6, a->max_ticks * ms);
    }
}
