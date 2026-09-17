# Stage 4.26 — SNES/GBA Engine Tuning

Targets:
- GBA gpSP: Cortex-A9 ARMv7 softfp, VFPv3-D16, no NEON, dynarec required, low-cost audio and no post-processing/frameskip by default.
- GBA video: specialized RGB565 240x160 -> 960x640 integer 4x fast path.
- SNES Snes9x2005: Cortex-A9 ARMv7 softfp, VFPv3-D16, no NEON, lightweight APU configuration.
- SNES video: specialized RGB565 256x224 -> 768x672 integer 3x fast path.
- Preserve existing H3531 AO/input stack and Stage4.22 V4 emulation-speed telemetry.
- No image-quality reduction, no frameskip, no NEON.
