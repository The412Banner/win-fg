
## 2026-08-31 — DEVICE-PROVEN: neural-flow-net via ncnn-Vulkan is ~order-of-magnitude too heavy; STRATEGIC PIVOT to classical-flow + tiny conv-only residual
- **v0.2.1 CUT + released.** Tagged the exact win-fg shipped in Bannerlator 3.0.2 (= `v0.2` + 12 commits, `512212e`), reconciled `master` (was a divergent branch tip), GitHub Release + assets (authentic `libwin_fg.so` sha `0096525b…`). CI now stamps `git describe` into every `.so` (`build.yml` fetch-depth:0 + `-DWINFG_GIT_DESCRIBE`; CMake `version_stamp.cpp`, `used`+default-vis `win_fg_build_version`; build FAILS if `strings|grep win-fg-build` absent). CI GREEN (run 33412960451). Recover a future binary's commit via that grep.
- **Own-weights model scoreboard** (30ep P100, our hard game val set): v5 full IFNet-lite 414K=**26.88 dB** · v6 residual(classical stand-in flow + 148K net)=**22.66 dB** (baseline-limited; stand-in ≠ FSR3, only 21.32) · shrink-v1 widths(64,48,32) 206K=**26.28 dB** (half size, −0.6 dB → neural flow shrinks gracefully). shrink-v2 (48,32,24 + half-res refine) = **112,197 params → 25.85 dB** (only −1.0 dB vs v5 / −0.43 vs shrink-v1 despite 27% of v5's params AND the refine head run at half-res). KEY POSITIVE: reduced-res neural correction keeps quality — validates the pivot's core bet. Still NOT deployable (same neural-flow family: GridSample warps CPU-only + est ~4.4 GMAC@720p ≈ ~7ms@360p device, ~2-3× over budget) → confirms the family FLOOR.
- **Exact FLOPs (shrink-v1, script-verified via ONNX export):** 17.86 GMAC/35.7 GFLOP @720p (=51% of v5) · 4.47 GMAC @360p · 1.27 @256. Graph carries GridSample + ConvTranspose.
- **DEVICE-MEASURED on real Adreno 750 (AYANEO Pocket FIT, Snapdragon 8 Gen 3), ncnn-Vulkan fp16, via root bridge** (GPU genuinely engaged: CPU backend 4.2× slower, ~3% variance): shrink-v1 conv/deconv backbone = **16 ms @256 / 35 ms @360p / 117 ms @720p** vs the 2-4 ms budget → **~7-14× over even at 360p, ~30× at 720p**. Realized only ~280-310 GFLOP/s (no coop-matrix on Turnip, tiny dispatches) → the earlier ~5× paper estimate was optimistic by ~2-4×.
- **Two hard blockers:** (1) **`GridSample` = `support_vulkan=0` on ncnn → CPU-ONLY** (Deconvolution/ConvTranspose = GPU, fine). The net's 4 warps cannot run on the GPU backend → break the pipeline. (2) full-res **refine head ≈ 70% of cost** at every resolution (same culprit as v5). Also: exact ONNX won't fully load in ncnn — 4 `ScatterND` (grid-from-flow) have no ncnn layer even after ORT constant-fold (278→113 layers); needs **pnnx** or a warp re-formulation (Concat/BinaryOp, not ScatterND).
- **VERDICT:** shrinking the whole neural-flow net and shipping it through ncnn-Vulkan is **not viable on this hardware** — ~an order of magnitude too slow at any resolution AND GridSample can't run on the GPU.
- **⭐ STRATEGIC PIVOT (device-justified):** the only viable shape = keep **flow + warp CLASSICAL** (the shipped FSR3 shaders — already on the GPU, essentially free) + a **TINY CONV-ONLY residual** (no GridSample/warps in the neural part, ideally at reduced resolution) as the sole neural cost. = the residual design done right. Prereq = measure the **REAL FSR3 baseline** on-device (deferred harness) and train the tiny residual against it (v6's 22.66 was capped by the weak 21.32 stand-in, not the design).
- Env note: device benchmark installed Termux pkgs (libncnn/protobuf/abseil/onnxruntime/…), killed a runaway `pip install ncnn`; artifacts at `/data/data/com.termux/files/usr/tmp/winfg/` (benign, note for cleanup). ncnn gets a real Vulkan device only via the root bridge (Adreno750, gpu_count=1), NOT the PRoot sandbox (gpu_count=0).


- **SUCCESS.** Kaggle kernel `the412banner/winfg-train-v1` v5 completed 30 epochs in ~174min (2.9h) on a Tesla P100 (~196 img/s steady). **Best val PSNR = 26.88 dB @ epoch 28**, clean monotonic curve ep0 21.44 → ep29 26.86, plateaued ~26.8-26.9 last ~6 epochs = converged for this 414K-param model on this data. First OWN-WEIGHTS VFI baseline for win-fg, trained on our own `.wfgcap` captures, from-scratch (clean license).
- Checkpoint saved: `/home/claude-user/winfg-kaggle/checkpoints/winfg_ifnetlite_v5_ep28_psnr26.88.pt` (1.67MB fp32, sha `a0df5f34…`). best.pt/last.pt also on the kernel Output tab.
- ⚠️ 26.88 dB is on OUR motion-biased game val set (deliberately hard: explosions/fast pans), NOT comparable to Vimeo90K SOTA ~35-36 dB (huge models, millions of curated frames). This is a legit first baseline, not a ceiling.
- **5 kernel versions to clear Kaggle gotchas (all baked into `winfg-kaggle/`):** (v1) no-internet pip → phone-verify + online qoi; (v2/v3) dataset mounts at `/kaggle/input/datasets/<owner>/<slug>/` → `resolve_data_dir()` globs for manifest.jsonl; (v3) no GPU → phone-verify; (v4) Kaggle torch dropped Pascal sm_60 (P100) → reinstall `torch==2.5.1+cu121`; (v5) model channel bug (refine cin 13→10). Model+decode+GPU-arch each validated via isolated probe kernels before the full run.
- **NEXT (owner graphics-vulkan-engineer): (1) residual design** — retrain net to CORRECT the classical FSR3 flow output (deployable, cheap) not full-synthesis; (2) perceptual/census loss + more captures; (3) optional RIFE-teacher distillation (license-check); (4) export best.pt → guest Vulkan compute path (ONNX) + C-series distillation. Full detail in Bannerlator memory `project_bannerlator_winfg_kaggle_training`.

## 2026-08-30 — ROUTE-B OWN-WEIGHTS: first REAL .wfgcap VALIDATED + Kaggle training pipeline built + FULL 24GB dataset uploading (from-scratch, user-confirmed)
- **MILESTONE: first real on-device `.wfgcap` capture decoded + validated** (clears the long-open "never verified a real .wfgcap" item). Capture = `/storage/emulated/0/Download/win-fg/session_1788126027889` on Pocket FIT (Bannerlator 3.0.2+76, consent v1): 90 shards (~269MB, 24.05GB) + `manifest.jsonl` (24,662 records) + `consent.json`. **73,986 training triplets.** NOTE: this Claude runs IN PRoot on the device → `/storage/emulated/0/...` reads DIRECTLY (no root bridge needed for the data).
- **`.wfgcap` FORMAT (reverse-engineered from header+manifest, verified — canonical spec):** `manifest.jsonl` = consent line, then HEADER (`format:qoi-rgba8`, `mode:patch`, `patch:256`, `patches_per_triplet:3`, `motion_thresh:2.0`, `shard_cap_bytes:256MiB`, src/dst 1280×720), then one record/triplet `{seq, src_center, ts_ns, motion, ps, shard, rec_off, patches:[{x,y,w,h,off,len}×3]}`. Shards = `WFGCAP01` blob store; each `patch{off,len}` = ABSOLUTE byte range holding a standard **QOI** blob (`qoif`, W=768 H=256 ch=4 RGBA) = `[prev|center|next]` L-to-R; **CENTER is the interpolation target (t=0.5)**. Extract = seek(off)/read(len)/QOI-decode → 256×768 → split 3× 256×256.
- **Validation evidence:** first 200 manifest offsets ALL land on `qoif`; seq5 explosion triplet has MAD(prev,cen)=84, MAD(cen,next)=86, MAD(prev,next)=171 ≈ 84+86 → center is a true temporal midpoint (visually confirmed: dark burst → mid-flame → white flash). Stats: motion p50=13.9 (0 records <2.0 → thresh held), 1931 records motion≥20, only 6 degenerate tiny patches (len≤8000) of 74k, 45 src-gaps>3 → clean contiguous triplets.
- **PIPELINE built `/home/claude-user/winfg-kaggle/`** (all py compile; reader verified on real shards): `wfgcap_dataset.py` (PyTorch Dataset; QOI backend qoi→Pillow→pure-py; yields prev,next,center CHW[0,1]); `curate.py` (curated_train/val JSON + upload_manifest; drops len≤8000; contiguous seq-block val hold-out = no leak; full=71586/2394, subset `--shards 20`=16542/594/5.38GB); `upload_kaggle.py` (PRIVATE dataset via symlinks, no bulk copy, dry-run unless --yes); `train_winfg.py` (**IFNet-lite**: coarse-to-fine bidir flow + backward warp + fusion mask; FROM-SCRATCH default = no external weights = clean license; optional --rife-warmstart; Charbonnier+0.5·gradient loss, OneCycle, AMP, val PSNR); `make_kernel.py` → `kernel/` (private GPU notebook, embeds both .py via %%writefile, attaches dataset).
- **DECISION — from-scratch (user-confirmed).** RIFE strategy verdict: full RIFE (even 4.25-lite) = ~10-30ms on DESKTOP GPU → NOT deployable in our ~2-4ms Adreno budget (single graphics+compute queue, no async) → would COST fps. Our IFNet-lite is already a mini-RIFE (same coarse-to-fine flow→warp→fuse shape, shrunk). RIFE's real role = optional TEACHER later (distill RIFE's outputs on our pairs into our tiny net) — ⚠️ non-commercial weight license makes even distillation legally grey, needs a real check. Smartest DEPLOYMENT target = **neural RESIDUAL** (tiny net outputs a correction ON TOP of the fast classical FSR3 flow, fixing ghosting/doubling on fast motion), not full-frame synthesis.
- **RELEASE LOOP (how learned weights reach users):** train on Kaggle → `best.pt` → (graphics-vulkan-engineer) export brain to phone-GPU shader form + wire into `libwin_fg.so` (likely residual-on-classical) → copy .so into Bannerlator `app/src/main/assets/win-fg/` → build APK on CI → device-prove (sharper AND still in fps budget) → cut release (⚠️ user's call, versioning rule). Flywheel: capture more games/motion → add shards → re-run same scripts → better best.pt → new release. Pipeline is reusable (no rebuild-from-scratch next cycle).
- **STATE:** PRIVATE Kaggle dataset `the412banner/winfg-triplets-full-v1` uploading (bg, ~78/90 shards, ~15-30MB/s phone uplink; superseded the tiny `winfg-vfi-smoke-subset` 23MB). Kaggle CLI = `kaggle==1.5.16` (newer pulls rpds-py which fails to build here; 1.5.16 has no jsonschema dep). Kernel `winfg-train-v1` staged. ⏭️ RESUME: upload done → `kaggle kernels push -p winfg-kaggle/kernel` → confirm running → PING USER (they asked) → watch status, pull best.pt → hand arch/export to graphics-vulkan-engineer. ⚠️ Kaggle GPU needs account phone-verify. Detail in Bannerlator memory `project_bannerlator_winfg_kaggle_training`.

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

---

## 🔖 CHECKPOINT 2026-08-26 — v0.2 shipped; Route-B data pipeline built

**Shipped:** win-fg **v0.2** released (tag `v0.2`, .so+manifest assets). Quality stack —
anti-ghost (quality-tier2) + **C1 global-motion prewarp** (LK affine; device-proven "feels
great" + logcat engaged) + **C2 TV-L1 flow-reg** (semi-implicit lagged-diffusivity; device
"runs great") — all merged to `master` (`0381044`) and baked into Bannerlator `main` with
model-3 as default. (The stale "FG on main is passthrough" note above is superseded — real
2× insertion since Phase-3b; now the full quality stack.)

**Route-B (own AI weights) — data pipeline BUILT:**
- `feat/capture-mode` (`9f22ff26`, .so `4b31e07f`): dev-only training capture — real
  pre-interpolation swapchain frames → lossless **QOI** `.wfgcap` containers + `manifest.jsonl`
  + anonymous consent block. Gated `WIN_FG_CAPTURE`/`capture=` (default off, zero overhead).
  Knobs: dir, mode(patch/frame), W/H, patches, motion, shard_mb. NOT merged to master.
- App side (Bannerlator `feat/winfg-training-capture`): toggle + "I understand" consent +
  720p/1080p/Match-game resolution picker + "?" help dialog; shareable pubg APK staged for
  crowdsourced footage collection.
- Kaggle token verified (curl/urllib REST, CLI won't build here); GPU needs phone-verify.

**NEXT:** user records PoC footage (good-motion games, stable FPS) → train tiny VFI residual
on Kaggle (warm-start RIFE MIT, fine-tune on our captures) → convert FP16 + inference shaders
→ hot-swap. Classical polish after: **C4 (occlusion/edge blend) before C3 (cost-volume/refine)**.
Parked: async-compute (wrong tool for GPU-bound). Open bug: Fold-8/Adreno-840/Wrapper crash.
