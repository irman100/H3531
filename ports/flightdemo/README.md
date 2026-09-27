# Stayplaytion Flight Core

Stage 1 is a native Hi3531 real-time graphics/game prototype.

## Goals

- reuse the proven 1280x720 A1R5G5B5 HIFB path;
- render at a smaller 640x360 internal resolution and integer-scale 2x;
- use vendor HIFB vblank ioctl 0x4664 when available;
- run through the existing Stage6.6D live native framebuffer lease;
- support keyboard arrows and Linux joystick input;
- provide a moving holographic panel placeholder for a later hardware VDEC/VO experiment.

## Current scene

- 320 perspective stars;
- moving perspective grid;
- controllable ship;
- exhaust particles;
- animated flying holographic screen;
- FPS telemetry every 300 frames.

Exit with **Esc/F12** on keyboard or **Select+Start** style button pairs (6+7 or 8+9) on a gamepad.

No video decoding is used in Stage 1. The video panel is deliberately a placeholder so the basic renderer can be measured first.
