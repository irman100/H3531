# H3531 RetroArch Stage 3.2 plan

Starting point: Stage 3.1 FastVideo hardware result: dynamic FCEUmm works and is much faster, but gameplay is still estimated by hardware test at roughly 80-90% of expected speed.

Stage 3.2 goals:

- Keep the Stage 3.1 RGB565 integer framebuffer fast path and dynamic-core architecture.
- Auto-select the external FCEUmm core at startup so `Load Content` works immediately.
- Compile RetroArch and FCEUmm for the Hi3531 Cortex-A9 using the hardware VFP/NEON execution units while retaining the soft-float EABI calling convention (`-mfloat-abi=softfp`).
- Add lightweight UART FPS/present-time diagnostics to separate core/runloop speed from framebuffer presentation cost.
- Keep audio disabled for this performance stage.
- Preserve the previous hardware-proven commits as rollback points.
