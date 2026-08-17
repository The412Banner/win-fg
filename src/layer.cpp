// win-fg — Vulkan implicit layer entry points and swapchain/present interception.
// Structure follows the public Khronos loader-layer interface and the renderdoc
// layer guide (Apache-2.0 / CC-BY learning refs). No code from bionic-fg / lsfg
// / GameScope. The frame-gen compute is win-fg's own (see framegen.*).
#include "vk_dispatch.hpp"
#include "framegen.hpp"
#include "config.hpp"
#include "log.hpp"
#include <vulkan/vk_layer.h>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

using namespace winfg;

// Khronos headers don't define this; layers declare their own export macro.
#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {
std::mutex g_lock;
std::map<void*, InstanceDispatch> g_inst;
std::map<void*, VkInstance>       g_instHandle;

struct DeviceState {
    DeviceDispatch dd;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    const InstanceDispatch* id = nullptr;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    FrameGen fg;
    Config cfg;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool fgInited = false;
    std::string confPath;          // guest conf.toml, watched for live hot-reload
    long long confMtime = 0;
    bool resetPrev = false;        // set on any conf change -> recapture prev frame
};

// Returns the conf.toml mtime (0 if absent).
static long long stat_mtime(const std::string& p) {
    struct stat sb;
    if (p.empty() || stat(p.c_str(), &sb) != 0) return 0;
    return (long long)sb.st_mtime * 1000000000LL + (long long)sb.st_mtim.tv_nsec;
}
std::map<void*, DeviceState> g_dev;

// One in-flight generate+present cycle's sync objects.
struct FrameCtx {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence   fence      = VK_NULL_HANDLE;  // compute+copy done
    VkSemaphore acquireSem = VK_NULL_HANDLE; // spare image acquired
    VkSemaphore genSem   = VK_NULL_HANDLE;   // generated frame ready to present
    VkSemaphore currSem  = VK_NULL_HANDLE;   // real frame ready to present
    bool submitted = false;
};

struct SwapState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format{}; VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;

    // ── Phase 3 frame-insertion resources ────────────────────────────────────
    VkImage       prevImg  = VK_NULL_HANDLE;  // owned copy of the last real frame
    VkDeviceMemory prevMem = VK_NULL_HANDLE;
    VkImageView   prevView = VK_NULL_HANDLE;
    bool          prevValid = false;
    VkCommandPool cmdPool  = VK_NULL_HANDLE;
    std::vector<FrameCtx> ring;               // small ring so we don't stall every frame
    uint32_t      ringIdx  = 0;
    bool          insertReady = false;
};

// Create an owned 2D color image + view (used for the prev-frame copy).
static bool makeOwnedImage(const DeviceDispatch& dd, const VkPhysicalDeviceMemoryProperties& mp,
                           VkDevice dev, VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage,
                           VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {ext.width, ext.height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dd.CreateImage(dev, &ci, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; dd.GetImageMemoryRequirements(dev, img, &mr);
    uint32_t idx = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { idx = i; break; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = idx;
    if (dd.AllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    if (dd.BindImageMemory(dev, img, mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return dd.CreateImageView(dev, &vi, nullptr, &view) == VK_SUCCESS;
}

static void imgBarrier(const DeviceDispatch& dd, VkCommandBuffer cmd, VkImage img,
                       VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dd.CmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}
std::map<VkSwapchainKHR, SwapState> g_swap;

// Owned RGBA8 target the synth writes into, then blit to the BGRA swapchain image
// (blit handles the R/B channel order, so no storage-format swizzle bug).
struct GenTarget { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
std::map<VkSwapchainKHR, GenTarget> g_gen;

static const int kRing = 2;

// Create prev-copy image, gen target, command pool + ring for one swapchain.
static bool initInsert(SwapState& s, DeviceState& ds) {
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    if (!makeOwnedImage(dd, ds.memProps, dev, s.extent, s.format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            s.prevImg, s.prevMem, s.prevView)) { WFG_LOGE("initInsert prev image failed"); return false; }
    GenTarget g;
    if (!makeOwnedImage(dd, ds.memProps, dev, s.extent, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            g.img, g.mem, g.view)) { WFG_LOGE("initInsert gen image failed"); return false; }
    g_gen[s.swapchain] = g;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = ds.queueFamily;
    if (dd.CreateCommandPool(dev, &pci, nullptr, &s.cmdPool) != VK_SUCCESS) { WFG_LOGE("initInsert cmdpool failed"); return false; }
    s.ring.resize(kRing);
    for (auto& fc : s.ring) {
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = s.cmdPool; ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1;
        dd.AllocateCommandBuffers(dev, &ci, &fc.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        dd.CreateFence(dev, &fi, nullptr, &fc.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        dd.CreateSemaphore(dev, &si, nullptr, &fc.acquireSem);
        dd.CreateSemaphore(dev, &si, nullptr, &fc.genSem);
        dd.CreateSemaphore(dev, &si, nullptr, &fc.currSem);
    }
    s.prevValid = false; s.ringIdx = 0; s.insertReady = true;
    WFG_LOGI("insert resources ready (%ux%u ring=%d)", s.extent.width, s.extent.height, kRing);
    return true;
}

static void destroyInsert(SwapState& s, DeviceState& ds) {
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    if (dev && dd.DeviceWaitIdle) dd.DeviceWaitIdle(dev);
    for (auto& fc : s.ring) {
        if (fc.fence) dd.DestroyFence(dev, fc.fence, nullptr);
        if (fc.acquireSem) dd.DestroySemaphore(dev, fc.acquireSem, nullptr);
        if (fc.genSem) dd.DestroySemaphore(dev, fc.genSem, nullptr);
        if (fc.currSem) dd.DestroySemaphore(dev, fc.currSem, nullptr);
    }
    s.ring.clear();
    if (s.cmdPool) dd.DestroyCommandPool(dev, s.cmdPool, nullptr); s.cmdPool = VK_NULL_HANDLE;
    if (s.prevView) dd.DestroyImageView(dev, s.prevView, nullptr);
    if (s.prevImg) dd.DestroyImage(dev, s.prevImg, nullptr);
    if (s.prevMem) dd.FreeMemory(dev, s.prevMem, nullptr);
    s.prevImg = VK_NULL_HANDLE; s.prevMem = VK_NULL_HANDLE; s.prevView = VK_NULL_HANDLE;
    auto it = g_gen.find(s.swapchain);
    if (it != g_gen.end()) {
        if (it->second.view) dd.DestroyImageView(dev, it->second.view, nullptr);
        if (it->second.img) dd.DestroyImage(dev, it->second.img, nullptr);
        if (it->second.mem) dd.FreeMemory(dev, it->second.mem, nullptr);
        g_gen.erase(it);
    }
    s.insertReady = false; s.prevValid = false;
}

template <typename T> T next_gipa(const void* pNext, VkStructureType, VkLayerFunction);
} // namespace

// ── instance chain ───────────────────────────────────────────────────────────
static VkLayerInstanceCreateInfo* findInstanceLink(const VkInstanceCreateInfo* ci) {
    auto* l = (VkLayerInstanceCreateInfo*)ci->pNext;
    while (l && !(l->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && l->function == VK_LAYER_LINK_INFO))
        l = (VkLayerInstanceCreateInfo*)l->pNext;
    return l;
}

extern "C" VkResult VKAPI_CALL winfg_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAlloc, VkInstance* pInstance) {
    auto* link = findInstanceLink(pCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext; // advance chain

    auto createInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = createInstance(pCreateInfo, pAlloc, pInstance);
    if (r != VK_SUCCESS) return r;

    InstanceDispatch d{};
    d.GetInstanceProcAddr = gipa;
    d.DestroyInstance = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");
    d.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*pInstance, "vkGetPhysicalDeviceMemoryProperties");
    d.GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    std::lock_guard<std::mutex> lk(g_lock);
    g_inst[dispatch_key(*pInstance)] = d;
    g_instHandle[dispatch_key(*pInstance)] = *pInstance;
    WFG_LOGI("CreateInstance ok — win-fg layer in the chain");
    return VK_SUCCESS;
}

extern "C" void VKAPI_CALL winfg_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAlloc) {
    std::lock_guard<std::mutex> lk(g_lock);
    void* k = dispatch_key(instance);
    auto it = g_inst.find(k);
    if (it != g_inst.end()) { it->second.DestroyInstance(instance, pAlloc); g_inst.erase(it); g_instHandle.erase(k); }
}

// ── device chain ─────────────────────────────────────────────────────────────
static VkLayerDeviceCreateInfo* findDeviceLink(const VkDeviceCreateInfo* ci) {
    auto* l = (VkLayerDeviceCreateInfo*)ci->pNext;
    while (l && !(l->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && l->function == VK_LAYER_LINK_INFO))
        l = (VkLayerDeviceCreateInfo*)l->pNext;
    return l;
}

extern "C" VkResult VKAPI_CALL winfg_CreateDevice(
    VkPhysicalDevice phys, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAlloc, VkDevice* pDevice) {
    auto* link = findDeviceLink(pCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto createDevice = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = createDevice(phys, pCreateInfo, pAlloc, pDevice);
    if (r != VK_SUCCESS) return r;

    DeviceState st;
    st.phys = phys;
    DeviceDispatch& dd = st.dd;
    dd.GetDeviceProcAddr = gdpa;
#define L(n) load(dd.n, gdpa, *pDevice, "vk" #n)
    L(DestroyDevice); L(GetDeviceQueue); L(QueueSubmit); L(QueueWaitIdle); L(DeviceWaitIdle);
    L(CreateSwapchainKHR); L(DestroySwapchainKHR); L(GetSwapchainImagesKHR); L(AcquireNextImageKHR); L(QueuePresentKHR);
    L(CreateImage); L(DestroyImage); L(CreateImageView); L(DestroyImageView);
    L(AllocateMemory); L(FreeMemory); L(BindImageMemory); L(GetImageMemoryRequirements);
    L(CreateBuffer); L(DestroyBuffer); L(GetBufferMemoryRequirements); L(BindBufferMemory); L(MapMemory); L(UnmapMemory);
    L(CreateSampler); L(DestroySampler);
    L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout); L(CreatePipelineLayout); L(DestroyPipelineLayout);
    L(CreateComputePipelines); L(DestroyPipeline); L(CreateDescriptorPool); L(DestroyDescriptorPool);
    L(AllocateDescriptorSets); L(UpdateDescriptorSets);
    L(CreateCommandPool); L(DestroyCommandPool); L(AllocateCommandBuffers); L(FreeCommandBuffers);
    L(BeginCommandBuffer); L(EndCommandBuffer); L(CmdBindPipeline); L(CmdBindDescriptorSets); L(CmdDispatch);
    L(CmdPipelineBarrier); L(CmdCopyImage); L(CmdBlitImage); L(CmdClearColorImage);
    L(CreateFence); L(DestroyFence); L(WaitForFences); L(ResetFences); L(CreateSemaphore); L(DestroySemaphore);
#undef L

    std::lock_guard<std::mutex> lk(g_lock);
    void* ik = dispatch_key(phys);
    st.id = &g_inst[ik];
    st.cfg = load_config();
    st.confPath = conf_path();
    st.confMtime = stat_mtime(st.confPath);
    // pick a queue family with compute
    uint32_t qfc = 0; st.id->GetPhysicalDeviceQueueFamilyProperties(phys, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfc);
    st.id->GetPhysicalDeviceQueueFamilyProperties(phys, &qfc, qf.data());
    for (uint32_t i = 0; i < qfc; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { st.queueFamily = i; break; }
    dd.GetDeviceQueue(*pDevice, st.queueFamily, 0, &st.queue);
    st.device = *pDevice;
    st.id->GetPhysicalDeviceMemoryProperties(phys, &st.memProps);
    g_dev[dispatch_key(*pDevice)] = std::move(st);
    auto& ds = g_dev[dispatch_key(*pDevice)];
    WFG_LOGI("CreateDevice ok (enable=%d model=%d mult=%d flowScale=%.2f computeQF=%u)",
             ds.cfg.enabled, ds.cfg.model, ds.cfg.multiplier, ds.cfg.flowScale, ds.queueFamily);
    return VK_SUCCESS;
}

extern "C" void VKAPI_CALL winfg_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAlloc) {
    std::lock_guard<std::mutex> lk(g_lock);
    void* k = dispatch_key(device);
    auto it = g_dev.find(k);
    if (it == g_dev.end()) return;
    it->second.fg.destroy();
    if (it->second.cmdPool) it->second.dd.DestroyCommandPool(device, it->second.cmdPool, nullptr);
    auto destroy = it->second.dd.DestroyDevice;
    g_dev.erase(it);
    destroy(device, pAlloc);
}

// ── swapchain tracking ───────────────────────────────────────────────────────
extern "C" VkResult VKAPI_CALL winfg_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAlloc, VkSwapchainKHR* pSwapchain) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto& st = g_dev[dispatch_key(device)];
    // force sampled+storage usage so the compute engine can read/write frames
    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    ci.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Bump minImageCount by +1 so 2x frame insertion has a reliable spare image to
    // acquire. Without this, the game holds most of the 2-3 swapchain images in
    // flight and AcquireNextImageKHR for the extra "generated" present starves,
    // dropping us into the 3a fallback (single present, no doubling). Standard
    // FG trick — LSFG/FSR3 do the same. If the driver rejects (surface max
    // exceeded, memory), retry with the game's original count so we don't take
    // the app down.
    const uint32_t origMin = pCreateInfo->minImageCount;
    ci.minImageCount = origMin + 1u;
    VkResult r = st.dd.CreateSwapchainKHR(device, &ci, pAlloc, pSwapchain);
    if (r != VK_SUCCESS) {
        WFG_LOGI("CreateSwapchain(minImageCount=%u) rejected (r=%d), retrying with app's minImageCount=%u",
                 ci.minImageCount, (int)r, origMin);
        ci.minImageCount = origMin;
        r = st.dd.CreateSwapchainKHR(device, &ci, pAlloc, pSwapchain);
    }
    if (r != VK_SUCCESS) return r;
    SwapState s; s.swapchain = *pSwapchain; s.format = pCreateInfo->imageFormat; s.extent = pCreateInfo->imageExtent;
    uint32_t n = 0; st.dd.GetSwapchainImagesKHR(device, *pSwapchain, &n, nullptr);
    s.images.resize(n); st.dd.GetSwapchainImagesKHR(device, *pSwapchain, &n, s.images.data());
    s.views.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.images[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        st.dd.CreateImageView(device, &vi, nullptr, &s.views[i]);
    }
    WFG_LOGI("CreateSwapchain %ux%u fmt=%d images=%u (enabled=%d)",
             s.extent.width, s.extent.height, (int)s.format, n, st.cfg.enabled);
    // Always initialise the compute engine when the layer is loaded (not gated on
    // enabled), so the pipelines/pyramids build up-front and a live in-game enable
    // has nothing left to set up. Proves the compute path on Turnip even while idle.
    if (!st.fgInited) {
        bool ok = st.fg.init(&st.dd, st.id, st.phys, device, st.queueFamily, st.queue);
        st.fg.configure(st.cfg);
        st.fgInited = ok;
        if (!ok) WFG_LOGE("framegen init FAILED — FG will not run this swapchain");
    }
    if (st.fgInited) {
        if (!st.fg.onResize(s.extent, s.format))
            WFG_LOGE("framegen onResize FAILED for %ux%u", s.extent.width, s.extent.height);
        if (!initInsert(s, st)) WFG_LOGE("initInsert failed — FG will passthrough");
    }
    g_swap[*pSwapchain] = std::move(s);
    return VK_SUCCESS;
}

extern "C" void VKAPI_CALL winfg_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAlloc) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto& st = g_dev[dispatch_key(device)];
    auto it = g_swap.find(swapchain);
    if (it != g_swap.end()) {
        destroyInsert(it->second, st);
        for (auto v : it->second.views) if (v) st.dd.DestroyImageView(device, v, nullptr);
        g_swap.erase(it);
    }
    st.dd.DestroySwapchainKHR(device, swapchain, pAlloc);
}

// ── present hook ─────────────────────────────────────────────────────────────
// First-draft bring-up: runs the flow+synth compute for (prev,curr) into an
// owned target to prove the pipeline end-to-end, then presents the real frame
// unchanged (safe passthrough). Inserting the generated frame as an extra
// present (the actual 2x) is the device bring-up step — see docs/BRINGUP.md.
extern "C" VkResult VKAPI_CALL winfg_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    std::lock_guard<std::mutex> lk(g_lock);
    // Queues share their device's dispatch pointer, so the device key IS the
    // queue's dispatch key — look it up directly (the map key is already a
    // dispatch key; applying dispatch_key() to it again would double-deref).
    auto it = g_dev.find(dispatch_key(queue));
    if (it == g_dev.end()) return VK_ERROR_DEVICE_LOST;
    DeviceState* st = &it->second;

    // Hot-reload conf.toml so the in-game controls (enable / multiplier / model /
    // flow scale) reach the layer live. Cheap stat every present; only re-read the
    // file when its mtime actually changes.
    long long m = stat_mtime(st->confPath);
    if (m != 0 && m != st->confMtime) {
        st->confMtime = m;
        Config nc = load_config();
        WFG_LOGI("conf reload: enabled=%d mult=%d model=%d flow=%.2f (was enabled=%d)",
                 nc.enabled, nc.multiplier, nc.model, nc.flowScale, st->cfg.enabled ? 1 : 0);
        st->cfg = nc;
        st->fg.configure(nc);
        st->resetPrev = true;   // toggle/model/flow change -> recapture prev next frame
    }

    // Trace first present + every enabled-state transition so the in-game toggle is
    // visible in logcat. Generation/insertion is Phase 3 — passthrough for now.
    static unsigned long long presents = 0;
    static int lastEnabled = -1;
    int en = st->cfg.enabled ? 1 : 0;
    if (presents == 0 || en != lastEnabled) {
        WFG_LOGI("present #%llu enabled=%d mult=%d fg.valid=%d (passthrough — insertion is Phase 3)",
                 presents, en, st->cfg.multiplier, st->fg.valid());
        lastEnabled = en;
    }
    ++presents;

    // ── Phase 3a: synthesise the interpolated frame and blit it over the current
    // swapchain image (single present). Proves the synth on real frames on-screen;
    // true extra-frame insertion (2x) is Phase 3b. ────────────────────────────
    bool doGen = st->cfg.enabled && st->cfg.multiplier >= 2 && st->fg.valid()
                 && pPresentInfo->swapchainCount == 1;
    if (doGen) {
        VkSwapchainKHR sc = pPresentInfo->pSwapchains[0];
        uint32_t idx = pPresentInfo->pImageIndices[0];
        auto sit = g_swap.find(sc);
        auto git = g_gen.find(sc);
        if (sit != g_swap.end() && sit->second.insertReady && git != g_gen.end()
            && idx < sit->second.images.size()) {
            SwapState& s = sit->second;
            GenTarget& gt = git->second;
            if (st->resetPrev) { s.prevValid = false; st->resetPrev = false; }  // fresh prev on toggle
            const DeviceDispatch& dd = st->dd;
            VkDevice dev = st->device;
            FrameCtx& fc = s.ring[s.ringIdx];
            s.ringIdx = (s.ringIdx + 1) % s.ring.size();
            if (fc.submitted) { dd.WaitForFences(dev, 1, &fc.fence, VK_TRUE, UINT64_MAX); dd.ResetFences(dev, 1, &fc.fence); fc.submitted = false; }

            VkImage currImg = s.images[idx];
            VkImageView currView = s.views[idx];
            const VkImageLayout SR = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const VkImageLayout PS = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            const VkImageLayout TS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkImageLayout TD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            const auto ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            const auto CS  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            const auto TR  = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; cp.dstSubresource = cp.srcSubresource;
            cp.extent = {s.extent.width, s.extent.height, 1};

            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            dd.BeginCommandBuffer(fc.cmd, &bi);

            auto submitOne = [&](VkSemaphore sig) {
                std::vector<VkPipelineStageFlags> ws(pPresentInfo->waitSemaphoreCount, ALL);
                VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                su.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
                su.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                su.pWaitDstStageMask = ws.empty() ? nullptr : ws.data();
                su.commandBufferCount = 1; su.pCommandBuffers = &fc.cmd;
                su.signalSemaphoreCount = 1; su.pSignalSemaphores = &sig;
                dd.QueueSubmit(st->queue, 1, &su, fc.fence); fc.submitted = true;
            };
            auto presentOne = [&](VkSemaphore wait, uint32_t image) {
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
                pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &image;
                return st->dd.QueuePresentKHR(queue, &pi);
            };

            if (!s.prevValid) {
                // first FG frame: capture curr -> prev, present curr unchanged
                imgBarrier(dd, fc.cmd, currImg, PS, TS, 0, VK_ACCESS_TRANSFER_READ_BIT, ALL, TR);
                imgBarrier(dd, fc.cmd, s.prevImg, VK_IMAGE_LAYOUT_UNDEFINED, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, TR);
                dd.CmdCopyImage(fc.cmd, currImg, TS, s.prevImg, TD, 1, &cp);
                imgBarrier(dd, fc.cmd, s.prevImg, TD, SR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
                imgBarrier(dd, fc.cmd, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL);
                dd.EndCommandBuffer(fc.cmd);
                submitOne(fc.currSem);
                s.prevValid = true;
                if (presents < 5) WFG_LOGI("prev captured (first FG frame)");
                return presentOne(fc.currSem, idx);
            }

            // ── generate the in-between frame; try to insert it as an EXTRA present (2x) ──
            uint32_t spareIdx = 0;
            VkResult ar = dd.AcquireNextImageKHR(dev, sc, 2000000ull, fc.acquireSem, VK_NULL_HANDLE, &spareIdx);
            bool insert = (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) && spareIdx < s.images.size();
            // Diagnostic: 2x insert vs 3a blit-over fallback. If fallback climbs during
            // motion, the spare-image acquire is starving -> every frame becomes the soft
            // interpolated one -> blur (which a bg/fg swapchain recreate would reset).
            { static unsigned long long nIns = 0, nFall = 0;
              if (insert) ++nIns; else ++nFall;
              if (((nIns + nFall) % 300ull) == 0)
                  WFG_LOGI("present path: insert=%llu fallback=%llu (last acquire=%d)", nIns, nFall, (int)ar); }

            imgBarrier(dd, fc.cmd, currImg, PS, SR, 0, VK_ACCESS_SHADER_READ_BIT, ALL, CS);
            imgBarrier(dd, fc.cmd, gt.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, CS);
            // alpha = temporal position of the generated frame between prev (0) and curr (1).
            // 0.5 = perfect midpoint but reads as maximum "half-and-half" ghost on HUD/text.
            // Biasing to 0.35 (closer to prev) reduces the visible double-exposure at the
            // cost of some pacing accuracy; the synth crossfade fallback also blends less
            // aggressively. Iterative bring-up knob — sits alongside the swapchain+1 fix.
            st->fg.record(fc.cmd, s.prevView, currView, gt.view, 0.35f);              // synth -> gen (rgba8)
            imgBarrier(dd, fc.cmd, gt.img, VK_IMAGE_LAYOUT_GENERAL, TS, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, CS, TR);
            imgBarrier(dd, fc.cmd, currImg, SR, TS, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, CS, TR);
            imgBarrier(dd, fc.cmd, s.prevImg, SR, TD, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, CS, TR);
            dd.CmdCopyImage(fc.cmd, currImg, TS, s.prevImg, TD, 1, &cp);              // real curr -> prev (next frame)
            imgBarrier(dd, fc.cmd, s.prevImg, TD, SR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
            VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bl.dstSubresource = bl.srcSubresource;
            bl.srcOffsets[1] = {(int)s.extent.width,(int)s.extent.height,1}; bl.dstOffsets[1] = bl.srcOffsets[1];

            if (insert) {
                imgBarrier(dd, fc.cmd, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL); // real frame -> present, untouched
                VkImage spareImg = s.images[spareIdx];
                imgBarrier(dd, fc.cmd, spareImg, VK_IMAGE_LAYOUT_UNDEFINED, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, TR);
                dd.CmdBlitImage(fc.cmd, gt.img, TS, spareImg, TD, 1, &bl, VK_FILTER_NEAREST);      // generated -> spare image
                imgBarrier(dd, fc.cmd, spareImg, TD, PS, VK_ACCESS_TRANSFER_WRITE_BIT, 0, TR, ALL);
                dd.EndCommandBuffer(fc.cmd);
                // one submit, waits app render-finished + acquire, signals gen + real present sems
                std::vector<VkSemaphore> waits(pPresentInfo->pWaitSemaphores, pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);
                waits.push_back(fc.acquireSem);
                std::vector<VkPipelineStageFlags> ws(waits.size(), ALL);
                VkSemaphore sigs[2] = { fc.genSem, fc.currSem };
                VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                su.waitSemaphoreCount = (uint32_t)waits.size(); su.pWaitSemaphores = waits.data(); su.pWaitDstStageMask = ws.data();
                su.commandBufferCount = 1; su.pCommandBuffers = &fc.cmd;
                su.signalSemaphoreCount = 2; su.pSignalSemaphores = sigs;
                dd.QueueSubmit(st->queue, 1, &su, fc.fence); fc.submitted = true;
                if (presents < 6) WFG_LOGI("2x insert live: spare=%u real=%u", spareIdx, idx);
                presentOne(fc.genSem, spareIdx);            // generated (in-between) frame first
                return presentOne(fc.currSem, idx);         // then the real frame
            } else {
                // no spare available -> fall back to 3a (blit generated over curr, single present)
                imgBarrier(dd, fc.cmd, currImg, TS, TD, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, TR, TR);
                dd.CmdBlitImage(fc.cmd, gt.img, TS, currImg, TD, 1, &bl, VK_FILTER_NEAREST);
                imgBarrier(dd, fc.cmd, currImg, TD, PS, VK_ACCESS_TRANSFER_WRITE_BIT, 0, TR, ALL);
                dd.EndCommandBuffer(fc.cmd);
                submitOne(fc.currSem);
                return presentOne(fc.currSem, idx);
            }
        }
    }
    return st->dd.QueuePresentKHR(queue, pPresentInfo);
}

// ── proc addr + negotiation ──────────────────────────────────────────────────
#define INTERCEPT(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)&winfg_##name;

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL winfg_GetDeviceProcAddr(VkDevice device, const char* pName) {
    INTERCEPT(CreateSwapchainKHR) INTERCEPT(DestroySwapchainKHR) INTERCEPT(QueuePresentKHR)
    INTERCEPT(DestroyDevice)
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_dev.find(dispatch_key(device));
    if (it == g_dev.end() || !it->second.dd.GetDeviceProcAddr) return nullptr;
    return it->second.dd.GetDeviceProcAddr(device, pName);
}

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL winfg_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    INTERCEPT(GetInstanceProcAddr) INTERCEPT(CreateInstance) INTERCEPT(DestroyInstance)
    INTERCEPT(CreateDevice) INTERCEPT(GetDeviceProcAddr)
    INTERCEPT(CreateSwapchainKHR) INTERCEPT(DestroySwapchainKHR) INTERCEPT(QueuePresentKHR)
    if (instance == VK_NULL_HANDLE) return nullptr;
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_inst.find(dispatch_key(instance));
    if (it == g_inst.end() || !it->second.GetInstanceProcAddr) return nullptr;
    return it->second.GetInstanceProcAddr(instance, pName);
}

extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = winfg_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = winfg_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

// Android/loader may also look up these unprefixed exports.
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char* n) { return winfg_GetInstanceProcAddr(i, n); }
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char* n) { return winfg_GetDeviceProcAddr(d, n); }
