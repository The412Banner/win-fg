# win-fg — device bring-up plan

The host compiles and runs the full flow+synth compute pipeline. Two things are
finished only on real hardware, because they are swapchain/driver-specific:

## 1. Frame insertion (the actual 2x)
`winfg_QueuePresentKHR` currently runs the compute for `(prev, curr)` and then
presents the real frame unchanged (safe passthrough). To insert the generated
frame:
1. Keep an owned copy of the previous presented image (`vkCmdCopyImage` on
   present, since the swapchain image is reused).
2. On present of `curr`, generate `G = interp(prev, curr, alpha=0.5)` into a
   spare swapchain image acquired via `vkAcquireNextImageKHR`.
3. Present `G` first, then `curr` — doubling the present rate.
4. Higher multipliers add more `alpha` steps.

## 2. Synchronisation
- Wait on the app's render-finished semaphore before sampling `curr`.
- Signal a semaphore the present waits on after the synth dispatch.
- One in-flight fence per frame (or a small ring) instead of `vkDeviceWaitIdle`.

## Validation order
1. Confirm the layer loads (`WIN_FG_ENABLE=1`) and passthrough present is stable.
2. Debug-readback the generated image; verify flow + synth output on a moving scene.
3. Wire insertion (step 1) behind a flag; measure pacing.
4. Tune flow (`WIN_FG_FLOWSCALE`) and synth (`WIN_FG_BETA/LAMBDA/EPSILON`) —
   resume the `fg011-m34` flow sign/scale work here.
