# SANFG — Smart Adaptive Neural Frame Generation

**The endgame vision for win-fg.**

Set 2026-08-17 after the research pass documented in `docs/research/`.
Locking in the naming and positioning so every doc that follows points
at the same target.

## Tagline

> **win-fg — Smart Adaptive Neural Frame Generation**
> Clean-room, self-tuning, content-adaptive neural frame synthesis for Vulkan.
> Motion-aware · HUD-aware · Per-game learned presets · On-device inference · License-clean.

## What each word means (and what's aspiration vs shipping)

| Word | Meaning | Status |
|---|---|---|
| **Smart** | Self-observes runtime behavior (fallback rate, present pacing, flow magnitude), makes decisions | Roadmap: `feat/adaptive-gate`, `feat/adaptive-alpha`, `feat/gpu-saturation-skip`, `feat/tier-gated-auto` |
| **Adaptive** | Adjusts parameters per-frame / per-game / per-scene without user intervention | Roadmap: `feat/per-game-preset-cache`, `feat/dynamic-flow-resolution`, `feat/motion-magnitude-bailout` |
| **Neural** | Uses learned model weights for motion + synthesis (not pure classical) | **Aspiration** — requires the full training pipeline in `03-training-pipelines.md`. First shipping win-fg is classical + adaptive; neural is the follow-on. |
| **Frame Generation** | 2× (or 3×/4×) presented FPS via inserted synthetic frames between real ones | Shipped (Phase 3b true 2× insertion, `61113bc`) |
| **Clean-room** | Zero code/coefficients/bytecode borrowed verbatim from any FG project. All patterns implemented from cited papers. | Enforced. See `docs/PROVENANCE.md`. |
| **Self-tuning** | No user knobs for quality/perf balance — only a master switch and an optional "quality vs speed" bias | Roadmap: 9-item adaptive-heuristics menu |
| **Content-adaptive** | Different rendering path for HUD vs world, static vs high-motion, low-texture vs high-detail | Roadmap: `feat/hud-rect-mask` in flight, `feat/heuristic-hud-detect` next |
| **Neural frame synthesis** | Interpolation quality via CNN, not just flow-warp | Aspiration — depends on RIFE-4.25.lite fine-tune |
| **Vulkan** | Ships as Vulkan implicit layer, works with any game that runs through Vulkan (i.e., every DXVK/VKD3D/Wine Vulkan game on Bannerlator) | Shipped |
| **Motion-aware** | Handles camera motion separately from object motion (global-motion pre-warp) | Roadmap: `feat/global-motion-prewarp` |
| **HUD-aware** | UI/HUD regions excluded from flow warping so text/overlays don't ghost | In flight: `feat/hud-rect-mask` at `22faf40` |
| **Per-game learned presets** | Best-known tuning cached per-game across sessions | Roadmap: `feat/per-game-preset-cache` |
| **On-device inference** | Weights + inference stay on device (privacy, offline, no cloud), sized to fit APK asset | Aspiration — 8-12 MB target for RIFE-lite fp16 |
| **License-clean** | Every shipped artifact defensible under MIT + attribution. No LSFG-derived, no proprietary bytecode. | Enforced from day one. `THIRD-PARTY.md` tracks every borrowed pattern. |

## Success criteria — how we know when we're done

A build ships as SANFG when it hits all five gates on Adreno 750 (Pocket
FIT class) reference hardware:

1. **Consistent 2× conversion** — >90% of frames get an inserted synthetic
   frame (measured by the `insert=X fallback=Y` counter). Currently ~25%
   after `feat/swapchain-plus-one`. Gap = `feat/present-wait-pacing` +
   swapchain +1 combined.
2. **No visible HUD ghost** — text overlays (game menus, DXVK HUD,
   FusionHUD, FPS counters) look identical on/off. Currently visible ghost
   on all HUD. Gap = `feat/hud-rect-mask` + `feat/heuristic-hud-detect`.
3. **No visible motion melt** — camera pans stay sharp. Currently visible
   smear on pans. Gap = `feat/global-motion-prewarp` + `feat/flow-regularization`.
4. **No pacing stutter** — inter-frame timing histogram is bimodal at
   `refresh/2` and `refresh` (or unimodal at `refresh/2`), not tail-heavy.
   Currently unmeasured. Gap = `feat/present-wait-pacing` + measurement
   tool.
5. **Zero user-facing knobs beyond On/Off** — the drawer shows a master
   toggle and (optionally) a "quality vs performance" bias slider.
   Everything else auto-tunes. Currently exposes model/flow-scale
   sliders. Gap = adaptive-heuristics menu (9 items in
   `reference_win_fg_adaptive_heuristics_menu` memory file).

**Neural gate** (deferred, not required for SANFG v1): additionally
integrate a fine-tuned RIFE-4.25.lite inference path. Ships as a second
model in the drawer alongside classical (`Optical flow`, `Bidirectional`,
`Neural`). Adds "no visible transparency artifacts" as a 6th success
gate (particles, water, glass).

## Positioning vs the ecosystem

| Project | Approach | Ceiling |
|---|---|---|
| **LSFG-Android** (Ludashi/GameNative) | ML weights extracted from user-owned Lossless.dll at runtime | Best quality currently on Android, but requires user Steam ownership + inherits proprietary-DLL fragility |
| **Max WinNative** (PR #537) | In-renderer RAFT-family classical, `wnfg_XX_spv.h` provenance unclear | High quality but locked to WinNative renderer + provenance ambiguity |
| **Isygold Vegas** (in-DXVK) | Block-SAD, HUD-rect API, tier-gated skip | Ships in Vegas DXVK, requires that fork |
| **win-fg / SANFG** | Clean-room Vulkan-layer, adaptive-heuristic (Phase 1) → learned RIFE fine-tune (Phase 2) | Universal (any Vulkan game on any Winlator-family emulator), clean provenance, no user Steam requirement |

**Our differentiator:** universal Vulkan-layer position + clean-room
license + fully-adaptive UX. Others have quality; we have deployability.

## Phased roadmap (see `ROADMAP.md` for detail)

**Phase 1 — Classical adaptive (current).** Ship as `libwin_fg.so` v1.0.
Tier 1-3 branches from `project_win_fg_clean_room_port_plan` + adaptive
heuristics menu. Target: hit gates 1-5 above with pure algorithmic FG.

**Phase 2 — Neural add-on.** Add fine-tuned RIFE-4.25.lite inference via
NCNN Vulkan or Vulkan-compute fp16. Ships as opt-in "quality" mode in
the drawer. Adds gate 6 (transparency artifacts).

**Phase 3 — Cross-vendor + HTP.** Deploy on non-Adreno Vulkan (Mali, Xclipse)
via NCNN's cross-vendor Vulkan backend. Add QNN HTP delegate for
Snapdragon users. Deprecate any Adreno-only code paths.

## Provenance guarantee (locked)

Every artifact win-fg ships must satisfy:
1. **Code:** MIT-original or borrowed from an MIT/BSD/Apache/zlib repo
   with attribution.
2. **Shaders:** every `.comp` header cites the paper/algorithm it implements.
3. **Weights (Phase 2 onward):** trained on self-captured game footage,
   optionally warm-started from a permissively-licensed public checkpoint
   (RIFE MIT), documented in `weights/PROVENANCE.md`.
4. **No LSFG / DLSS3 / XeSS / proprietary bytecode** in any form.
5. `THIRD-PARTY.md` names every pattern + license + attribution.

**This is non-negotiable.** The bionic-fg takedown is the reason this
project exists; we don't repeat it.
