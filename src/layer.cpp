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
    VkQueue queue = VK_NULL_HANDLE;         // graphics/present queue (app's) — sync-path submits + present-side transfers
    uint32_t queueFamily = 0;               // == gfxFamily
    FrameGen fg;
    Config cfg;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool fgInited = false;
    std::string confPath;          // guest conf.toml, watched for live hot-reload
    long long confMtime = 0;
    bool resetPrev = false;        // set on any conf change -> recapture prev frame

    // ── async compute (separate queue reserved at vkCreateDevice) ──────────────
    uint32_t gfxFamily = 0;                 // family the app renders/presents on
    VkQueue  computeQueue = VK_NULL_HANDLE; // reserved async-compute queue (VK_NULL if none)
    uint32_t computeFamily = 0;
    uint32_t computeQueueIndex = 0;
    bool     asyncAvail = false;            // a spare compute queue was obtained
    int      asyncStrategy = 0;             // 0 none, 1 dedicated compute family, 2 second queue in gfx+compute family
    bool     concurrentSharing = false;     // computeFamily != gfxFamily -> owned shared images must be CONCURRENT
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

// ── async-compute frame-insertion (deferred-gen pipeline) ────────────────────
// The synth runs on a separate compute queue so it can overlap the game's NEXT
// frame's graphics work. To keep the graphics/present queue from ever stalling on
// the current synth (which would defeat the overlap), the generated frame is
// deferred by ONE real frame: at cycle i we START synth(i-1,i) on the compute
// queue and PRESENT the gen frame computed last cycle (already done). Real frames
// are presented immediately with no added latency; only the interpolated frame is
// one cycle "stale" (smooth, no judder). The compute queue only ever touches our
// OWNED images (never the swapchain), so no swapchain sharing change is needed;
// the two owned image classes that cross the graphics<->compute family boundary
// (srcImg copies, outImg gen targets) are created CONCURRENT to avoid QFOT.
struct AsyncSlot {
    VkCommandBuffer cmdG = VK_NULL_HANDLE;    // graphics: copy curr, blit gen->spare, present transitions
    VkCommandBuffer cmdC = VK_NULL_HANDLE;    // compute: synth + prev update
    VkSemaphore copyForCurr = VK_NULL_HANDLE; // cmdG -> cmdC (curr copy ready to sample)
    VkSemaphore genReady  = VK_NULL_HANDLE;   // cmdG -> present(gen)
    VkSemaphore realReady = VK_NULL_HANDLE;   // cmdG -> present(real)
    VkSemaphore acquireSem = VK_NULL_HANDLE;  // acquire(spare) -> cmdG
    VkFence gFence = VK_NULL_HANDLE;
    VkFence cFence = VK_NULL_HANDLE;
};
struct AsyncCtx {
    bool ready = false;
    VkCommandPool gPool = VK_NULL_HANDLE;     // gfxFamily pool
    VkCommandPool cPool = VK_NULL_HANDLE;     // computeFamily pool
    // ping-pong owned images (2). srcImg[s] = owned copy of a real frame (CONCURRENT);
    // outImg[s] = the gen target the synth writes (CONCURRENT). computeDone[s] ties a
    // finished outImg[s] to the next cycle's graphics blit.
    VkImage srcImg[2]{}; VkDeviceMemory srcMem[2]{}; VkImageView srcView[2]{};
    VkImage outImg[2]{}; VkDeviceMemory outMem[2]{}; VkImageView outView[2]{};
    VkSemaphore computeDone[2]{};
    bool outValid[2] = {false, false};
    // prevOwned — read+written ONLY on the compute queue, so no cross-family concern.
    VkImage prevImg = VK_NULL_HANDLE; VkDeviceMemory prevMem = VK_NULL_HANDLE; VkImageView prevView = VK_NULL_HANDLE;
    bool prevValid = false;
    static const int kSlots = 3;
    AsyncSlot slot[kSlots];
    uint64_t cycle = 0;                        // increments per async present
    int warmup = 0;                            // cycles since (re)start
    VkFence prevGFence = VK_NULL_HANDLE;       // last cycle's fences — waited before reusing scratch/desc
    VkFence prevCFence = VK_NULL_HANDLE;
};

struct SwapState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format{}; VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;

    // ── Phase 3 frame-insertion resources (synchronous path) ─────────────────
    VkImage       prevImg  = VK_NULL_HANDLE;  // owned copy of the last real frame
    VkDeviceMemory prevMem = VK_NULL_HANDLE;
    VkImageView   prevView = VK_NULL_HANDLE;
    bool          prevValid = false;
    VkCommandPool cmdPool  = VK_NULL_HANDLE;
    std::vector<FrameCtx> ring;               // small ring so we don't stall every frame
    uint32_t      ringIdx  = 0;
    bool          insertReady = false;

    AsyncCtx      async;                       // async-compute path resources (when a compute queue was reserved)
};

// Create an owned 2D color image + view (used for the prev-frame copy and the
// async realCopy/gen images). When shareFamilies names two DISTINCT families the
// image is created VK_SHARING_MODE_CONCURRENT so it can be written on one queue
// family and read on the other with no queue-family-ownership-transfer barriers
// (the standard FG-layer choice over QFOT — see the async notes in this file).
static bool makeOwnedImage(const DeviceDispatch& dd, const VkPhysicalDeviceMemoryProperties& mp,
                           VkDevice dev, VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage,
                           VkImage& img, VkDeviceMemory& mem, VkImageView& view,
                           const uint32_t* shareFamilies = nullptr, uint32_t shareCount = 0) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {ext.width, ext.height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (shareCount >= 2 && shareFamilies && shareFamilies[0] != shareFamilies[1]) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = shareCount; ci.pQueueFamilyIndices = shareFamilies;
    }
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

// Build the async-compute resources for one swapchain (owned images + two command
// pools + per-slot sync). Only called when a compute queue was reserved.
static bool initAsync(SwapState& s, DeviceState& ds) {
    AsyncCtx& a = s.async;
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    const uint32_t fams[2] = { ds.gfxFamily, ds.computeFamily };
    const uint32_t* share = ds.concurrentSharing ? fams : nullptr;
    const uint32_t shareN = ds.concurrentSharing ? 2u : 0u;

    for (int i = 0; i < 2; ++i) {
        if (!makeOwnedImage(dd, ds.memProps, dev, s.extent, s.format,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                a.srcImg[i], a.srcMem[i], a.srcView[i], share, shareN)) { WFG_LOGE("initAsync srcImg failed"); return false; }
        if (!makeOwnedImage(dd, ds.memProps, dev, s.extent, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                a.outImg[i], a.outMem[i], a.outView[i], share, shareN)) { WFG_LOGE("initAsync outImg failed"); return false; }
    }
    // prevOwned lives entirely on the compute queue -> EXCLUSIVE is correct.
    if (!makeOwnedImage(dd, ds.memProps, dev, s.extent, s.format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            a.prevImg, a.prevMem, a.prevView)) { WFG_LOGE("initAsync prevImg failed"); return false; }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ds.gfxFamily;
    if (dd.CreateCommandPool(dev, &pci, nullptr, &a.gPool) != VK_SUCCESS) { WFG_LOGE("initAsync gPool failed"); return false; }
    pci.queueFamilyIndex = ds.computeFamily;
    if (dd.CreateCommandPool(dev, &pci, nullptr, &a.cPool) != VK_SUCCESS) { WFG_LOGE("initAsync cPool failed"); return false; }

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < 2; ++i) dd.CreateSemaphore(dev, &si, nullptr, &a.computeDone[i]);
    for (int k = 0; k < AsyncCtx::kSlots; ++k) {
        AsyncSlot& sl = a.slot[k];
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1;
        ci.commandPool = a.gPool; dd.AllocateCommandBuffers(dev, &ci, &sl.cmdG);
        ci.commandPool = a.cPool; dd.AllocateCommandBuffers(dev, &ci, &sl.cmdC);
        dd.CreateSemaphore(dev, &si, nullptr, &sl.copyForCurr);
        dd.CreateSemaphore(dev, &si, nullptr, &sl.genReady);
        dd.CreateSemaphore(dev, &si, nullptr, &sl.realReady);
        dd.CreateSemaphore(dev, &si, nullptr, &sl.acquireSem);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        dd.CreateFence(dev, &fi, nullptr, &sl.gFence);
        dd.CreateFence(dev, &fi, nullptr, &sl.cFence);
    }
    a.cycle = 0; a.warmup = 0; a.prevValid = false;
    a.outValid[0] = a.outValid[1] = false;
    a.prevGFence = VK_NULL_HANDLE; a.prevCFence = VK_NULL_HANDLE;
    a.ready = true;
    WFG_LOGI("async resources ready (%ux%u slots=%d concurrent=%d)", s.extent.width, s.extent.height, AsyncCtx::kSlots, ds.concurrentSharing ? 1 : 0);
    return true;
}

static void destroyAsync(SwapState& s, DeviceState& ds) {
    AsyncCtx& a = s.async;
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    if (!a.ready && !a.gPool && !a.cPool) return;
    if (dev && dd.DeviceWaitIdle) dd.DeviceWaitIdle(dev);
    for (int k = 0; k < AsyncCtx::kSlots; ++k) {
        AsyncSlot& sl = a.slot[k];
        if (sl.copyForCurr) dd.DestroySemaphore(dev, sl.copyForCurr, nullptr);
        if (sl.genReady)   dd.DestroySemaphore(dev, sl.genReady, nullptr);
        if (sl.realReady)  dd.DestroySemaphore(dev, sl.realReady, nullptr);
        if (sl.acquireSem) dd.DestroySemaphore(dev, sl.acquireSem, nullptr);
        if (sl.gFence) dd.DestroyFence(dev, sl.gFence, nullptr);
        if (sl.cFence) dd.DestroyFence(dev, sl.cFence, nullptr);
        sl = AsyncSlot{};
    }
    for (int i = 0; i < 2; ++i) {
        if (a.computeDone[i]) dd.DestroySemaphore(dev, a.computeDone[i], nullptr);
        if (a.srcView[i]) dd.DestroyImageView(dev, a.srcView[i], nullptr);
        if (a.srcImg[i])  dd.DestroyImage(dev, a.srcImg[i], nullptr);
        if (a.srcMem[i])  dd.FreeMemory(dev, a.srcMem[i], nullptr);
        if (a.outView[i]) dd.DestroyImageView(dev, a.outView[i], nullptr);
        if (a.outImg[i])  dd.DestroyImage(dev, a.outImg[i], nullptr);
        if (a.outMem[i])  dd.FreeMemory(dev, a.outMem[i], nullptr);
    }
    if (a.prevView) dd.DestroyImageView(dev, a.prevView, nullptr);
    if (a.prevImg)  dd.DestroyImage(dev, a.prevImg, nullptr);
    if (a.prevMem)  dd.FreeMemory(dev, a.prevMem, nullptr);
    if (a.gPool) dd.DestroyCommandPool(dev, a.gPool, nullptr);
    if (a.cPool) dd.DestroyCommandPool(dev, a.cPool, nullptr);
    a = AsyncCtx{};
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

    // ── async-compute FEASIBILITY: enumerate queue families and, if a spare
    // compute-capable queue exists, rewrite pQueueCreateInfos to reserve it BEFORE
    // the down-chain create. Strategy (a) a DEDICATED async-compute family (COMPUTE
    // without GRAPHICS); else (b) a 2nd queue in the graphics+compute family if the
    // family has spare queueCount. If neither is possible (e.g. Turnip/Adreno often
    // exposes ONE family with queueCount==1) we leave the app's request untouched
    // and fall back to the synchronous path — no regression. ────────────────────
    Config cfg = load_config();
    InstanceDispatch instDisp{};
    {
        std::lock_guard<std::mutex> lk0(g_lock);
        auto iit = g_inst.find(dispatch_key(phys));
        if (iit != g_inst.end()) instDisp = iit->second;
    }
    uint32_t qfc = 0;
    std::vector<VkQueueFamilyProperties> qf;
    if (instDisp.GetPhysicalDeviceQueueFamilyProperties) {
        instDisp.GetPhysicalDeviceQueueFamilyProperties(phys, &qfc, nullptr);
        qf.resize(qfc);
        instDisp.GetPhysicalDeviceQueueFamilyProperties(phys, &qfc, qf.data());
    }
    for (uint32_t f = 0; f < qfc; ++f)
        WFG_LOGI("queue family %u: flags=0x%x count=%u (G=%d C=%d T=%d)", f, qf[f].queueFlags, qf[f].queueCount,
                 (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? 1 : 0,
                 (qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT) ? 1 : 0,
                 (qf[f].queueFlags & VK_QUEUE_TRANSFER_BIT) ? 1 : 0);

    // gfxFamily = first REQUESTED family with graphics (so index 0 is guaranteed to exist)
    uint32_t gfxFamily = ~0u;
    for (uint32_t k = 0; k < pCreateInfo->queueCreateInfoCount; ++k) {
        uint32_t f = pCreateInfo->pQueueCreateInfos[k].queueFamilyIndex;
        if (f < qfc && (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { gfxFamily = f; break; }
    }
    if (gfxFamily == ~0u) for (uint32_t f = 0; f < qfc; ++f) if (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxFamily = f; break; }
    if (gfxFamily == ~0u) gfxFamily = 0;

    int strategy = 0; uint32_t cFam = 0, cIdx = 0;
    std::vector<VkDeviceQueueCreateInfo> qcis(pCreateInfo->pQueueCreateInfos,
                                              pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
    std::vector<std::vector<float>> prioStore; prioStore.reserve(qcis.size() + 2);
    auto findReq = [&](uint32_t fam) -> int {
        for (size_t k = 0; k < qcis.size(); ++k) if (qcis[k].queueFamilyIndex == fam) return (int)k; return -1; };
    auto bump = [&](int at) {                 // add one queue to an existing request
        uint32_t oldc = qcis[at].queueCount;
        prioStore.emplace_back(qcis[at].pQueuePriorities, qcis[at].pQueuePriorities + oldc);
        prioStore.back().push_back(1.0f);
        qcis[at].pQueuePriorities = prioStore.back().data();
        qcis[at].queueCount = oldc + 1;
        return oldc; };
    const bool wantAsync = (cfg.asyncMode != 2) && qfc > 0;
    if (wantAsync) {
        for (uint32_t f = 0; f < qfc && !strategy; ++f) {   // (a) dedicated compute family
            if ((qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                int at = findReq(f);
                if (at < 0) {
                    prioStore.push_back({1.0f});
                    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                    q.queueFamilyIndex = f; q.queueCount = 1; q.pQueuePriorities = prioStore.back().data();
                    qcis.push_back(q);
                    strategy = 1; cFam = f; cIdx = 0;
                } else if (qcis[at].queueCount < qf[f].queueCount) {
                    cIdx = bump(at); strategy = 1; cFam = f;
                }
            }
        }
        if (!strategy && gfxFamily < qfc && (qf[gfxFamily].queueFlags & VK_QUEUE_COMPUTE_BIT)) { // (b) 2nd queue, gfx family
            int at = findReq(gfxFamily);
            if (at >= 0 && qcis[at].queueCount < qf[gfxFamily].queueCount) {
                cIdx = bump(at); strategy = 2; cFam = gfxFamily;
            }
        }
    }

    VkDeviceCreateInfo ci = *pCreateInfo;
    if (strategy) { ci.queueCreateInfoCount = (uint32_t)qcis.size(); ci.pQueueCreateInfos = qcis.data(); }
    VkResult r = createDevice(phys, strategy ? &ci : pCreateInfo, pAlloc, pDevice);
    if (r != VK_SUCCESS && strategy) {
        WFG_LOGE("async: CreateDevice with reserved compute queue failed (r=%d) — retrying with app's queues (sync fallback)", (int)r);
        strategy = 0;
        r = createDevice(phys, pCreateInfo, pAlloc, pDevice);
    }
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
    st.cfg = cfg;
    st.confPath = conf_path();
    st.confMtime = stat_mtime(st.confPath);
    st.device = *pDevice;

    // graphics/present queue = gfxFamily index 0 (the family the app renders on)
    st.gfxFamily = gfxFamily; st.queueFamily = gfxFamily;
    dd.GetDeviceQueue(*pDevice, gfxFamily, 0, &st.queue);
    // reserved async-compute queue (if any)
    if (strategy) {
        dd.GetDeviceQueue(*pDevice, cFam, cIdx, &st.computeQueue);
        st.computeFamily = cFam; st.computeQueueIndex = cIdx;
        st.asyncAvail = (st.computeQueue != VK_NULL_HANDLE);
        st.asyncStrategy = strategy;
        st.concurrentSharing = (cFam != gfxFamily);
    }
    st.id->GetPhysicalDeviceMemoryProperties(phys, &st.memProps);
    g_dev[dispatch_key(*pDevice)] = std::move(st);
    auto& ds = g_dev[dispatch_key(*pDevice)];
    if (ds.asyncAvail)
        WFG_LOGI("CreateDevice ok — ASYNC engaged: strategy=%s gfxFamily=%u computeFamily=%u qIdx=%u concurrent=%d "
                 "(enable=%d model=%d mult=%d asyncMode=%d)",
                 ds.asyncStrategy == 1 ? "dedicated-compute" : "2nd-queue-gfx-family",
                 ds.gfxFamily, ds.computeFamily, ds.computeQueueIndex, ds.concurrentSharing ? 1 : 0,
                 ds.cfg.enabled, ds.cfg.model, ds.cfg.multiplier, ds.cfg.asyncMode);
    else
        WFG_LOGI("CreateDevice ok — SYNC path (no spare compute queue reserved; asyncMode=%d gfxFamily=%u families=%u) "
                 "(enable=%d model=%d mult=%d)",
                 ds.cfg.asyncMode, ds.gfxFamily, qfc, ds.cfg.enabled, ds.cfg.model, ds.cfg.multiplier);
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
        // Build the async-compute resources only when a compute queue was reserved
        // and async isn't force-off; otherwise the synchronous insert path is used.
        if (st.asyncAvail && st.cfg.asyncMode != 2) {
            if (!initAsync(s, st)) { WFG_LOGE("initAsync failed — falling back to synchronous path"); destroyAsync(s, st); }
        }
    }
    g_swap[*pSwapchain] = std::move(s);
    return VK_SUCCESS;
}

extern "C" void VKAPI_CALL winfg_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAlloc) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto& st = g_dev[dispatch_key(device)];
    auto it = g_swap.find(swapchain);
    if (it != g_swap.end()) {
        destroyAsync(it->second, st);
        destroyInsert(it->second, st);
        for (auto v : it->second.views) if (v) st.dd.DestroyImageView(device, v, nullptr);
        g_swap.erase(it);
    }
    st.dd.DestroySwapchainKHR(device, swapchain, pAlloc);
}

// ── async-compute deferred-gen present ───────────────────────────────────────
// Runs the flow+synth graph on the reserved compute queue so it overlaps the
// game's NEXT-frame graphics work. The generated frame is deferred by one real
// frame (present the gen computed last cycle, start this cycle's gen now) so the
// graphics/present queue never stalls on the current synth. Real frames are
// presented immediately (no added latency); only the interpolated frame is one
// cycle stale (smooth, no judder). Returns the real frame's present result.
static VkResult asyncGenPresent(DeviceState& st, SwapState& s, VkQueue queue,
                                const VkPresentInfoKHR* pPresentInfo, uint32_t idx) {
    AsyncCtx& a = s.async;
    const DeviceDispatch& dd = st.dd; VkDevice dev = st.device;

    if (st.resetPrev) { a.warmup = 0; a.prevValid = false; a.outValid[0] = a.outValid[1] = false; st.resetPrev = false; }

    const uint64_t cyc = a.cycle;
    const int imgSlot  = (int)(cyc & 1);
    const int prevSlot = 1 - imgSlot;
    const int r        = (int)(cyc % AsyncCtx::kSlots);
    AsyncSlot& sl = a.slot[r];

    // Reclaim: waiting last cycle's fences guarantees (queue submission order) that
    // ALL earlier cmdG/cmdC finished -> the ping-pong images + g_scratch descriptor
    // sets are free to reuse and every binary semaphore signalled earlier has been
    // consumed. This paces the pipeline to depth 1 (synth_i overlaps render_{i+1}).
    if (a.prevGFence) dd.WaitForFences(dev, 1, &a.prevGFence, VK_TRUE, UINT64_MAX);
    if (a.prevCFence) dd.WaitForFences(dev, 1, &a.prevCFence, VK_TRUE, UINT64_MAX);
    dd.ResetFences(dev, 1, &sl.gFence);
    dd.ResetFences(dev, 1, &sl.cFence);

    const DeviceDispatch& D = dd;   // shorthand
    const VkImageLayout SR = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageLayout PS = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkImageLayout TS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout TD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageLayout GENl = VK_IMAGE_LAYOUT_GENERAL;
    const VkImageLayout UND = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const auto CS  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const auto TR  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const auto TOP = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    VkImage currImg = s.images[idx];
    VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; cp.dstSubresource = cp.srcSubresource;
    cp.extent = {s.extent.width, s.extent.height, 1};

    const bool doSynth        = a.prevValid;              // false on the very first (seed) cycle
    const bool waitPrevCompute= a.outValid[prevSlot];    // a finished gen from last cycle is pending
    // Acquire a spare swapchain image for the deferred gen frame (only if we have one).
    uint32_t spareIdx = 0; bool haveSpare = false;
    if (waitPrevCompute) {
        VkResult acq = D.AcquireNextImageKHR(dev, s.swapchain, 2000000ull, sl.acquireSem, VK_NULL_HANDLE, &spareIdx);
        haveSpare = (acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR) && spareIdx < s.images.size();
    }
    const bool doGenPresent = waitPrevCompute && haveSpare;

    { static unsigned long long nIns=0, nFall=0, nCyc=0;
      if (doGenPresent) ++nIns; else if (waitPrevCompute) ++nFall;
      if ((++nCyc % 300ull) == 0)
          WFG_LOGI("async present: overlap live — genInsert=%llu spareStarve=%llu (strategy=%d concurrent=%d)",
                   nIns, nFall, st.asyncStrategy, st.concurrentSharing ? 1 : 0); }

    // ── record graphics cmd: copy curr->src, restore curr, (optional) blit gen->spare ──
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D.BeginCommandBuffer(sl.cmdG, &bi);
    imgBarrier(D, sl.cmdG, currImg, PS, TS, 0, VK_ACCESS_TRANSFER_READ_BIT, ALL, TR);
    imgBarrier(D, sl.cmdG, a.srcImg[imgSlot], UND, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, TOP, TR);
    D.CmdCopyImage(sl.cmdG, currImg, TS, a.srcImg[imgSlot], TD, 1, &cp);
    imgBarrier(D, sl.cmdG, a.srcImg[imgSlot], TD, SR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
    imgBarrier(D, sl.cmdG, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL);
    if (doGenPresent) {
        VkImage spareImg = s.images[spareIdx];
        imgBarrier(D, sl.cmdG, a.outImg[prevSlot], GENl, TS, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, CS, TR);
        imgBarrier(D, sl.cmdG, spareImg, UND, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, TR);
        VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bl.dstSubresource = bl.srcSubresource;
        bl.srcOffsets[1] = {(int)s.extent.width,(int)s.extent.height,1}; bl.dstOffsets[1] = bl.srcOffsets[1];
        D.CmdBlitImage(sl.cmdG, a.outImg[prevSlot], TS, spareImg, TD, 1, &bl, VK_FILTER_NEAREST);
        imgBarrier(D, sl.cmdG, spareImg, TD, PS, VK_ACCESS_TRANSFER_WRITE_BIT, 0, TR, ALL);
    }
    D.EndCommandBuffer(sl.cmdG);

    // ── record compute cmd: synth (prev,curr)->out, then curr->prev for next cycle ──
    D.BeginCommandBuffer(sl.cmdC, &bi);
    if (doSynth) {
        imgBarrier(D, sl.cmdC, a.outImg[imgSlot], UND, GENl, 0, VK_ACCESS_SHADER_WRITE_BIT, TOP, CS);
        st.fg.record(sl.cmdC, a.prevView, a.srcView[imgSlot], a.outView[imgSlot], 0.35f);
        imgBarrier(D, sl.cmdC, a.srcImg[imgSlot], SR, TS, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, CS, TR);
        imgBarrier(D, sl.cmdC, a.prevImg, SR, TD, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, CS, TR);
        D.CmdCopyImage(sl.cmdC, a.srcImg[imgSlot], TS, a.prevImg, TD, 1, &cp);
        imgBarrier(D, sl.cmdC, a.prevImg, TD, SR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
    } else {
        // seed cycle: no prev yet -> just capture curr into prevOwned
        imgBarrier(D, sl.cmdC, a.srcImg[imgSlot], SR, TS, 0, VK_ACCESS_TRANSFER_READ_BIT, ALL, TR);
        imgBarrier(D, sl.cmdC, a.prevImg, UND, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, TOP, TR);
        D.CmdCopyImage(sl.cmdC, a.srcImg[imgSlot], TS, a.prevImg, TD, 1, &cp);
        imgBarrier(D, sl.cmdC, a.prevImg, TD, SR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
    }
    D.EndCommandBuffer(sl.cmdC);

    // ── submit graphics cmd (signals copyForCurr for compute + present-ready sems) ──
    {
        std::vector<VkSemaphore> waits(pPresentInfo->pWaitSemaphores,
                                       pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);
        if (waitPrevCompute) waits.push_back(a.computeDone[prevSlot]);   // gen from last cycle is visible
        if (doGenPresent)    waits.push_back(sl.acquireSem);
        std::vector<VkPipelineStageFlags> ws(waits.size(), ALL);
        VkSemaphore sigs[3]; uint32_t nsig = 0;
        sigs[nsig++] = sl.copyForCurr; sigs[nsig++] = sl.realReady;
        if (doGenPresent) sigs[nsig++] = sl.genReady;
        VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        su.waitSemaphoreCount = (uint32_t)waits.size();
        su.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
        su.pWaitDstStageMask = waits.empty() ? nullptr : ws.data();
        su.commandBufferCount = 1; su.pCommandBuffers = &sl.cmdG;
        su.signalSemaphoreCount = nsig; su.pSignalSemaphores = sigs;
        D.QueueSubmit(st.queue, 1, &su, sl.gFence);
    }
    // ── submit compute cmd on the ASYNC queue (overlaps the game's next render) ──
    {
        VkPipelineStageFlags w = ALL;
        VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        su.waitSemaphoreCount = 1; su.pWaitSemaphores = &sl.copyForCurr; su.pWaitDstStageMask = &w;
        su.commandBufferCount = 1; su.pCommandBuffers = &sl.cmdC;
        VkSemaphore csig = a.computeDone[imgSlot];
        if (doSynth) { su.signalSemaphoreCount = 1; su.pSignalSemaphores = &csig; }
        D.QueueSubmit(st.computeQueue, 1, &su, sl.cFence);
    }

    // ── present: gen (deferred, in-between) first, then the real current frame ──
    auto presentOne = [&](VkSemaphore wait, uint32_t image) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
        pi.swapchainCount = 1; pi.pSwapchains = &s.swapchain; pi.pImageIndices = &image;
        return D.QueuePresentKHR(queue, &pi);
    };
    if (doGenPresent) presentOne(sl.genReady, spareIdx);
    VkResult pr = presentOne(sl.realReady, idx);

    // ── advance state ─────────────────────────────────────────────────────────
    if (doSynth)         a.outValid[imgSlot]  = true;   // computeDone[imgSlot] now pending
    if (waitPrevCompute) a.outValid[prevSlot] = false;  // its computeDone was consumed by cmdG above
    a.prevValid = true;                                 // prevOwned now holds this cycle's real frame
    a.prevGFence = sl.gFence; a.prevCFence = sl.cFence;
    if (a.warmup < 1000) ++a.warmup;
    ++a.cycle;
    return pr;
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
        // ── ASYNC path: run the synth on the reserved compute queue (overlaps the
        // game's next render). Only when a compute queue was reserved, async isn't
        // force-off, and the async resources built. Otherwise use the proven
        // synchronous path below (no regression). ─────────────────────────────
        if (st->asyncAvail && st->cfg.asyncMode != 2 && sit != g_swap.end()
            && sit->second.async.ready && idx < sit->second.images.size()) {
            static int loggedAsync = 0;
            if (!loggedAsync) { WFG_LOGI("present: ASYNC compute path ACTIVE (strategy=%d)", st->asyncStrategy); loggedAsync = 1; }
            return asyncGenPresent(*st, sit->second, queue, pPresentInfo, idx);
        }
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
                // No spare image available -> we cannot true-2x this frame.
                // Old behavior: blit the synthesized frame over curr and present
                // that (Phase 3a). Problem: on the OCCASIONAL fallback (say
                // acquire starves for ~1 frame), the display gets a warped-and-
                // wrong-looking single frame instead of the real one — reads as
                // the "frame or two of ghost/jitter" that the user reported
                // 2026-08-17. The interpolated synthesis is only trustworthy
                // when we can also show the REAL frame alongside it (the true
                // 2x insert path); shown alone, a single-frame warp with any
                // small flow error is highly visible.
                //
                // New behavior: present the real curr UNTOUCHED on fallback.
                // Compute + prev-copy already recorded above still run (needed
                // so next frame has a valid prev). We just skip the blit-over.
                // Net effect of a fallback: one plain real frame instead of one
                // ghosted frame. Invisible to the user; the 2x FPS boost only
                // pauses for that single present.
                imgBarrier(dd, fc.cmd, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL);
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
