# ROADMAP — win-fg → SANFG

Synthesis of the 6-part research pass in this directory + the current
in-flight branches + the ecosystem survey. Prioritized branches ordered
by ROI to the SANFG endgame described in `SANFG.md`.

**Every branch listed here has a citation trail.** Follow the linked
research doc for algorithm sources and license notes.

> **Maintained doc — last reconciled with reality 2026-09-09.** The tiered
> branch list below was written 2026-08-17; items that have since shipped are
> marked ✅ in place rather than deleted, so the citation trail survives. The
> current-state table and the Tier-4 (neural) section have been rewritten
> against on-device measurement — see `PROGRESS_LOG.md` for the dated detail.

## Design principle — FG is binary (2026-08-17)

**Frame generation is either fully on or fully off — no runtime toggling.**
Dynamic "skip FG under load" or "auto-disable during heavy scenes" behaviors
are REJECTED because turning FG on/off mid-play introduces visible stutter
(sudden change in perceived framerate). If the user wants FG off, they
turn it off in the drawer. If they want it on, it stays on for the whole
session.

**Rejected patterns** (do not revisit):
- `feat/gpu-saturation-skip` — Isygold's 60-frame skip window on watchdog trip. **Rejected 2026-08-17**: causes stutter every time skip window fires/ends.
- `feat/tier-gated-auto` — auto-enable/disable FG from frame-time EMA. **Rejected 2026-08-17**: hysteresis notwithstanding, the on↔off transitions are visible.
- `feat/motion-magnitude-bailout` — pass through during fast pans. **Rejected 2026-08-17**: same class of problem.

**Adaptive patterns that ARE allowed** (per-pixel or per-parameter
continuous change, not on/off toggles):
- `feat/adaptive-gate` — per-frame tune of synth `photoScale`/`epsilon`. Smooth parameter drift, FG stays on throughout.
- `feat/adaptive-alpha` — per-frame tune of temporal alpha. Smooth drift.
- `feat/heuristic-hud-detect` — auto-populate HUD rect from motion history. Rect adjusts smoothly; FG stays on.
- `feat/per-game-preset-cache` — load per-game knobs at launch. No runtime toggling.
- `feat/dynamic-flow-resolution` — flow-solve resolution changes with budget. **Under review — quality shift may be visible; may end up rejected.**
- `feat/confidence-driven-crossfade` — per-pixel smooth blend of synth vs crossfade based on confidence. Already partly in `wfg_synth`.

Any future branch that adds runtime on/off transitions of FG dispatch
gets rejected up front.

## Current state (2026-09-09)

| Component | State |
|---|---|
| **Win-FG Native** — chain compiled into Bannerlator's compositor | ✅ Shipped, device-proven |
| Vulkan layer build (`libwin_fg.so`) | ✅ Shipped; retained for training capture |
| Phase 3a — interpolate + blit | ✅ Shipped |
| Phase 3b — true 2× spare-image insertion | ✅ Shipped (`61113bc`) |
| swapchain +1 (Isygold pattern port) | ✅ Shipped (`c91bb50`) |
| synth alpha 0.5 → 0.35 | ✅ Shipped (`2f50e40`) |
| HUD-rect exclusion | ✅ Shipped |
| Anti-ghost (parabola fit, 5-tap median, content-diff snap) | ✅ Shipped |
| **C1 · global-motion pre-warp** (was T2-E) | ✅ Shipped, engages automatically |
| **C2 · TV-L1 flow regularization** (was T3-A) | ✅ Shipped, 4 iterations default |
| `perf_preset` (flow-resolution knob, live rebuild) | ✅ Shipped |
| Adreno 840 / Fold 8 present-id freeze | ✅ Fixed + device-proven 2026-08-26 |
| Training capture (`.wfgcap`) | ✅ Shipped + validated (73,986 real triplets) |
| Device-verified fps lift — **native** | **45 → 90 @ 2× (system counter); 58 → 115 uncapped** |
| Device-verified fps lift — layer | 84 → 105 (≈25% doubling; the host discarded the rest) |
| Known symptoms remaining | 3×/4× cost (no flow/synth split), no rate telemetry, pacing off by default |

The older 2026-08-17 table listed "HUD ghost, camera-pan melt, sub-100%
doubling" as the open symptoms. Camera-pan melt is addressed by C1+C2;
sub-100% doubling was the host discarding the layer's frames and is solved by
running native.

## Priority order — Tier 1 (small, high ROI, ships weeks not months)

Each ~1-2 days of implementation + 1 hotswap cycle. All classical, all
clean-room, all inside `libwin_fg.so`.

### ~~T1-A. `feat/gpu-saturation-skip`~~ — REJECTED (see design principle above)

### T1-B. `feat/present-wait-pacing`
- **What:** use `VK_KHR_present_wait` (or `VK_EXT_present_timing` on Mesa 26.1+) to space generated frame at `T` and real frame at `T + refresh/2`.
- **Effort:** ~50 LOC + ext enable.
- **Source:** `04-adreno-deployment.md` (Turnip extension survey).
- **Fixes:** doubling-rate lift beyond swapchain+1 alone. Push ~25% → ~90%.
- **Depends on:** nothing.

### ~~T1-C. `feat/tier-gated-auto`~~ — REJECTED (see design principle above)

### ~~T1-D. `feat/median-flow-filter`~~ — ✅ SHIPPED (5-tap median in `of3_expand`)
- **What:** 3×3 median filter post-process on `of3_flow` output. Kills outlier flow vectors.
- **Effort:** ~30 LOC GLSL + 1 dispatch stage.
- **Source:** `06-max-winnative-inspirations.md` (Max's `flowfix.comp` pattern; algorithm = Tukey 1977 median-as-robust-estimator).
- **Fixes:** flow-field speckle → cleaner warp.
- **Depends on:** nothing.

### T1-E. `feat/adaptive-gate`
- **What:** PID-lite loop reads insert/fallback ratio, auto-tunes `photoScale`/`epsilon` to hit ~15% fallback target.
- **Effort:** ~40 LOC.
- **Source:** `reference_win_fg_adaptive_heuristics_menu` memory file; Isygold's dominance-stats-buffer pattern.
- **Fixes:** eliminates manual gate tuning; auto-adapts per scene.
- **Depends on:** nothing.

## Priority order — Tier 2 (medium, high quality lift)

Each ~3-7 days of shader authoring + wiring + hotswap testing.

### T2-A. `feat/correlation-cost9`
- **What:** 3×3 cost-volume around current flow estimate + sub-pixel parabola fit for argmin.
- **Effort:** ~80 LOC GLSL, 1 new `.comp`, 1 dispatch stage.
- **Source:** `01-optical-flow-algorithms.md` (RAFT arxiv:2003.12039 §3.2 lookup + Lowe 2004 §5 parabola fit).
- **Reference impl to study:** FSR3 `ffx_frameinterpolation_optical_flow_vector_field.glsl` (MIT).
- **Fixes:** sub-pixel accuracy → less texture crawl on smooth pans.

### T2-B. `feat/fb-consistency` (+ `of3_fb_consist.comp`)
- **What:** run flow bidirectionally, compute FB consistency → emit occlusion mask as 4th input to `wfg_synth`.
- **Effort:** ~40 LOC GLSL, 1 new dispatch, `wfg_synth.comp` binding update.
- **Source:** `01-optical-flow-algorithms.md` (Sundaram-Brox-Keutzer ECCV 2010).
- **Reference impl to study:** FSR3 `ffx_frameinterpolation_disocclusion_mask.glsl` (MIT). See `08-reusable-shaders.md` #1.
- **Fixes:** silhouette ghost, gives synth real confidence signal.

### T2-C. `feat/heuristic-hud-detect`
- **What:** accumulate per-16×16-tile motion history over 200 frames; tiles with mean motion ≈0 while surrounding tiles move → auto-mark as HUD. Auto-populates `hudRect` without host cooperation.
- **Effort:** ~60 LOC C++ + 1 tiny SSBO.
- **Source:** `reference_win_fg_adaptive_heuristics_menu` memory file; FSR3's "identify UI by diffing" HUD detector pattern (MIT).
- **Fixes:** HUD masking works without app-side layout knowledge. Extends `feat/hud-rect-mask`.

### T2-D. `feat/flow-inpainting`
- **What:** push-pull pyramid inpainting to fill flow-field holes where FB consistency rejected flow.
- **Effort:** ~1-2 days (~150 LOC total across 2-3 shaders).
- **Source:** `08-reusable-shaders.md` #2 — FSR3 `ffx_frameinterpolation_inpainting_pass.glsl` + inpainting pyramid pair (MIT).
- **Fixes:** the "black region where flow was untrusted" failure mode we currently don't handle.

### ~~T2-E. `feat/global-motion-prewarp`~~ — ✅ SHIPPED as C1
- **What:** fit 6-parameter affine between prev/curr via Lucas-Kanade, warp `I1` before dense flow → residual flow is object-only.
- **Effort:** ~150 LOC across 2 shaders (`of3_gm_reduce.comp` + `of3_gm_prewarp.comp`) + CPU 6×6 solve.
- **Source:** `01-optical-flow-algorithms.md` (Szeliski Ch. 6.2 + Baker-Matthews 2001 inverse-compositional LK).
- **Fixes:** camera-pan melt at the source. **Biggest single quality lever** — removes ~70% of translational-camera flow magnitude.

## Priority order — Tier 3 (big, structural)

### ~~T3-A. `feat/flow-regularization` (+ `of3_flowreg.comp`)~~ — ✅ SHIPPED as C2
- **What:** TV-L1 smoothness prior post-process on flow field.
- **Effort:** ~60 LOC GLSL.
- **Source:** `01-optical-flow-algorithms.md` (Horn-Schunck 1981 or Chambolle-Pock TV-L1).
- **Fixes:** flow discontinuity artifacts, edge-flicker.

### T3-B. `feat/warp-follow-refine`
- **What:** iterative refinement — warp `I1` by current flow, recompute correlation cost against `I0`, update flow. Repeat 3-5×.
- **Effort:** ~120 LOC; reuses `feat/correlation-cost9`.
- **Source:** `01-optical-flow-algorithms.md` (RAFT-family standard, PWC-Net).
- **Fixes:** flow quality on fast motion. Major step toward PWC-Net-2018-level classical flow.

### ~~T3-C. `feat/capture-mode`~~ — ✅ SHIPPED and validated on real data
- **What:** `WFG_CAPTURE_DIR` env dumps prev/mid/curr triplets from present hook. Feeds training pipeline + confidence auto-calibration.
- **Effort:** ~80 LOC C++.
- **Source:** `03-training-pipelines.md` §2 + `06-max-winnative-inspirations.md` (Max's empirical calibration workflow).
- **Fixes:** enables Phase 2 (neural). Also drives auto-calibration of gate thresholds.
- **Depends on:** nothing (independent branch).

## Priority order — Tier 4 (neural — Phase 2) — ⚠️ RE-SCOPED BY MEASUREMENT 2026-08-31

**The original Tier 4 (fine-tune RIFE-4.25.lite, run it through ncnn-Vulkan) was
built far enough to measure, and it does not fit this hardware.** Kept here in
full because the numbers are the useful part — do not re-plan this from theory.

### ~~T4-A. `feat/rife-lite-fine-tune`~~ / ~~T4-B. `feat/ncnn-vulkan-inference`~~ — MEASURED, NOT VIABLE AS SPECIFIED

What was actually done, on our own captured data, from scratch (no scraped
datasets, no pretrained weights):

| Model | Params | Val PSNR (our hard game set) |
|---|---|---|
| IFNet-lite v5 (full) | 414K | **26.88 dB** |
| shrink-v1 (widths 64/48/32) | 206K | 26.28 dB |
| shrink-v2 (48/32/24 + half-res refine) | **112K** | 25.85 dB |

Quality shrinks gracefully — 27% of the parameters for −1.0 dB. The problem is
runtime, measured on a real Adreno 750 (Pocket FIT, Snapdragon 8 Gen 3) through
ncnn-Vulkan fp16, GPU genuinely engaged (CPU backend was 4.2× slower):

| Resolution | shrink-v1 backbone |
|---|---|
| 256 | 16 ms |
| 360p | 35 ms |
| 720p | 117 ms |

Against a **2–4 ms** budget. Ten to thirty times over at any usable resolution.
Two blockers beyond raw speed:

1. **`GridSample` is `support_vulkan=0` in ncnn** — the net's four warps fall
   back to CPU, which breaks the pipeline outright.
2. The full-resolution **refine head is ~70% of total cost** at every resolution.

Also: the exact ONNX will not fully load in ncnn — four `ScatterND` ops
(grid-from-flow) have no ncnn layer even after constant-folding (278 → 113
layers). Needs pnnx or a warp re-formulation.

### T4-D. `feat/neural-residual` — the surviving design

Keep flow and warping **classical** — the shipped FSR3-family shaders, already
on the GPU and effectively free — and add a **tiny conv-only residual** as the
sole neural cost: no GridSample, no warps in the neural part, ideally at reduced
resolution. shrink-v2 already showed that a reduced-resolution correction head
holds quality, which is the core bet.

**Prerequisite:** measure the real classical FSR3 baseline on-device and train
the residual against *that*. The earlier residual attempt (v6, 148K, 22.66 dB)
scored badly because its stand-in baseline was weak (21.32), not because the
design is wrong.

### T4-C. `feat/qnn-htp-delegate` (optional Snapdragon fast path)

Unchanged as an idea, but gated behind T4-D — there is no point routing a model
to the NPU until there is a model small enough to be worth routing.

## What's missing today — gap analysis

| SANFG success gate | Current | Gap-closer branches |
|---|---|---|
| 1. Consistent ≥90% doubling | ~25% (post swapchain+1) | T1-B (`present-wait-pacing`) + measurement tool |
| 2. No HUD ghost | Visible on all HUD | `feat/hud-rect-mask` (in flight) + T2-C (`heuristic-hud-detect`) |
| 3. No motion melt | Visible on pans | T2-E (`global-motion-prewarp`) + T3-A (`flow-regularization`) |
| 4. No pacing stutter | Unmeasured | T1-B + T1-C + `dumpsys SurfaceFlinger --latency` measurement tool |
| 5. Zero user knobs | Model + flow-scale sliders exposed | T1-C + T1-E + T2-C fold into "auto" mode; slider hidden by default |
| 6. No transparency artifacts (neural) | N/A (classical) | T4-A + T4-B |

## Suggested implementation order (my recommendation)

**Sprint 1 (this week):**
1. Finish `feat/hud-rect-mask` device-verify + merge to master → bake into Bannerlator
2. `feat/median-flow-filter` (T1-D) — smallest quality win, easiest to verify

**Sprint 2:**
3. `feat/gpu-saturation-skip` (T1-A) — stability first
4. `feat/present-wait-pacing` (T1-B) — biggest doubling-rate win
5. `feat/tier-gated-auto` (T1-C) — auto-mode groundwork

**Sprint 3:**
6. `feat/fb-consistency` (T2-B) — feeds T2-C and unblocks better synth gating
7. `feat/heuristic-hud-detect` (T2-C) — auto-populates hudRect
8. `feat/adaptive-gate` (T1-E) — bundle with T2-B since both use dominance-stats-buffer pattern

**Sprint 4:**
9. `feat/correlation-cost9` (T2-A) — sub-pixel flow accuracy
10. `feat/flow-inpainting` (T2-D) — hole-fill

**Sprint 5:**
11. `feat/global-motion-prewarp` (T2-E) — biggest single quality lever
12. `feat/flow-regularization` (T3-A) — TV-L1 smoothness

**Sprint 6:**
13. `feat/warp-follow-refine` (T3-B) — RAFT-family iterative refinement
14. `feat/capture-mode` (T3-C) — enables Phase 2

**⇒ Ship SANFG v1.0 (Phase 1 classical + all adaptive heuristics).**

**Later (Phase 2 — neural, ~2-3 months of focused work):**
15. `feat/rife-lite-fine-tune` (T4-A)
16. `feat/ncnn-vulkan-inference` (T4-B)
17. `feat/qnn-htp-delegate` (T4-C, optional)

**⇒ Ship SANFG v2.0 (Neural quality mode).**

## Related docs
- `SANFG.md` — vision and success criteria
- `01-optical-flow-algorithms.md` — flow algorithm survey
- `02-learned-fg-models.md` — learned models and licenses
- `03-training-pipelines.md` — training pipeline end-to-end
- `04-adreno-deployment.md` — runtime + Vulkan extension survey
- `05-prior-art.md` — competitor + provenance landscape
- `06-max-winnative-inspirations.md` — Max's PR #537 clean-room borrows
- `07-isygold-vegas-inspirations.md` — Isygold Vegas kit clean-room borrows
- `08-reusable-shaders.md` — permissively-licensed shader catalog
- `../PROVENANCE.md` — clean-room protocol
- `../THIRD-PARTY.md` — attribution ledger
