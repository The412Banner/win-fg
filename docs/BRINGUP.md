# Win-FG Native — device bring-up notes

Written as a plan before first hardware; rewritten 2026-09-09 as **what
actually happened**, because everything the original plan called "to do" has
since shipped and been proven on device. Kept for the gotchas — they are the
part that generalises to the next device.

## Status

Both builds are up and device-proven on an AYANEO Pocket FIT (Adreno 750,
Snapdragon 8 Gen 3, 144 Hz, Turnip), and the layer build additionally on a
Galaxy Fold 8 (Adreno 840).

| Stage | State |
|---|---|
| Layer loads, passthrough present stable | ✅ |
| Flow + synth verified on a moving scene | ✅ |
| True 2× insertion (spare image, generated then real) | ✅ |
| Synchronisation (semaphores + per-frame fence, no `vkDeviceWaitIdle`) | ✅ |
| Native mode — chain inside the host compositor | ✅ device-proven 45 → 90 fps |
| C1 global-motion + C2 flow-reg engaging automatically | ✅ confirmed in a device log |

## The five things that actually cost time

**1. Generated frames reaching the compositor and being thrown away.**
The layer inserted correctly — the in-game counter read 2× — and the panel
still showed 1×. Each present arrives at the host as a distinct buffer under
one window id, and the host kept only the newest. Nothing in the layer can fix
that from inside the guest. It is why the native build exists.
*Lesson: never trust an in-guest frame counter as evidence that frames reached
the display. Confirm with the platform's own counter or `dumpsys
SurfaceFlinger --latency`.*

**2. A generated frame carrying the guest's present id (Adreno 840 freeze).**
The insert path built a fresh `VkPresentInfoKHR` with `pNext = nullptr` for
*both* presents, which stripped the guest's `VkPresentIdKHR` off the real
frame. DXVK throttles via `vkWaitForPresentKHR`; a present id that is never
queued wedges that wait forever, so the guest render thread stalled after the
first insert and the app was force-closed ~30 s later. Only Adreno 840/gen8
advertises `VK_KHR_present_wait`, which is why the 750 never showed it.
*Fix: forward `pNext` onto real-frame presents only. A skippable frame must
never carry a present id.*

**3. Present mode is not a preference, it is a requirement — and the two
builds want opposite ones.** The native path queues the real frame and its
generated frames together and needs **FIFO** so they scan out on consecutive
vblanks; under Mailbox the presentation engine keeps only the newest queued
image per vblank and the whole batch collapses. A guest-side layer wants the
opposite, because FIFO back-pressure strangles its extra presents.

**4. The log tag is `win-fg`, lowercase.** Grep case-insensitively. An earlier
triage session concluded "this build has no telemetry at all" purely from
grepping `Win-FG`. Everything was there.

**5. Android's log buffer rolls faster than you think.** Under load the system
writes ~3,000 lines every few seconds, so a tag-filtered capture started after
the fact catches nothing, and the interesting lines here are deliberately
rate-limited (C1 prints on state change, then every 300 frames).
*Capture unfiltered to a file and grep afterwards.* That is what caught the
double chain build in the sibling LSFG engine.

## Validation order that works

1. Confirm the engine initialises — `framegen init ok (… 10 pipelines, model=M,
   C1 gm=1, C2 flow_reg=1 iters=4)`.
2. Confirm it resizes to the real surface — `framegen resized to WxH (7 pyramid
   levels, kFlowFinest=…)`.
3. Confirm C1 engages on a moving scene — `C1 global-motion: engaged=1 …
   stable=N`, where `stable` should climb into the hundreds and stay.
4. **Confirm the panel, not the counter.** Cap the game at something it can
   actually hold, arm 2×, and read the platform frame counter: it should read
   double the cap. A cap the game cannot reach makes the base rate wander and
   the result unreadable.
5. Only then judge image quality — and know that C1 and C2 are on by default,
   so you are judging the corrected output unless you turned them off.

## Knobs worth reaching for during bring-up

`WIN_FG_DEBUG=1` for the per-frame present-path trace (a freeze then leaves an
obvious last line at the stage that hung). `WIN_FG_GM=off` / `WIN_FG_FLOWREG=off`
to A/B the quality stages — both make the chain byte-identical to the version
without that stage, so a difference is real. `WIN_FG_PERF_PRESET` to trade flow
resolution for cost; it rebuilds live.
