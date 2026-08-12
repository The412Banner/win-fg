// win-fg — Vulkan implicit layer entry points and swapchain/present interception.
// Structure follows the public Khronos loader-layer interface and the renderdoc
// layer guide (Apache-2.0 / CC-BY learning refs). No code from bionic-fg / lsfg
// / GameScope. The frame-gen compute is win-fg's own (see framegen.*).
#include "vk_dispatch.hpp"
#include "framegen.hpp"
#include "config.hpp"
#include <vulkan/vk_layer.h>
#include <map>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdio>

using namespace winfg;

namespace {
std::mutex g_lock;
std::map<void*, InstanceDispatch> g_inst;
std::map<void*, VkInstance>       g_instHandle;

struct DeviceState {
    DeviceDispatch dd;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    const InstanceDispatch* id = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    FrameGen fg;
    Config cfg;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool fgInited = false;
};
std::map<void*, DeviceState> g_dev;

struct SwapState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format{}; VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    // owned prev-frame copy + generated target (device bring-up)
    bool havePrev = false;
    uint32_t lastIndex = 0;
};
std::map<VkSwapchainKHR, SwapState> g_swap;

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
    L(CmdPipelineBarrier); L(CmdCopyImage); L(CmdBlitImage);
    L(CreateFence); L(DestroyFence); L(WaitForFences); L(ResetFences); L(CreateSemaphore); L(DestroySemaphore);
#undef L

    std::lock_guard<std::mutex> lk(g_lock);
    void* ik = dispatch_key(phys);
    st.id = &g_inst[ik];
    st.cfg = load_config();
    // pick a queue family with compute
    uint32_t qfc = 0; st.id->GetPhysicalDeviceQueueFamilyProperties(phys, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfc);
    st.id->GetPhysicalDeviceQueueFamilyProperties(phys, &qfc, qf.data());
    for (uint32_t i = 0; i < qfc; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { st.queueFamily = i; break; }
    dd.GetDeviceQueue(*pDevice, st.queueFamily, 0, &st.queue);
    g_dev[dispatch_key(*pDevice)] = std::move(st);
    std::fprintf(stderr, "[win-fg] device created (enable=%d model=%d)\n", g_dev[dispatch_key(*pDevice)].cfg.enabled, g_dev[dispatch_key(*pDevice)].cfg.model);
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
    VkResult r = st.dd.CreateSwapchainKHR(device, &ci, pAlloc, pSwapchain);
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
    if (!st.fgInited && st.cfg.enabled) {
        st.fg.init(&st.dd, st.id, st.phys, device, st.queueFamily, st.queue);
        st.fg.configure(st.cfg);
        st.fgInited = true;
    }
    if (st.fgInited) st.fg.onResize(s.extent, s.format);
    g_swap[*pSwapchain] = std::move(s);
    return VK_SUCCESS;
}

extern "C" void VKAPI_CALL winfg_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAlloc) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto& st = g_dev[dispatch_key(device)];
    auto it = g_swap.find(swapchain);
    if (it != g_swap.end()) { for (auto v : it->second.views) if (v) st.dd.DestroyImageView(device, v, nullptr); g_swap.erase(it); }
    st.dd.DestroySwapchainKHR(device, swapchain, pAlloc);
}

// ── present hook ─────────────────────────────────────────────────────────────
// First-draft bring-up: runs the flow+synth compute for (prev,curr) into an
// owned target to prove the pipeline end-to-end, then presents the real frame
// unchanged (safe passthrough). Inserting the generated frame as an extra
// present (the actual 2x) is the device bring-up step — see docs/BRINGUP.md.
extern "C" VkResult VKAPI_CALL winfg_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceState* st = nullptr;
    { std::lock_guard<std::mutex> lk(g_lock);
      for (auto& kv : g_dev) if (dispatch_key(kv.first) == dispatch_key(queue)) { st = &kv.second; break; } }
    if (!st || !st->cfg.enabled || !st->fgInited) {
        // passthrough
        std::lock_guard<std::mutex> lk(g_lock);
        return g_dev[dispatch_key(queue)].dd.QueuePresentKHR(queue, pPresentInfo);
    }
    // NOTE: real generation/insert wired during device bring-up; passthrough now.
    std::lock_guard<std::mutex> lk(g_lock);
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
