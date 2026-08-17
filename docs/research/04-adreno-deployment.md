# 04 — Adreno / Android Deployment for Compute-Heavy FG + ML

Research pass 2026-08-17. Scope: how compute-heavy FG (and eventual ML
inference) actually runs on Adreno / Android — runtimes, Vulkan
extensions, licensing, real shipping FG on mobile, and a reality check.

## Runtime landscape table

| Runtime | Availability | Perf on Adreno 750 | Integration complexity | Provenance risk |
|---|---|---|---|---|
| **QNN SDK / QAIRT** | Maven Central `com.qualcomm.qti:qnn-runtime:2.34.0`, public. HTP (NPU) + Adreno GPU + CPU backends. Snapdragon-only. | Very fast on HTP (0.04ms for small ops; ~1000 tok/s LLM prefill on 8 Gen 3). Best throughput of any listed. | Medium — ONNX→QNN conversion tool, `qnn-htp-*` context binaries per SoC. | Runtime is Qualcomm-proprietary but free-use. Models you ship must be your own. |
| **LiteRT (ex-TFLite)** | Google's replacement for NNAPI (Android 15 deprecated NNAPI). GPU delegate = OpenCL on Adreno (~2× OpenGL). Auto-FP16. | Solid; OpenCL kernels are Adreno-tuned. Slower than QNN HTP but far more portable. | Low — 1-line delegate swap; ONNX→TFLite is straightforward. QNN LiteRT delegate exists for hybrid. | Apache 2 runtime, permissive. |
| **ONNX Runtime Mobile** | Multi-EP: QNN EP (Snapdragon), NNAPI EP (deprecated), CoreML (iOS). | Depends on EP. Good for cross-platform. | Medium — sessions/EPs to wire. | MIT. |
| **NCNN + Vulkan backend** | BSD-3. Cross-vendor mobile-native inference. What Allen Kuo's RIFE-on-Android uses. | Achieves 90%+ Adreno 750 GPU util at 480p–4K for RIFE. | Low-medium — pure Vulkan compute, no Qualcomm dep. | BSD-3 runtime, permissive. |
| **NNAPI** | Deprecated Android 15 (still works for back-compat). Existing NNAPI code keeps running. | Was OK; being retired. | N/A — don't build new against it. | AOSP. |
| **Raw Vulkan compute** (what win-fg does today) | Universal. Works everywhere Vulkan 1.1+ runs. | Slower than QNN HTP for matmul-heavy nets. Fine for classical FG. | Zero new dependency. | Fully clean. |

## Vulkan extensions on Turnip / Adreno (confirmed)

- **VK_KHR_present_wait** — SUPPORTED on Turnip; requires driconf `vk_khr_present_wait=true`. Frame-pacing tool.
- **VK_EXT_present_timing** — Added in **Mesa 26.1.0** for Turnip (May 2026). Better than present_wait for precise scheduling. **Use this for our pacing branch.**
- **VK_QCOM_image_processing** — Added Mesa 26.1.0 for Turnip. Includes weighted sample / box-filter / block-match — relevant for warp/filter ops.
- **VK_KHR_ray_query** — Supported (Phoronix confirmed).
- **VK_KHR_shader_float16_int8** — Standard extension, generally available on Adreno via Turnip; enable for FP16 compute throughput.
- **VK_KHR_cooperative_matrix** — **NOT reliably available on Adreno/Turnip.** Allen Kuo's RIFE-on-Android article explicitly says: *"Cooperative matrix was investigated as a capability-gated path. The fallback to standard Vulkan compute is not a failure."* i.e., treat as opportunistic, not a base assumption.
- **Qualcomm proprietary driver** — generally exposes more ML-oriented extensions than Turnip, but users on our audience run mixed drivers.

## Real mobile FG shipping (2026)

1. **LSFG-Android (FrankBarretta)** — most widely adopted Winlator FG. Compositor-side via MediaProjection + system overlay (NOT Vulkan layer — Android 12+ blocks that). **Requires user to supply their own `Lossless.dll` (proprietary Lossless Scaling license) — the app extracts shaders from it on-device.** MIT root + custom license (no Play Store, no commercial). Adreno 7xx+. Integrated into GameNative v0.9.1. **This is the exact provenance pattern that killed bionic-fg.** Currently the "reference" mobile FG people compare against.
2. **RIFE via NCNN Vulkan (Allen Kuo blog)** — clean-room-viable path: RIFE MIT model, NCNN BSD-3, Vulkan compute universal. 90%+ Adreno 750 GPU util at 480p–4K. Real deployment evidence exists.
3. **Qualcomm SGSR 2** — SPATIAL/TEMPORAL **UPSCALER**, NOT frame gen. Open source (SnapdragonGameStudios/snapdragon-gsr). Qualcomm has **not shipped mobile FG** and PC Gamer notes SGSR 2 "lacks AI smarts" vs DLSS. Not a near-term FG source.

## Recommended paths

### If we ship a learned model (weeks-of-work track)
- Base on **RIFE-4.x (MIT)** for provenance safety.
- Runtime = **NCNN Vulkan backend** as primary (cross-vendor, no Qualcomm lock-in), with optional **QNN HTP delegate** for Snapdragon users (biggest install base among Bannerlator devices).
- Fallback = LiteRT GPU delegate for anything else.
- Conversion: PyTorch → ONNX → both NCNN's `onnx2ncnn` and TFLite converter.
- Ship both `.param/.bin` (NCNN) and `.tflite` files as APK assets.
- Model size 8–12 MB fp16.

### If we stay classical (recommended for now)
- Keep raw Vulkan compute.
- Enable **VK_KHR_shader_float16_int8** for FP16 throughput on flow shaders.
- Wire **VK_EXT_present_timing** (Mesa 26.1+) for pacing — falls back to **VK_KHR_present_wait** on older Turnip.
- Consider **VK_QCOM_image_processing** for the warp pass (native block-match / weighted-sample ops could replace parts of `wfg_synth`).
- No new runtime dependency; ships as-is in `libwin_fg.so`.

## Reality check — DLSS3-FG-quality on mobile

**2026 problem, not 2027 — but only via the RIFE-Vulkan-NCNN path, not via Qualcomm-native.** Qualcomm has not shipped FG (SGSR 2 is upscale-only); their public roadmap has no FG entry. Allen Kuo's Medium series (April 2026) documents RIFE-on-Android running at real-time frame rates via NCNN Vulkan on Adreno 750, so the raw capability exists today. The gap is a clean, MIT-provenance, shippable RIFE derivative + the zero-copy `AHardwareBuffer → VkImage → NCNN → back to surface` pipeline (which is non-trivial: UBWC handling, per-frame ownership discipline, no CPU staging).

The mobile FG that actually ships in emulators today (**LSFG-Android**) achieves 30→100 FPS (4× mode) but has the exact provenance liability that killed bionic-fg — users bring the tainted DLL themselves as a workaround, but any project that bundles it is exposed.

**Bottom line:** clean-room mobile-FG parity with DLSS3-FG is technically reachable now via RIFE + NCNN Vulkan; the missing pieces are (a) our own trained/fine-tuned RIFE variant with data-provenance discipline, (b) the AHardwareBuffer zero-copy pipeline, (c) VK_EXT_present_timing pacing. None require Qualcomm to ship anything new.

## Sources
- [QNN Maven / SDK access — Edge Impulse guide](https://docs.edgeimpulse.com/tutorials/topics/android/qnn-acceleration)
- [Hexagon NPU SDK — Qualcomm Developer](https://www.qualcomm.com/developer/software/hexagon-npu-sdk)
- [SNPE deprecated in favor of QNN — ONNX Runtime discussion](https://github.com/microsoft/onnxruntime/discussions/16214)
- [NNAPI deprecated Android 15 — Android NDK Migration Guide](https://developer.android.com/ndk/guides/neuralnetworks/migration-guide)
- [LiteRT delegates overview](https://ai.google.dev/edge/litert/performance/delegates)
- [LiteRT GPU delegate on Adreno / OpenCL](https://blog.tensorflow.org/2020/08/faster-mobile-gpu-inference-with-opencl.html)
- [Mesa 26.1.0 release notes — Turnip present_timing + QCOM_image_processing](https://docs.mesa3d.org/relnotes/26.1.0.html)
- [Turnip ray query support — Phoronix](https://www.phoronix.com/news/Mesa-TURNIP-VK_KHR_ray_query)
- [VK_KHR_present_wait on Turnip via driconf — Phoronix](https://www.phoronix.com/news/Mesa-VK_KHR_present_wait)
- [VK_EXT_present_timing — Khronos blog](https://www.khronos.org/blog/vk-ext-present-timing-the-journey-to-state-of-the-art-frame-pacing-in-vulkan)
- [VK_KHR_cooperative_matrix proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_cooperative_matrix.html)
- [Snapdragon GSR 2 — Qualcomm blog (upscale, not FG)](https://www.qualcomm.com/developer/blog/2024/10/introducing-snapdragon-game-super-resolution-2)
- [SGSR 2 lacks AI smarts vs DLSS — PC Gamer](https://www.pcgamer.com/hardware/graphics-cards/qualcomm-upgrades-its-gaming-upscaler-from-spatial-to-temporal-tech-but-it-lacks-ai-smarts-and-may-struggle-to-compete-with-nvidias-highly-polished-dlss-performance/)
- [LSFG-Android repo (FrankBarretta) — MediaProjection + user-supplied Lossless.dll](https://github.com/FrankBarretta/LSFG-Android)
- [LSFG comes to Android via GameNative — Retro Handhelds](https://retrohandhelds.gg/lsfg-comes-to-android-via-gamenative-tripling-frame-rates-in-pc-game-tests/)
- [GPU-Resident Frame Interpolation on Android (Allen Kuo, April 2026)](https://allenkuo.medium.com/gpu-resident-frame-interpolation-on-android-e9558d19cfab)
- [Building RIFE on Android with Vulkan + NCNN (Allen Kuo)](https://allenkuo.medium.com/building-a-high-performance-ai-frame-interpolation-pipeline-on-android-with-vulkan-ncnn-rife-8f279cef51cd)
- [RIFE 4.25 model card — ~23MB fp32](https://huggingface.co/mlx-community/RIFE-4.25)
- [FastRIFE paper — optimized RIFE for real-time](https://arxiv.org/pdf/2105.13482)
- [HeteroInfer — QNN benchmarks on 8 Gen 3](https://arxiv.org/html/2501.14794v2)
