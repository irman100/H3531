# H3531 Stage6.1 — Xfbdev + Matchbox WM

Stage6.0E hardware proof on 2026-09-18 confirmed the complete X11 input path:

- Debian Wheezy armel runtime runs on Hi3531.
- Xfbdev opens /dev/fb0 and stays alive.
- KDrive evdev receives /dev/input/event0 mouse events.
- KDrive evdev receives /dev/input/event1 keyboard events.
- XKB compiles the keymap.
- xev receives MotionNotify, ButtonPress/ButtonRelease and KeyPress/KeyRelease.
- The vendor rootfs boots with loopback unconfigured; Stage6 launchers now configure 127.0.0.1 automatically.

Stage6.1 adds only the next mature X11 layer:

```text
Hi3531 HIFB /dev/fb0
  -> rebuilt Debian Wheezy Xfbdev
  -> KDrive evdev
  -> Matchbox Window Manager
  -> visible X11 client
```

Hardware proof goals:

1. Matchbox remains alive above the proven Xfbdev server.
2. A managed X11 window is visible.
3. Matchbox titlebar/decorations render correctly.
4. Mouse pointer is visibly usable.
5. Mouse click and keyboard interaction work in the managed client.
6. No repeated full-screen flicker or cursor trails.
7. Matchbox exits cleanly and releases Xfbdev.

Safety remains unchanged: USB/RAM only, no saveenv, no SPI writes, and the resident Monitor must be STOPped before the proof and CONTinued afterward.
