# 05 — Prior-Art Landscape for Real-Time Frame Generation

Research pass 2026-08-17. Scope: publicly-documented FG projects — what
shipped, what worked, what failed, and what got taken down. This is the
provenance-hygiene reference we build all clean-room decisions against.

## Prior-art comparison

| Project | Year | Approach | License | Takedown / provenance | Safe to study? |
|---|---|---|---|---|---|
| **DLSS 3 FG** (NVIDIA) | 2022 | Conv autoencoder + Ada OFA hardware; inputs = curr+prev+OFA-flow+game MV/depth | Proprietary | NDA-only whitepapers; safe to read public blogs, not to reproduce weights | Read-only |
| **FSR 3 FG** (AMD) | 2023 | Interpolation + swap-chain proxy + 3 UI-composite options; MV+depth required | **MIT (GPUOpen)** | Clean origin; motion-vector inputs mandatory | ✅ Safe to reuse code and lift patterns |
| **FSR 3.1** (AMD) | 2024 | Adds ellipsoid color clamp (ghosting fix), velocity factor (bright-pixel flicker), frame distortion texture | MIT | Same | ✅ |
| **ExtraSS** (Intel) | SIGGRAPH Asia 2023 | Frame *extrapolation* (curr → next), not interpolation → lower latency, no MV lookahead | Research paper only, no shipped code | Clean idea, no code to copy | ✅ Read paper |
| **RIFE / Practical-RIFE** | 2020-2026 | IFNet, 5 coarse-to-fine IFBlocks, LeakyReLU, ResConv+β | **MIT** (both hzwer repos) | Trained on Vimeo90K (51,312 triplets 448×256) — research dataset | ✅ Clean base for fine-tuning |
| **FILM** (Google Research) | ECCV 2022 | Motion-based interp with large-motion focus | **MIT** | Clean | ✅ |
| **IFRNet** | CVPR 2022 | Intermediate feature refinement network | **MIT** | Clean | ✅ |
| **XVFI** | ICCV 2021 | 4K arbitrary-time VFI | **Non-commercial research only** | Usable for eval, not for shipping | ⚠️ Don't ship |
| **rife-ncnn-vulkan** (nihui) | 2020-2026 | RIFE via NCNN+Vulkan on mobile GPUs | MIT app + BSD-3 NCNN | Reference impl for Android RIFE | ✅ Direct model for our ML path |
| **NCNN** (Tencent) | 2017+ | BSD-3, pure-C++ Vulkan-compute inference | **BSD-3-Clause** | Tencent-maintained, permissive | ✅ Our runtime if we go ML |
| **SVP / SVPFlow** | 2010+ | SVPFlow1 (motion vectors, MVTools2-derived) + SVPFlow2 (rendering) | **SVPFlow1 GPL, SVPFlow2 closed** | 20-year clean track record via split-license model | ✅ Study SVPFlow1 |
| **LSFG** (Lossless Scaling) | 2024→3.0 Jan 2026 | ML frame-interp; on Steam; 40% GPU-load reduction in v3, up to ×20 multiplier | Proprietary, closed, Steam-only | **Never redistribute the DLL or its extracted shaders** | ⚠️ Hard no on any bytecode reuse |
| **lsfg-vk** (PancakeTAS) | 2024 | Vulkan implicit layer; extracts SPIR-V from user-owned Lossless.dll at runtime | GPL-3.0 layer | User must own Steam Lossless Scaling; no bundled proprietary bytes | ✅ Study the extraction pattern (not our model) |
| **Magpie** (Blinue) | 2020+ | Spatial upscaler only; FG explicitly rejected | GPL-3.0 | Clean | ✅ Reference for shader plumbing, no FG to steal |
| **SGSR 2** (Qualcomm) | 2024 | Temporal upscaling for Adreno; NOT frame-gen | Proprietary/Adreno | Clean but not our tool | Reference-only |
| **GameNative 1.0** (Winlator fork) | June 2026 | Vulkan renderer + integrated **lsfg-vk** for FG | GPL | Requires user-owned Lossless Scaling | Competitor uses the "user brings dll" pattern |
| **Winlator-Ludashi** | 2025-26 | Vulkan renderer, no public FG surfaced | GPL | ✅ | No FG to compete with |
| **Max's WinNative PR #537** | 2026-06 open | In-renderer CNN-style RAFT-family pipeline; ~20KB hand-crafted fp16 coeff buffers | Project license | 8 `wnfg_XX_spv.h` blobs w/o `.comp` source — verify provenance | Mixed — see `06-max-winnative-inspirations.md` |

## What killed bionic-fg specifically

Confirmed from search results and ecosystem context: **bionic-fg statically
bundled the SPIR-V shader blobs that lsfg-vk extracts at runtime from the
user-owned Lossless.dll.** The "extraction on-device" is what makes lsfg-vk
defensible: the user owns the DLL, the layer just repackages it live in
that user's environment. Bionic-fg baked the compiled shader bytecode as
constant arrays inside `libbionic_fg.so`, then shipped that `.so` as a
public APK asset. That crosses the line from "layer that runs your DLL"
to "redistribution of the model." The specific artifacts were the compiled
compute shaders holding LSFG's model weights — not new code, but new
re-distribution of proprietary bytecode.

**win-fg's shield:** we own every shader we ship. Every `.comp` in
`shaders/` has a paper citation in its header. Every SPIR-V blob in
`build/spv/` is generated from a `.comp` we own. Zero third-party
bytecode enters `libwin_fg.so`.

## Lessons from SVP's 20-year run

- **Split license model works.** SVPFlow1 (motion search) is GPL and open;
  SVPFlow2 (rendering / frame synthesis) is closed. They ship 23 rendering
  modes (algorithms 1, 2, 11, 13, 21, 22, 23) — one FG algorithm never
  fits all content. Our `feat/block-sad-motion` alongside our `of3_flow`
  mirrors this exact "multiple estimators, host picks" pattern.
- **Scene handling + block masking** is where SVP puts UI/hard-cut
  robustness. Their handling of scene cuts is the pattern behind
  Isygold's dominance-buffer stats — you measure the regime, then
  dispatch a different rendering mode per regime.

## What FSR3-FG actually gives us (MIT, real)

Repo: `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`, `release-FSR3-3.0.3`
branch. Ships:

- **Full HLSL shader source** for motion generation + frame interpolation + composition passes
- **Proxy DXGI swap chain** that handles frame pacing + UI compose + present
- **3 documented UI-composition strategies** (composite UI on a separate RT, callback for engine to re-render UI, identify UI by diffing with/without-UI frames — literally an HUD-mask detector)
- **Motion Vector Generation** helper for third-party upscalers
- **3.1.2 ghosting fixes:** ellipsoid color clamp (was AABB — the exact fix for the "streak" ghosting we're seeing), velocity factor for bright-pixel flicker
- **DirectX 12 + Vulkan + UE5** target
- **Requires host game to provide motion vectors + depth** — the ceiling for our post-DXVK layer position

We can't drop FSR3 in whole (needs MV/depth from the game), but the
**3.1.2 color-clamp fix (ellipsoid vs AABB)** is directly portable to
`wfg_synth`, and the **"identify UI by diffing" HUD detector** is exactly
what our `feat/heuristic-hud-detect` needs — the algorithm is documented
in FSR3's docs and code.

## Winlator-ecosystem competitors

- **GameNative 1.0** ships lsfg-vk. They inherit lsfg-vk's user-must-own-Lossless-Scaling constraint. That's their model.
- **Winlator-Ludashi (+Plus)** has a Vulkan renderer but no public FG. Open door for us.
- **Max's WinNative PR #537** — RAFT-family CNN-style pipeline in the renderer. Coeff buffers are small (~20KB) so not learned weights, but 8 `wnfg_XX_spv.h` pre-compiled blobs lack source — provenance unverified. Full analysis in `06-max-winnative-inspirations.md`.
- **Isygold Vegas-DXVK** — 3-pass block SAD + median + warp. Sits inside DXVK. See `07-isygold-vegas-inspirations.md`.

## Provenance red lines — exact things that got projects in trouble

1. **Don't bundle extracted proprietary shader bytecode**, ever — that's what killed bionic-fg. Runtime extraction from a user-owned licensed DLL (lsfg-vk model) is the only defensible LSFG path, and it means shipping the extractor + requiring the user to buy LSFG.
2. **Don't reuse XVFI weights** — non-commercial license. Fine to cite the paper.
3. **Don't disassemble Max's `wnfg_XX_spv.h`** with intent to re-implement. Even reading proprietary/unclear-provenance bytecode contaminates the clean-room record.
4. **Don't scrape gameplay video from YouTube/Twitch for training data** — copyrighted. Self-captured under `win-fg`'s own capture mode is the only clean route.
5. **Do fine-tune from RIFE / IFRNet / FILM (MIT).** Do use NCNN (BSD-3) as runtime. Do lift patterns from FSR3 (MIT) with attribution.
6. **Do maintain a `THIRD-PARTY.md`** naming every borrowed pattern + its origin + its license.

## Sources
- [Lossless Scaling LSFG 3.0](https://www.dsogaming.com/articles/weve-tried-lossless-scaling-3-0-with-multi-frame-generation/)
- [lsfg-vk on GitHub (PancakeTAS)](https://github.com/PancakeTAS/lsfg-vk)
- [Decky LSFG-VK plugin](https://github.com/xXJSONDeruloXx/decky-lsfg-vk)
- [LSFG-Android (FrankBarretta) — user-supplies Lossless.dll](https://github.com/FrankBarretta/LSFG-Android)
- [SmoothVideo Project — SVPFlow docs](https://www.svp-team.com/wiki/Manual:SVPflow)
- [SVPFlow source-code forum post — GPL/closed split](https://www.svp-team.com/forum/viewtopic.php?id=4129)
- [open-svpflow reimplementation](https://github.com/Z1xus/open-svpflow)
- [NVIDIA DLSS 3 overview (OFA + autoencoder inputs)](https://www.nvidia.com/en-us/geforce/news/dlss3-ai-powered-neural-graphics-innovations/)
- [DLSS3 explanation (Digital Trends)](https://www.digitaltrends.com/computing/how-nvidia-dlss-3-works/)
- [AMD FSR 3 GPUOpen page](https://gpuopen.com/fidelityfx-super-resolution-3/)
- [FSR3 source announcement (GPUOpen)](https://gpuopen.com/news/fsr3-source-available/)
- [FSR 3.1 release notes](https://gpuopen.com/learn/amd_fsr_3_1_release/)
- [FSR 3.0.3 GitHub branch](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/release-FSR3-3.0.3)
- [Intel ExtraSS paper coverage](https://www.techpowerup.com/316835/extrass-framework-paper-details-intels-take-on-frame-generation)
- [Magpie FAQ — no FG planned](https://github.com/Blinue/Magpie/discussions/852)
- [Snapdragon GSR 2](https://www.qualcomm.com/developer/blog/2024/10/introducing-snapdragon-game-super-resolution-2)
- [rife-ncnn-vulkan (nihui)](https://github.com/nihui/rife-ncnn-vulkan)
- [RIFE Practical (hzwer, MIT)](https://github.com/hzwer/Practical-RIFE)
- [FILM (Google Research, MIT)](https://github.com/google-research/frame-interpolation)
- [IFRNet paper](https://openaccess.thecvf.com/content/CVPR2022/papers/Kong_IFRNet_Intermediate_Feature_Refine_Network_for_Efficient_Frame_Interpolation_CVPR_2022_paper.pdf)
- [NCNN (Tencent, BSD-3)](https://github.com/Tencent/ncnn)
- [Allen Kuo: Android RIFE pipeline on Adreno 750](https://allenkuo.medium.com/building-a-high-performance-ai-frame-interpolation-pipeline-on-android-with-vulkan-ncnn-rife-8f279cef51cd)
- [GameNative 1.0 pre-release (uses lsfg-vk)](https://retrohandhelds.gg/gamenative-finally-reaches-1-0-pre-release/)
- [Winlator-Ludashi (Vulkan renderer)](https://github.com/StevenMXZ/Winlator-Ludashi)
- [Bannerlator (win-fg is our clean-room replacement)](https://github.com/The412Banner/Bannerlator)
