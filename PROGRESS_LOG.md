
## 2026-08-26 — ADRENO-840 FREEZE FIXED (present-id) + PERF PRESET + PACING (branch feat/debug-logging; HEAD 82fb2ca8; definitive .so 7f2e709c, CI run 32996280723)
- **FREEZE ROOT CAUSE (device-proven Fold 8 / Adreno 840 / Turnip):** the insert-path `presentOne` built a fresh `VkPresentInfoKHR{pNext=nullptr}` for BOTH presents → STRIPPED the guest's `VkPresentIdKHR` off the REAL frame. DXVK throttles via present_wait (`vkWaitForPresentKHR`); a never-queued present-id wedges that wait forever → the guest render thread stalls after the FIRST insert (all steps r=0), force-close ~30s later. Device split: `VK_KHR_present_wait` is advertised on a840/gen8 but not 750/gen7 → only the 840 wedged. Corroborated by GameNative `lsfg-vk-android` `b2e989cbf` (identical a840 lsfg fix). Prior theories (image-pool starvation, FIFO present mode) BOTH disproven on device.
- **FIX = forward pNext to REAL presents only:** `presentOne` now takes a `const void* pNext`; every REAL-frame present forwards `pPresentInfo->pNext` (first-FG `:915`, insert `:1014`, fallback `:1052`); the GENERATED/spare present keeps `pNext=nullptr` (a skippable frame must never carry a present-id). No-op where the guest attaches none (750). **DEVICE-PROVEN Fold/840: freeze gone + 2× FPS.**
- Also on the branch: guest-acquire/submit diagnostic probes (gated `WIN_FG_DEBUG && enabled`) + surfaceCaps logging + `insert-complete` marker; `extra_images` knob + pool-headroom guard + hardened passthrough (`80c1f08` — kept as harmless headroom, NOT the freeze fix).
- **PERF PRESET (`2b38d232`):** conf `perf_preset` (0=Quality/kFlowFinest1, 1=Balanced/kFlowFinest2 DEFAULT, 2=Performance/kFlowFinest3) + env `WIN_FG_PERF_PRESET`. Fully HOT — `configure()` self-rebuilds flow images (DeviceWaitIdle+destroyScratch+ResetDescriptorPool+onResize(force)) + resets the flow predictor on change; no app reset needed. Default byte-identical to before.
- **PACING (`82fb2ca8`):** conf `pacing` default ON + env `WIN_FG_PACING`. Fixes clustered-pair judder — EMA of the real-frame interval (CLOCK_MONOTONIC, minus self-injected wait so BASE FPS isn't throttled), place generated near the temporal midpoint, hold the real present to `paceLastReal+dt` via `clock_nanosleep(ABSTIME)`; skip-late + per-wait cap `min(dt,20ms)`, never GPU-blocks; FIFO collapses to ~0. Adds ≤dt/2 phase latency; degrades to back-to-back when saturated. NOT device-proven (heuristic) — verify cadence via `pace:`/`pacing:` markers.
- **Video diagnosis (Fold):** base 14–28fps GPU-bound; presented unstable 18.8↔59.3 with 53ms spikes at 61% GPU = pacing; user was on Model=Bidirectional (m4) → recommend Optical flow (m3). Ghosting = classical flow ceiling (tune + weights).
- **Shipped/staged:** freeze-fix .so `680411a3` (present-id + probes) MERGED to Bannerlator main `35601f0f` (+ debug-logging UI + capture + steam). Definitive .so `7f2e709c` (all of the above) baked into the staged FINAL combined APK `/sdcard/Download/Bannerlator-allfixes-combined-pubg.apk` (+ GameNative lsfg-vk v1.0.4 gen8-fix .so `6eecff37`). NOT yet merged to main (test build); NOT a Bannerlator stable cut.

## 2026-08-25 — C2 FLOW REGULARIZATION (TV-L1 smoothness prior) implemented (branch feat/c2-flow-regularization, off feat/c1-global-motion-prewarp @ f841efc)
- Cleans the LEFTOVER OBJECT flow that C1 leaves behind. C1 removes the CAMERA motion at
  the source; C2 denoises the object-only residual (flowLvl_[kFlowFinest], 1/4-res) with a
  variational TV-L1 prior: L1 data term + edge-aware Total-Variation regularizer. Kills
  incoherent/spurious per-block vectors while KEEPING true motion discontinuities (object
  silhouettes) sharp — L1/TV, not H-S's L2 which would over-smooth the edges. ROADMAP T3-A.
- New shader `of3_flowreg.comp` = ONE regularization iteration; N iterations ping-pong two
  rgba16f flow images. Scheme is a lagged-diffusivity SEMI-IMPLICIT (Jacobi) TV-L1 solve,
  NOT textbook Chambolle-Pock primal-dual: same energy, but unconditionally stable for any
  dt>0 (primal is a convex combo of centre+neighbours ⇒ never NaN/diverges), no persistent
  dual images, one-shader/ping-pong contract kept. Turnip-safe by construction. The optimal
  TV dual p=g·∇u/|∇u|_ε (|p|≤1) is re-derived closed-form each iter; L1 data step is the
  exact soft-threshold u = uDiff − clamp(uDiff−f0, −θ, θ), θ=dt·λ (branchless). Edge weight
  g=exp(−edgeAlpha·|∇I|) from prev luma at the flow level ⇒ respects object boundaries.
  (Zach-Pock-Bischof 2007; Chambolle-Pock 2011; ROF 1992; Vogel-Oman 1996 / Chan-Mulet 1999
  for the solver; Perona-Malik / Weickert for the edge weight; H-S 1981 baseline.)
- ORDERING: slots AFTER of3_flow and BEFORE of3_expand. f0 (data term) = flowLvl_[F], read
  every iter, NEVER written; u ping-pongs flowRegA_↔flowRegB_; final iterate copied back into
  flowLvl_[F] (GENERAL→GENERAL vkCmdCopyImage, barriered) so of3_expand's binding is unchanged.
  Effective order = TV-L1 (C2) THEN expand's existing 5-tap median (final speckle scrub — L1
  is already outlier-robust, so no destructive double-smooth). Composes with C1: C2 cleans the
  RESIDUAL; expand still adds the global affine back → end-to-end flow correct. 0 iters / off ⇒
  flowLvl_[F] untouched ⇒ BYTE-IDENTICAL to pre-C2 (bail is never worse).
- Knobs (user tweaks after): `WIN_FG_FLOWREG` env / `flow_reg=auto|on|off` (default auto=on);
  `fr_iters` (default 4), `fr_lambda` (2.0), `fr_dt` (0.25), `fr_edge` (8.0), `fr_eps` (0.05)
  — env WIN_FG_FR_ITERS/LAMBDA/DT/EDGE/EPS. Iteration count is the primary cost knob. Logs
  (tag win-fg) engage+iters+knobs in `framegen init` and every 300 frames.
- Cost: N tiny dispatches at 1/4-res (~57k px @720p, ~5 flow + 5 luma fetches each) + 1 image
  copy; est. ~0.05–0.15 ms/frame at 4 iters. New images: flowRegA_/flowRegB_ (2× rgba16f @
  1/4-res, ~0.5 MB each @720p). CI-green target; NOT device-proven (owner's next step).

## 2026-08-25 — C1 GLOBAL-MOTION PRE-WARP implemented (branch feat/c1-global-motion-prewarp, off feat/quality-tier2 @ a1ca5e6)
- Attacks the settled root cause below (flow SATURATION on fast camera motion → oil-paint
  melt) at its source: estimate the per-frame camera AFFINE with inverse-compositional
  Lucas-Kanade (Baker & Matthews 2004; Szeliski §6.2) on 1/8-res luma and REMOVE it
  before the dense SAD search, so the search only ever sees small, coherent, object-only
  residual motion. Global affine is composed back in of3_expand(_m4) → wfg_synth unchanged.
- New shaders: `of3_gm_reduce.comp` (accumulate 6x6 LK normal equations, NO float atomics —
  fixed 512 threads each write their own partial sum, CPU sums in double + Cholesky solve;
  Huber-robust), `of3_gm_prewarp.comp` (bilinear affine warp of the curr luma pyramid).
- Solve is DECOUPLED / stall-free: reads back the reduce SSBO the present ring already
  fence-waited (per-slot SSBO bound to the FrameCtx), one Gauss-Newton step/frame warm-
  started from the running estimate (~1-2 frame latency; camera motion is temporally
  smooth so it tracks). SAME affine removed + added back ⇒ result exact for any affine;
  IDENTITY affine ⇒ byte-identical to pre-C1, so bail-to-identity ≤ "no pre-warp" always.
- Robustify + bail: coverage floor, bounded GN step, PD Cholesky, final sanity clamp →
  otherwise keep identity. Auto-engages after 3 consecutive good solves.
- Knob: `WIN_FG_GM` env / `global_motion=auto|on|off` in conf.toml (default auto). Logs
  (tag win-fg) engage state + estimated global translation (px) every 300 frames.
- Added GPU cost estimate: ~0.1–0.2 ms/frame (reduce on ~40k texels + 5 tiny prewarp
  levels); CPU solve ~10–20 µs. CI-green target; NOT device-proven (owner's next step).

## 2026-08-12 — blur/melt root-caused: flow SATURATION on camera motion
- Long device debug of "blurry in motion, bg/fg fixes it". Ruled out (proven in code
  + logs): NOT 3a-fallback (insert=N fallback=0 always), NOT flow accumulation (flow
  cleared every frame, stateless), NOT prev staleness (prev re-copied every gen frame),
  NOT framegen recreate on bg/fg (no CreateSwapchain across bg/fg). bg/fg only "fixes"
  it by feeding zero-motion frames (flow~=0 -> no warp -> sharp).
- Screenshots at flowScale=1.0 + loose gate = full-frame OIL-PAINT MELT (incl ~0mph
  after a crash = residual cam motion). Tight gate (0.04-0.18) + flowScale 0.70 =
  much better, center sharp, fast PERIPHERY still smears.
- ROOT CAUSE: kLevels=5 -> solver reach only ~±84px/frame @720p; fast camera motion
  (150-300px) saturates the SAD search -> each block latches a different spurious
  match -> incoherent vectors -> smear/melt. Trust knobs (gate/flowScale) only trade
  melt<->blur; neither is sharp because the flow itself is wrong.
- FIX (staged, .so d83c40c5, NOT merged): kLevels 5->7 (reach ~±300px), smoothness
  prior 0.002->0.006. Conf clamp is [0.05,4.0] (config.hpp:28) — that's why the
  flowScale=9 viz sentinel got clamped to 4.0 and never tripped the >5 gate; gate now
  >3.5. App rewrites conf on every FG toggle (clobbers live edits).
- PENDING device retest: does wider pyramid kill the camera-motion melt at 0.70, and
  can flowScale then go back to 1.0 sharp.

## 2026-08-12 — PARKED (moving to other 3.0 work). State + findings.
### Where it stands
- Branch feat/bringup-logging, best dev .so = d83c40c5 (CI run 31614138581), NOT merged.
  Contents: tight photometric gate (0.04-0.18), kLevels 5->7 (solver reach ~±84 ->
  ~±300 px/frame), smoothness prior 0.002->0.006, live flow-visualiser (gate
  flowScale>3.5), per-present insert/fallback diagnostic logging.
- Device: .so un-pinned (chattr -i) so a future Bannerlator install manages win-fg
  normally. Re-stage d83c40c5 from CI when resuming.

### Root cause (settled)
- "Blurry in motion, bg/fg fixes it" = FLOW QUALITY ceiling, not a state bug.
  Proven in code+logs it is NOT: 3a-fallback (insert=N fallback=0), flow accumulation
  (flow cleared every frame), prev staleness (re-copied every gen frame), or framegen
  recreate on bg/fg. bg/fg only helps by feeding zero-motion frames (flow~=0 -> no warp).
- Melt/smear = solver saturation on fast CAMERA motion + noisy block matching. Trust
  knobs (flowScale, gate) only slide between MELT (trust bad flow) and BLUR (suppress
  it); neither is sharp because the underlying vectors are wrong.

### Gotchas for next time
- conf flowScale is CLAMPED to [0.05,4.0] (config.hpp:28).
- The APK rewrites conf.toml on EVERY in-game FG toggle (and relaunch) with the side-
  menu slider value -> live `bridge` conf edits get clobbered on toggle. To test a
  flowScale, set the in-game slider (persists) OR patch writeWinFgConfig default.
- .so changes need a full game RELAUNCH (loaded at process start); conf hot-reloads live.

### Apex / Winlator-Mali research (user asked)
- Apex Frame Generation = based on MCFI (Motion Compute/Compensated Frame Interpolation)
  — block motion-compensated interpolation on Vulkan, SAME family as win-fg. No secret
  sauce; community reports "impressive but overhead limits usefulness" = same ceiling.
- Beating it in quality means AI (RIFE via NCNN) — reintroduces trained-weights /
  licensing risk, conflicts with the clean-room mandate unless a permissive model.
- Standard MCFI coherence levers (our NEXT dev steps if resumed): OBMC (overlapped
  block motion compensation) + motion-vector MEDIAN filtering + occlusion handling ->
  coherent field, less smear. Maps to: MV median + edge-aware (color-guided) upsample
  in of3_expand.comp.
- ANVIL (arXiv 2603.26835): use game/codec motion vectors as PRIORS. Not available to
  us (DXVK present gives no game MVs).

### NEXT (when resumed)
1. Test wider-pyramid (d83c40c5) at flowScale=1.0 — blocked only by the conf-clobber;
   set in-game slider to 1.0 first. Does 1.0 now give SHARP (vs the old melt)?
2. If still melts: add MV median + color-guided (edge-aware) upsample in of3_expand
   (the MCFI/OBMC coherence lever).
3. Product decision for 3.0: ship the conservative soft-but-stable FG honestly, or keep
   win-fg experimental/passthrough until coherence pass lands. FG on main is passthrough.
