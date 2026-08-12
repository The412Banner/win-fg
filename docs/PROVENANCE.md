# win-fg — provenance & clean-room boundary

This document is the authoritative record of where every part of win-fg comes
from. It exists so the licensing problem that took down the upstream bionic-fg
project **cannot recur here**. If a change would violate a rule below, it does
not go in.

## Background (why this repo exists)

The upstream `bionic-fg` frame-generation layer, and the GameScope
`libGameScopeVK.so` it descends from, were found to embed **model weights
essentially identical to the proprietary fp16 Lossless Scaling frame-generation
model**. Those weights are proprietary; anything derived from them is
incompatible with an open-source license and cannot be distributed. Upstream
took its repository down and asked downstream forks to do the same.

Our previous frame-gen models 3 and 4 used a **clean, MIT optical-flow front
end** (our own adaptation of AMD FidelityFX FSR3 optical flow) but then fed the
result into the **traced Lossless-derived "stage 6"** — the warp pass
(`shader_14`) and synthesis pass (`shader_04`). That back end is the tainted
part. win-fg keeps the clean front end and replaces the back end with an
independently-written one.

## What is IN this repo, and its origin

| Component | Files | Origin | License |
|---|---|---|---|
| Perceptual luma | `shaders/of3_luma.comp` | our adaptation of FSR3 `ffx_opticalflow_prepare_luma` | MIT (FidelityFX) |
| Luma pyramid | `shaders/of3_downsample.comp` | our adaptation of FSR3 luminance pyramid | MIT (FidelityFX) |
| Coarse→fine flow (m3) | `shaders/of3_flow.comp` | our subgroup-free reimpl of FSR3 SAD search | MIT (FidelityFX) |
| Flow expand (m3) | `shaders/of3_expand.comp` | our adaptation of FSR3 scale pass | MIT (FidelityFX) |
| Block/bidir flow (m4) | `shaders/of3_flow_m4.comp` | our extension of the above | MIT (ours) |
| Occlusion-gated expand (m4) | `shaders/of3_expand_m4.comp` | our extension of the above | MIT (ours) |
| **Interpolated-frame synthesis** | **`shaders/wfg_synth.comp`** | **written from first principles for win-fg** | **MIT (ours)** |

`wfg_synth.comp` is textbook motion-compensated interpolation: bidirectional
backward warp under a linear-motion assumption, linear temporal blend, and a
confidence-gated cross-fade fallback. It was written from the algorithm, not
from `shader_04`.

## What is DELIBERATELY EXCLUDED — never add these

The following are the tainted / proprietary-derived assets. They must never
enter this repo in any form (source, embedded SPIR-V, or reimplementation-from):

- The traced embedded shader table `shaders_embedded.hpp` (the ~54 Lossless-
  lineage SPIR-V blobs) — **excluded in full**.
- Model **0** ("full OF chain") and model **1** ("traced graph") dispatch code.
- Model **2** ("V2 engine"): `kV2ShaderMap`, any `libGameScopeV2.so` variants.
- Stage-6 traced passes: warp `shader_14`, synthesis `shader_04`, and their
  confidence/feature intermediates (`shader_05/06/26/27/28`, `shader_03`,
  `shader_09..53`).
- Anything traced from, disassembled from, or numerically derived from Lossless
  Scaling or GameScope binaries.

**Rule of thumb — weights, not vibes:** if a pass carries learned or traced
numeric values (a table, a weight blob, a hand-tuned constant lifted from a
disassembly), it does not belong here. Optical flow is an *algorithm* with no
learned weights, which is why the front end is safe; the synthesis we now write
is likewise pure algorithm.

## The clean boundary, in one line

```
[ real prev frame ] [ real curr frame ]
          │                  │
          ▼                  ▼
   our FSR3 optical flow  (of3_luma → downsample → flow → expand)   ← MIT, no weights
          │
          ▼   flowExpA (curr→prev, conf) + flowExpB (prev→curr, conf)
          │
   our clean synthesis   (wfg_synth: warp + blend + confidence cross-fade)   ← written here
          │
          ▼
   [ generated in-between frame ]
```

Everything above this line is ours or MIT. Nothing below the old boundary
(traced warp/synth) is present. There is no path by which a proprietary weight
reaches the output.

## Host layer & synthesis math — sources (added with the Vulkan host)

The Vulkan implicit-layer host (`src/layer.cpp`, `src/framegen.*`,
`src/vk_dispatch.hpp`) was written from these open, permissively-licensed
references. None of it is copied from bionic-fg / lsfg / GameScope.

| Part | Sourced from | License |
|---|---|---|
| Layer negotiation / dispatch / present interception skeleton | Khronos Vulkan-Loader layer interface; renderdoc "Vulkan layer guide" | Apache-2.0 / CC-BY (docs) |
| Vulkan headers (`vk_layer.h`, `vulkan.h`) | KhronosGroup/Vulkan-Headers | Apache-2.0 |
| Compute pipeline / descriptor patterns | Khronos Vulkan-Samples; general Vulkan spec | Apache-2.0 |

Synthesis **math** (implemented independently from the published equations — the
math itself is not copyrightable; we do not use these projects' code):

| Idea | Source |
|---|---|
| Importance-weighted blend of warped candidates ("softmax splatting") | Niklaus & Liu, CVPR 2020 (arXiv:2003.05534) — *code is non-commercial; we use only the published math* |
| Brightness-constancy photometric residual | Horn & Schunck 1981 |
| Forward/backward flow consistency occlusion test | Sundaram et al. 2010; OCAI (arXiv:2403.18092) |
| Dual (prev/curr) disocclusion fallback concept | AMD FidelityFX FSR3 Frame Interpolation (MIT) |

**Explicitly NOT used:** lsfg-vk and obs-vkcapture (GPL / avoided lineage), and
the softmax-splatting *code* (non-commercial). Only permissive code and
published math enter win-fg.
