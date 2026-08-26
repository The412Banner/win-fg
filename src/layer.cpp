// win-fg — Vulkan implicit layer entry points and swapchain/present interception.
// Structure follows the public Khronos loader-layer interface and the renderdoc
// layer guide (Apache-2.0 / CC-BY learning refs). No code from bionic-fg / lsfg
// / GameScope. The frame-gen compute is win-fg's own (see framegen.*).
#include "vk_dispatch.hpp"
#include "framegen.hpp"
#include "config.hpp"
#include "capture.hpp"
#include "log.hpp"
#include <vulkan/vk_layer.h>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <time.h>
#include <cerrno>

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
    // ── EVEN-CADENCE FRAME PACING state (see cfg.pacing) ──────────────────────
    // paceDtNs  : EMA of the guest's real-frame interval (ns). 0 = not warm yet.
    // paceLastReal : monotonic ns of the last REAL present (scheduling anchor).
    // paceLastEntry: monotonic ns of the previous present-hook ENTRY (dt sample).
    // paceLastWaitNs: total sleep WE injected in the previous hook, subtracted
    //   from the next entry interval so the dt estimate tracks the GUEST's own
    //   cadence (can shrink when base FPS rises) rather than the paced cadence.
    double   paceDtNs      = 0.0;
    uint64_t paceLastReal  = 0;
    uint64_t paceLastEntry = 0;
    uint64_t paceLastWaitNs = 0;
    uint64_t paceLate      = 0;    // count of pacing-skip (late) events
    uint64_t paceInserts   = 0;    // count of paced inserts (periodic summary)
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
    bool heldSpare = false;  // this slot presented a GENERATED spare image (still in
                             // flight to the host compositor); drives the pool-headroom
                             // guard's estimate of how many images win-fg holds.
};

struct SwapState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format{}; VkExtent2D extent{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;  // pacing note: FIFO self-paces
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    uint32_t appMinImages = 0;   // the app's ORIGINAL minImageCount (its own working
                                 // set); the pool-headroom guard reserves this many
                                 // images for the guest before letting FG take a spare.

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

// ── TRAINING-DATA CAPTURE (GPU side) ─────────────────────────────────────────
// Per-swapchain: a small ring of {downscaled image + host-visible readback buffer
// + cmd + fence + doneSem}. Each present blits the real curr swapchain image into
// the ring slot's downscaled image, copies it to the buffer, and signals doneSem
// (which the real present then waits on, so the app's render-finished sems are
// consumed exactly once). Readback is ASYNC: we ship the slot that's `kCapRing`
// presents old (its fence has long since signaled), so no fresh stall. The heavy
// motion/patch/QOI/manifest work runs on CaptureEngine's own thread — the present
// thread only pays a blit+copy submit and one memcpy.
struct CapSlot {
    VkImage        down    = VK_NULL_HANDLE;
    VkDeviceMemory downMem = VK_NULL_HANDLE;
    VkImageView    downView= VK_NULL_HANDLE;   // unused; makeOwnedImage returns one
    VkBuffer       buf     = VK_NULL_HANDLE;
    VkDeviceMemory bufMem  = VK_NULL_HANDLE;
    void*          bufPtr  = nullptr;
    VkCommandBuffer cmd    = VK_NULL_HANDLE;
    VkFence        fence   = VK_NULL_HANDLE;
    VkSemaphore    doneSem = VK_NULL_HANDLE;
    bool           submitted = false;
    uint64_t       srcIndex  = 0;
    uint64_t       tsNs      = 0;
};
struct CaptureState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D  srcExtent{}; VkExtent2D dstExtent{}; VkFormat srcFormat{};
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<CapSlot> ring; uint32_t ringIdx = 0;
    size_t      bufBytes = 0;
    bool        ready = false;
    bool        failed = false;
    unsigned long long captured = 0;   // presents captured (for periodic logging)
    winfg::CaptureEngine engine;       // non-movable — only ever built in place in g_cap
};
std::map<VkSwapchainKHR, CaptureState> g_cap;

static const int kCapRing = 3;

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Human names for the device-context fingerprint log (Fold8/Adreno840 vs AYANEO/750).
static const char* driverIdName(VkDriverId id) {
    switch (id) {
        case VK_DRIVER_ID_MESA_TURNIP:          return "MESA_TURNIP";
        case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: return "QUALCOMM_PROPRIETARY";
        case VK_DRIVER_ID_ARM_PROPRIETARY:      return "ARM_PROPRIETARY";
        case VK_DRIVER_ID_MESA_VENUS:           return "MESA_VENUS";
        case VK_DRIVER_ID_MESA_DOZEN:           return "MESA_DOZEN";
        default:                                return "other";
    }
}
static const char* presentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "other";
    }
}

// Aspect-preserving downscale target that never upscales; dims rounded to >=2 even.
static VkExtent2D computeDstExtent(VkExtent2D src, int targetW, int targetH) {
    double sw = src.width ? (double)targetW / src.width : 1.0;
    double sh = src.height ? (double)targetH / src.height : 1.0;
    double sc = sw < sh ? sw : sh; if (sc > 1.0) sc = 1.0;   // never upscale
    uint32_t w = (uint32_t)(src.width  * sc + 0.5);
    uint32_t h = (uint32_t)(src.height * sc + 0.5);
    if (w < 2) w = 2; if (h < 2) h = 2;
    w &= ~1u; h &= ~1u;
    return { w, h };
}

static bool makeReadbackBuffer(const DeviceDispatch& dd, const VkPhysicalDeviceMemoryProperties& mp,
                               VkDevice dev, VkDeviceSize size,
                               VkBuffer& buf, VkDeviceMemory& mem, void*& ptr) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dd.CreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; dd.GetBufferMemoryRequirements(dev, buf, &mr);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { idx = i; break; }
    if (idx == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = idx;
    if (dd.AllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    if (dd.BindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS) return false;
    return dd.MapMemory(dev, mem, 0, size, 0, &ptr) == VK_SUCCESS;
}

// Minimal JSON string escape (quotes/backslash; control chars → space).
static std::string jsonEscape(const std::string& x) {
    std::string o; o.reserve(x.size());
    for (char c : x) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n' || c == '\r' || c == '\t') o += ' ';
        else o += c;
    }
    return o;
}
static bool allDigits(const std::string& s) { if (s.empty()) return false; for (char c : s) if (c < '0' || c > '9') return false; return true; }

// Build the anonymous consent-attestation JSON (or empty ⇒ null). Sources, in
// order: env WIN_FG_CAPTURE_CONSENT (compact
// consent_version|epochMs|anonUUID|appVer|model|agreed), else consent.json in the
// capture root. No PII — the app is responsible for supplying only anonymous fields.
static std::string readConsent(const Config& cfg) {
    if (const char* e = std::getenv("WIN_FG_CAPTURE_CONSENT")) {
        std::string s = e;
        std::vector<std::string> f; size_t start = 0;
        for (;;) { size_t p = s.find('|', start);
                   f.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
                   if (p == std::string::npos) break; start = p + 1; }
        auto g = [&](size_t i){ return i < f.size() ? f[i] : std::string(); };
        bool agreed = (g(5) == "true" || g(5) == "1" || g(5) == "yes");
        std::string ts = g(1); if (!allDigits(ts)) ts = "0";
        return "{\"consent_version\":\"" + jsonEscape(g(0)) + "\",\"ts_ms\":" + ts +
               ",\"anon_uuid\":\"" + jsonEscape(g(2)) + "\",\"app_ver\":\"" + jsonEscape(g(3)) +
               "\",\"model\":\"" + jsonEscape(g(4)) + "\",\"agreed\":" + (agreed ? "true" : "false") +
               ",\"source\":\"env\"}";
    }
    std::string path = capture_root(cfg) + "/consent.json";
    if (FILE* fp = std::fopen(path.c_str(), "rb")) {
        std::string raw; char buf[1024]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) raw.append(buf, n);
        std::fclose(fp);
        for (char& c : raw) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        size_t a = raw.find_first_not_of(" "); size_t b = raw.find_last_not_of(" ");
        if (a != std::string::npos) raw = raw.substr(a, b - a + 1); else raw.clear();
        if (!raw.empty() && raw.front() == '{' && raw.back() == '}') return raw;  // trust only a JSON object
    }
    return std::string();   // null / no consent supplied
}

static void destroyCapture(CaptureState& cap, DeviceState& ds) {
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    if (dev && dd.DeviceWaitIdle) dd.DeviceWaitIdle(dev);
    // Flush any submitted-but-unshipped slots to the encoder in source order.
    if (cap.engine.running()) {
        std::vector<CapSlot*> pend;
        for (auto& slot : cap.ring) if (slot.submitted) pend.push_back(&slot);
        std::sort(pend.begin(), pend.end(), [](CapSlot* a, CapSlot* b){ return a->srcIndex < b->srcIndex; });
        for (auto* slot : pend) {
            if (dd.WaitForFences) dd.WaitForFences(dev, 1, &slot->fence, VK_TRUE, UINT64_MAX);
            cap.engine.submit((const uint8_t*)slot->bufPtr, cap.dstExtent.width, cap.dstExtent.height, slot->srcIndex, slot->tsNs);
            slot->submitted = false;
        }
    }
    cap.engine.stop();
    for (auto& slot : cap.ring) {
        if (slot.fence)    dd.DestroyFence(dev, slot.fence, nullptr);
        if (slot.doneSem)  dd.DestroySemaphore(dev, slot.doneSem, nullptr);
        if (slot.downView) dd.DestroyImageView(dev, slot.downView, nullptr);
        if (slot.down)     dd.DestroyImage(dev, slot.down, nullptr);
        if (slot.downMem)  dd.FreeMemory(dev, slot.downMem, nullptr);
        if (slot.bufMem)   dd.UnmapMemory(dev, slot.bufMem);
        if (slot.buf)      dd.DestroyBuffer(dev, slot.buf, nullptr);
        if (slot.bufMem)   dd.FreeMemory(dev, slot.bufMem, nullptr);
    }
    cap.ring.clear();
    if (cap.pool) dd.DestroyCommandPool(dev, cap.pool, nullptr);
    cap.pool = VK_NULL_HANDLE; cap.ready = false;
}

static bool initCapture(CaptureState& cap, DeviceState& ds, SwapState& s, const Config& cfg) {
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    cap.swapchain = s.swapchain; cap.srcExtent = s.extent; cap.srcFormat = s.format;
    cap.dstExtent = computeDstExtent(s.extent, cfg.captureW, cfg.captureH);
    cap.bufBytes = (size_t)cap.dstExtent.width * cap.dstExtent.height * 4;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = ds.queueFamily;
    if (dd.CreateCommandPool(dev, &pci, nullptr, &cap.pool) != VK_SUCCESS) { WFG_LOGE("capture: cmdpool failed"); return false; }
    cap.ring.resize(kCapRing);
    for (auto& slot : cap.ring) {
        if (!makeOwnedImage(dd, ds.memProps, dev, cap.dstExtent, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                slot.down, slot.downMem, slot.downView)) { WFG_LOGE("capture: down image failed"); destroyCapture(cap, ds); return false; }
        if (!makeReadbackBuffer(dd, ds.memProps, dev, cap.bufBytes, slot.buf, slot.bufMem, slot.bufPtr)) {
            WFG_LOGE("capture: readback buffer failed"); destroyCapture(cap, ds); return false; }
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = cap.pool; ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1;
        dd.AllocateCommandBuffers(dev, &ci, &slot.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        dd.CreateFence(dev, &fi, nullptr, &slot.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        dd.CreateSemaphore(dev, &si, nullptr, &slot.doneSem);
    }
    winfg::CaptureConfig cc;
    cc.mode = cfg.capMode; cc.patchSize = cfg.capPatchSize; cc.nPatches = cfg.capPatches;
    cc.motionThresh = cfg.capMotion; cc.root = capture_root(cfg);
    cc.srcW = (int)s.extent.width; cc.srcH = (int)s.extent.height;
    cc.dstW = (int)cap.dstExtent.width; cc.dstH = (int)cap.dstExtent.height;
    cc.shardCapBytes = (uint64_t)cfg.capShardMB * 1024ull * 1024ull;
    cc.consent = readConsent(cfg);
    if (!cap.engine.start(cc)) { WFG_LOGE("capture: engine start failed"); destroyCapture(cap, ds); return false; }
    cap.ready = true; cap.ringIdx = 0; cap.captured = 0;
    WFG_LOGI("capture ON dir=%s mode=%s target=%ux%u (src %ux%u) patches=%d patchsz=%d motion=%.2f shard=%dMB ring=%d consent=%s",
             cap.engine.sessionDir().c_str(), cfg.capMode == 0 ? "patch" : "frame",
             cap.dstExtent.width, cap.dstExtent.height, s.extent.width, s.extent.height,
             cfg.capPatches, cfg.capPatchSize, cfg.capMotion, cfg.capShardMB, kCapRing,
             cc.consent.empty() ? "null(FLAGGED)" : "present");
    return true;
}

// Record + submit this present's downscale/readback into the ring; ship the slot
// that is kCapRing presents old. Returns the doneSem the caller must route the
// real present's wait through (or VK_NULL_HANDLE on failure — capture disabled).
static VkSemaphore captureFrame(CaptureState& cap, DeviceState& ds, SwapState& s,
                                uint32_t idx, const VkPresentInfoKHR* pPresentInfo,
                                unsigned long long srcIndex) {
    const DeviceDispatch& dd = ds.dd; VkDevice dev = ds.device;
    CapSlot& slot = cap.ring[cap.ringIdx];
    cap.ringIdx = (cap.ringIdx + 1) % cap.ring.size();
    // The slot we are about to reuse holds a frame from kCapRing presents ago; its
    // fence is (almost surely) already signaled — ship its bytes to the encoder.
    if (slot.submitted) {
        WFG_LOGD(ds.cfg.debug, "P#%llu capture ship-old-slot fence-wait BEGIN (src=%llu)",
                 srcIndex, (unsigned long long)slot.srcIndex);
        dd.WaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        dd.ResetFences(dev, 1, &slot.fence);
        slot.submitted = false;
        WFG_LOGD(ds.cfg.debug, "P#%llu capture ship-old-slot fence-wait DONE", srcIndex);
        cap.engine.submit((const uint8_t*)slot.bufPtr, cap.dstExtent.width, cap.dstExtent.height, slot.srcIndex, slot.tsNs);
    }

    VkImage currImg = s.images[idx];
    const VkImageLayout PS = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkImageLayout TS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout TD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const auto ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const auto TR  = VK_PIPELINE_STAGE_TRANSFER_BIT;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd.BeginCommandBuffer(slot.cmd, &bi);
    imgBarrier(dd, slot.cmd, currImg, PS, TS, 0, VK_ACCESS_TRANSFER_READ_BIT, ALL, TR);
    imgBarrier(dd, slot.cmd, slot.down, VK_IMAGE_LAYOUT_UNDEFINED, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, TR);
    // Downscale blit: LINEAR filter, aspect-preserving dst. Blit maps color
    // components semantically, so a BGRA swapchain lands correct in the RGBA8 dst.
    VkImageBlit bl{}; bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1] = {(int)cap.srcExtent.width, (int)cap.srcExtent.height, 1};
    bl.dstOffsets[1] = {(int)cap.dstExtent.width, (int)cap.dstExtent.height, 1};
    dd.CmdBlitImage(slot.cmd, currImg, TS, slot.down, TD, 1, &bl, VK_FILTER_LINEAR);
    imgBarrier(dd, slot.cmd, slot.down, TD, TS, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, TR, TR);
    VkBufferImageCopy bic{}; bic.bufferOffset = 0; bic.bufferRowLength = 0; bic.bufferImageHeight = 0;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bic.imageOffset = {0,0,0};
    bic.imageExtent = {cap.dstExtent.width, cap.dstExtent.height, 1};
    dd.CmdCopyImageToBuffer(slot.cmd, slot.down, TS, slot.buf, 1, &bic);
    VkMemoryBarrier hmb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    dd.CmdPipelineBarrier(slot.cmd, TR, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hmb, 0, nullptr, 0, nullptr);
    imgBarrier(dd, slot.cmd, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL);
    dd.EndCommandBuffer(slot.cmd);

    std::vector<VkPipelineStageFlags> ws(pPresentInfo->waitSemaphoreCount, ALL);
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    su.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    su.pWaitDstStageMask = ws.empty() ? nullptr : ws.data();
    su.commandBufferCount = 1; su.pCommandBuffers = &slot.cmd;
    su.signalSemaphoreCount = 1; su.pSignalSemaphores = &slot.doneSem;
    if (dd.QueueSubmit(ds.queue, 1, &su, slot.fence) != VK_SUCCESS) {
        WFG_LOGE("capture: QueueSubmit failed — disabling capture for this swapchain");
        cap.failed = true; cap.ready = false;
        return VK_NULL_HANDLE;
    }
    slot.submitted = true; slot.srcIndex = srcIndex; slot.tsNs = now_ns();
    if ((++cap.captured % 300ull) == 0) {
        WFG_LOGI("capture rate: captured=%llu written=%llu patches=%llu skipped=%llu gaps=%llu drops=%llu bytes=%lluMB",
                 (unsigned long long)cap.captured, (unsigned long long)cap.engine.written(),
                 (unsigned long long)cap.engine.patches(), (unsigned long long)cap.engine.skipped(),
                 (unsigned long long)cap.engine.gaps(), (unsigned long long)cap.engine.drops(),
                 (unsigned long long)(cap.engine.bytes() >> 20));
    }
    return slot.doneSem;
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
    d.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gipa(*pInstance, "vkGetPhysicalDeviceProperties");
    d.GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*pInstance, "vkGetPhysicalDeviceProperties2");
    if (!d.GetPhysicalDeviceProperties2)  // 1.0 instance: fall back to the KHR alias if the ext is present
        d.GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*pInstance, "vkGetPhysicalDeviceProperties2KHR");
    d.GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
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
    L(QueueSubmit2); L(QueueSubmit2KHR);   // guest-submit diag (may be null on this device)
    L(CreateSwapchainKHR); L(DestroySwapchainKHR); L(GetSwapchainImagesKHR); L(AcquireNextImageKHR); L(QueuePresentKHR);
    L(AcquireNextImage2KHR);               // guest-acquire diag (may be null on this device)
    L(CreateImage); L(DestroyImage); L(CreateImageView); L(DestroyImageView);
    L(AllocateMemory); L(FreeMemory); L(BindImageMemory); L(GetImageMemoryRequirements);
    L(CreateBuffer); L(DestroyBuffer); L(GetBufferMemoryRequirements); L(BindBufferMemory); L(MapMemory); L(UnmapMemory);
    L(CreateSampler); L(DestroySampler);
    L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout); L(CreatePipelineLayout); L(DestroyPipelineLayout);
    L(CreateComputePipelines); L(DestroyPipeline); L(CreateDescriptorPool); L(DestroyDescriptorPool);
    L(ResetDescriptorPool);                // live perf_preset rebuild reclaims old scratch sets
    L(AllocateDescriptorSets); L(UpdateDescriptorSets);
    L(CreateCommandPool); L(DestroyCommandPool); L(AllocateCommandBuffers); L(FreeCommandBuffers);
    L(BeginCommandBuffer); L(EndCommandBuffer); L(CmdBindPipeline); L(CmdBindDescriptorSets); L(CmdDispatch);
    L(CmdPipelineBarrier); L(CmdCopyImage); L(CmdCopyImageToBuffer); L(CmdBlitImage); L(CmdClearColorImage);
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

    // ── DEVICE-CONTEXT FINGERPRINT (always logged; once per device, no per-frame
    // spam). The exact profile of the affected device — GPU, driver + driverID +
    // version, and the compute queue-family layout — so a broken Fold8/Adreno840/
    // Wrapper build can be compared against a known-good AYANEO/Adreno750. ────────
    {
        VkPhysicalDeviceProperties props{};
        if (st.id->GetPhysicalDeviceProperties) st.id->GetPhysicalDeviceProperties(phys, &props);
        const char* drvName = "?"; VkDriverId drvId = (VkDriverId)0;
        if (st.id->GetPhysicalDeviceProperties2) {
            VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &dp;
            st.id->GetPhysicalDeviceProperties2(phys, &p2);
            drvName = dp.driverName[0] ? dp.driverName : "?";
            drvId   = dp.driverID;
        }
        WFG_LOGI("DEVICE-CTX gpu=\"%s\" vendorID=0x%04x deviceID=0x%04x api=%u.%u.%u "
                 "driver=\"%s\" driverID=%d(%s) driverVersion=0x%08x computeQF=%u qfCount=%u",
                 props.deviceName, props.vendorID, props.deviceID,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 VK_API_VERSION_PATCH(props.apiVersion), drvName, (int)drvId, driverIdName(drvId),
                 props.driverVersion, st.queueFamily, qfc);
        for (uint32_t i = 0; i < qfc; ++i) {
            VkQueueFlags f = qf[i].queueFlags;
            WFG_LOGI("DEVICE-CTX  qf[%u] count=%u flags=0x%x [%s%s%s%s]", i, qf[i].queueCount, f,
                     (f & VK_QUEUE_GRAPHICS_BIT) ? "G" : "", (f & VK_QUEUE_COMPUTE_BIT) ? "C" : "",
                     (f & VK_QUEUE_TRANSFER_BIT) ? "T" : "", (f & VK_QUEUE_SPARSE_BINDING_BIT) ? "S" : "");
        }
    }

    g_dev[dispatch_key(*pDevice)] = std::move(st);
    auto& ds = g_dev[dispatch_key(*pDevice)];
    WFG_LOGI("CreateDevice ok (enable=%d model=%d mult=%d flowScale=%.2f perf_preset=%d computeQF=%u)",
             ds.cfg.enabled, ds.cfg.model, ds.cfg.multiplier, ds.cfg.flowScale, ds.cfg.perfPreset, ds.queueFamily);
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
    // Query the surface's max image count so the extra-headroom request never asks
    // for more than the driver can grant (0 = unlimited -> no clamp). Keep the full
    // caps struct around so SWAPCHAIN-CTX can log the TRUE simultaneous-acquire limit
    // (needs surfaceCaps.minImageCount, which the app-requested min does NOT reveal).
    uint32_t maxImg = 0;
    VkSurfaceCapabilitiesKHR scaps{};
    bool haveCaps = false;
    if (st.id && st.id->GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        if (st.id->GetPhysicalDeviceSurfaceCapabilitiesKHR(st.phys, pCreateInfo->surface, &scaps) == VK_SUCCESS) {
            maxImg = scaps.maxImageCount;
            haveCaps = true;
        }
    }
    // requested = app_min + 1 (spare) + extra_images headroom, clamped to the surface
    // max (0 = unlimited), never below the +1 spare. The extra images stop the guest's
    // next AcquireNextImageKHR from starving while win-fg holds a spare + a real frame
    // in flight to the AHB-backed host compositor (the Fold8/Adreno840/Turnip freeze).
    uint32_t desired = origMin + 1u + (uint32_t)st.cfg.extraImages;
    if (maxImg != 0u && desired > maxImg) desired = maxImg;
    if (desired < origMin + 1u) desired = origMin + 1u;   // always keep at least the spare
    ci.minImageCount = desired;
    VkResult r = st.dd.CreateSwapchainKHR(device, &ci, pAlloc, pSwapchain);
    if (r != VK_SUCCESS) {
        WFG_LOGI("CreateSwapchain(minImageCount=%u) rejected (r=%d), retrying with app's minImageCount=%u",
                 ci.minImageCount, (int)r, origMin);
        ci.minImageCount = origMin;
        r = st.dd.CreateSwapchainKHR(device, &ci, pAlloc, pSwapchain);
    }
    if (r != VK_SUCCESS) return r;
    SwapState s; s.swapchain = *pSwapchain; s.format = pCreateInfo->imageFormat; s.extent = pCreateInfo->imageExtent;
    s.presentMode = pCreateInfo->presentMode;   // pacing: FIFO self-paces (waits skip)
    s.appMinImages = origMin;   // reserve this many for the guest in the headroom guard
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
    // ── SWAPCHAIN-CONTEXT FINGERPRINT (always logged; once per swapchain). The
    // present-mode + image-count + usage shape drives the spare-image insert path;
    // if the +1 minImageCount fell back (driver rejected the bump) the game holds
    // more images in flight and 2x insert starves — this line says which case we
    // are in on the affected device. ────────────────────────────────────────────
    WFG_LOGI("SWAPCHAIN-CTX %ux%u fmt=%d colorSpace=%d presentMode=%d(%s) "
             "minImageCount(app=%u requested=%u) actualImages=%u usage(app=0x%x forced=0x%x) "
             "spare+1=%s extra_req=%d granted_total=%u maxImageCount=%u enabled=%d debug=%d",
             s.extent.width, s.extent.height, (int)s.format, (int)pCreateInfo->imageColorSpace,
             (int)pCreateInfo->presentMode, presentModeName(pCreateInfo->presentMode),
             origMin, ci.minImageCount, n, pCreateInfo->imageUsage, ci.imageUsage,
             (ci.minImageCount > origMin) ? "granted" : "FELL-BACK",
             st.cfg.extraImages, n, maxImg, st.cfg.enabled, st.cfg.debug);
    // REAL surface limits (from vkGetPhysicalDeviceSurfaceCapabilitiesKHR, not the
    // app's requested min). The true number of images win-fg can hold acquired at
    // once = actualImages - surfaceCaps.minImageCount + 1; the guest's next acquire
    // blocks once win-fg's held spare(s) push it past that — the Fold8/Adreno840
    // freeze. This line gives surfaceCaps.minImageCount so that limit is computable.
    if (haveCaps) {
        int simAcquire = (int)n - (int)scaps.minImageCount + 1;
        WFG_LOGI("SWAPCHAIN-CTX surfaceCaps.minImageCount=%u maxImageCount=%u "
                 "currentExtent=%ux%u supportedUsage=0x%x | actualImages=%u "
                 "sim-acquire-limit(images-surfMin+1)=%d",
                 scaps.minImageCount, scaps.maxImageCount,
                 scaps.currentExtent.width, scaps.currentExtent.height,
                 scaps.supportedUsageFlags, n, simAcquire);
    } else {
        WFG_LOGI("SWAPCHAIN-CTX surfaceCaps UNAVAILABLE "
                 "(GetPhysicalDeviceSurfaceCapabilitiesKHR missing/failed) — "
                 "cannot compute true simultaneous-acquire limit");
    }
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
    auto cit = g_cap.find(swapchain);
    if (cit != g_cap.end()) { destroyCapture(cit->second, st); g_cap.erase(cit); }
    auto it = g_swap.find(swapchain);
    if (it != g_swap.end()) {
        destroyInsert(it->second, st);
        for (auto v : it->second.views) if (v) st.dd.DestroyImageView(device, v, nullptr);
        g_swap.erase(it);
    }
    st.dd.DestroySwapchainKHR(device, swapchain, pAlloc);
}

// ── FRAME-PACING CLOCK HELPERS (even-cadence 2x present) ──────────────────────
// mono_ns(): monotonic wall clock in ns. sleep_until_ns(): precise, bounded,
// CPU-only absolute sleep on the SAME clock (no GPU wait); restarts on EINTR.
// Callers guarantee the target is bounded (≤ dt and ≤ kPaceCeilNs from now).
static inline uint64_t mono_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline void sleep_until_ns(uint64_t target_ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(target_ns / 1000000000ull);
    ts.tv_nsec = (long)  (target_ns % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}
static const double   kPaceAlpha   = 0.10;          // EMA smoothing for dt estimate
static const uint64_t kPaceCeilNs  = 20000000ull;   // absolute per-wait ceiling (20 ms)
static const uint64_t kPaceMinDtNs = 500000ull;     // reject dt samples < 0.5 ms
static const uint64_t kPaceMaxDtNs = 100000000ull;  // reject dt samples > 100 ms

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
    const bool dbg = st->cfg.debug;   // granular present-path trace gate (default off)

    // Hot-reload conf.toml so the in-game controls (enable / multiplier / model /
    // flow scale) reach the layer live. Cheap stat every present; only re-read the
    // file when its mtime actually changes.
    long long m = stat_mtime(st->confPath);
    if (m != 0 && m != st->confMtime) {
        st->confMtime = m;
        Config nc = load_config();
        WFG_LOGI("conf reload: enabled=%d mult=%d model=%d flow=%.2f perf_preset=%d (was enabled=%d preset=%d)",
                 nc.enabled, nc.multiplier, nc.model, nc.flowScale, nc.perfPreset,
                 st->cfg.enabled ? 1 : 0, st->cfg.perfPreset);
        st->cfg = nc;
        st->fg.configure(nc);   // recomputes kFlowFinest from perf_preset; live-rebuilds if it changed
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
    WFG_LOGD(dbg, "P#%llu present-enter: enabled=%d mult=%d fg.valid=%d capture=%d swapCount=%u",
             presents, en, st->cfg.multiplier, st->fg.valid() ? 1 : 0,
             st->cfg.capture ? 1 : 0, pPresentInfo->swapchainCount);

    // ── FRAME-PACING dt ESTIMATE ─────────────────────────────────────────────
    // This hook runs exactly once per REAL frame, so the entry-to-entry interval
    // measures the guest's real-frame cadence. Subtract the wait WE injected last
    // hook (paceLastWaitNs) so the estimate tracks the GUEST's own interval — it
    // can therefore SHRINK when the base FPS rises rather than pinning to the
    // paced display cadence. Outlier-rejected (pause / alt-tab / warm-up).
    // Cheap and runs regardless of enable, so dt is warm the instant FG turns on.
    const uint64_t t_entry = mono_ns();
    if (st->paceLastEntry != 0) {
        long long adj = (long long)(t_entry - st->paceLastEntry) - (long long)st->paceLastWaitNs;
        if (adj >= (long long)kPaceMinDtNs && adj <= (long long)kPaceMaxDtNs) {
            double smp = (double)adj;
            st->paceDtNs = (st->paceDtNs <= 0.0) ? smp
                         : st->paceDtNs + kPaceAlpha * (smp - st->paceDtNs);
        }
    }
    st->paceLastEntry  = t_entry;
    st->paceLastWaitNs = 0;   // accumulate THIS hook's injected wait in the insert path

    // ── TRAINING-DATA CAPTURE (dev mode) ─────────────────────────────────────
    // Default OFF ⇒ the branch is one bool test and the present path below is
    // byte-identical. When ON, capture the REAL curr swapchain image (before any
    // interpolation; the HUD is composited downstream so this frame is clean) into
    // an async readback ring, then route the real present's wait through capture's
    // doneSem so the app render-finished sems are consumed exactly once.
    const VkPresentInfoKHR* pi = pPresentInfo;
    VkPresentInfoKHR effPresent;
    VkSemaphore effWait[1];
    if (st->cfg.capture && pPresentInfo->swapchainCount == 1) {
        VkSwapchainKHR csc = pPresentInfo->pSwapchains[0];
        uint32_t cidx = pPresentInfo->pImageIndices[0];
        auto csit = g_swap.find(csc);
        if (csit != g_swap.end() && cidx < csit->second.images.size()) {
            auto& cap = g_cap[csc];
            if (!cap.ready && !cap.failed && !initCapture(cap, *st, csit->second, st->cfg))
                cap.failed = true;
            if (cap.ready) {
                WFG_LOGD(dbg, "P#%llu capture-frame BEGIN (idx=%u)", presents, cidx);
                VkSemaphore done = captureFrame(cap, *st, csit->second, cidx, pPresentInfo, presents);
                WFG_LOGD(dbg, "P#%llu capture-frame DONE (doneSem=%s)", presents,
                         done != VK_NULL_HANDLE ? "routed" : "null/failed");
                if (done != VK_NULL_HANDLE) {
                    effPresent = *pPresentInfo;
                    effWait[0] = done;
                    effPresent.pWaitSemaphores = effWait;
                    effPresent.waitSemaphoreCount = 1;
                    pi = &effPresent;   // downstream consumes doneSem, not the app sems
                }
            }
        }
    }

    // ── Phase 3a: synthesise the interpolated frame and blit it over the current
    // swapchain image (single present). Proves the synth on real frames on-screen;
    // true extra-frame insertion (2x) is Phase 3b. ────────────────────────────
    bool doGen = st->cfg.enabled && st->cfg.multiplier >= 2 && st->fg.valid()
                 && pi->swapchainCount == 1;
    if (doGen) {
        VkSwapchainKHR sc = pi->pSwapchains[0];
        uint32_t idx = pi->pImageIndices[0];
        WFG_LOGD(dbg, "P#%llu fg-path enter (image idx=%u)", presents, idx);
        auto sit = g_swap.find(sc);
        auto git = g_gen.find(sc);
        if (sit != g_swap.end() && sit->second.insertReady && git != g_gen.end()
            && idx < sit->second.images.size()) {
            SwapState& s = sit->second;
            GenTarget& gt = git->second;
            if (st->resetPrev) { s.prevValid = false; st->resetPrev = false; }  // fresh prev on toggle
            const DeviceDispatch& dd = st->dd;
            VkDevice dev = st->device;
            uint32_t fcIdx = s.ringIdx;                 // C1: this slot indexes the GM SSBO to read/reduce
            FrameCtx& fc = s.ring[fcIdx];
            s.ringIdx = (s.ringIdx + 1) % s.ring.size();
            WFG_LOGD(dbg, "P#%llu fg-generate (prevValid=%d ring-slot=%u fc.submitted=%d)",
                     presents, s.prevValid ? 1 : 0, fcIdx, fc.submitted ? 1 : 0);
            // Waiting fc.fence guarantees this slot's PREVIOUS reduce (its SSBO
            // write) has completed, so record() can safely read it back on the host.
            if (fc.submitted) {
                WFG_LOGD(dbg, "P#%llu ring-fence wait BEGIN (slot=%u, infinite timeout)", presents, fcIdx);
                dd.WaitForFences(dev, 1, &fc.fence, VK_TRUE, UINT64_MAX);
                dd.ResetFences(dev, 1, &fc.fence); fc.submitted = false;
                WFG_LOGD(dbg, "P#%llu ring-fence wait DONE", presents);
            }

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

            auto submitOne = [&](VkSemaphore sig) -> VkResult {
                std::vector<VkPipelineStageFlags> ws(pi->waitSemaphoreCount, ALL);
                VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                su.waitSemaphoreCount = pi->waitSemaphoreCount;
                su.pWaitSemaphores = pi->pWaitSemaphores;
                su.pWaitDstStageMask = ws.empty() ? nullptr : ws.data();
                su.commandBufferCount = 1; su.pCommandBuffers = &fc.cmd;
                su.signalSemaphoreCount = 1; su.pSignalSemaphores = &sig;
                VkResult sr = dd.QueueSubmit(st->queue, 1, &su, fc.fence);
                fc.submitted = (sr == VK_SUCCESS);   // only arm a fence wait we truly queued
                fc.heldSpare = false;                // this path presents no generated spare
                WFG_LOGD(dbg, "P#%llu compute-submit(single) DONE r=%d", presents, (int)sr);
                if (sr != VK_SUCCESS) WFG_LOGE("P#%llu QueueSubmit(single) FAILED r=%d", presents, (int)sr);
                return sr;
            };
            // PRESENT-ID FORWARDING (Fold8/Adreno840 wedge fix). DXVK throttles with
            // VK_KHR_present_wait: it chains a VkPresentIdKHR onto the guest's present
            // and then blocks in vkWaitForPresentKHR(id=N). If win-fg builds a fresh
            // VkPresentInfoKHR with pNext=nullptr for the REAL frame, present-id N is
            // never queued to the driver, so that wait NEVER returns → the guest
            // present/render thread wedges forever (first insert completes clean, then
            // no next present, force-close). Fix: forward the guest's original pNext
            // (present-id + timing structs) onto every REAL present; the GENERATED
            // spare present MUST pass nullptr — a generated frame may be skipped, so it
            // must never carry a present-id (mirrors GameNative's Adreno-840 lsfg fix).
            // present_wait is advertised on gen8/Adreno840 Turnip but not gen7/750, so
            // this is a no-op where the guest attaches no present-id (typically the 750).
            auto presentOne = [&](VkSemaphore wait, uint32_t image, const void* pNext) {
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.pNext = pNext;
                pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
                pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &image;
                return st->dd.QueuePresentKHR(queue, &pi);
            };

            if (!s.prevValid) {
                // first FG frame: capture curr -> prev, present curr unchanged
                WFG_LOGD(dbg, "P#%llu first-FG-frame: record curr->prev copy (no gen this frame)", presents);
                imgBarrier(dd, fc.cmd, currImg, PS, TS, 0, VK_ACCESS_TRANSFER_READ_BIT, ALL, TR);
                imgBarrier(dd, fc.cmd, s.prevImg, VK_IMAGE_LAYOUT_UNDEFINED, TD, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, TR);
                dd.CmdCopyImage(fc.cmd, currImg, TS, s.prevImg, TD, 1, &cp);
                imgBarrier(dd, fc.cmd, s.prevImg, TD, SR, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, CS);
                imgBarrier(dd, fc.cmd, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL);
                dd.EndCommandBuffer(fc.cmd);
                WFG_LOGD(dbg, "P#%llu first-FG-frame compute-submit BEGIN", presents);
                if (submitOne(fc.currSem) != VK_SUCCESS) {
                    // Submit failed -> currSem will never signal; do NOT present against
                    // it (would block the compositor). Passthrough the real frame using
                    // the app's own wait sems and leave prev invalid so we retry next frame.
                    WFG_LOGD(dbg, "P#%llu first-FG-frame submit FAILED -> passthrough real", presents);
                    VkResult ptr = st->dd.QueuePresentKHR(queue, pi);
                    return ptr;
                }
                s.prevValid = true;
                if (presents < 5) WFG_LOGI("prev captured (first FG frame)");
                WFG_LOGD(dbg, "P#%llu first-FG-frame present-real BEGIN (image=%u)", presents, idx);
                VkResult pfr = presentOne(fc.currSem, idx, nullptr);   // PRESENT-ID REVERT (750 A/B): pre-fix behaviour
                WFG_LOGD(dbg, "P#%llu first-FG-frame present-real DONE r=%d", presents, (int)pfr);
                return pfr;
            }

            // ── generate the in-between frame; try to insert it as an EXTRA present (2x) ──
            uint32_t spareIdx = 0;
            VkResult ar = VK_NOT_READY;
            bool insert = false;

            // ── POOL-HEADROOM GUARD ──────────────────────────────────────────────
            // The extra present consumes an AHB-backed swapchain image the host
            // compositor holds until it is displayed+released. If win-fg takes a spare
            // when the pool is already near empty, the guest's NEXT vkAcquireNextImageKHR
            // blocks waiting on the compositor -> render-thread freeze (Fold8/Adreno840
            // /Turnip; the AYANEO/Adreno750 recycles fast enough to never hit this).
            // Estimate free images = total - app's reserved working set - the spares
            // win-fg still has in flight for its own generated presents. If taking a
            // spare would leave < 1 image for the guest, SKIP the insert and pass the
            // real frame through. With the extra_images headroom this is a no-op on a
            // healthy device (free_est stays >= 1); it only bites a starved pool.
            int held = 0;
            for (uint32_t r = 0; r < s.ring.size(); ++r)
                if (r != fcIdx && s.ring[r].submitted && s.ring[r].heldSpare) ++held;
            int free_est = (int)s.images.size() - (int)s.appMinImages - held;
            if (free_est < 1) {
                WFG_LOGD(dbg, "P#%llu insert-skip-headroom held=%d free_est=%d (images=%zu appMin=%u) -> passthrough real",
                         presents, held, free_est, s.images.size(), s.appMinImages);
            } else {
                WFG_LOGD(dbg, "P#%llu acquire-spare BEGIN (timeout=2ms) [headroom held=%d free_est=%d]",
                         presents, held, free_est);
                ar = dd.AcquireNextImageKHR(dev, sc, 2000000ull, fc.acquireSem, VK_NULL_HANDLE, &spareIdx);
                insert = (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) && spareIdx < s.images.size();
                WFG_LOGD(dbg, "P#%llu acquire-spare RESULT ar=%d spareIdx=%u insert=%d", presents, (int)ar, spareIdx, insert ? 1 : 0);
            }
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
            WFG_LOGD(dbg, "P#%llu gen-record BEGIN (flow+synth compute, alpha=0.35 slot=%u)", presents, fcIdx);
            st->fg.record(fc.cmd, s.prevView, currView, gt.view, 0.35f, fcIdx);       // synth -> gen (rgba8); fcIdx = C1 GM slot
            WFG_LOGD(dbg, "P#%llu gen-record DONE", presents);
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
                WFG_LOGD(dbg, "P#%llu gen-blit gen->spare (spareIdx=%u)", presents, spareIdx);
                dd.CmdBlitImage(fc.cmd, gt.img, TS, spareImg, TD, 1, &bl, VK_FILTER_NEAREST);      // generated -> spare image
                imgBarrier(dd, fc.cmd, spareImg, TD, PS, VK_ACCESS_TRANSFER_WRITE_BIT, 0, TR, ALL);
                dd.EndCommandBuffer(fc.cmd);
                // one submit, waits app render-finished + acquire, signals gen + real present sems
                std::vector<VkSemaphore> waits(pi->pWaitSemaphores, pi->pWaitSemaphores + pi->waitSemaphoreCount);
                waits.push_back(fc.acquireSem);
                std::vector<VkPipelineStageFlags> ws(waits.size(), ALL);
                VkSemaphore sigs[2] = { fc.genSem, fc.currSem };
                VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                su.waitSemaphoreCount = (uint32_t)waits.size(); su.pWaitSemaphores = waits.data(); su.pWaitDstStageMask = ws.data();
                su.commandBufferCount = 1; su.pCommandBuffers = &fc.cmd;
                su.signalSemaphoreCount = 2; su.pSignalSemaphores = sigs;
                WFG_LOGD(dbg, "P#%llu compute-submit BEGIN (insert: %u waits +acquire, 2 sig sems)",
                         presents, pi->waitSemaphoreCount);
                VkResult isr = dd.QueueSubmit(st->queue, 1, &su, fc.fence);
                fc.submitted = (isr == VK_SUCCESS);   // only arm a fence wait we truly queued
                fc.heldSpare = (isr == VK_SUCCESS);   // spare is in flight only if the submit took
                WFG_LOGD(dbg, "P#%llu compute-submit DONE r=%d (insert)", presents, (int)isr);
                if (isr != VK_SUCCESS) {
                    // genSem/currSem will never signal -> presenting against them would
                    // block the compositor. Passthrough the real frame with the app's
                    // own wait sems (no 2x this frame). The just-acquired spare is dropped
                    // for this frame; on a submit failure (device-lost/OOM) that is the
                    // safe outcome — never a lockup.
                    WFG_LOGE("P#%llu insert QueueSubmit FAILED r=%d -> passthrough real", presents, (int)isr);
                    VkResult ptr = st->dd.QueuePresentKHR(queue, pi);
                    return ptr;
                }
                if (presents < 6) WFG_LOGI("2x insert live: spare=%u real=%u", spareIdx, idx);
                if (presents < 6 && s.presentMode == VK_PRESENT_MODE_FIFO_KHR)
                    WFG_LOGI("pacing note: swapchain is FIFO — display self-paces, pacing waits skip");

                // ── EVEN-CADENCE PACING ───────────────────────────────────────
                // Present the GENERATED frame, then hold the REAL present to its
                // scheduled beat (paceLastReal + dt) so the generated frame lands
                // near the temporal MIDPOINT between consecutive real frames —
                // even gen,real,gen,real cadence instead of a clustered pair + gap.
                // The real cadence stays locked to dt (base FPS is NOT throttled;
                // the wait only consumes the guest's render slack). Bounds: each
                // sleep ≤ min(dt, 20 ms); if we are already behind the beat the
                // wait is SKIPPED (present immediately) — worst case one un-paced
                // pair, NEVER a hitch. Pure CPU sleep (no GPU wait added). Off, or
                // dt not warm / anchor stale (post-fallback gap) ⇒ back-to-back.
                const uint64_t dtNs = (st->paceDtNs > 0.0) ? (uint64_t)st->paceDtNs : 0ull;
                const bool pace = st->cfg.pacing && dtNs > 0 && st->paceLastReal != 0
                                  && (t_entry - st->paceLastReal) <= 2ull * dtNs;  // fresh anchor only
                const uint64_t capNs = (dtNs < kPaceCeilNs) ? dtNs : kPaceCeilNs;  // ≤ dt and ≤ 20 ms
                const uint64_t genTarget  = st->paceLastReal + dtNs / 2;           // midpoint slot
                const uint64_t realTarget = st->paceLastReal + dtNs;               // next real beat

                // (a) GENERATED present. Nudge to the midpoint slot only if the
                //     guest ran far enough ahead that the slot is still in the
                //     future (needs >2x slack); otherwise present immediately.
                uint64_t tg = mono_ns();
                if (pace && genTarget > tg) {
                    uint64_t gt = (genTarget > tg + capNs) ? tg + capNs : genTarget;
                    sleep_until_ns(gt);
                    st->paceLastWaitNs += (mono_ns() - tg);
                }
                WFG_LOGD(dbg, "P#%llu present-generated BEGIN (spare=%u)", presents, spareIdx);
                VkResult pg = presentOne(fc.genSem, spareIdx, nullptr);   // generated frame: NEVER carry a present-id (may be skipped)
                uint64_t tGenDone = mono_ns();
                WFG_LOGD(dbg, "P#%llu present-generated DONE r=%d", presents, (int)pg);

                // (b) REAL present. Hold to the scheduled beat so gen↔real end up
                //     ~dt/2 apart; skip the wait cleanly if already late.
                bool lateSkip = false;
                uint64_t tr = mono_ns();
                if (pace) {
                    if (realTarget > tr) {
                        uint64_t rt = (realTarget > tr + capNs) ? tr + capNs : realTarget;
                        if (rt > tGenDone + capNs) rt = tGenDone + capNs;  // hard per-wait ceiling
                        sleep_until_ns(rt);
                        st->paceLastWaitNs += (mono_ns() - tr);
                    } else {
                        lateSkip = true; ++st->paceLate;   // pacing-skip (late): behind schedule
                    }
                }
                WFG_LOGD(dbg, "P#%llu present-real BEGIN (image=%u)", presents, idx);
                VkResult prr = presentOne(fc.currSem, idx, nullptr);   // PRESENT-ID REVERT (750 A/B): pre-fix behaviour
                uint64_t tRealDone = mono_ns();
                WFG_LOGD(dbg, "P#%llu present-real DONE r=%d", presents, (int)prr);
                WFG_LOGD(dbg, "P#%llu pace: dt=%.2fms gen@+%.2fms real@+%.2fms spacing=%.2fms cap=%.2fms%s",
                         presents, dtNs / 1e6,
                         (double)((long long)tGenDone  - (long long)st->paceLastReal) / 1e6,
                         (double)((long long)tRealDone - (long long)st->paceLastReal) / 1e6,
                         (double)((long long)tRealDone - (long long)tGenDone) / 1e6,
                         capNs / 1e6, lateSkip ? " pacing-skip(late)" : "");
                st->paceLastReal = tRealDone;   // re-anchor the schedule to the actual beat
                if (pace) {
                    ++st->paceInserts;
                    if ((st->paceInserts % 300ull) == 0)
                        WFG_LOGI("pacing: dt=%.2fms lastSpacing=%.2fms lateSkips=%llu (paced=%llu)",
                                 dtNs / 1e6, (double)((long long)tRealDone - (long long)tGenDone) / 1e6,
                                 (unsigned long long)st->paceLate, (unsigned long long)st->paceInserts);
                }
                // Unambiguous last win-fg line before the guest thread runs its NEXT
                // call — which the guest-acquire/guest-submit hooks then catch. If a
                // guest-acquire/submit BEGIN with no END follows THIS line, that guest
                // call is the one that hangs after a clean insert (the Fold8 freeze).
                WFG_LOGD(dbg, "P#%llu insert-complete -> returning r=%d (VK_SUCCESS=0) to guest",
                         presents, (int)prr);
                return prr;
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
                WFG_LOGD(dbg, "P#%llu spare-starve -> 3a fallback (present real untouched, ar=%d)", presents, (int)ar);
                imgBarrier(dd, fc.cmd, currImg, TS, PS, VK_ACCESS_TRANSFER_READ_BIT, 0, TR, ALL);
                dd.EndCommandBuffer(fc.cmd);
                WFG_LOGD(dbg, "P#%llu fallback compute-submit BEGIN", presents);
                if (submitOne(fc.currSem) != VK_SUCCESS) {
                    // currSem will never signal -> passthrough with the app's own sems.
                    WFG_LOGD(dbg, "P#%llu fallback submit FAILED -> passthrough real", presents);
                    VkResult ptr = st->dd.QueuePresentKHR(queue, pi);
                    return ptr;
                }
                WFG_LOGD(dbg, "P#%llu fallback present-real BEGIN (image=%u)", presents, idx);
                VkResult prf = presentOne(fc.currSem, idx, nullptr);   // PRESENT-ID REVERT (750 A/B): pre-fix behaviour
                WFG_LOGD(dbg, "P#%llu fallback present-real DONE r=%d", presents, (int)prf);
                return prf;
            }
        } else {
            WFG_LOGD(dbg, "P#%llu fg-path NOT-ready -> passthrough (swapFound=%d insertReady=%d genFound=%d)",
                     presents, (sit != g_swap.end()) ? 1 : 0,
                     (sit != g_swap.end() && sit->second.insertReady) ? 1 : 0,
                     (git != g_gen.end()) ? 1 : 0);
        }
    }
    WFG_LOGD(dbg, "P#%llu passthrough present-real BEGIN (doGen=%d)", presents, doGen ? 1 : 0);
    VkResult ptr = st->dd.QueuePresentKHR(queue, pi);
    WFG_LOGD(dbg, "P#%llu passthrough present-real DONE r=%d", presents, (int)ptr);
    return ptr;
}

// ── GUEST-CALL DIAGNOSTIC HOOKS (gated behind WIN_FG_DEBUG) ───────────────────
// win-fg's own present hook goes SILENT the instant the GUEST render thread blocks
// on its NEXT Vulkan call after an inserted frame (device-proven Fold8/Adreno840/
// Turnip: the first insert completes clean — all r=0 through insert-complete — then
// the guest never calls present again and force-closes ~30s later). To see WHICH
// guest call hangs we shadow the guest's OWN vkAcquireNextImage{,2}KHR and
// vkQueueSubmit{,2,2KHR}: a BEGIN with no matching END pinpoints the stuck stage.
//
// win-fg's internal spare-acquire and compute-submits go through the dd.* down-call
// pointers, NOT these wrappers, so they are NOT double-logged — these lines are the
// GUEST's calls only. Verbose BEGIN/END is emitted ONLY when cfg.debug (WIN_FG_DEBUG)
// AND cfg.enabled (FG on) — the freeze window — so the pre-enable stream stays quiet.
//
// CRITICAL: g_lock is taken ONLY to snapshot the down-call pointer + gate flags, then
// RELEASED before the (possibly never-returning) down-call. Holding g_lock across a
// hung guest call would deadlock the present thread and mask the very freeze we are
// trying to observe. The BEGIN line is written (android_log is synchronous → on the
// logcat socket) BEFORE the down-call, so it survives even if the call never returns.
namespace {
struct GuestHookCtx {
    bool found = false;
    bool log   = false;   // cfg.debug && cfg.enabled
    PFN_vkAcquireNextImageKHR   AcquireNextImageKHR   = nullptr;
    PFN_vkAcquireNextImage2KHR  AcquireNextImage2KHR  = nullptr;
    PFN_vkQueueSubmit           QueueSubmit           = nullptr;
    PFN_vkQueueSubmit2          QueueSubmit2          = nullptr;
    PFN_vkQueueSubmit2KHR       QueueSubmit2KHR       = nullptr;
};
// key = dispatch_key(device) OR dispatch_key(queue) — a queue shares its device's
// dispatch key, so both resolve the same DeviceState.
static GuestHookCtx snapshotGuestCtx(void* key) {
    GuestHookCtx c;
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_dev.find(key);
    if (it == g_dev.end()) return c;
    const DeviceState& ds = it->second;
    c.found = true;
    c.log   = ds.cfg.debug && ds.cfg.enabled;
    c.AcquireNextImageKHR  = ds.dd.AcquireNextImageKHR;
    c.AcquireNextImage2KHR = ds.dd.AcquireNextImage2KHR;
    c.QueueSubmit          = ds.dd.QueueSubmit;
    c.QueueSubmit2         = ds.dd.QueueSubmit2;
    c.QueueSubmit2KHR      = ds.dd.QueueSubmit2KHR;
    return c;
}
} // namespace

extern "C" VkResult VKAPI_CALL winfg_AcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
    GuestHookCtx c = snapshotGuestCtx(dispatch_key(device));
    if (!c.AcquireNextImageKHR) return VK_ERROR_DEVICE_LOST;   // no down-call — cannot service
    static std::atomic<unsigned long long> ctr{0};
    unsigned long long n = ++ctr;
    unsigned long long t0 = 0;
    if (c.log) {
        t0 = now_ns();
        WFG_LOGI("guest-acquire BEGIN #%llu (timeout=%llu sem=0x%llx fence=0x%llx swapchain=0x%llx)",
                 n, (unsigned long long)timeout,
                 (unsigned long long)(uintptr_t)semaphore, (unsigned long long)(uintptr_t)fence,
                 (unsigned long long)(uintptr_t)swapchain);
    }
    VkResult r = c.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    if (c.log) {
        unsigned long long e = (now_ns() - t0) / 1000ull;
        uint32_t idx = (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pImageIndex ? *pImageIndex : 0xffffffffu;
        WFG_LOGI("guest-acquire END   #%llu r=%d imageIndex=%u elapsed_us=%llu", n, (int)r, idx, e);
    }
    return r;
}

extern "C" VkResult VKAPI_CALL winfg_AcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex) {
    GuestHookCtx c = snapshotGuestCtx(dispatch_key(device));
    if (!c.AcquireNextImage2KHR) return VK_ERROR_DEVICE_LOST;
    static std::atomic<unsigned long long> ctr{0};
    unsigned long long n = ++ctr;
    unsigned long long t0 = 0;
    if (c.log) {
        t0 = now_ns();
        WFG_LOGI("guest-acquire BEGIN #%llu [2KHR] (timeout=%llu sem=0x%llx fence=0x%llx swapchain=0x%llx)",
                 n, (unsigned long long)(pAcquireInfo ? pAcquireInfo->timeout : 0),
                 (unsigned long long)(uintptr_t)(pAcquireInfo ? pAcquireInfo->semaphore : VK_NULL_HANDLE),
                 (unsigned long long)(uintptr_t)(pAcquireInfo ? pAcquireInfo->fence : VK_NULL_HANDLE),
                 (unsigned long long)(uintptr_t)(pAcquireInfo ? pAcquireInfo->swapchain : VK_NULL_HANDLE));
    }
    VkResult r = c.AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    if (c.log) {
        unsigned long long e = (now_ns() - t0) / 1000ull;
        uint32_t idx = (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pImageIndex ? *pImageIndex : 0xffffffffu;
        WFG_LOGI("guest-acquire END   #%llu [2KHR] r=%d imageIndex=%u elapsed_us=%llu", n, (int)r, idx, e);
    }
    return r;
}

extern "C" VkResult VKAPI_CALL winfg_QueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    GuestHookCtx c = snapshotGuestCtx(dispatch_key(queue));
    if (!c.QueueSubmit) return VK_ERROR_DEVICE_LOST;
    static std::atomic<unsigned long long> ctr{0};
    unsigned long long n = ++ctr;
    unsigned long long t0 = 0;
    if (c.log) {
        t0 = now_ns();
        WFG_LOGI("guest-submit BEGIN #%llu (submitCount=%u fence=0x%llx queue=0x%llx)",
                 n, submitCount, (unsigned long long)(uintptr_t)fence, (unsigned long long)(uintptr_t)queue);
    }
    VkResult r = c.QueueSubmit(queue, submitCount, pSubmits, fence);
    if (c.log) {
        unsigned long long e = (now_ns() - t0) / 1000ull;
        WFG_LOGI("guest-submit END   #%llu r=%d elapsed_us=%llu", n, (int)r, e);
    }
    return r;
}

extern "C" VkResult VKAPI_CALL winfg_QueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    GuestHookCtx c = snapshotGuestCtx(dispatch_key(queue));
    if (!c.QueueSubmit2) return VK_ERROR_DEVICE_LOST;
    static std::atomic<unsigned long long> ctr{0};
    unsigned long long n = ++ctr;
    unsigned long long t0 = 0;
    if (c.log) {
        t0 = now_ns();
        WFG_LOGI("guest-submit BEGIN #%llu [2] (submitCount=%u fence=0x%llx queue=0x%llx)",
                 n, submitCount, (unsigned long long)(uintptr_t)fence, (unsigned long long)(uintptr_t)queue);
    }
    VkResult r = c.QueueSubmit2(queue, submitCount, pSubmits, fence);
    if (c.log) {
        unsigned long long e = (now_ns() - t0) / 1000ull;
        WFG_LOGI("guest-submit END   #%llu [2] r=%d elapsed_us=%llu", n, (int)r, e);
    }
    return r;
}

extern "C" VkResult VKAPI_CALL winfg_QueueSubmit2KHR(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    GuestHookCtx c = snapshotGuestCtx(dispatch_key(queue));
    if (!c.QueueSubmit2KHR) return VK_ERROR_DEVICE_LOST;
    static std::atomic<unsigned long long> ctr{0};
    unsigned long long n = ++ctr;
    unsigned long long t0 = 0;
    if (c.log) {
        t0 = now_ns();
        WFG_LOGI("guest-submit BEGIN #%llu [2KHR] (submitCount=%u fence=0x%llx queue=0x%llx)",
                 n, submitCount, (unsigned long long)(uintptr_t)fence, (unsigned long long)(uintptr_t)queue);
    }
    VkResult r = c.QueueSubmit2KHR(queue, submitCount, pSubmits, fence);
    if (c.log) {
        unsigned long long e = (now_ns() - t0) / 1000ull;
        WFG_LOGI("guest-submit END   #%llu [2KHR] r=%d elapsed_us=%llu", n, (int)r, e);
    }
    return r;
}

// ── proc addr + negotiation ──────────────────────────────────────────────────
#define INTERCEPT(name) if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)&winfg_##name;

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL winfg_GetDeviceProcAddr(VkDevice device, const char* pName) {
    INTERCEPT(CreateSwapchainKHR) INTERCEPT(DestroySwapchainKHR) INTERCEPT(QueuePresentKHR)
    INTERCEPT(DestroyDevice)
    // Guest-call diagnostic hooks. AcquireNextImageKHR + QueueSubmit are always present
    // when a swapchain is in use (same assumption as the present/swapchain intercepts).
    INTERCEPT(AcquireNextImageKHR) INTERCEPT(QueueSubmit)
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_dev.find(dispatch_key(device));
    if (it == g_dev.end() || !it->second.dd.GetDeviceProcAddr) return nullptr;
    const DeviceDispatch& dd = it->second.dd;
    // Conditional intercepts — only SHADOW a 2-variant the device actually provides,
    // so we never advertise an entry point the driver/device does not support.
    if (!strcmp(pName, "vkAcquireNextImage2KHR") && dd.AcquireNextImage2KHR) return (PFN_vkVoidFunction)&winfg_AcquireNextImage2KHR;
    if (!strcmp(pName, "vkQueueSubmit2")    && dd.QueueSubmit2)    return (PFN_vkVoidFunction)&winfg_QueueSubmit2;
    if (!strcmp(pName, "vkQueueSubmit2KHR") && dd.QueueSubmit2KHR) return (PFN_vkVoidFunction)&winfg_QueueSubmit2KHR;
    return dd.GetDeviceProcAddr(device, pName);
}

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL winfg_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    INTERCEPT(GetInstanceProcAddr) INTERCEPT(CreateInstance) INTERCEPT(DestroyInstance)
    INTERCEPT(CreateDevice) INTERCEPT(GetDeviceProcAddr)
    INTERCEPT(CreateSwapchainKHR) INTERCEPT(DestroySwapchainKHR) INTERCEPT(QueuePresentKHR)
    // Guest-call diagnostic hooks (always-present entries; the 2-variants are resolved
    // conditionally in winfg_GetDeviceProcAddr, which is how DXVK looks them up).
    INTERCEPT(AcquireNextImageKHR) INTERCEPT(QueueSubmit)
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
