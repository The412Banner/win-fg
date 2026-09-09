# Contributing to Win-FG Native

Contributions are welcome. This project has one rule that is genuinely unusual
and matters more than anything else here, so it goes first.

## 1. The clean-room rule - read this before writing a line

**This project exists because a previous frame-generation layer was taken down
for shipping compute shaders derived from proprietary Lossless Scaling work.**
Everything here was rebuilt from published algorithms specifically so that
cannot happen again. A single well-meant pull request can undo that.

So, without exception:

- **Never** contribute shader code, bytecode, weights or coefficients copied or
  adapted from a proprietary source - Lossless Scaling, DLSS, XeSS, or any
  frame-generation binary you disassembled. This includes "I read the
  disassembly and then wrote my own version".
- **Never** contribute code adapted from a **GPL or LGPL** project. This
  codebase is MIT and ships inside a `.so`; a GPL kernel would relicense the
  whole thing. HopperRender and ffmpeg's `minterpolate` are the two people reach
  for most often. Both are off-limits here.
- **Permissive only**: MIT, BSD, Apache-2.0, zlib, or public-domain mathematics.
  With attribution, in `THIRD-PARTY.md`.
- **Every `.comp` shader header must cite the paper or algorithm it
  implements.** Not "adapted from project X" - the actual published source. If
  you cannot name a paper, that is the signal to stop.

If you are working from another implementation as *inspiration*, use the
protocol we use: read the idea, locate the published algorithm behind it, close
their code, implement from the paper's maths, cite the paper. Do not have their
source open while writing.

**A PR that cannot state where its algorithm came from will be declined**, even
if it works and even if it is faster. This is not bureaucracy - it is the only
reason this project is allowed to exist.

## 2. What is worth working on

`docs/research/ROADMAP.md` carries a tiered list, each item with a citation
trail to the paper behind it. The highest-value open items right now:

- **Flow / synthesis split.** The chain currently recomputes the optical flow
  for *every* generated frame. Computing it once per real frame and re-running
  only the final blend would make 3x and 4x nearly free instead of expensive.
  This is the single biggest efficiency gap.
- **Rate telemetry.** The engine logs what is *engaged*, never what is being
  *achieved*, so throughput has to be inferred from the host. A periodic line
  with source and presented rates would make device triage far easier.
- **C3 - correlation cost-volume + iterative refine** (RAFT / PWC-Net family)
  for sharper fast-motion flow.
- **C4 - occlusion- and edge-aware blend** for cleaner silhouettes.
- **The neural residual** (`ROADMAP.md` Tier 4). Note that full-synthesis CNNs
  have already been measured on Adreno and do not fit - read that section before
  proposing one, so a month of work is not repeated.

## 3. Building

```
./tools/build_shaders.sh          # glslang -> SPIR-V -> src/embedded_shaders.hpp
mkdir build && cd build
cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DVULKAN_INCLUDE=/path/to/Vulkan-Headers/include ..
cmake --build .
```

Shaders are compiled to SPIR-V and embedded into the binary at build time -
`src/embedded_shaders.hpp` is **generated**. Edit `shaders/*.comp` and re-run the
script; never hand-edit the generated header.

CI builds every push and **fails if the version stamp is missing**, so a shipped
binary can always be traced back to a commit:

```
strings libwin_fg.so | grep win-fg-build
```

## 4. Testing a change

Frame generation is unusually easy to fool yourself about, so:

- **Never trust an in-game frame counter as evidence.** It counts what the game
  submits, which is not what reaches the display. This project spent a long time
  believing it achieved 2x because a counter said so, while the panel was
  showing 1x and the generated frames were being discarded downstream. Confirm
  against the platform's own counter, or `dumpsys SurfaceFlinger --latency`.
- **Cap the frame rate to something the game can actually hold.** A cap it
  cannot reach makes the base rate wander, and every measurement after that is
  noise.
- **A/B properly.** `WIN_FG_GM=off` and `WIN_FG_FLOWREG=off` make the chain
  byte-identical to the version without that stage, so any difference you see is
  real. Both default to *on*, which means you are judging corrected output
  unless you turned them off.
- **Quality claims need a before/after** on the same scene at the same settings.
  Frame generation looks different frame to frame; impressions drift.

`docs/BRINGUP.md` collects the failure modes that have actually cost us time -
worth reading before debugging anything that looks like a driver problem.

## 5. Pull requests

- One logical change per PR. A shader change and a refactor in the same diff is
  hard to review and harder to revert.
- Say **what you measured**, on what hardware. "Feels smoother" is not a result;
  neither is a synthetic benchmark that never ran on a phone.
- Say **where the algorithm came from**, with the citation. See rule 1.
- Comments should explain *why*, not restate the code. The tricky parts of this
  codebase are tricky for reasons that are not visible locally - the existing
  comments try to record those, and that convention is worth keeping.
- If a change affects licensing or provenance in any way, update
  `THIRD-PARTY.md` and `docs/PROVENANCE.md` in the same PR.

## 6. Reporting a problem

Useful reports include the GPU and driver (Adreno 7xx/8xx, Turnip or a wrapper),
the host app and version, and the log - everything here logs to logcat under the
tag **`win-fg`**, lowercase. Two notes that will save you time:

- Android's log buffer rolls fast under load. **Capture unfiltered to a file and
  grep afterwards**; a tag-filtered capture started after the fact catches
  nothing.
- `WIN_FG_DEBUG=1` enables a per-frame trace of the present path, so a freeze
  leaves an obvious last line at the stage that hung.

## Licence

By contributing you agree your work is released under the [MIT licence](LICENSE)
that covers this project.
