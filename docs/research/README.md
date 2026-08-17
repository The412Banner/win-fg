# win-fg — Research Index

Deep-dive research pass completed 2026-08-17. Every doc has its own
sources section at the bottom. Read `SANFG.md` first for the endgame
vision, then `ROADMAP.md` for the prioritized branch list.

## Docs

| # | File | Scope |
|---|---|---|
| — | [`SANFG.md`](SANFG.md) | The endgame — Smart Adaptive Neural Frame Generation. Naming, success criteria, phased roadmap. **Start here.** |
| — | [`ROADMAP.md`](ROADMAP.md) | Prioritized branch list Tier 1-4, gap analysis vs SANFG gates, 6-sprint implementation order. |
| 01 | [`01-optical-flow-algorithms.md`](01-optical-flow-algorithms.md) | Classical + learned optical flow survey. Top 3 techniques to port to `of3_*`. Ceiling of pure classical. |
| 02 | [`02-learned-fg-models.md`](02-learned-fg-models.md) | RIFE / IFRNet / XVFI / DAIN / DLSS3 / FSR3 — params, weights, licenses. Recommendation: RIFE-4.25.lite (MIT). |
| 03 | [`03-training-pipelines.md`](03-training-pipelines.md) | End-to-end how to train our own weights. Datasets, self-capture, losses, compute, export path, tooling gaps. |
| 04 | [`04-adreno-deployment.md`](04-adreno-deployment.md) | Runtime landscape (QNN / LiteRT / NCNN / raw Vulkan). Vulkan extensions on Turnip. What actually ships mobile FG in 2026. |
| 05 | [`05-prior-art.md`](05-prior-art.md) | DLSS3 / FSR3 / RIFE / LSFG / SVP / competitors. What killed bionic-fg. Provenance red lines. |
| 06 | [`06-max-winnative-inspirations.md`](06-max-winnative-inspirations.md) | Max's WinNative PR #537 clean-room analysis. What to borrow (patterns), what not to touch (bytecode). |
| 07 | [`07-isygold-vegas-inspirations.md`](07-isygold-vegas-inspirations.md) | Isygold Vegas-DXVK kit analysis. Adreno hardening insights. HUD-rect API + skip-window patterns. |
| 08 | [`08-reusable-shaders.md`](08-reusable-shaders.md) | Permissively-licensed shader catalog: FSR3-FG HLSL/GLSL, rife-ncnn-vulkan, NCNN Vulkan, SGSR. Top 5 to port. GPL red-flag list. |

## Provenance chain

- `../PROVENANCE.md` — clean-room shader-by-shader attribution (current)
- `../../THIRD-PARTY.md` — full attribution ledger for every borrowed pattern
- Every `.comp` header cites the paper/algorithm it implements

## Referenced from memory

Persistent memory pointers live under
`~/.claude/projects/-home-claude-user/memory/`:
- `project_win_fg_ecosystem_survey_202608.md`
- `project_win_fg_clean_room_port_plan.md`
- `reference_win_fg_shader_and_weight_authoring.md`
- `reference_win_fg_adaptive_heuristics_menu.md`
