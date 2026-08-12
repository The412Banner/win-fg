
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
