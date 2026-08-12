// win-fg — FrameGen::record() and its lazy per-size descriptor/UBO setup.
// Included from framegen.cpp (has access to FrameGen privates). Written for
// win-fg; standard Vulkan compute wiring, no third-party code.

namespace {
struct ScratchState {
    // host-visible UBO buffers
    VkBuffer synthUbo = VK_NULL_HANDLE; VkDeviceMemory synthUboMem = VK_NULL_HANDLE; void* synthUboPtr = nullptr;
    std::vector<VkBuffer> flowUbo; std::vector<VkDeviceMemory> flowUboMem; std::vector<void*> flowUboPtr;
    VkBuffer expandUbo = VK_NULL_HANDLE; VkDeviceMemory expandUboMem = VK_NULL_HANDLE; void* expandUboPtr = nullptr;
    // persistent descriptor sets
    VkDescriptorSet sLumaCurr = VK_NULL_HANDLE, sLumaPrev = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sDownA, sDownB, sFlow;
    VkDescriptorSet sExpand = VK_NULL_HANDLE, sSynth = VK_NULL_HANDLE;
    VkExtent2D builtFor{0,0};
    bool built = false;
};
ScratchState g_scratch; // one swapchain in flight in this first-draft engine
}

static bool makeUbo(const winfg::DeviceDispatch* dd, VkDevice dev,
                    const VkPhysicalDeviceMemoryProperties& mp,
                    VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem, void*& ptr) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dd->CreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; dd->GetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t idx = 0; VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { idx = i; break; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = idx;
    if (dd->AllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    if (dd->BindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS) return false;
    return dd->MapMemory(dev, mem, 0, size, 0, &ptr) == VK_SUCCESS;
}

static VkDescriptorSet allocSet(const winfg::DeviceDispatch* dd, VkDevice dev,
                                VkDescriptorPool pool, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    VkDescriptorSet s = VK_NULL_HANDLE; dd->AllocateDescriptorSets(dev, &ai, &s); return s;
}

// write helpers
static void wImg(const winfg::DeviceDispatch* dd, VkDevice dev, VkDescriptorSet set, uint32_t binding,
                 VkDescriptorType type, VkImageView view, VkSampler samp, VkImageLayout layout) {
    VkDescriptorImageInfo ii{}; ii.imageView = view; ii.sampler = samp; ii.imageLayout = layout;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = type; w.pImageInfo = &ii;
    dd->UpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}
static void wUbo(const winfg::DeviceDispatch* dd, VkDevice dev, VkDescriptorSet set, uint32_t binding,
                 VkBuffer buf, VkDeviceSize range) {
    VkDescriptorBufferInfo bi{}; bi.buffer = buf; bi.range = range;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi;
    dd->UpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

namespace winfg {

// Lazily allocate per-size UBOs + descriptor sets and wire the fixed (internal-
// resource) bindings once. External views (prev/curr/out) are wired per frame.
static bool ensureScratch(FrameGen* fg, const DeviceDispatch* dd, VkDevice dev,
                          const VkPhysicalDeviceMemoryProperties& mp, VkDescriptorPool pool,
                          VkSampler samp, VkExtent2D ext,
                          std::vector<FrameGen::Img>& pyrA, std::vector<FrameGen::Img>& pyrB,
                          std::vector<FrameGen::Img>& flowLvl, FrameGen::Img& expA, FrameGen::Img& expB,
                          const FrameGen::Pipe& pLuma, const FrameGen::Pipe& pDown,
                          const FrameGen::Pipe& pFlow, const FrameGen::Pipe& pExpand,
                          const FrameGen::Pipe& pSynth, int kLevels, const Config& cfg);

void FrameGen::record(VkCommandBuffer cmd, VkImageView prevView, VkImageView currView,
                      VkImageView outView, float alpha) {
    if (!valid()) return;
    const bool m4 = (cfg_.model == 4);
    const Pipe& flowP   = m4 ? pFlowM4_   : pFlow_;
    const Pipe& expandP = m4 ? pExpandM4_ : pExpand_;

    if (!g_scratch.built || g_scratch.builtFor.width != extent_.width || g_scratch.builtFor.height != extent_.height) {
        ensureScratch(this, dd_, device_, memProps_, descPool_, linear_, extent_,
                      pyrA_, pyrB_, flowLvl_, flowExpA_, flowExpB_,
                      pLuma_, pDown_, flowP, expandP, pSynth_, kLevels, cfg_);
    }

    // update UBOs for this frame
    SynthUBO su{cfg_.flowScale, alpha, cfg_.beta, cfg_.lambda, cfg_.epsilon, cfg_.photoScale, 0, 0};
    if (g_scratch.synthUboPtr) std::memcpy(g_scratch.synthUboPtr, &su, sizeof(su));

    // wire per-frame external views
    wImg(dd_, device_, g_scratch.sLumaCurr, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, currView, linear_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wImg(dd_, device_, g_scratch.sLumaPrev, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, prevView, linear_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wImg(dd_, device_, g_scratch.sSynth, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, prevView, linear_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wImg(dd_, device_, g_scratch.sSynth, 33, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, currView, linear_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    wImg(dd_, device_, g_scratch.sSynth, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, outView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);

    auto disp = [&](const Pipe& p, VkDescriptorSet set, VkExtent2D e) {
        dd_->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        dd_->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        dd_->CmdDispatch(cmd, (e.width + 15) / 16, (e.height + 15) / 16, 1);
    };
    auto bar = [&](FrameGen::Img& i){ barrier(cmd, i.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL); };

    // 1) luma of curr/prev at full res
    disp(pLuma_, g_scratch.sLumaCurr, levelExtent(extent_, 0));
    disp(pLuma_, g_scratch.sLumaPrev, levelExtent(extent_, 0));
    bar(pyrA_[0]); bar(pyrB_[0]);
    // 2) downsample pyramids
    for (int l = 1; l < kLevels; ++l) {
        disp(pDown_, g_scratch.sDownA[l - 1], levelExtent(extent_, l));
        disp(pDown_, g_scratch.sDownB[l - 1], levelExtent(extent_, l));
        bar(pyrA_[l]); bar(pyrB_[l]);
    }
    // 3) coarse->fine flow
    for (int l = kLevels - 1; l >= 0; --l) {
        disp(flowP, g_scratch.sFlow[l], levelExtent(extent_, l));
        bar(flowLvl_[l]);
    }
    // 4) expand finest flow -> fwd/bwd (+conf)
    disp(expandP, g_scratch.sExpand, levelExtent(extent_, 0));
    bar(flowExpA_); bar(flowExpB_);
    // 5) synthesise interpolated frame
    disp(pSynth_, g_scratch.sSynth, levelExtent(extent_, 0));
}

// ---- lazy scratch builder ----
static bool ensureScratch(FrameGen* /*fg*/, const DeviceDispatch* dd, VkDevice dev,
                          const VkPhysicalDeviceMemoryProperties& mp, VkDescriptorPool pool,
                          VkSampler samp, VkExtent2D ext,
                          std::vector<FrameGen::Img>& pyrA, std::vector<FrameGen::Img>& pyrB,
                          std::vector<FrameGen::Img>& flowLvl, FrameGen::Img& expA, FrameGen::Img& expB,
                          const FrameGen::Pipe& pLuma, const FrameGen::Pipe& pDown,
                          const FrameGen::Pipe& pFlow, const FrameGen::Pipe& pExpand,
                          const FrameGen::Pipe& pSynth, int kLevels, const Config& cfg) {
    auto& S = g_scratch;
    S = ScratchState{};
    const auto SR = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto GEN = VK_IMAGE_LAYOUT_GENERAL;

    makeUbo(dd, dev, mp, sizeof(SynthUBO), S.synthUbo, S.synthUboMem, S.synthUboPtr);
    makeUbo(dd, dev, mp, sizeof(FlowUBO), S.expandUbo, S.expandUboMem, S.expandUboPtr);
    if (S.expandUboPtr) { FlowUBO u{cfg.flowScale, 0, 0.6f, 1.6f}; std::memcpy(S.expandUboPtr, &u, sizeof(u)); }
    S.flowUbo.resize(kLevels); S.flowUboMem.resize(kLevels); S.flowUboPtr.resize(kLevels);
    for (int l = 0; l < kLevels; ++l) {
        makeUbo(dd, dev, mp, sizeof(FlowUBO), S.flowUbo[l], S.flowUboMem[l], S.flowUboPtr[l]);
        if (S.flowUboPtr[l]) { FlowUBO u{cfg.flowScale, (uint32_t)l, 0.6f, 1.6f}; std::memcpy(S.flowUboPtr[l], &u, sizeof(u)); }
    }

    // luma sets (external src wired per frame; dst is pyramid[0])
    S.sLumaCurr = allocSet(dd, dev, pool, pLuma.setLayout);
    S.sLumaPrev = allocSet(dd, dev, pool, pLuma.setLayout);
    wImg(dd, dev, S.sLumaCurr, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pyrA[0].view, VK_NULL_HANDLE, GEN);
    wImg(dd, dev, S.sLumaPrev, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pyrB[0].view, VK_NULL_HANDLE, GEN);

    // downsample sets (fixed)
    S.sDownA.resize(kLevels - 1); S.sDownB.resize(kLevels - 1);
    for (int l = 1; l < kLevels; ++l) {
        S.sDownA[l-1] = allocSet(dd, dev, pool, pDown.setLayout);
        S.sDownB[l-1] = allocSet(dd, dev, pool, pDown.setLayout);
        wImg(dd, dev, S.sDownA[l-1], 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pyrA[l-1].view, samp, GEN);
        wImg(dd, dev, S.sDownA[l-1], 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pyrA[l].view, VK_NULL_HANDLE, GEN);
        wImg(dd, dev, S.sDownB[l-1], 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pyrB[l-1].view, samp, GEN);
        wImg(dd, dev, S.sDownB[l-1], 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pyrB[l].view, VK_NULL_HANDLE, GEN);
    }

    // flow sets (fixed): predictor = coarser flow level, or self at coarsest
    S.sFlow.resize(kLevels);
    for (int l = 0; l < kLevels; ++l) {
        S.sFlow[l] = allocSet(dd, dev, pool, pFlow.setLayout);
        int pred = (l + 1 < kLevels) ? l + 1 : l;
        wUbo(dd, dev, S.sFlow[l], 0, S.flowUbo[l], sizeof(FlowUBO));
        wImg(dd, dev, S.sFlow[l], 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pyrB[l].view, samp, GEN);
        wImg(dd, dev, S.sFlow[l], 33, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pyrA[l].view, samp, GEN);
        wImg(dd, dev, S.sFlow[l], 34, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, flowLvl[pred].view, samp, GEN);
        wImg(dd, dev, S.sFlow[l], 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, flowLvl[l].view, VK_NULL_HANDLE, GEN);
    }

    // expand set (fixed)
    S.sExpand = allocSet(dd, dev, pool, pExpand.setLayout);
    wUbo(dd, dev, S.sExpand, 0, S.expandUbo, sizeof(FlowUBO));
    wImg(dd, dev, S.sExpand, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, flowLvl[0].view, samp, GEN);
    wImg(dd, dev, S.sExpand, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, expA.view, VK_NULL_HANDLE, GEN);
    wImg(dd, dev, S.sExpand, 49, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, expB.view, VK_NULL_HANDLE, GEN);

    // synth set: external prev/curr/out wired per frame; flow inputs fixed
    S.sSynth = allocSet(dd, dev, pool, pSynth.setLayout);
    wUbo(dd, dev, S.sSynth, 0, S.synthUbo, sizeof(SynthUBO));
    wImg(dd, dev, S.sSynth, 34, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, expA.view, samp, SR);
    wImg(dd, dev, S.sSynth, 35, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, expB.view, samp, SR);

    S.builtFor = ext; S.built = true;
    return true;
}

} // namespace winfg
