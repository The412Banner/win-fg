# 02 — Learned Frame Interpolation Models


> **⚠️ Overtaken by measurement, 2026-08-31.** This survey recommended
> fine-tuning RIFE-4.25.lite as the Phase-2 base. We instead trained our own
> IFNet-lite from scratch on self-captured data (414K → 26.88 dB, shrunk to
> 112K → 25.85 dB) and measured it on an Adreno 750: **16 ms @256, 35 ms
> @360p, 117 ms @720p against a 2-4 ms budget.** Full-synthesis CNNs of this
> family do not fit, at any size we tested. The model survey below is still
> accurate as a survey; the *recommendation* is superseded by the tiny
> conv-only residual design in `ROADMAP.md` Tier 4.

Research pass 2026-08-17. Scope: what learned VFI models exist, what they
cost in params/MB, what license they ship under, and which — if any — we
could realistically train, fine-tune, and ship on Adreno-class mobile
hardware.

## Master comparison table

| Model | Year | Params | fp16 weight | License | Mobile-viable | Best for |
|---|---|---|---|---|---|---|
| **RIFE v4.25** | 2024 | 5.66M | ~11 MB (≈23 MB fp32) | **MIT** | Marginal on Adreno 750 | Best quality/perf balance in permissive space |
| **RIFE v4.25.lite** | 2024 | ~3M est. | ~5–7 MB est. | **MIT** | ✅ Yes | Mobile ceiling with clean license |
| **RIFE v4.22.lite (ONNX)** | 2024 | ~2M est. | ~9.8 MB fp16 reported | **MIT** | ✅ Yes | ONNX-exported mobile variant (via `square-zero-labs/rife-onnx`; HF repo has no model card so size unverified — treat as reference, re-export ourselves) |
| **IFRNet** | 2022 | ~5M | ~10 MB | **MIT** | Marginal | 15× faster than DAIN at similar quality |
| **IFRNet_S** | 2022 | ~2.8M | ~5.5 MB | **MIT** | ✅ Yes | Cleanest lightweight architecture |
| **IFRNet_L** | 2022 | ~19M | ~38 MB | **MIT** | ❌ | Quality-first desktop |
| **AMT-S** | 2023 | ~3M | ~6 MB | **CC-BY-NC 4.0** ⚠ | ✅ tech / ❌ license | RAFT-family, non-commercial only |
| **AMT-L / AMT-G** | 2023 | ~15M / ~30M | ~30/60 MB | CC-BY-NC 4.0 ⚠ | ❌ | Quality benchmark, non-commercial |
| **EMA-VFI** | 2023 | ~66M | ~130 MB | **Apache 2.0** | ❌ (too big) | Not mobile |
| **VFIformer** | 2022 | ~24M | ~48 MB | Research code | ❌ | Not mobile, 600-epoch training |
| **FLAVR** | 2020 | 42.1M | ~84 MB | Non-commercial | ❌ | Not mobile |
| **SepConv** | 2017 | 21.7M | ~43 MB | Non-commercial | ❌ | Legacy baseline |
| **DAIN** | 2019 | ~24M | ~48 MB + depth net | MIT (some deps GPL) | ❌ (needs depth) | Superseded |
| **FILM (Google)** | 2022 | ~50M (unified) | ~100 MB | **Apache 2.0** | ❌ (too big) | Best for large motion, not mobile |
| **XVFI (XVFI-Net)** | 2021 | ~5M | ~10 MB | **Research-only** ⚠ | ❌ license | 4K + large motion |
| **DLSS3-FG** | 2022 | Undisclosed CNN autoencoder | N/A | **Proprietary NVIDIA** | ❌ | Reference architecture only |
| **FSR3-FG** | 2023 | 0 (algorithmic, not learned) | 0 | **MIT** | ✅ conceptually | We can study the algorithm and clean-room it |
| **XeSS-FG** | 2024 | Undisclosed | N/A | Proprietary Intel | ❌ | Reference only |

## License breakdown — this is the game

### Safe permissive bases (Apache/MIT — can ship modified weights)
- **RIFE (all v4.x + all .lite)** — MIT. Cleanest single choice. hzwer confirmed as copyright holder.
- **IFRNet** — MIT (ltkong218). All variants same license.
- **EMA-VFI** — Apache 2.0 (too big to ship, but clean base for distillation).
- **FILM (Google)** — Apache 2.0. ⚠️ Upstream repo is ARCHIVED as of 2025; code still usable under Apache-2.0 but no new commits.
- **FSR3-FG** — MIT, but NOT a learned model (algorithmic).

### Research-only / non-commercial (do NOT ship weights or derivatives commercially)
- **XVFI** — "research and education only"
- **AMT (all variants)** — CC-BY-NC 4.0
- **VFIformer, FLAVR, SepConv, DAIN** — mixed non-commercial / research

### Proprietary black boxes (only reference for architecture ideas)
- DLSS3-FG, XeSS-FG — NVIDIA/Intel closed, no weights, no source

## Recommendation — one model to ship

**RIFE v4.25.lite (MIT), fine-tuned on our own captured game footage.**

### Rationale
1. **License is airtight** — MIT, hzwer holds copyright, upstream explicitly states weights "under the same MIT license as this project." We can ship modified weights, fine-tune, redistribute, commercial use OK.
2. **Size fits mobile** — 4.25 upstream is 5.66M params ≈ 11 MB fp16 (verified on `mlx-community/RIFE-4.25` HF card). 4.22.lite ONNX exports exist in the wild (~9.8 MB fp16 reported on `square-zero-labs/rife-onnx`; that HF repo has no model card so treat the exact size as unverified until we re-export ourselves). Fits comfortably in an APK asset.
3. **Architecture is right** — IFNet-based (5 coarse-to-fine IFBlocks c=[192,128,96,64,32]), converges quickly on 720p–1080p inputs, real-time-capable on desktop GPUs, adaptable to Adreno via SPIR-V compute or QNN HTP.
4. **Confirmed export path** — ONNX exports exist in the wild (HF, SVFI project), so PyTorch → ONNX → NCNN/QNN/SPIR-V is a walked path.
5. **Fallback path** — if we can't hit budget, IFRNet_S (MIT, ~2.8M params) is the second choice with an even smaller footprint.

### Do NOT pick
AMT (non-commercial), XVFI (research-only), anything from the proprietary column. Each would replay bionic-fg's takedown.

## What we'd need to modify vs use as-is

### Use as-is from RIFE MIT base
- IFNet architecture definition (PyTorch model class)
- Training loop scaffold (Charbonnier + smoothness losses)
- Data augmentation pipeline
- Basic ONNX export tooling

### We'd need to author ourselves
- **Game-specific fine-tune dataset**: capture prev/curr/midpoint triplets from Bannerlator games (win-fg already sees them — add a capture mode). RIFE upstream trained on Vimeo90K (video-natural), which under-represents fast camera pans and HUD compositing.
- **Vulkan compute inference kernels** (or QNN HTP export) — RIFE upstream ships PyTorch inference only. For Android/Adreno, we need to either (a) rewrite forward pass as GLSL compute shaders + fp16 weight buffers, or (b) convert to QNN model and use HTP DSP acceleration.
- **HUD-aware training** — add a HUD-preservation loss term so the model learns to pass HUD regions through untouched (extends what our `feat/hud-rect-mask` does at the shader level).
- **Warm-start from RIFE MIT weights** → fine-tune ~10K–50K game-frame triplets on a single consumer GPU (12–24 hours).

### Attribution to ship
`THIRD-PARTY.md` entry stating:
> win-fg's learned interpolator is fine-tuned from RIFE v4.25.lite (MIT, © hzwer, https://github.com/hzwer/Practical-RIFE), with a custom training dataset of Bannerlator-captured game frame triplets and a HUD-preservation loss extension. Modified weights released under MIT.

## Sources
- [Practical-RIFE (hzwer)](https://github.com/hzwer/Practical-RIFE)
- [RIFE ECCV 2022 paper (arXiv 2011.06294)](https://arxiv.org/abs/2011.06294)
- [mlx-community/RIFE-4.25 model card](https://huggingface.co/mlx-community/RIFE-4.25)
- [square-zero-labs/rife-onnx (verified 9.8 MB fp16 lite)](https://huggingface.co/square-zero-labs/rife-onnx)
- [IFRNet CVPR 2022 (arXiv 2205.14620)](https://arxiv.org/abs/2205.14620) / [github ltkong218/IFRNet](https://github.com/ltkong218/IFRNet)
- [XVFI ICCV 2021 (arXiv 2103.16206)](https://arxiv.org/abs/2103.16206) / [github JihyongOh/XVFI](https://github.com/JihyongOh/XVFI) — research-only
- [AMT CVPR 2023 (arXiv 2304.09790)](https://arxiv.org/abs/2304.09790) / [github MCG-NKU/AMT](https://github.com/MCG-NKU/AMT) — CC-BY-NC
- [EMA-VFI (github MCG-NJU/EMA-VFI)](https://github.com/MCG-NJU/EMA-VFI) — Apache 2.0
- [FILM (github google-research/frame-interpolation)](https://github.com/google-research/frame-interpolation) — Apache 2.0
- [FSR3 open-source announcement](https://gpuopen.com/news/fsr3-source-available/) / [FidelityFX-SDK docs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/) — MIT algorithm
- [DLSS3 Frame Generation architecture (NVIDIA)](https://www.nvidia.com/en-us/geforce/news/dlss3-ai-powered-neural-graphics-innovations/)
