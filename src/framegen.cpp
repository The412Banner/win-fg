// win-fg — compute frame-generation engine implementation.
#include "framegen.hpp"
#include "embedded_shaders.hpp"
#include <cmath>
#include <cstdio>

namespace winfg {

#define VKOK(x) do { if ((x) != VK_SUCCESS) { std::fprintf(stderr, "[win-fg] fail %s\n", #x); return false; } } while (0)

uint32_t FrameGen::findMemType(uint32_t bits, VkMemoryPropertyFlags props) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

bool FrameGen::makeImage(Img& out, VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage) {
    out.extent = ext; out.fmt = fmt;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {ext.width, ext.height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(dd_->CreateImage(device_, &ci, nullptr, &out.image));
    VkMemoryRequirements mr; dd_->GetImageMemoryRequirements(device_, out.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKOK(dd_->AllocateMemory(device_, &ai, nullptr, &out.mem));
    VKOK(dd_->BindImageMemory(device_, out.image, out.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKOK(dd_->CreateImageView(device_, &vi, nullptr, &out.view));
    return true;
}

void FrameGen::destroyImage(Img& i) {
    if (i.view) dd_->DestroyImageView(device_, i.view, nullptr);
    if (i.image) dd_->DestroyImage(device_, i.image, nullptr);
    if (i.mem) dd_->FreeMemory(device_, i.mem, nullptr);
    i = Img{};
}

bool FrameGen::makePipe(Pipe& p, embedded::Shader shader,
                        const std::vector<VkDescriptorType>&) {
    // binding layout is derived per shader below in init(); here we only build
    // the module + a generic layout supplied by caller via setLayout already set.
    embedded::Blob b = embedded::blob(shader);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = b.words * 4; mi.pCode = b.code;
    VKOK(dd_->CreateShaderModule(device_, &mi, nullptr, &p.module));
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &p.setLayout;
    VKOK(dd_->CreatePipelineLayout(device_, &li, nullptr, &p.layout));
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = p.module; pi.stage.pName = "main";
    pi.layout = p.layout;
    VKOK(dd_->CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &p.pipeline));
    return true;
}

void FrameGen::destroyPipe(Pipe& p) {
    if (p.pipeline) dd_->DestroyPipeline(device_, p.pipeline, nullptr);
    if (p.layout) dd_->DestroyPipelineLayout(device_, p.layout, nullptr);
    if (p.setLayout) dd_->DestroyDescriptorSetLayout(device_, p.setLayout, nullptr);
    if (p.module) dd_->DestroyShaderModule(device_, p.module, nullptr);
    p = Pipe{};
}

// Build a descriptor-set layout from an explicit (binding, type) list.
static bool buildSetLayout(const DeviceDispatch* dd, VkDevice dev,
                           const std::vector<std::pair<uint32_t, VkDescriptorType>>& binds,
                           VkDescriptorSetLayout& out) {
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (auto& kv : binds)
        b.push_back({kv.first, kv.second, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)b.size(); ci.pBindings = b.data();
    return dd->CreateDescriptorSetLayout(dev, &ci, nullptr, &out) == VK_SUCCESS;
}

bool FrameGen::init(const DeviceDispatch* dd, const InstanceDispatch* id,
                    VkPhysicalDevice phys, VkDevice dev, uint32_t queueFamily, VkQueue queue) {
    dd_ = dd; id_ = id; phys_ = phys; device_ = dev; queueFamily_ = queueFamily; queue_ = queue;
    id_->GetPhysicalDeviceMemoryProperties(phys_, &memProps_);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKOK(dd_->CreateSampler(device_, &si, nullptr, &linear_));

    // descriptor pool sized generously for our per-frame sets.
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 256; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
    VKOK(dd_->CreateDescriptorPool(device_, &pci, nullptr, &descPool_));

    using B = std::pair<uint32_t, VkDescriptorType>;
    const auto S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const auto W = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const auto U = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

    // per-shader binding sets (match the GLSL layout(binding=...) declarations)
    if (!buildSetLayout(dd_, dev, {B{32,S},B{48,W}}, pLuma_.setLayout)) return false;
    if (!buildSetLayout(dd_, dev, {B{32,S},B{48,W}}, pDown_.setLayout)) return false;
    if (!buildSetLayout(dd_, dev, {B{0,U},B{32,S},B{33,S},B{34,S},B{48,W}}, pFlow_.setLayout)) return false;
    if (!buildSetLayout(dd_, dev, {B{0,U},B{32,S},B{33,S},B{34,S},B{48,W}}, pFlowM4_.setLayout)) return false;
    if (!buildSetLayout(dd_, dev, {B{0,U},B{32,S},B{48,W},B{49,W}}, pExpand_.setLayout)) return false;
    if (!buildSetLayout(dd_, dev, {B{0,U},B{32,S},B{48,W},B{49,W}}, pExpandM4_.setLayout)) return false;
    if (!buildSetLayout(dd_, dev, {B{0,U},B{32,S},B{33,S},B{34,S},B{35,S},B{48,W}}, pSynth_.setLayout)) return false;

    std::vector<VkDescriptorType> unused;
    if (!makePipe(pLuma_,     embedded::OF3_LUMA, unused)) return false;
    if (!makePipe(pDown_,     embedded::OF3_DOWNSAMPLE, unused)) return false;
    if (!makePipe(pFlow_,     embedded::OF3_FLOW, unused)) return false;
    if (!makePipe(pFlowM4_,   embedded::OF3_FLOW_M4, unused)) return false;
    if (!makePipe(pExpand_,   embedded::OF3_EXPAND, unused)) return false;
    if (!makePipe(pExpandM4_, embedded::OF3_EXPAND_M4, unused)) return false;
    if (!makePipe(pSynth_,    embedded::WFG_SYNTH, unused)) return false;
    return true;
}

static VkExtent2D levelExtent(VkExtent2D e, int l) {
    return { e.width >> l ? e.width >> l : 1u, e.height >> l ? e.height >> l : 1u };
}

bool FrameGen::onResize(VkExtent2D extent, VkFormat /*colorFormat*/) {
    if (ready_ && extent.width == extent_.width && extent.height == extent_.height) return true;
    // tear down previous size
    for (auto& i : pyrA_) destroyImage(i); pyrA_.clear();
    for (auto& i : pyrB_) destroyImage(i); pyrB_.clear();
    for (auto& i : flowLvl_) destroyImage(i); flowLvl_.clear();
    destroyImage(flowExpA_); destroyImage(flowExpB_);
    extent_ = extent; ready_ = false;

    const VkImageUsageFlags lumaUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags flowUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    pyrA_.resize(kLevels); pyrB_.resize(kLevels); flowLvl_.resize(kLevels);
    for (int l = 0; l < kLevels; ++l) {
        VkExtent2D e = levelExtent(extent, l);
        if (!makeImage(pyrA_[l], e, VK_FORMAT_R32_SFLOAT, lumaUsage)) return false;
        if (!makeImage(pyrB_[l], e, VK_FORMAT_R32_SFLOAT, lumaUsage)) return false;
        if (!makeImage(flowLvl_[l], e, VK_FORMAT_R16G16B16A16_SFLOAT, flowUsage)) return false;
    }
    if (!makeImage(flowExpA_, extent, VK_FORMAT_R16G16B16A16_SFLOAT, flowUsage)) return false;
    if (!makeImage(flowExpB_, extent, VK_FORMAT_R16G16B16A16_SFLOAT, flowUsage)) return false;
    ready_ = true;
    return true;
}

void FrameGen::barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dd_->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// NOTE: descriptor-set allocation/update per record() and the exact per-level
// flow wiring are implemented in record_impl.inc for readability; the graph is:
//   luma(curr)->pyrA[0], luma(prev)->pyrB[0]
//   downsample pyrA/pyrB to kLevels
//   coarse->fine flow (per level) into flowLvl
//   expand flowLvl[0] -> flowExpA/flowExpB
//   synth(prev,curr,flowExpA,flowExpB, alpha) -> out
#include "record_impl.inc"

void FrameGen::destroy() {
    if (!device_) return;
    dd_->DeviceWaitIdle(device_);
    for (auto& i : pyrA_) destroyImage(i); pyrA_.clear();
    for (auto& i : pyrB_) destroyImage(i); pyrB_.clear();
    for (auto& i : flowLvl_) destroyImage(i); flowLvl_.clear();
    destroyImage(flowExpA_); destroyImage(flowExpB_);
    destroyPipe(pLuma_); destroyPipe(pDown_); destroyPipe(pFlow_); destroyPipe(pFlowM4_);
    destroyPipe(pExpand_); destroyPipe(pExpandM4_); destroyPipe(pSynth_);
    if (descPool_) dd_->DestroyDescriptorPool(device_, descPool_, nullptr);
    if (linear_) dd_->DestroySampler(device_, linear_, nullptr);
    descPool_ = VK_NULL_HANDLE; linear_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE; ready_ = false;
}

} // namespace winfg
