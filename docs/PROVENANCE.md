# win-fg — sources & licenses

Where every part of win-fg comes from, how it's built, and under what license.

> Verified current as of 2026-08-12 (through device bring-up).

## What win-fg is

A color-only frame-generation layer for Android / Vulkan (Turnip / Adreno). From
two real frames it synthesises an interpolated in-between frame to raise perceived
frame rate. Two stages — optical-flow motion estimation, then frame synthesis —
wired into a standard Vulkan implicit layer.

## Shaders

Motion estimation is our own subgroup-free reimplementation of the **AMD
FidelityFX FSR3 optical flow** algorithm (MIT); synthesis is written from first
principles. Full FidelityFX attribution in
[`NOTICE_FIDELITYFX_OPTICALFLOW.md`](../NOTICE_FIDELITYFX_OPTICALFLOW.md).

| Component | File | Built from | License |
|---|---|---|---|
| Perceptual luma | `shaders/of3_luma.comp` | our adaptation of FSR3 `prepare_luma` | MIT (AMD FidelityFX) |
| Luma pyramid | `shaders/of3_downsample.comp` | our adaptation of FSR3 luminance pyramid | MIT (AMD FidelityFX) |
| Coarse→fine flow | `shaders/of3_flow.comp` | our subgroup-free reimpl of the FSR3 SAD block search | MIT (AMD FidelityFX) |
| Flow expand | `shaders/of3_expand.comp` | our adaptation of the FSR3 scale pass | MIT (AMD FidelityFX) |
| Bidirectional flow | `shaders/of3_flow_m4.comp` | our extension (independent fwd + bwd search) | MIT (ours) |
| Occlusion-gated expand | `shaders/of3_expand_m4.comp` | our extension of the above | MIT (ours) |
| **Frame synthesis** | `shaders/wfg_synth.comp` | **written from first principles** | MIT (ours) |

## Synthesis math

`wfg_synth.comp` is motion-compensated interpolation: a bidirectional backward
warp, an importance-weighted (softmax) blend of the two warped candidates, a
flow-magnitude clamp, and a photometric + forward/backward-consistency gate that
cross-fades where the flow is unreliable. The underlying math is **reimplemented
from published papers** (equations only — no third-party code):

| Idea | Source |
|---|---|
| Importance-weighted blend ("softmax splatting") | Niklaus & Liu, CVPR 2020 (arXiv:2003.05534) |
| Brightness-constancy photometric residual | Horn & Schunck, 1981 |
| Forward/backward consistency (occlusion) | Sundaram et al. 2010; OCAI (arXiv:2403.18092) |
| Dual disocclusion / reprojection fallback | AMD FidelityFX FSR3 frame interpolation (MIT) |

## Host layer & tooling — win-fg's own code

Everything under `src/` and `tools/`, plus the build and manifest files, is
win-fg's own code, written from the open Vulkan-layer references below.

| Files | What |
|---|---|
| `src/layer.cpp` | Vulkan implicit layer: negotiation, dispatch, swapchain + present interception, frame insertion |
| `src/framegen.{hpp,cpp}`, `src/record_impl.inc` | compute engine: pipelines, per-size resources, the flow→synth dispatch graph |
| `src/vk_dispatch.hpp`, `src/config.hpp`, `src/log.hpp` | dispatch tables, runtime config + hot-reload, logging |
| `src/embedded_shaders.hpp`, `tools/embed_spv.py` | generator + embedded SPIR-V of win-fg's own shaders |
| `CMakeLists.txt`, `manifest/*.json`, `tools/build_shaders.sh`, `VERSION` | build, layer manifest, shader compile, version |

Written from these open, permissively-licensed references:

| Part | Reference | License |
|---|---|---|
| Layer skeleton (negotiation, dispatch, present interception) | Khronos Vulkan-Loader layer interface; renderdoc "Vulkan layer guide" | Apache-2.0 / CC-BY |
| Vulkan headers (`vk_layer.h`, `vulkan.h`) | KhronosGroup/Vulkan-Headers | Apache-2.0 |
| Compute pipeline / descriptor patterns | Khronos Vulkan-Samples; Vulkan specification | Apache-2.0 |

## How it fits together

```
[ real prev frame ]   [ real curr frame ]
        │                     │
        ▼                     ▼
  of3_luma → of3_downsample → of3_flow → of3_expand      (FSR3 optical flow, MIT)
        │
        ▼   flowFwd (curr→prev) + flowBwd (prev→curr) + confidence
        │
      wfg_synth      warp + importance blend + consistency gate      (written here, MIT)
        │
        ▼
[ generated in-between frame ]
```

## License

win-fg is **MIT**. The FidelityFX-derived optical-flow passes are MIT — see
[`NOTICE_FIDELITYFX_OPTICALFLOW.md`](../NOTICE_FIDELITYFX_OPTICALFLOW.md).
Every third-party reference used is listed above under its own permissive license.
