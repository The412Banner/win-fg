# 08 — Permissively-Licensed Shaders We Can Reuse

Research pass 2026-08-17. Scope: hunt for GLSL/HLSL/SPIR-V compute shaders
under permissive licenses (MIT/BSD/Apache/zlib) that could feed win-fg
directly — either as-is or as clean-room reference implementations.

## Grouped list

### FSR3-FG — GPUOpen `FidelityFX-SDK` (MIT, © AMD 2023)

Two ref branches: `release-FSR3-3.0.4` (both HLSL+GLSL variants) and
`main` (HLSL only, newer). Every FG pass exists as compilable source.

| Shader | What it does | Portable to us? |
|---|---|---|
| `ffx_frameinterpolation_setup_pass.{hlsl,glsl}` | Per-frame constant/uniform setup | ✅ Direct port |
| `ffx_frameinterpolation_reconstruct_previous_depth.{hlsl,glsl}` | Depth reproject | ❌ Needs game MV/depth |
| `ffx_frameinterpolation_game_motion_vector_field.{hlsl,glsl}` | Game-MV field | ❌ MV-required |
| `ffx_frameinterpolation_optical_flow_vector_field.{hlsl,glsl}` | OF pyramid → per-pixel MV field | ✅ **Directly portable** — mature version of what our `of3_expand` does |
| `ffx_frameinterpolation_disocclusion_mask.{hlsl,glsl}` | Occlusion mask from FB-consistency | ✅ **Directly portable** — matches our planned `of3_fb_consist.comp` |
| `ffx_frameinterpolation_compute_inpainting_pyramid_pass.{hlsl,glsl}` | Pyramid to fill flow-field holes | ✅ **Portable, valuable** — solves the "black hole where flow was rejected" problem we don't have anything for |
| `ffx_frameinterpolation_game_vector_field_inpainting_pyramid_pass.{hlsl,glsl}` | Same, MV variant | ⚠️ Adapt to drop MV branch |
| `ffx_frameinterpolation_inpainting_pass.{hlsl,glsl}` | Actual inpainting from pyramid | ✅ **Portable, valuable** |
| `ffx_frameinterpolation_pass.{hlsl,glsl}` | Main interpolation kernel | ⚠️ Portable but MV-flavored — needs adaptation to drop game-MV path |
| `ffx_frameinterpolation_debug_view_pass.{hlsl,glsl}` | Visualiser | ✅ Drop-in for our own bring-up |

FSR3 optical-flow half (`sdk/include/FidelityFX/gpu/opticalflow/*.h` +
`samples/.../shaders/opticalflow/*_v5.{hlsl,glsl}`):
- `compute_optical_flow_advanced_v5` — luminance-pyramid + coarse-to-fine solve
- `filter_optical_flow_v5` — post-process filter
- SCD (scene-change divergence)
- **All portable, directly replace pieces of our `of3_*`.**

### RIFE inference plumbing — `nihui/rife-ncnn-vulkan` (MIT)

16 GLSL compute shaders wired to run RIFE via NCNN's fp16-buffer
conventions:
- `rife_preproc.comp`, `rife_postproc.comp` (+ TTA variants) — layout/format shims
- `rife_v4_timestep.comp` (+ TTA) — timestep parameter fill for v4 IFNet
- `rife_flow_tta_avg.comp`, `rife_flow_tta_temporal_avg.comp`, `rife_v2_*`, `rife_v4_*` — test-time-augmentation averaging (skip for real-time)
- `rife_out_tta_temporal_avg.comp` — output TTA average
- `warp.comp`, `warp_pack4.comp`, `warp_pack8.comp` — **the RIFE-specific bilinear-warp shaders, portable, three vectorization variants**

### NCNN Vulkan compute-layer library — `Tencent/ncnn` (BSD-3)

~300 `.comp` shaders under `src/layer/vulkan/shader/`. The neural-inference
building blocks if we ever ship a learned model:

- **Convolution family:** `convolution_1x1s1d1_cm.comp`,
  `convolution_3x3s1d1_winograd23/43_*.comp`, `convolution_gemm_cm.comp`,
  plus `_int8`, `_pack1to4`, `_pack4`, `_pack8` variants (cooperative-matrix
  and non-CM paths).
- **`innerproduct*.comp`** (dense/matmul layers, ~18 variants).
- **`interp.comp`, `interp_bicubic.comp`, `interp_pack4.comp`** — upsampling.
- **`padding*.comp`, `packing_*_pack*.comp`** — layout ops.
- All fp16-aware, all Adreno-tested. **The complete stack for running
  RIFE-lite ourselves in raw Vulkan without depending on NCNN as a
  runtime.**

### Snapdragon GSR — `SnapdragonStudios/snapdragon-gsr` (BSD-3-Clause, Qualcomm)

- `sgsr/v2/include/glsl_2_pass_cs/sgsr2_convert.comp` + `sgsr2_upscale.comp` (2-pass compute)
- `sgsr/v2/include/glsl_3_pass_cs/sgsr2_{activate,convert,upscale}.comp` (3-pass compute)
- **Upscaler, NOT FG** — but Adreno-tuned reference for how Qualcomm
  writes compute shaders for their own hardware. Study for TBDR patterns.

### open-svpflow — `Z1xus/open-svpflow` (Apache 2.0)
Rust reimpl in progress; **no shaders yet**. Placeholder to watch.

### Isygold's Vegas-DXVK kit (zlib/libpng)
- `star_fg_motion_v2.comp` — block-SAD motion. Permissive, portable.
  Full analysis in `07-isygold-vegas-inspirations.md`.
- Compiled SPIR-V for median + warp passes (`fg_median.spv`, `fg_warp.spv`)
  — bytecode only, no `.comp` source. **Prefer to re-implement from
  same math** (Isygold's algorithms are well-documented in his README)
  rather than ship his bytecode.

## Top 5 to port — with cite + effort + reason

| # | Shader | Source | Effort | Why |
|---|---|---|---|---|
| 1 | `ffx_frameinterpolation_disocclusion_mask.glsl` | GPUOpen MIT | 4-6 hrs | Same algorithm as our Tier-1 `of3_fb_consist.comp` plan, but battle-tested. Use as reference implementation. |
| 2 | `ffx_frameinterpolation_inpainting_pass.glsl` + inpainting pyramid pair | GPUOpen MIT | 1-2 days | Directly attacks "black hole where flow was rejected" — nothing in our current stack does this. |
| 3 | `ffx_frameinterpolation_optical_flow_vector_field.glsl` | GPUOpen MIT | 1 day | More mature than our `of3_expand`; includes explicit fallback and dilation. Study side-by-side, adopt what improves us. |
| 4 | FSR3 optical-flow v5 pass (`compute_optical_flow_advanced_pass_v5.glsl` + `filter_optical_flow_pass_v5.glsl`) | GPUOpen MIT | 2-3 days | Alternate motion estimator alongside our `of3_flow`. Ship as user-selectable in the drawer. |
| 5 | RIFE `warp_pack4.comp` | nihui MIT | 0.5 day | If/when we ship a learned model, this is the exact fp16-aware bilinear-warp kernel. Reference for the "warp features by flow" ML step. |

## Red-flag list — do NOT port

| Repo | License | Reason |
|---|---|---|
| `HopperLogger/mpv-frame-interpolator` | **GPL-3.0** | OpenCL kernels look interesting but GPL would viral our MIT stance. Read for algorithm intuition only. |
| `HopperLogger/HopperRender` | **GPL-3.0** | Same. Kernels: `warpFrameKernel.cl`, `blurFlowKernel.cl`, `calcDeltaSumsKernel.cl`, `adjustOffsetArrayKernel.cl`, `determineLowestLayerKernel.cl`, `HopperFlow`. Do NOT port. |
| `PancakeTAS/lsfg-vk` | **GPL-3.0 layer + proprietary DLL runtime** | Extracts user-owned Lossless.dll at runtime. Do not touch its extraction path or any shader it produces. |
| `FFmpeg/libavfilter/vf_minterpolate.c` | **LGPL** | Algorithm well-documented but license would viral if statically linked. Use as academic reference, don't copy. |
| `NVIDIA/apex`, PWC-Net weights | **NVIDIA non-commercial research** | Same class as XVFI weights. |
| Any `.spv` blob shipped without matching `.comp` source | Provenance unknown | Including Isygold's median/warp SPIR-Vs — treat as read-only, re-implement from published math. |

## License summary for `THIRD-PARTY.md`
- **MIT-safe:** FidelityFX-SDK, rife-ncnn-vulkan, ffx_cauldron sample shaders
- **BSD-3 safe:** NCNN, SGSR
- **Apache-safe:** open-svpflow (nothing to port yet)
- **Zlib safe:** Isygold Vegas kit
- **Avoid:** any GPL/LGPL kernel, any bytecode without source

## Sources
- [FidelityFX-SDK release-FSR3-3.0.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/release-FSR3-3.0.4)
- [FidelityFX-SDK main (FSR3.1)](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/main)
- [rife-ncnn-vulkan (nihui)](https://github.com/nihui/rife-ncnn-vulkan)
- [Tencent/ncnn Vulkan layer shaders](https://github.com/Tencent/ncnn/tree/master/src/layer/vulkan/shader)
- [SnapdragonStudios/snapdragon-gsr](https://github.com/SnapdragonStudios/snapdragon-gsr)
- [Z1xus/open-svpflow](https://github.com/Z1xus/open-svpflow)
- [HopperLogger/HopperRender (GPL — reference only)](https://github.com/HopperLogger/HopperRender)
- [HopperLogger/mpv-frame-interpolator (GPL — reference only)](https://github.com/HopperLogger/mpv-frame-interpolator)
- [PancakeTAS/lsfg-vk (GPL + proprietary DLL — do not touch)](https://github.com/PancakeTAS/lsfg-vk)
