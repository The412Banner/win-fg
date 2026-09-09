# Win-FG Native

**Clean-room frame generation for Vulkan** — content-adaptive frame
synthesis for Android emulation (Winlator / Wine on Adreno).

> **Status: v0.2.1 — device-proven, private repo.** The classical adaptive
> quality path is complete and validated on hardware. The shipping form is
> **Win-FG Native**: the chain compiled directly into
> [Bannerlator](https://github.com/The412Banner/Bannerlator)'s own Vulkan
> compositor, where it generates into the compositor's swapchain images.
> The original Vulkan *layer* build (`libwin_fg.so`) is still here and still
> built — it is what the training-data capture rides on — but the native
> engine is the one users get, and the one the numbers below come from.
> Neural (Phase 2) has been measured on-device and re-scoped; the remaining
> classical polish (C3–C4) is still ahead. See
> [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md) and
> [`docs/research/SANFG.md`](docs/research/SANFG.md).
>
> **On the name:** the project is Win-FG Native after the 2026-09 move
> host-side. The build outputs keep their existing names on purpose —
> `libwin_fg.so`, `VkLayer_win_framegen.json`, CMake target `win_fg` — because
> Bannerlator bundles them by path and renaming them would break that for no
> gain.

## What this is

Frame generation that needs no cooperation from the game and no proprietary
files. It takes the previous and current real frames and synthesizes an
in-between frame, so the display sees ~2× the source FPS with no additional
game rendering.

Born from the [bionic-fg takedown](#clean-room-protocol): every shader is
written from published algorithms, no proprietary bytecode or weights. Nothing
has to be bought, downloaded or imported for it to work — which is the whole
point, and the practical difference from LSFG-based frame generation.

### Why "Native", and what the layer is still for

The compute chain is identical in both builds; only where it runs differs.

**Win-FG Native (the shipping form)** — `framegen.cpp` / `record_impl.inc`
compiled directly into Bannerlator's Vulkan compositor, generating into the
compositor's own swapchain images.

**The Vulkan implicit layer (`libwin_fg.so`)** — sits between the game's
swapchain and the compositor, after DXVK/VKD3D, hooking
`CreateSwapchainKHR`/`QueuePresentKHR`.

The move host-side happened because the layer's generated frames were reaching
the compositor and being **discarded**: each present arrives as a distinct
buffer under one window id, and the host kept only the newest — the in-game
counter read 2×, the panel showed 1×. Generating inside the compositor removes
that hop entirely, and the measured difference is stark (see below: ~25%
doubling as a layer, an exact 2× native).

The layer build is kept and still maintained because **training-data capture
rides on it** — capture needs to see the game's frames before the host
composites its HUD over them, which only the in-guest position gives.

The native path also arms far faster: **~124 ms**, versus ~2.2 s for the
LSFG engine it sits beside in Bannerlator (which must build a 25-stage
pipeline from shaders parsed out of a user-supplied DLL). win-fg's shaders are
embedded, so there is no multi-second stall when frame generation is switched
on mid-game.

## v0.2 — the quality stack (device-proven)

Validated on AYANEO Pocket FIT / Adreno 750, DiRT Rally 2.0 and DiRT Showdown.
Three quality layers, each independently gate-able and each provably degrading
to "identical-to-off" when disabled:

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

**Both C1 and C2 default to `auto` and engage on their own.** Confirmed live on
an Adreno 750 (2026-09-09): C1 locks on within ~3 frames of arming and holds —
`C1 global-motion: engaged=1 mode=0 stable=750 transPx=0.3` — with C2 running 4
TV-L1 iterations alongside it. If you are evaluating image quality, you are
seeing it *with* both applied unless you turned them off.

Plus: our subgroup-free reimplementation of the AMD **FidelityFX FSR3** optical
flow (MIT), bidirectional flow with occlusion-gated confidence,
softmax-splatting-inspired backward-warp blend, and HUD-rect exclusion so
text/overlays don't ghost.

### Models
- **4 · Bidirectional** — independent forward/backward flow + occlusion gate.
  **The layer's default** (`model = 4`); sharper on complex motion, heavier.
- **3 · Optical flow** — symmetric flow. Cheaper (~2 ms/frame) and the better
  choice on GPU-bound titles, where retaining base FPS matters more than peak
  sharpness.

### Performance preset
`perf_preset` picks how fine the flow search goes — it sets `kFlowFinest`,
the finest pyramid level the block search reaches:

| preset | `kFlowFinest` | trade |
|---|---|---|
| 0 · Quality | 1 | finest search, most GPU |
| 1 · Balanced | 2 | **layer default** |
| 2 · Performance | 3 | coarsest search, cheapest |

Changing it live is fully hot — `configure()` rebuilds the flow images and
resets the flow predictor without an app restart — but it *is* a full rebuild
of the pyramid, so it is not free to flip repeatedly mid-scene.

## Architecture at a glance

```
Layer mode:
  Game ─► DXVK ─► ⟨win-fg layer⟩ ─► Host compositor ─► Display

Native mode (Bannerlator):
  Game ─► DXVK ─► Host compositor ⟨win-fg chain⟩ ─► Display

Chain (identical in both):
  ├─ of3_luma / of3_downsample     luma + pyramid
  ├─ of3_gm_reduce / gm_prewarp    C1: global-motion pre-warp
  ├─ of3_flow(_m4)                 dense block-search flow
  ├─ of3_flowreg                   C2: TV-L1 regularization
  ├─ of3_expand(_m4)               median + upscale + GM add-back
  ├─ wfg_synth                     synthesize midpoint frame
  └─ present                       generated then real — true 2×
```

Ten compute shaders, all authored here, all embedded as SPIR-V generated from
`.comp` sources in this repo (`tools/build_shaders.sh` → `embed_spv.py` →
`src/embedded_shaders.hpp`). No runtime shader source, no external blobs.

## Measured on hardware

| What | Where | Result |
|---|---|---|
| 2× at a 45 fps cap | Adreno 750, native mode, system frame counter | **45 → 90 fps** on the panel |
| 2× uncapped | Adreno 750, native mode, in-app readout | **58 → 115 fps**, 6.6 ms/frame GPU |
| 2× | Adreno 750, layer mode, DiRT | 84 → 105 fps (~25% doubling — the discard problem native mode fixes) |
| Arm cost | Adreno 750, native mode | ~124 ms |
| Adreno 840 freeze fix | Galaxy Fold 8 / Turnip | freeze gone, 2× FPS |

## Runtime knobs

Set via `conf.toml` (hot-reloaded live) or `WIN_FG_*` env vars. In Win-FG
Native these are driven by the host app rather than a file.

| conf.toml | env | default | meaning |
|---|---|---|---|
| `model` | `WIN_FG_MODEL` | `4` | 3 = optical flow, 4 = bidirectional |
| `multiplier` | `WIN_FG_MULT` | `2` | presented ÷ real (2–4) |
| `perf_preset` | `WIN_FG_PERF_PRESET` | `1` | 0 quality / 1 balanced / 2 performance (see above) |
| `flowScale` | — | `1.0` | scales solved flow magnitude |
| `pacing` | `WIN_FG_PACING` | `off` | place the generated frame near the true temporal midpoint instead of back-to-back with the real one |
| `extra_images` | `WIN_FG_EXTRA_IMAGES` | `2` | extra swapchain images requested beyond app-min + 1 spare |
| `global_motion` | `WIN_FG_GM` | `auto` | C1 pre-warp: `auto`\|`on`\|`off` |
| `flow_reg` | `WIN_FG_FLOWREG` | `auto` | C2 regularization: `auto`\|`on`\|`off` |
| `fr_iters` | `WIN_FG_FR_ITERS` | `4` | TV-L1 iterations (0 ⇒ off; primary cost knob) |
| `fr_lambda` | `WIN_FG_FR_LAMBDA` | `2.0` | L1 data fidelity (higher ⇒ trust raw flow more) |
| `fr_dt` | `WIN_FG_FR_DT` | `0.25` | smoothing step (higher ⇒ smoother, risks soft edges) |
| `fr_edge` | `WIN_FG_FR_EDGE` | `8.0` | luma-gradient edge sensitivity |
| `fr_eps` | `WIN_FG_FR_EPS` | `0.05` | Charbonnier epsilon (px) |
| `debug` | `WIN_FG_DEBUG` | `off` | granular per-frame present-path trace to logcat for freeze triage; default off = zero overhead |

Setting `global_motion` or `flow_reg` to `off` makes the chain byte-identical
to the version without that stage.

**`pacing` defaults off.** It was defaulted on when it landed, then turned off
to A/B a flicker regression after live setting changes, and has stayed off
pending that being settled.

`WIN_FG_ASYNC` (async-compute path) exists but is **parked** — on a fully
GPU-bound title it can't beat the GPU-throughput wall, and Adreno/Turnip
typically exposes no spare compute queue, so it falls back to the (proven)
synchronous path.

### Logging

Everything logs to logcat under the tag **`win-fg`** (lowercase — grep
case-insensitively). Useful lines, all at INFO and ungated:

```
framegen init ok (queueFamily=N, 10 pipelines, model=M, C1 gm=1, C2 flow_reg=1 iters=4)
framegen resized to 1920x1080 (7 pyramid levels, kFlowFinest=2 perf_preset=1)
C1 global-motion: engaged=1 mode=0 stable=750 transPx=0.3 (model=4)
C2 flow-reg: engaged=1 mode=0 iters=4 lambda=2.00 dt=0.25 edge=8.0 eps=0.050
```

C1 prints on every engage-state change and then every 300 frames. **There is
no periodic rate line** — the log tells you what is *engaged*, never what is
being *achieved*; for throughput you need the host's own counter. Adding one
is an open item.

## Training-data capture mode (Route-B dataset collection)

A **dev-only** gate (`src/capture.hpp`) that dumps the *real, pre-interpolation*
game frames the layer sees at present time — the raw swapchain color image copied
to `prevImg`, **before** win-fg's synthesis and with **no HUD/overlay** (the HUD is
host-composited downstream, so the layer's frame is clean). It **never** captures
generated frames. Purpose: build clean `(i-1, i+1) → i` triplets for a future VFI
neural model, offline.

**Validated end to end** (2026-08-30): one real device session produced 90 shards
(~24 GB) and **73,986 training triplets**, format verified — every sampled manifest
offset lands on a QOI blob, and the centre frame is a true temporal midpoint by
mean-absolute-difference. Capture rides the layer, so it needs layer mode, not
native mode.

Consent handling: a consent block is written into **every** shard so it cannot be
separated from the data; capture without consent is flagged `consent: null` rather
than silently trusted. Byte layout in `src/capture.hpp`.

## Roadmap

- ✅ **Done (v0.2 / v0.2.1):** anti-ghost, C1 global-motion pre-warp, C2 TV-L1
  flow-reg, perf-preset, present-id fix, native-mode integration. Device-proven,
  shipped in Bannerlator.
- ⏭️ **Next (classical polish):**
  - **Flow / synthesis split** — the chain currently recomputes the flow for
    *every* generated frame, so 3× and 4× scale far worse than they should. LSFG
    computes the flow once per real frame and re-runs only the final blend. This
    is the single biggest efficiency gap and the prerequisite for multipliers
    above 2× being worth offering.
  - **C3** — a correlation cost-volume + iterative warp-follow refine
    (RAFT / PWC-Net family) for sharper fast-motion flow.
  - **C4** — occlusion-aware + edge-aware blend (forward/backward-consistency
    disocclusion mask, OBMC, joint-bilateral upsample) for cleaner silhouettes
    and no block-grid seams.
  - **A rate telemetry line**, so throughput is readable from a log.

### Phase 2 (neural) — measured, and re-scoped

The original plan was a tiny VFI CNN doing full frame synthesis. **That has been
tested on real hardware and does not fit.** Recorded here so it is not re-attempted
from theory:

- Trained from scratch on our own captured triplets (no scraped data, no
  pretrained weights): IFNet-lite **414K params → 26.88 dB** val PSNR on a
  deliberately hard, motion-biased game set.
- Shrunk twice: 206K → 26.28 dB, then **112K → 25.85 dB** with a half-resolution
  refine head. Quality degrades gracefully — the architecture shrinks well.
- **But on an Adreno 750 via ncnn-Vulkan fp16: 16 ms @256, 35 ms @360p, 117 ms
  @720p — against a 2–4 ms budget.** Ten to thirty times over at any usable
  resolution. Only ~280–310 GFLOP/s realized (no cooperative-matrix on Turnip,
  dispatches too small).
- Two hard blockers beyond speed: `GridSample` is `support_vulkan=0` in ncnn, so
  the net's warps fall back to **CPU** and break the pipeline; and the full-res
  refine head is ~70% of total cost at every resolution.

**Surviving design:** keep flow and warping **classical** — the shipped FSR3-family
shaders, already on the GPU and effectively free — and add a **tiny conv-only
residual** (no GridSample, no warps in the neural part, ideally at reduced
resolution) as the sole neural cost. Prerequisite is measuring the real classical
baseline on-device to train the residual against; the earlier residual attempt
scored poorly because its stand-in baseline was weak, not because the design is.

Endgame and success gates in [`docs/research/SANFG.md`](docs/research/SANFG.md).

## Known issues

- **No rate telemetry** — see Logging above.
- **3× / 4× cost more than they should** until the flow/synthesis split lands.
- **Pacing is off by default** pending resolution of a flicker regression seen
  after live setting changes.
- ~~Crashes on Adreno 840 / Wrapper (Galaxy Fold 8)~~ — **fixed 2026-08-26,
  device-proven.** Root cause: the insert path built a fresh `VkPresentInfoKHR`
  with `pNext = nullptr` for *both* presents, stripping the guest's
  `VkPresentIdKHR` off the real frame. DXVK throttles via `vkWaitForPresentKHR`,
  and a present id that is never queued wedges that wait forever — so the guest
  render thread stalled after the first insert. Only Adreno 840/gen8 advertises
  `VK_KHR_present_wait`, which is why 750/gen7 never showed it. Fix: forward
  `pNext` onto real-frame presents only; a generated frame must never carry a
  present id.

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

Every CI build stamps `git describe` into the binary, and the build **fails** if
the stamp is missing. To recover which commit a `.so` in the wild came from:

```
strings libwin_fg.so | grep win-fg-build
```

For native mode, the chain sources are consumed directly by the host app's build
rather than linked as a library — see Bannerlator's `cpp/winlator/winfg/`.

## Docs

| File | What |
|---|---|
| [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md) | Tiered branch list, gap analysis, implementation order |
| [`docs/research/SANFG.md`](docs/research/SANFG.md) | The neural endgame — success gates, phased plan |
| [`docs/research/`](docs/research/) | 8 research writeups (algorithms, models, training, deployment, prior art) |
| [`docs/PROVENANCE.md`](docs/PROVENANCE.md) | Shader-by-shader source + license attribution |
| [`docs/BRINGUP.md`](docs/BRINGUP.md) | Device bring-up notes |
| [`PROGRESS_LOG.md`](PROGRESS_LOG.md) | Dated engineering log — what was tried, measured and rejected |
| [`THIRD-PARTY.md`](THIRD-PARTY.md) | Full attribution ledger for every borrowed pattern |

## Clean-room protocol

win-fg exists because [bionic-fg was taken down](https://github.com/The412Banner/Bannerlator)
in 2026-08 for shipping SPIR-V shader bytecode derived from proprietary
Lossless Scaling weights. **We do not repeat that mistake.** Every artifact:
1. MIT-original or borrowed from an MIT/BSD/Apache/zlib repo with attribution.
2. Every `.comp` header cites the paper / algorithm it implements.
3. Phase-2 weights are trained on self-captured footage, optionally warm-started
   from a permissive public checkpoint (RIFE MIT), documented in
   `weights/PROVENANCE.md`. The models trained so far are from scratch on our own
   captures.
4. Zero LSFG / DLSS3 / XeSS / proprietary bytecode enters the tree, ever.
5. `THIRD-PARTY.md` names every borrowed pattern + license + attribution.

## License

[MIT](LICENSE) © 2026 The412Banner. Third-party attributions in
[`THIRD-PARTY.md`](THIRD-PARTY.md).

## Related projects

- [Bannerlator](https://github.com/The412Banner/Bannerlator) — the Android emulator that consumes this, both as a layer and compiled in as Win-FG Native
- [AMD FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) — motion-estimation base (FSR3 optical flow, MIT)
- [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) — Phase-2 reference architecture (MIT)
- [NCNN](https://github.com/Tencent/ncnn) — Phase-2 inference runtime evaluated on-device (BSD-3)
