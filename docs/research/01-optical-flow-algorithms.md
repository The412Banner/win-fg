# 01 — Optical Flow Algorithms for Real-Time Game FG

Research pass 2026-08-17. Scope: dense optical flow algorithms — classical
and learned — that could feed win-fg's synthesis stage. What works, what's
already been ported, and what to port next.

## Algorithm survey

### Classical dense flow
- **Horn-Schunck (1981)** — variational, minimizes `‖∇I·v + I_t‖² + α·‖∇v‖²`. Foundational; too smoothing-biased to use directly, but the L2 smoothness term is the seed for TV-L1. Public domain math.
- **Farneback (2003)** — polynomial expansion, dense per-pixel flow. In OpenCV. Fast on CPU, mediocre on sharp motion. BSD via OpenCV.
- **DIS Flow (Kroeger et al. 2016, arxiv:1603.03590)** — three-stage: inverse-compositional patch search (Baker-Matthews 2001) → dense aggregation over pyramid → variational refinement. 300–600 Hz on a single CPU core at 1024×436. BSD via OpenCV `cv::optflow::createOptFlow_DIS`. Closest classical peer to what win-fg is doing.
- **TV-L1 (Chambolle-Pock class)** — dual-primal solver, L1 data + TV regularizer. Handles motion discontinuities cleanly (unlike H-S L2). GPU implementations achieve real-time on OpenCL/CUDA. Public domain math.
- **FSR3-FG** — AMD's approach: ML-trained but weights baked into HLSL shaders, uses game-provided motion vectors + optical flow reprojection, swapchain-proxy scheduling. MIT source. Not pure classical — but the shader-only architecture is directly applicable.

### Learned dense flow
- **FlowNet2 (2016)** — 160M params, 640 MB. Obsolete for mobile.
- **PWC-Net (arxiv:1709.02371)** — pyramid + warp + cost volume. 8.75M params, 17× smaller than FlowNet2, 2× faster, 11% more accurate on Sintel-final. The architectural template everyone iterates on. NVIDIA license, mostly research-only.
- **RAFT (arxiv:2003.12039)** — per-pixel features → 4D all-pairs correlation volume → GRU-based iterative refinement. ~5M params standard variant. **BSD-3-Clause**. Dominant SOTA reference for the last 5 years.
- **GMFlow (CVPR 2022)** — global matching via cross-attention, no cost volume. MIT via mmflow.
- **SEA-RAFT (arxiv:2405.14793)** — simpler RAFT, mixture-of-Laplace loss, comparable accuracy at higher speed. BSD-adjacent.
- **NeuFlow v2 (arxiv:2408.10161)** — edge-device design. 30 FPS on Jetson Orin Nano, 10–80× speedup vs SOTA. Global-to-local at 1/16 → 1/8 res. **The most Adreno-viable modern learned flow model.**
- **FastFlowNet (arxiv:2103.04524)** and **CompactFlowNet (arxiv:2412.13273)** — mobile-first, both target <5M params.

## Top 3 techniques to port to win-fg's `of3_*` — clean-room, zero code copied

### 1. Global-motion pre-warp — biggest single quality lever
Attacks the current #1 ceiling (camera-pan melt). Cite Szeliski *Computer Vision: Algorithms and Applications* Ch. 6.2, and Baker-Matthews 2001 inverse-compositional LK.

- Fit 6-parameter affine (or 8-param homography) between prev/curr downsampled luma via Lucas-Kanade gradient descent
- Warp `I1` by inverse global motion **before** dense flow → residual flow is object-only
- Implementation: `of3_gm_reduce.comp` accumulates gradients/Hessian into a 6×1 + 6×6 SSBO via atomicAdd (Isygold's pattern for reductions); CPU-side 6×6 solve (~10–20 μs); `of3_gm_prewarp.comp` bilinear warp with affine
- Zero learned params, ~150 lines of GLSL

### 2. Correlation cost volume with sub-pixel parabola fit
Attacks sub-pixel accuracy (texture crawl on smooth pans). Cite RAFT (arxiv:2003.12039) §3.2 lookup and Lowe 2004 §5 parabola fit.

- At each pixel, evaluate `cost[δ] = ‖I0(p) − I1(p + flow + δ)‖²` for δ ∈ {−1,0,+1}² using 9 bilinear texture fetches from I1 pre-warped by current flow
- Fit parabola through 3-neighbor costs in x and y independently: `δ_sub = 0.5·(c[-1] − c[+1]) / (c[-1] − 2c[0] + c[+1])`
- Refined flow = current + argmin(parabola)
- Implementation: `of3_flow_cost9.comp` chained after existing `of3_flow.comp`. Runs at flow-field resolution (1/4 or 1/8 image), cheap.
- Zero learned params, ~80 lines of GLSL

### 3. Forward-backward consistency + occlusion mask
Attacks silhouette ghost, gives `wfg_synth` a real per-pixel confidence signal (currently gates on photometric residual alone). Cite Sundaram-Brox-Keutzer ECCV 2010 §2.

- Run `of3_flow` in both directions (I0→I1 and I1→I0) — already possible with our existing shader, just needs a second dispatch with swapped inputs
- Compute `err = ‖F01(x) + F10(x + F01(x))‖` via 1 bilinear fetch of F10 warped by F01
- Occluded where `err > α + β·max(|F01|², |F10|²)` — Sundaram's adaptive threshold (α=0.01, β=0.5 typical)
- Emit rgba: (confidence, |flow|, occlusion, mask) → wfg_synth binds and gates
- Implementation: `of3_fb_consist.comp` — 4 fetches, 1 store, ~40 lines of GLSL

## Implementation notes for our stack

- All three fit our existing `of3_*` pipeline (single UBO at binding 0, samplers at 32+, storage images at 48+). No architectural rework.
- Adreno TBDR — use 16×16 workgroups matching existing shaders, keep shared-memory footprint <8KB per workgroup (documented 11.5KB cliff on A610 per Isygold's motion_v2 notes). Cost-volume 9-point fetch stays register-resident, no shared needed.
- Sub-pixel refinement runs at 1/4 res like current flow — negligible cost.
- FB consistency doubles flow dispatch cost (both directions) — mitigate by running backward at 1/2 flow resolution.
- Global-motion pre-warp is the biggest single win: measured on video-stabilization literature it removes ~70% of translational-camera flow magnitude, freeing the solver's dynamic range for actual objects.

## Ceiling of pure classical flow

With **global-motion pre-warp + correlation cost volume + FB consistency + TV-L1 regularization** (add TV-L1 as a Tier-4 branch), the achievable quality lands roughly at **PWC-Net 2018 level** — Sintel-clean EPE ~2.0–2.5px equivalent. That's genuinely usable for game FG because:

- Game scenes have **much narrower motion distribution** than Sintel/KITTI (no wildly non-rigid subjects, most motion is camera-induced global + a few rigid objects)
- Global-motion pre-warp handles the dominant motion component
- Sub-pixel refinement handles texture crawl
- FB consistency + occlusion mask lets `wfg_synth` cleanly fall back to real pixels where flow is untrustworthy

### Where classical hits its ceiling and needs learning
1. **Transparent particles** (fire, smoke, glass) — brightness constancy breaks; classical flow smears
2. **Reflective / refractive surfaces** (mirrors, water) — the "moving" content doesn't correspond to the underlying geometry
3. **Camera-orthogonal parallax at close range** (fast strafe past a pillar) — flow discontinuities exceed the cost-volume search radius
4. **Motion-blurred input frames** — the underlying signal is already gone

For (1)–(3) you need either engine-provided motion vectors (DXVK-side cooperation) or a small learned prior (~1–4M param mobile network) that fills in unresolved regions. For (4) you need pre-DXVK access to the pre-blur frames, which is architecturally impossible from a Vulkan implicit layer.

**Practical estimate:** classical (Tier-1 through Tier-4) can hit ~85–90% of DLSS3-FG's perceived quality on titles without heavy transparency, at ~2–5% of the compute cost, and stays entirely inside win-fg's current architecture — no weight files, no training pipeline, no license risk.

## Sources
- [RAFT: Recurrent All-Pairs Field Transforms for Optical Flow (arxiv:2003.12039)](https://arxiv.org/abs/2003.12039)
- [PWC-Net (CVPR 2018)](https://openaccess.thecvf.com/content_cvpr_2018/papers/Sun_PWC-Net_CNNs_for_CVPR_2018_paper.pdf)
- [GMFlow (CVPR 2022)](https://openaccess.thecvf.com/content/CVPR2022/papers/Xu_GMFlow_Learning_Optical_Flow_via_Global_Matching_CVPR_2022_paper.pdf)
- [SEA-RAFT (arxiv:2405.14793)](https://arxiv.org/pdf/2405.14793)
- [NeuFlow v2 (arxiv:2408.10161)](https://arxiv.org/pdf/2408.10161)
- [FastFlowNet (arxiv:2103.04524)](https://arxiv.org/pdf/2103.04524)
- [CompactFlowNet (arxiv:2412.13273)](https://arxiv.org/html/2412.13273v1)
- [Fast Optical Flow Using Dense Inverse Search (Kroeger et al. ECCV 2016)](https://ar5iv.labs.arxiv.org/html/1603.03590)
- [TV-L1 GPU implementation notes (GTC 2014)](https://linchaobao.github.io/gtc2014/TVL1flow_GTC2014_notes.pdf)
- [AMD FSR 3 (GPUOpen)](https://gpuopen.com/fidelityfx-super-resolution-3/)
- [Occlusion and Consistency Aware Interpolation (arxiv:2403.18092)](https://arxiv.org/html/2403.18092v1)
- [Robust Global Motion Estimation (arxiv:1911.01734)](https://arxiv.org/pdf/1911.01734)
