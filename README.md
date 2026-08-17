# win-fg

**Smart Adaptive Neural Frame Generation for Vulkan.**
Clean-room, self-tuning, content-adaptive frame synthesis for Android
emulation (Winlator / Wine on Adreno).

> **Status: pre-alpha, private repo.** Not yet ready to ship; will go public
> when the classical adaptive path (Phase 1) hits its success gates. See
> [`docs/research/SANFG.md`](docs/research/SANFG.md) for the endgame and
> [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md) for the branch plan.

## What this is

A Vulkan implicit layer that sits between a game's swapchain and the
compositor. On each present it synthesizes an interpolated in-between
frame from the previous and current real frames, so the display sees
roughly 2× the source FPS with no additional game rendering.

Ships today as `libwin_fg.so` bundled with
[Bannerlator](https://github.com/The412Banner/Bannerlator) — the Android
Wine+DXVK+Turnip emulator this project targets first.

### Motion approach (Phase 1 — shipping)
- Dense optical flow via our own subgroup-free reimplementation of
  the AMD FidelityFX FSR3 pyramid + block-search algorithm (MIT).
- Bidirectional flow with occlusion-gated confidence.
- Softmax-splatting-inspired backward-warp blend (adapted for mobile).
- Photometric-gate + cross-fade fallback for disoccluded regions.
- HUD-rect exclusion so text/overlays don't ghost.

### Motion approach (Phase 2 — planned)
- Fine-tuned RIFE-4.25.lite (MIT) inference via NCNN Vulkan.
- Self-captured game-frame training data.
- Weights ship under MIT with full provenance chain.

Full research writeup: [`docs/research/`](docs/research/).

## Architecture at a glance

```
Game  ──►  DXVK  ──►  ⟨win-fg Vulkan layer⟩  ──►  Compositor  ──►  Display
                       │
                       ├── AcquireNextImageKHR      (grabs a spare swapchain image)
                       ├── of3_flow / of3_expand    (optical flow, our shaders)
                       ├── wfg_synth                (synthesizes midpoint frame)
                       └── QueuePresentKHR          (generated then real — true 2×)
```

Sits **after** DXVK in the render chain — works with any game that
already runs through DXVK/VKD3D. No engine cooperation required.

## Building

Compiles as a standard Android Vulkan layer. CI in `.github/workflows/build.yml`
produces `libwin_fg.so` + `VkLayer_win_framegen.json` artifacts per push.

Local build (Linux host, NDK):
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
| [`docs/research/SANFG.md`](docs/research/SANFG.md) | The endgame vision — Smart Adaptive Neural Frame Generation, success gates, phased plan |
| [`docs/research/ROADMAP.md`](docs/research/ROADMAP.md) | Tier 1-4 branch list, gap analysis, 6-sprint implementation order |
| [`docs/research/`](docs/research/) | 8 research writeups (algorithms, models, training pipelines, deployment, prior art) |
| [`docs/PROVENANCE.md`](docs/PROVENANCE.md) | Shader-by-shader source + license attribution |
| [`docs/BRINGUP.md`](docs/BRINGUP.md) | Device bring-up notes |
| [`THIRD-PARTY.md`](THIRD-PARTY.md) | Full attribution ledger for every borrowed pattern |

## Clean-room protocol

win-fg exists because [bionic-fg was taken down](https://github.com/The412Banner/Bannerlator)
in 2026-08 for shipping SPIR-V shader bytecode derived from proprietary
Lossless Scaling weights. **We do not repeat that mistake.**

Every artifact this repo ships must satisfy:
1. Code is MIT-original or borrowed from an MIT/BSD/Apache/zlib repo with attribution.
2. Every `.comp` shader header cites the paper / algorithm it implements.
3. Phase-2 model weights (when they arrive) are trained on self-captured
   footage, optionally warm-started from a permissively-licensed public
   checkpoint (RIFE MIT). Documented in `weights/PROVENANCE.md` at that time.
4. Zero LSFG / DLSS3 / XeSS / proprietary bytecode enters the tree in any
   form.
5. `THIRD-PARTY.md` names every borrowed pattern + license + attribution.

## License

[MIT](LICENSE) © 2026 The412Banner. Third-party attributions in
[`THIRD-PARTY.md`](THIRD-PARTY.md).

## Related projects

- [Bannerlator](https://github.com/The412Banner/Bannerlator) — the Android emulator that consumes this layer
- [AMD FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) — motion-estimation base (FSR3 optical flow, MIT)
- [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) — planned Phase-2 base model (MIT)
- [NCNN](https://github.com/Tencent/ncnn) — planned Phase-2 inference runtime (BSD-3)
