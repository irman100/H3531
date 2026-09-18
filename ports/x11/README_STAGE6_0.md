# H3531 Stage6.0 — Xfbdev hardware proof

Stage5.x custom desktop is frozen as an experimental branch.

Goal of Stage6.0:
1. Run X.Org KDrive/Xfbdev directly on the already proven HIFB /dev/fb0.
2. Let X own the real evdev input devices.
3. Prove a clean hardware cursor, keyboard and mouse without the custom desktop input/repaint loop.
4. Keep Monitor and the current USB system untouched.

Known real-device input mapping from the 2026-09-18 hardware log:
- /dev/input/event0 — SIGMACHIP Usb Mouse
- /dev/input/event1 — CASUE USB KB, primary keyboard interface
- /dev/input/event2 — CASUE USB KB, secondary HID interface

Build target:
- Cortex-A9 / ARMv7-A
- EABI soft-float
- NEON disabled
- X.Org 1.14.7 KDrive/Xfbdev
- uClibc runtime bundled on USB

No saveenv. No SPI flashing.
