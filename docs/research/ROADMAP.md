# ROADMAP — win-fg → SANFG

Synthesis of the 6-part research pass in this directory + the current
in-flight branches + the ecosystem survey. Prioritized branches ordered
by ROI to the SANFG endgame described in `SANFG.md`.

**Every branch listed here has a citation trail.** Follow the linked
research doc for algorithm sources and license notes.

## Current state (2026-08-17)

| Component | State |
|---|---|
| Vulkan layer scaffold | Shipped (`libwin_fg.so` in Bannerlator/main asset) |
| Phase 3a — interpolate + blit | Shipped |
| Phase 3b — true 2× spare-image insertion | Shipped (`61113bc`) |
| swapchain +1 (Isygold pattern port) | Shipped (`c91bb50`) |
| synth alpha 0.5 → 0.35 | Shipped (`2f50e40`) |
| `feat/hud-rect-mask` — HUD-rect exclusion push const | In flight, `22faf40`, CI green, hotswappable |
| Device-verified fps lift | 84 → 105 fps DiRT (25% doubling) |
| Known symptoms remaining | HUD ghost, camera-pan melt, sub-100% doubling |

## Priority order — Tier 1 (small, high ROI, ships weeks not months)

Each ~1-2 days of implementation + 1 hotswap cycle. All classical, all
clean-room, all inside `libwin_fg.so`.

### T1-A. `feat/gpu-saturation-skip`
- **What:** watchdog on fence-wait latency. N slow trips (>25ms) → skip FG for W frames → probe.
- **Effort:** ~30 LOC C++.
- **Source:** `07-isygold-vegas-inspirations.md` (Isygold Vegas kit constants N=5, W=60; behavior re-implemented).
- **Fixes:** graceful degradation under load; stops FG from making choking games worse.
- **Depends on:** nothing.

### T1-B. `feat/present-wait-pacing`
- **What:** use `VK_KHR_present_wait` (or `VK_EXT_present_timing` on Mesa 26.1+) to space generated frame at `T` and real frame at `T + refresh/2`.
- **Effort:** ~50 LOC + ext enable.
- **Source:** `04-adreno-deployment.md` (Turnip extension survey).
- **Fixes:** doubling-rate lift beyond swapchain+1 alone. Push ~25% → ~90%.
- **Depends on:** nothing.

### T1-C. `feat/tier-gated-auto`
- **What:** auto-decides FG on/off from smoothed frame-time EMA.
- **Effort:** ~50 LOC.
- **Source:** `07-isygold-vegas-inspirations.md` (thresholds T2≤29ms, T3≤33ms borrowed as Snapdragon-tier starting points; state machine independent).
- **Fixes:** stops FG punishing games that can't afford compute budget.
- **Depends on:** nothing.

### T1-D. `feat/median-flow-filter`
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

### T2-E. `feat/global-motion-prewarp`
- **What:** fit 6-parameter affine between prev/curr via Lucas-Kanade, warp `I1` before dense flow → residual flow is object-only.
- **Effort:** ~150 LOC across 2 shaders (`of3_gm_reduce.comp` + `of3_gm_prewarp.comp`) + CPU 6×6 solve.
- **Source:** `01-optical-flow-algorithms.md` (Szeliski Ch. 6.2 + Baker-Matthews 2001 inverse-compositional LK).
- **Fixes:** camera-pan melt at the source. **Biggest single quality lever** — removes ~70% of translational-camera flow magnitude.

## Priority order — Tier 3 (big, structural)

### T3-A. `feat/flow-regularization` (+ `of3_flowreg.comp`)
- **What:** TV-L1 smoothness prior post-process on flow field.
- **Effort:** ~60 LOC GLSL.
- **Source:** `01-optical-flow-algorithms.md` (Horn-Schunck 1981 or Chambolle-Pock TV-L1).
- **Fixes:** flow discontinuity artifacts, edge-flicker.

### T3-B. `feat/warp-follow-refine`
- **What:** iterative refinement — warp `I1` by current flow, recompute correlation cost against `I0`, update flow. Repeat 3-5×.
- **Effort:** ~120 LOC; reuses `feat/correlation-cost9`.
- **Source:** `01-optical-flow-algorithms.md` (RAFT-family standard, PWC-Net).
- **Fixes:** flow quality on fast motion. Major step toward PWC-Net-2018-level classical flow.

### T3-C. `feat/capture-mode`
- **What:** `WFG_CAPTURE_DIR` env dumps prev/mid/curr triplets from present hook. Feeds training pipeline + confidence auto-calibration.
- **Effort:** ~80 LOC C++.
- **Source:** `03-training-pipelines.md` §2 + `06-max-winnative-inspirations.md` (Max's empirical calibration workflow).
- **Fixes:** enables Phase 2 (neural). Also drives auto-calibration of gate thresholds.
- **Depends on:** nothing (independent branch).

## Priority order — Tier 4 (neural — Phase 2 gate)

Only start once Tier 1-3 ships and all 5 classical SANFG success gates are hit.

### T4-A. `feat/rife-lite-fine-tune`
- **What:** fine-tune RIFE-4.25.lite (MIT) on 50-150K self-captured Bannerlator triplets with Charbonnier + Laplacian + Census + Warp loss + HUD-preservation extension.
- **Effort:** 4-6 weeks — capture pipeline (`feat/capture-mode` prereq) + training rig (weekend on RTX 4080) + on-device deploy tuning.
- **Source:** `02-learned-fg-models.md` (RIFE-4.25.lite recommendation) + `03-training-pipelines.md` (end-to-end pipeline).
- **License:** MIT, warm-start from hzwer's public checkpoint, fine-tune erases Vimeo90K taint via data replacement, ship modified weights under MIT.

### T4-B. `feat/ncnn-vulkan-inference`
- **What:** integrate NCNN Vulkan backend to run the fine-tuned RIFE weights on device. Ship `.param/.bin` as APK asset.
- **Effort:** 2-3 weeks.
- **Source:** `04-adreno-deployment.md` + `08-reusable-shaders.md` (RIFE `warp_pack4.comp` reference).
- **Reference impl:** Allen Kuo's Android RIFE Medium series (2026-04).

### T4-C. `feat/qnn-htp-delegate` (optional Snapdragon fast path)
- **What:** ONNX → QDQ INT8/INT16 → QNN Execution Provider → Hexagon HTP.
- **Effort:** 3-4 weeks.
- **Source:** `04-adreno-deployment.md` (QNN SDK survey).
- **Fixes:** 3-4× inference speedup on Snapdragon devices. Falls back to NCNN Vulkan elsewhere.

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
