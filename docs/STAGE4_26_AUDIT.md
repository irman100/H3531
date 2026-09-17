# Stage 4.26 preliminary audit

Verified before gameplay logging:

- Target SoC CPU is ARMv7 Cortex-A9 with VFPv3-D16 and no NEON.
- Existing Stage4.23 SNES/GBA cores already use Cortex-A9 softfp compile flags.
- gpSP core exposes `gpsp_drc`; default is enabled when HAVE_DYNAREC is compiled.
- gpSP low-overhead sound option is 32768 Hz; frameskip, colour correction and frame mixing can remain disabled.
- Existing SNES/GBA RetroArch profiles already use H3531 video/input/AO, CC resampler and integer S16 audio fast path.
- Missing shared optimization relative to PS1 is specialized framebuffer scaling for native 16-bit source modes.

Stage4.26 therefore adds exact RGB565 integer scale paths for GBA 240x160 -> 960x640 and common SNES 256x224 -> 768x672, plus pinned gpSP performance-safe options. No quality reduction or frameskip is introduced.
