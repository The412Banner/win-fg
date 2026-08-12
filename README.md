# win-fg

A clean-room, color-only frame-generation engine for Android/Vulkan (Turnip/
Adreno) — the successor to models 3 & 4, rebuilt so **no part derives from the
proprietary Lossless Scaling weights**. See [docs/PROVENANCE.md](docs/PROVENANCE.md)
for the clean-room boundary; that document governs what may enter this repo.

## What it does

Generates an in-between frame from two real frames to raise perceived frame
rate. Two stages, both ours:

1. **Optical flow** (`of3_*`) — our MIT adaptation of AMD FidelityFX FSR3
   optical flow, subgroup-free for Turnip. Produces two full-resolution flow
   fields with a confidence channel.
2. **Synthesis** (`wfg_synth`) — written from first principles here: motion-
   compensated warp + temporal blend + confidence-gated cross-fade. Replaces the
   traced bionic-fg/GameScope "stage 6".

Two selectable models share the same synthesis:

- **Model 3** — symmetric flow (`backward = -forward`), confidence = 1.
- **Model 4** — independently searched forward/backward flow with block-grid
  search and sub-pixel refinement; confidence is gated by fwd/bwd disagreement
  in `of3_expand_m4`, so occlusions and scene cuts degrade to a clean cross-fade
  instead of ghosting.

## Pipeline

```
prev,curr ─▶ of3_luma ─▶ of3_downsample ×N ─▶ of3_flow[_m4] (coarse→fine)
                                                   │
                                          of3_expand[_m4]
                                                   │
                                    flowExpA + flowExpB (+conf)
                                                   │
                                             wfg_synth ─▶ generated frame
```

## Build the shaders

```sh
tools/build_shaders.sh          # -> build/spv/*.spv  (Vulkan 1.1 / SPIR-V 1.3)
```

All shaders must compile clean with `glslangValidator` for `--target-env
vulkan1.1` (caps: Shader + ImageQuery only; `r32f`/`rgba16f`/`rgba8` storage) so
they run on Turnip without extended-format or subgroup features.

## Status

- [x] Clean optical-flow front end (carried over, MIT)
- [x] Clean synthesis back end (`wfg_synth.comp`, written here)
- [x] All shaders compile Turnip-safe
- [ ] Minimal Vulkan implicit-layer host (weight-free; scaffolds flow+synth only)
- [ ] SPIR-V embed generator (no traced table)
- [ ] Device build + on-device validation (resume `fg011-m34` flow tuning)
- [ ] Bundle into Bannerlator, retire the traced layer

## License

MIT (ours). FidelityFX-derived optical-flow passes are MIT — see
[NOTICE_FIDELITYFX_OPTICALFLOW.md](NOTICE_FIDELITYFX_OPTICALFLOW.md).
