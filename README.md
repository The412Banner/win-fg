# win-fg

**Clean-room frame generation for Vulkan** — content-adaptive frame
synthesis for Android emulation (Winlator / Wine on Adreno).

> **Status: v0.2 — device-proven, private repo.** The classical adaptive
> quality path is complete and validated on hardware; ships as `libwin_fg.so`
> bundled with [Bannerlator](https://github.com/The412Banner/Bannerlator)
> (merged to `main`, pending the next app release). Neural (Phase 2) and the
> remaining classical polish (C3–C4) are still ahead — see
> [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md) and
> [`docs/research/SANFG.md`](docs/research/SANFG.md).

## What this is

A Vulkan implicit layer that sits between a game's swapchain and the
compositor. On each present it synthesizes an interpolated in-between frame
from the previous and current real frames, so the display sees ~2× the source
FPS with no additional game rendering. It sits **after** DXVK/VKD3D, so it
works with any game that already runs through them — no engine cooperation.

Born from the [bionic-fg takedown](#clean-room-protocol): every shader is
written from published algorithms, no proprietary bytecode or weights.

## v0.2 — the quality stack (device-proven)

Validated on AYANEO Pocket FIT / Adreno 750, DiRT Rally 2.0. Three quality
layers, each independently gate-able and each provably degrading to
"identical-to-off" when disabled:

- **Anti-ghost** — sub-pixel parabola-fit flow refinement, a 5-tap median
  filter on the flow field, a tightened disocclusion gate, and a content-diff
  fallback that snaps to the real frame where a blend would double-expose.
- **C1 · Global-motion pre-warp** — an inverse-compositional Lucas-Kanade
  affine estimate of the whole-frame (camera) motion, removed before the dense
  flow search so the search only sees object motion. **Kills camera-pan
  "melt"** at the source. (No GPU float atomics — Turnip-safe; CPU 6×6 solve.)
- **C2 · TV-L1 flow regularization** — an edge-preserving smoothness prior on
  the residual flow (semi-implicit lagged-diffusivity — unconditionally stable
  on Turnip). Cleans leftover incoherent object flow while keeping silhouettes
  sharp.

Plus: our subgroup-free reimplementation of the AMD **FidelityFX FSR3** optical
flow (MIT), bidirectional flow with occlusion-gated confidence,
softmax-splatting-inspired backward-warp blend, and HUD-rect exclusion so
text/overlays don't ghost.

### Models
- **3 · Optical flow** — symmetric flow. **The default** (~2 ms/frame; best
  base-FPS retention on GPU-bound titles).
- **4 · Bidirectional** — independent forward/backward flow + occlusion gate
  (heavier; sharper on complex motion).

## Architecture at a glance

```
Game ─► DXVK ─► ⟨win-fg Vulkan layer⟩ ─► Compositor ─► Display
                 │
                 ├─ of3_luma / of3_downsample     luma + pyramid
                 ├─ of3_gm_reduce / gm_prewarp    C1: global-motion pre-warp
                 ├─ of3_flow(_m4)                 dense block-search flow
                 ├─ of3_flowreg                   C2: TV-L1 regularization
                 ├─ of3_expand(_m4)               median + upscale + GM add-back
                 ├─ wfg_synth                     synthesize midpoint frame
                 └─ QueuePresentKHR               generated then real — true 2×
```

## Runtime knobs

Set via `conf.toml` (hot-reloaded live) or `WIN_FG_*` env vars.

| conf.toml | env | default | meaning |
|---|---|---|---|
| `model` | — | `3` | 3 = optical flow, 4 = bidirectional |
| `multiplier` | — | `2` | presented ÷ real (2–4; layer inserts 1 gen frame = true 2×) |
| `flowScale` | — | `1.0` | scales solved flow magnitude |
| `global_motion` | `WIN_FG_GM` | `auto` | C1 pre-warp: `auto`\|`on`\|`off` |
| `flow_reg` | `WIN_FG_FLOWREG` | `auto` | C2 regularization: `auto`\|`on`\|`off` |
| `fr_iters` | `WIN_FG_FR_ITERS` | `4` | TV-L1 iterations (0 ⇒ off; primary cost knob) |
| `fr_lambda` | `WIN_FG_FR_LAMBDA` | `2.0` | L1 data fidelity (higher ⇒ trust raw flow more) |
| `fr_dt` | `WIN_FG_FR_DT` | `0.25` | smoothing step (higher ⇒ smoother, risks soft edges) |
| `fr_edge` | `WIN_FG_FR_EDGE` | `8.0` | luma-gradient edge sensitivity |
| `fr_eps` | `WIN_FG_FR_EPS` | `0.05` | Charbonnier epsilon (px) |
| `global_motion`/`flow_reg` `off` | | | ⇒ byte-identical to the layer without that stage |

`WIN_FG_ASYNC` (async-compute path) exists but is **parked** — on a
fully GPU-bound title it can't beat the GPU-throughput wall, and Adreno/Turnip
typically exposes no spare compute queue, so it falls back to the (proven)
synchronous path. See [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md).

## Roadmap

- ✅ **Done (v0.2):** anti-ghost, C1 global-motion pre-warp, C2 TV-L1 flow-reg,
  model-3 default. Device-proven, merged to Bannerlator `main`.
- ⏭️ **Next (classical polish):**
  - **C3** — a correlation cost-volume + iterative warp-follow refine
    (RAFT / PWC-Net family) for sharper fast-motion flow.
  - **C4** — occlusion-aware + edge-aware blend (forward/backward-consistency
    disocclusion mask, OBMC, joint-bilateral upsample) for cleaner silhouettes
    and no block-grid seams.
- 🔭 **Later (neural, Phase 2):** a tiny VFI CNN — either hand-crafted
  coefficient kernels (Route A) or a fine-tuned RIFE-4.25.lite / IFRNet_S (MIT)
  trained on self-captured game frames (Route B), FP16 inference on Turnip.
  Endgame in [`docs/research/SANFG.md`](docs/research/SANFG.md).

**Known issue:** win-fg crashes the host app on some **Adreno 840 / Wrapper
driver** setups (e.g. Galaxy Fold 8) — under investigation; Adreno 750/Turnip
is unaffected.

## Building

Standard Android Vulkan layer. CI in `.github/workflows/build.yml` produces
`libwin_fg.so` + `VkLayer_win_framegen.json` per push (NDK r27d, arm64-v8a,
android-26).

```
./tools/build_shaders.sh                    # glslang → SPIR-V → embedded_shaders.hpp
mkdir build && cd build
cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DVULKAN_INCLUDE=/path/to/Vulkan-Headers/include ..
cmake --build .
```

## Docs

| File | What |
|---|---|
| [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md) | Tiered branch list, gap analysis, implementation order |
| [`docs/research/SANFG.md`](docs/research/SANFG.md) | The neural endgame — success gates, phased plan |
| [`docs/research/`](docs/research/) | 8 research writeups (algorithms, models, training, deployment, prior art) |
| [`docs/PROVENANCE.md`](docs/PROVENANCE.md) | Shader-by-shader source + license attribution |
| [`docs/BRINGUP.md`](docs/BRINGUP.md) | Device bring-up notes |
| [`THIRD-PARTY.md`](THIRD-PARTY.md) | Full attribution ledger for every borrowed pattern |

## Clean-room protocol

win-fg exists because [bionic-fg was taken down](https://github.com/The412Banner/Bannerlator)
in 2026-08 for shipping SPIR-V shader bytecode derived from proprietary
Lossless Scaling weights. **We do not repeat that mistake.** Every artifact:
1. MIT-original or borrowed from an MIT/BSD/Apache/zlib repo with attribution.
2. Every `.comp` header cites the paper / algorithm it implements.
3. Phase-2 weights (when they arrive) are trained on self-captured footage,
   optionally warm-started from a permissive public checkpoint (RIFE MIT),
   documented in `weights/PROVENANCE.md`.
4. Zero LSFG / DLSS3 / XeSS / proprietary bytecode enters the tree, ever.
5. `THIRD-PARTY.md` names every borrowed pattern + license + attribution.

## License

[MIT](LICENSE) © 2026 The412Banner. Third-party attributions in
[`THIRD-PARTY.md`](THIRD-PARTY.md).

## Related projects

- [Bannerlator](https://github.com/The412Banner/Bannerlator) — the Android emulator that consumes this layer
- [AMD FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) — motion-estimation base (FSR3 optical flow, MIT)
- [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) — planned Phase-2 base model (MIT)
- [NCNN](https://github.com/Tencent/ncnn) — planned Phase-2 inference runtime (BSD-3)
