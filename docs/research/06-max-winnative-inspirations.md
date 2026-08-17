# 06 — Max's WinNative Frame-Gen — Inspirations for win-fg

Read + audit of the sibling FG effort by `maxjivi05` on WinNative
(PR #537, open since 2026-06-09, actively developed). Written 2026-08-17
after reading `vk_cnn_fg.c` and the `weights_v2/` + `wnfg_spv/` layouts.

**Rule:** clean-room protocol throughout. Patterns and inspirations only —
zero code, zero coefficient values, zero SPIR-V bytecode copied from his
tree. Every pattern we borrow is implemented from published literature
we cite, or from Max's *design intent* alone (not his numbers).

Source of truth: [WinNative-Emu/WinNative#537](https://github.com/WinNative-Emu/WinNative/pull/537)
by `maxjivi05` on branch `frame-gen`.

## What Max's stack actually is

- **Architecture:** in-tree modification of WinNative's Vulkan renderer.
  Adds `vk_cnn_fg.c` (894 lines) + ~2,300 lines to `vk_renderer.c`.
  Compositor-side, not a Vulkan layer.
- **6 core compute pipelines from `.comp` source** — this is the
  RAFT-family playbook:
  1. `cnn_pyramid` — multi-scale feature pyramid
  2. `cnn_conv` — feature extraction at each level
  3. `cnn_correlation_cost9` — 9-point cost volume comparing curr/prev features
  4. `cnn_correlation_warpfollow` — iterative flow refinement using warped features
  5. `cnn_flowreg` — flow-field regularization (smoothness prior)
  6. `cnn_generate` — multi-input synthesis (6 bindings: prev, curr, both flows, both features, occlusion)
- **8 additional pipelines from pre-compiled SPIR-V blobs**
  (`wnfg_04/13/25/27/28/29/51/53_spv.h`) — no `.comp` source in his tree.
  **Provenance unverified — do not read, do not disassemble.**
- **"Weights" are misleading naming** — the `weights_v2/wnfg_XX.weights.fp16`
  files are 54–4,704 bytes each, ~20 KB total. Real learned models are MB
  not KB. These are hand-crafted filter coefficients (Sobel, Gaussian,
  correlation kernels, warp coefficients), not trained neural weights.
- **Runtime gate:** activated via `__system_property_get("debug.winnative.fgcnn")` —
  he treats it as experimental / opt-in.
- **Scheduler:** headroom-driven, non-blocking present mode under adaptive
  panels; passes through under FIFO; real frame always presents.

## Recent commit themes — production-hardening

His PR log (2026-06 → 2026-08-15) reads like ours but ahead:

| commit | why it's inspiring |
|---|---|
| median-filter the flow | Outlier removal on the raw flow field. We should add this — 1 shader, ~30 lines. |
| stop the warp tearing thin objects apart | Thin-object handling — likely occlusion/edge mask + spatial refinement of flow around silhouettes. |
| stop cross-dissolving the two real frames over half the output | Says: the naive "just alpha-blend" fallback is worse than expected. Their fix likely mirrors what we already have with our photometric gate. |
| calibrate the confidence against ground truth | They measure their gate calibration empirically instead of hand-tuning constants. |
| hold a real frame where the flow is not trusted | Same principle as our current 3a fallback path. |
| fit the flow to each generated frame | Per-frame flow re-solve instead of temporal reuse. |
| present pacing, slot chatter | They're battling the same pacing issues we are. |
| resolve promotes per ... | Layout-transition minimization — Adreno-side optimization. |

**Lesson:** Max is 6 months into the same problem space and has shipped
fixes for many of the specific artifacts we're seeing today. The commit
titles alone are a roadmap of what breaks and needs solving.

## What to clean-room borrow — inspiration + implementation notes

### 1. Multi-pass RAFT-style architecture (biggest inspiration)
His pipeline order — **pyramid → feature-conv → cost-volume → flow-reg →
warp-follow-refine → generate** — is the same RAFT-family playbook we
proposed in `01-optical-flow-algorithms.md`. Confirms our port plan is
sound. Clean-room from RAFT paper (Teed & Deng 2020, arxiv:2003.12039);
zero of Max's code touched. Maps to our proposed branches:
- `feat/correlation-cost9`
- `feat/flow-regularization`
- `feat/warp-follow-refine`

### 2. Multi-input generate pass
His `cnn_generate` takes 6 inputs (prev, curr, forward flow, backward
flow, learned features, occlusion mask). Our current `wfg_synth.comp` takes
4 (prev, curr, F01, F10). Adding the occlusion mask from
`feat/backward-flow-consistency` gives us the fifth input trivially. The
"learned features" input we skip until we have a network to produce them.
Clean-room from any RAFT-derivative synthesis paper.

### 3. Median filter on the flow field
Simple, well-known, no learning needed. `flowfix.comp`-style post-process
after `of3_flow`. Cite Tukey 1977 (median as robust estimator). ~1 shader,
~30 lines of GLSL. Big win on outlier flow vectors that cause the current
speckle artifacts.

### 4. Per-frame flow re-solve
His "fit the flow to each generated frame" commit says he re-solves flow
per generated frame rather than temporally caching. We currently do
temporal cache-and-reuse; his approach costs more compute per frame but
tracks fast motion better. Worth A/B'ing when we have adaptive-flow-resolution
in place (`feat/dynamic-flow-resolution`).

### 5. Runtime `debug.<pkg>.<flag>` sysprop for opt-in
Instead of a compile-time flag, he uses an Android system property to
opt-in FG at runtime. Cleaner than our env var for the deployed case —
users can toggle without relaunching. Adopt for the shipped SANFG rollout:
`debug.winfg.enabled=1`. ~5 lines of Java + a `__system_property_get` call
in our present hook.

### 6. Empirical confidence calibration
His commit "calibrate the confidence against ground truth" says he stopped
hand-tuning gate thresholds and started measuring against ground-truth
midpoint frames. That's a real workflow: run FG offline against a known
120fps clip, compare the generated midpoint to the actual real midpoint,
walk thresholds to minimize error. We should build this into our capture
mode (`feat/capture-mode`) so the same tool that captures training data
also drives gate auto-calibration.

## What NOT to touch — provenance red lines

- **`wnfg_XX_spv.h`** (04, 13, 25, 27, 28, 29, 51, 53) — pre-compiled
  SPIR-V blobs without matching `.comp` source. **Do not read the SPIR-V
  disassembly.** Do not re-implement based on any bytecode inspection.
  Even indirect exposure creates a clean-room record contamination.
- **`weights_v2/wnfg_XX.weights.fp16`** — small, hand-crafted, likely
  Sobel/Gaussian/correlation kernels. But **do not copy the numeric
  values.** Derive our own from the same math formulas. The 5×5 Gaussian
  values everyone gets differ only in σ choice.
- **His `vk_cnn_fg.c` code itself** — we can read it once for architectural
  understanding (already done), but do not re-open when implementing.
  Implement from the RAFT paper + our own design.

## Clean-room protocol we're following (for the record)

1. Read the *idea* from Max's file names, PR body, commit titles.
2. Find the *published algorithm* it implements — cite paper (arxiv ID + year).
3. Close his file. Do not re-read while implementing.
4. Implement from the paper's math. Cite in shader header.
5. `THIRD-PARTY.md` entry:
   > Pattern inspired by architectural approach in `maxjivi05/WinNative#frame-gen`
   > (PR #537); implementation independent, derived from [paper citation]. No code,
   > coefficients, or bytecode copied.

## Recommendation for near-term win-fg planning

Rank the borrowed patterns by ROI × implementation cost:

| Pattern | Effort | Impact | Order |
|---|---|---|---|
| Median filter on flow (Max #4-inspired) | Low (~30 LOC) | Mid | **1st** |
| Correlation cost volume + parabola fit | Low (~80 LOC) | High | **2nd** |
| Backward-flow FB consistency (adds occlusion input to synth) | Low (~40 LOC) | High | **3rd** |
| Flow regularization pass | Med (~60 LOC) | High | **4th** |
| Global-motion pre-warp | Med-High (~150 LOC) | Very High | **5th** |
| Warp-follow iterative refinement | High (~120 LOC) | Very High | **6th** |
| Runtime sysprop toggle | Trivial | UX | Bundle with any of the above |
| Empirical confidence calibration tool | Med (capture-mode dep) | Enables auto-tune | With capture-mode branch |

Ordering matches `ROADMAP.md`'s Tier 1-3 progression exactly. Max's PR
confirms this ordering matters — his early commits were all in the same
Tier 1-2 space; the later commits (thin-object, confidence calibration,
pacing) are Tier 3 refinement.

## Should we align stacks with Max?

Argument for cooperation:
- Same problem space, same target hardware, same emulator ecosystem
- Compatible licenses (his project + our MIT-target)
- Both benefit from shared training data / benchmarking / diagnostic tools

Argument against tight coupling:
- Different architectures (his = in-renderer, ours = layer). Games see
  different swapchain semantics.
- Provenance clarity — the moment we accept a PR/patch from him and it
  contains anything derived from `wnfg_XX_spv.h`, our clean-room record
  is compromised.

Practical stance: **share diagnostics and benchmarks, not code.** Trade
notes on what works. If he ever open-sources the `.comp` for his
`wnfg_XX_spv.h` shaders under a permissive license, revisit.

## Sources
- [WinNative-Emu/WinNative PR #537 — Native frame generation (optical-flow interpolation)](https://github.com/WinNative-Emu/WinNative/pull/537)
- [RAFT: Recurrent All-Pairs Field Transforms for Optical Flow (arxiv:2003.12039)](https://arxiv.org/abs/2003.12039)
- [PWC-Net (CVPR 2018)](https://openaccess.thecvf.com/content_cvpr_2018/papers/Sun_PWC-Net_CNNs_for_CVPR_2018_paper.pdf)
- [Sundaram et al. Dense point trajectories by GPU-accelerated large displacement optical flow (ECCV 2010)](https://link.springer.com/chapter/10.1007/978-3-642-15549-9_32)
