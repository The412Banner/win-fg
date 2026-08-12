// win-fg — compute frame-generation engine.
// Owns the compute pipelines (our of3_* optical flow + wfg_synth) and the
// per-resolution intermediate images, and records the flow+synth graph that
// turns (prev, curr) into an interpolated frame. Written for win-fg; no code
// from bionic-fg / lsfg / GameScope.
#pragma once
#include "vk_dispatch.hpp"
#include "config.hpp"
#include <vector>

namespace winfg {

class FrameGen {
public:
    bool init(const DeviceDispatch* dd, const InstanceDispatch* id,
              VkPhysicalDevice phys, VkDevice dev, uint32_t queueFamily, VkQueue queue);
    void configure(const Config& c) { cfg_ = c; cfg_.sanitize(); }
    // (Re)build per-resolution resources for a swapchain of the given size/format.
    bool onResize(VkExtent2D extent, VkFormat colorFormat);
    // Record the whole flow+synth graph into cmd. prevView/currView are SAMPLED
    // views of the two real frames; outView is a STORAGE view of the target
    // image the generated frame is written into. alpha in (0,1).
    void record(VkCommandBuffer cmd, VkImageView prevView, VkImageView currView,
                VkImageView outView, float alpha);
    void destroy();
    bool valid() const { return device_ != VK_NULL_HANDLE && ready_; }
    const Config& config() const { return cfg_; }

    // public so the record_impl.inc free helpers can reference them
    struct Img { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
                 VkImageView view = VK_NULL_HANDLE; VkExtent2D extent{}; VkFormat fmt{}; };
    struct Pipe { VkPipeline pipeline = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE;
                  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE; VkShaderModule module = VK_NULL_HANDLE; };

private:
    uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags props) const;
    bool makeImage(Img& out, VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage);
    void destroyImage(Img& i);
    bool makePipe(Pipe& p, embedded::Shader shader,
                  const std::vector<VkDescriptorType>& bindings);
    void destroyPipe(Pipe& p);
    void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to);

    const DeviceDispatch*   dd_ = nullptr;
    const InstanceDispatch*  id_ = nullptr;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;
    uint32_t         queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkSampler        linear_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;

    Config    cfg_;
    VkExtent2D extent_{};
    bool ready_ = false;

    // pipelines
    Pipe pLuma_, pDown_, pFlow_, pFlowM4_, pExpand_, pExpandM4_, pSynth_;

    // per-resolution resources
    static const int kLevels = 5;
    std::vector<Img> pyrA_, pyrB_;   // luma pyramids for curr / prev
    std::vector<Img> flowLvl_;       // per-level flow scratch
    Img flowExpA_, flowExpB_;        // fwd / bwd expanded flow (+conf)
};

} // namespace winfg
