# 07 — Isygold's Vegas-DXVK Frame-Gen Kit — Inspirations for win-fg

Read + analysis of the framegen kit sent by Isygold on 2026-08-17 (author
of the Vegas DXVK fork). Kit received as `framegen-kit.zip`, extracted to
scratchpad. **License: zlib/libpng — MIT-compatible, we can borrow with
attribution.**

**Rule:** patterns and Adreno-hardening insights only. Zero code, zero
coefficient values, zero SPIR-V copied from the kit. Every borrowed
pattern is implemented from first principles or from published literature,
with attribution to the kit as inspiration source.

## What the kit is

- **Architecture:** in-DXVK. Called from D3D11/9/DXGI `PresentImage()`.
  Host owns the frame loop.
- **Motion:** block SAD on 16×16 tiles, hierarchical ±8 coarse + ±2 refine,
  variance-aware confidence gate.
- **3-pass pipeline:** motion → 3×3 median filter → warp+blend @ 0.5 alpha.
- **HUD rect** passed as `vec4 hudRect` push constant (block coords),
  motion disabled inside the rect.
- **Runtime discipline:** `FG_SKIP_WINDOW = 60` — 60-frame skip when GPU
  saturated (queue timeout >25ms trips slowCount, 5 trips = 60-frame skip).
  Tier-gated auto: T1 disabled, T2 ≤29ms, T3 ≤33ms.
- **Stats:** bimodal-dominance atomic SSBO tracks per-block classification.
- **License:** zlib/libpng — MIT-compatible.

## Adreno hardening insights (documented in his shader comments)

These are the kind of insights only shipping to Adreno silicon produces:

1. **Butterfly reductions collapse occupancy at 11.5KB shared on A610.**
   His `star_fg_motion_v2.comp` header notes: 36 barriers/workgroup +
   11.5KB shared collapsed occupancy → 91% dispatch-timeout rate. Fix =
   shared atomics (4 barriers). We should keep our shared-memory footprint
   under 8KB per workgroup as a hard rule. Documented in Isygold's
   inline comment.
2. **Packed atomicMin ordering.** He uses `uint(cost·16) << 8 | idx` for
   packed argmin over shared atomics. Note: `floatBitsToUint(cost) << 8`
   would wrap the exponent for costs ≥ 2.0, breaking ordering. Fixed-point
   packing is required. We adopt this exact approach for any argmin we
   need in shared memory.
3. **VK_QCOM_image_processing** — Turnip Mesa 26.1+ supports native
   block-match / weighted-sample ops. Isygold isn't using these yet but
   they're the perfect fit for a block-SAD motion pass. We should watch
   for this.
4. **AAudio-tight-latency parallel** — while unrelated to FG, his kit
   demonstrates the tight-latency mobile Vulkan work culture. Serves as
   proof-of-concept that mobile GPUs can hit tight budgets when you code
   to their quirks.

## What to clean-room borrow

### 1. HUD-rect exclusion API (implementation in flight)
`feat/hud-rect-mask` on our repo (upstream `22faf40`) implements Isygold's
`vec4 hudRect` push-constant pattern in `wfg_synth.comp`. Ours plumbed via
`WIN_FG_HUD_RECT` env / `hudRect` conf.toml key. **Design borrowed;
implementation independent.**

### 2. GPU-saturation skip window
`feat/gpu-saturation-skip` (planned). Watchdog counter on fence-wait
latency; N slow trips (>25 ms) → skip FG for W frames → probe. Constants
`N=5, W=60` come from Isygold's values as a starting point; we retune on
Pocket FIT / Adreno 750.

### 3. Tier-gated auto activation
`feat/tier-gated-auto` (planned). EMA on smoothed frame time; below
threshold → FG on, above → off. Boundaries derived from measurement on
our target devices — Isygold's 29/33 ms values are Snapdragon-tier
starting points.

### 4. Bimodal-dominance stats SSBO
Isygold emits per-block classification (unimodal / bimodal / flat) into an
atomic SSBO. Lets him confirm the actual scene regime instead of guessing
from visuals. We adopt the pattern for our adaptive-gate branch — emit
insert/fallback + confidence histogram per 300 presents into a small
stats buffer, read back to auto-tune photometric gate.

### 5. Fixed-point packed atomicMin for shared-memory argmin
Whenever we do a per-block argmin (e.g. cost-volume min pass), use the
`uint(cost·16) << 8 | idx` pattern. Documented in Isygold's shader header
as a gotcha; we cite him for the trap.

### 6. Shared-memory footprint discipline
Every new compute shader we add stays under 8 KB shared per workgroup on
Adreno TBDR to preserve occupancy. This is a repo-wide standard we adopt
based on Isygold's A610 experience report.

## Attribution to ship

`THIRD-PARTY.md` entry:

> **Isygold Vegas-DXVK framegen kit** (zlib/libpng, 2026):
> - HUD-rect exclusion API design — pattern borrowed, `wfg_synth.comp`
>   implementation independent.
> - GPU-saturation skip window — starting constants (N=5 slow trips,
>   W=60 skip window) borrowed as tuning seed; behavior re-implemented.
> - Tier-gated auto activation — 29/33 ms thresholds borrowed as Snapdragon
>   starting points; state machine re-implemented.
> - Adreno-hardening notes (11.5KB shared cliff, packed atomicMin ordering,
>   avoid butterfly reductions) — technical notes acknowledged; no shader
>   code borrowed.
> No source code, coefficient values, or SPIR-V bytecode copied.

## Sources
- Isygold's framegen kit: private zip, received 2026-08-17. Kit README
  cites: `isygold` (lead dev), `@devaspe` (testing), DXVK v2.4.1 by
  doitsujin, zlib/libpng license.
- [DXVK upstream](https://github.com/doitsujin/dxvk)
- [Vegas DXVK fork commentary — Reddit r/EmulationOnAndroid](https://www.reddit.com/r/EmulationOnAndroid/search/?q=vegas+dxvk)
- Adreno TBDR occupancy — [Qualcomm Adreno Vulkan best practices](https://developer.qualcomm.com/software/adreno-gpu-sdk/gpu)
- [Mesa 26.1.0 — VK_QCOM_image_processing on Turnip](https://docs.mesa3d.org/relnotes/26.1.0.html)
