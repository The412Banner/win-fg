# 03 — Training Pipelines for Frame Interpolation

Research pass 2026-08-17. Scope: end-to-end how to train our own weights,
license-clean throughout — datasets, self-capture, losses, compute,
export path, tooling gaps.

## 1. Dataset landscape

| Dataset | Content | Volume | License (as of 2026-08) | Suitable for us? |
|---|---|---|---|---|
| **Vimeo90K (triplet)** | 91,701 triplets @ 448×256 from Vimeo | 33 GB train+test | Video content bound by Vimeo ToS; academic norm is fair-use pretraining. Original *code* license unclear; commercial ship of derived weights = legal gray. | **Pretraining only, not for release weights.** Widely used but not defensible if challenged. |
| **X4K1000FPS** (KAIST) | 175 scenes 4K @ 1000 fps, Phantom Flex | ~large | Code MIT (github.com/JihyongOh/XVFI). Dataset license unstated on public page. | Same gray zone as Vimeo90K. |
| **SNU-FILM** | 1,240 triplets @ 1280×720, 4 motion tiers | Small | Benchmark-only, not for training. | **Evaluation only.** |
| **REDS / Adobe240** | Various | Mid | Research-use; commercial gray. | Pretraining only. |
| **MSU VFI Benchmark** | Includes 7 games @ 1920×1080/120fps OBS captures | ~1s clips × N | Non-commercial research. | **Reference only** — do not train on it, but proves gameplay-VFI validity. |
| **Self-captured game footage** | Pairs from win-fg during play | Sized to fit training budget | **Owned by us if games permit capture (most do — nothing published/distributed).** Frames stay local to training rig. | **Only license-defensible source for the final release weights.** |

**Bottom line:** for any weights we ever ship, **the training set must be self-captured**. Vimeo90K / X4K are fine for pretraining checkpoints we throw away; final fine-tune must be on our own captures.

## 2. Self-captured game footage — practical workflow

**Capture point:** win-fg's own present hook. Add a `WFG_CAPTURE_DIR` env var. When set, on every real present dump prev+curr as fp16 EXR or lossless PNG. Also dump every N-th "triplet" (prev, midpoint from a native-recorded 120fps run of the same game, curr) — the midpoint is the ground truth.

**Trick for ground truth:** two capture modes.
- **Mode A (cheap, common):** use recorded 120fps game footage as ground truth. Downsample to 60fps input pairs; every skipped frame is a target. No engine cooperation needed. This is what MSU's gameplay benchmark does (OBS at 120 fps).
- **Mode B (better, harder):** recompute the true midpoint using pre-classical flow at capture time. Only if Mode A gaps quality.

**Volume needed** for a ~1–2M param model, fine-tune not from-scratch: **50K–150K triplets** suffices. Below 30K starts to overfit; above 300K diminishing returns.

**Storage:**
- 1080p RGB fp16 = 1920×1080×3×2 = ~12 MB/frame → 36 MB/triplet.
- 100K triplets ≈ **3.6 TB raw**. Manageable on a single spinning drive or NVMe. Down to ~800 GB if we drop to lossless PNG8.
- 720p halves the footprint; sensible for early runs.

**No preexisting public gameplay VFI-training set exists that's redistribution-safe.** MSU's is closest but non-commercial. We build our own.

## 3. Loss functions — what actually works

Standard for RIFE/IFRNet-class training:
- **Charbonnier photometric loss** `ρ(x) = √(x² + ε²)` — robust L1 variant, primary reconstruction term.
- **Laplacian pyramid L1** (RIFE) — L1 between multi-scale Laplacian decompositions. Kills residual blur at edges. Current RIFE default.
- **Census loss / ternary** (IFRNet) — soft Hamming distance on 7×7 census-transformed patches. Robust to illumination shift; RIFE removed it 2021.8.12 but IFRNet keeps it. Recommended for game content because HDR/exposure often jumps.
- **Warp loss** — L1 between warp(I₀→t) and target, and warp(I₁→t) and target separately. Regularises the flow.
- **EPE (endpoint error)** — only meaningful with GT flow (synthetic data). Skip for gameplay.
- **LPIPS** — VGG feature-space perceptual loss. **VGG weights = BSD-3, safe to use in training-only, but common practice is to strip perceptual loss for final fine-tune runs and rely on Charbonnier+Laplacian.**
- **Adversarial GAN** — overkill for our size class; adds instability. Skip.

**Recommended mix for us:** Charbonnier (weight 1.0) + Laplacian L1 (0.5) + Census (0.3 during warmup, 0.1 later) + Warp (0.2). All four are original/permissive.

## 4. Training compute — feasibility on one GPU

- Target model: **RIFE-4-lite scale** (~1.5–2M params) — small enough for Adreno 750, big enough to learn.
- Data pipeline: 100K triplets, ~50K epochs of 32-triplet minibatches, augment (crop 256×256 + flip + rotate).
- **RTX 3080 (10 GB)**: ~24-36 hours to convergence on Vimeo90K + fine-tune on 50K self-capture (~8-12 hours extra).
- **RTX 4090 (24 GB)**: ~10-14 hours total.
- **RTX 4080 (16 GB)**: ~14-20 hours total.
- **All feasible over one weekend on any consumer top-tier card.** No cluster needed for this size class.

## 5. Export path — what runs fast on Adreno 750

Three options, cleanest to most powerful:

### A. TFLite GPU delegate (fastest to ship)
- PyTorch → ONNX → tf2onnx → TFLite → ship as APK asset.
- GPU delegate runs fp16 compute on the Adreno via internal SPIR-V dispatch.
- `allow_precision_loss=true` → fp16 pipeline.
- ~2 weeks to wire end-to-end. Works on any Vulkan-capable Android. **Recommended for first shipping weights.**

### B. Qualcomm QNN via ORT's QNN Execution Provider (fastest inference)
- ONNX → QDQ-quantised ONNX (INT8 weights, INT16 activations — Hexagon's sweet spot) → ORT QNN-EP runs on **Hexagon HTP** (the NPU, not the GPU).
- **Runtime target: 8–15 ms per 1080p interpolation frame** on Snapdragon 8 Gen 3, per ANVIL benchmarks (12.8 ms verified for their architecture, same class as ours).
- Downside: locked to Snapdragon-class devices. MediaTek/Exynos need a fallback (TFLite GPU delegate).
- ~4 weeks to wire cleanly; requires calibration dataset for QDQ.

### C. Native Vulkan compute with fp16 SPIR-V (max control)
- Export model weights as fp16 buffers, embed via our existing `embed_spv.py`-style tooling.
- Write compute shaders for each layer type (conv2d, activation, warp).
- Works on any Vulkan 1.3 device (already the win-fg baseline).
- **Slowest to build, fastest to iterate on**, no runtime dependency on TFLite/QNN.
- Existing precedent: `rife-ncnn-vulkan` (nihui) — proven pattern for exactly this on Android.
- ~6+ weeks initial; but reusable for every future model rev.

**Best-of-both plan:** ship (A) first for coverage, add (B) as an opt-in fast path on Snapdragon devices, keep (C) as the escape hatch.

## 6. Quantization — expected quality delta

| Method | Precision | Adreno-750 speedup | Quality delta (PSNR on VFI benchmarks) |
|---|---|---|---|
| fp32 baseline | 32-bit | 1.0× | 0 |
| fp16 GPU compute | 16-bit | ~1.8–2× | ≈ 0 (imperceptible) |
| INT8 weights + INT16 activations (QNN QDQ) | mixed | ~3–4× (HTP) | ANVIL: **-0.19 dB**, RIFE flow: **-0.89 dB**, IFRNet frame: **-4.38 dB** — architecture-dependent |
| Pure INT8 | 8-bit | ~5× | -1 to -5 dB, often visible |

**Design lesson from ANVIL:** architectures that avoid iterative-flow accumulation quantise much better. If we design our model with quantization in mind (avoid deep flow-refinement chains, prefer wider single-pass convs), we can hit ANVIL's -0.19 dB territory.

## 7. License-clean guarantee — the exact recipe

For our shipped weights to be defensible:
1. **Training data:** self-captured game footage, or synthetic pairs, or public-domain source. Nothing sourced from a scraped commercial platform.
2. **Model architecture:** either our own design or a published architecture (RIFE-lite, ANVIL, IFRNet-lite) whose *code* license permits derivative work. All three are MIT/BSD.
3. **Pretraining checkpoints:** if used, must come from an MIT/BSD-licensed release (RIFE MIT ✅). Fine-tune erases the Vimeo90K taint through data replacement.
4. **Attribution:** `THIRD-PARTY.md` cites the architecture paper + explicitly says "trained from scratch on self-captured footage" or "fine-tuned from [X] MIT-licensed checkpoint on self-captured footage."
5. **No LSFG / DLSS3-FG / FSR3-FG binaries touched.** Not even for reference. Zero-derivation from proprietary tools.

**What killed bionic-fg:** distributing LSFG-derived weight blobs whose original file the LSFG team had proprietary rights to. **Our shield:** the training data is ours, the checkpoint provenance is documented, no proprietary weights entered the pipeline.

## 8. Tooling we don't have and need to build

1. **`tools/capture_triplets.py`** — CLI that consumes a directory of PNG dumps from win-fg's capture-mode present hook and packages them into a training-ready HDF5 or WebDataset shards.
2. **win-fg capture-mode branch** — `feat/capture-mode` — add `WFG_CAPTURE_DIR` env; dump every N-th present as prev/mid/curr triplet. ~50 lines.
3. **`train/model.py`** — RIFE-lite-scale PyTorch model. ~200 LOC. Base on published architecture + our own scaling.
4. **`train/train.py`** — Charbonnier + Laplacian + Census + Warp loss combo, AdamW optimizer, cosine LR, mixed-precision training. ~300 LOC.
5. **`train/export.py`** — PyTorch → ONNX → (a) TFLite via tf2onnx, (b) QDQ ONNX for QNN. ~150 LOC.
6. **`train/eval.py`** — SNU-FILM Easy/Medium/Hard eval; PSNR/SSIM/LPIPS reports. ~100 LOC.
7. **CI (win-fg repo)** — a nightly workflow that runs `train.py` for a short sanity epoch to detect regressions. Optional.
8. **Weight embed step** — `tools/embed_weights.py` to bake fp16 SPIR-V constant buffers if we go route C.

**Total scope:** ~1,500 LOC + capture branch + one weekend of RTX 4080 training + one week of on-device deployment tuning. **Solo-devable in 4-6 weeks of focused work.**

## Sources
- [Vimeo90K structure/download — CAIN DeepWiki](https://deepwiki.com/myungsub/CAIN/5.1-vimeo90k-dataset)
- [TOFlow / Vimeo-90K origin (Xue et al.)](http://toflow.csail.mit.edu/)
- [XVFI / X4K1000FPS paper](https://arxiv.org/abs/2103.16206)
- [XVFI GitHub](https://github.com/JihyongOh/XVFI)
- [SNU-FILM benchmark — CAIN GitHub](https://github.com/myungsub/CAIN)
- [Practical-RIFE (MIT)](https://github.com/hzwer/Practical-RIFE)
- [ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE)
- [rife-ncnn-vulkan (nihui)](https://github.com/nihui/rife-ncnn-vulkan)
- [GPU-Resident Frame Interpolation on Android (Kuo, 2026)](https://allenkuo.medium.com/gpu-resident-frame-interpolation-on-android-e9558d19cfab)
- [IFRNet paper](https://arxiv.org/pdf/2205.14620)
- [IFRNet GitHub](https://github.com/ltkong218/IFRNet)
- [ANVIL — codec-motion-vector VFI on Qualcomm HTP](https://arxiv.org/abs/2603.26835)
- [MSU Video Frame Interpolation Benchmark (gameplay dataset)](https://videoprocessing.ai/benchmarks/video-frame-interpolation-dataset.html)
- [Qualcomm QNN ONNX EP](https://onnxruntime.ai/docs/execution-providers/QNN-ExecutionProvider.html)
- [QNN SDK model conversion tutorial](https://docs.qualcomm.com/bundle/publicresource/topics/80-63442-10/tutorial_convert_execute_cnn_model.html)
- [TFLite GPU delegate README](https://github.com/tensorflow/tensorflow/blob/master/tensorflow/lite/delegates/gpu/README.md)
- [Snapdragon 8 Gen 3 NPU/HTP details](https://futurumgroup.com/insights/qualcomm-snapdragon-8-gen-3-brings-generative-ai-to-smartphones/)
- [ExecuTorch Qualcomm backend](https://docs.pytorch.org/executorch/stable/backends-qualcomm.html)
