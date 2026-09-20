# H3531 Home Computer — Stage 6.6A

Persistent desktop environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

## Current architecture

Stage 6.6A replaces the old "Monitor -> exclusive application -> Monitor" user
experience with a persistent X11/LXDE desktop.

```text
boot
  -> /sbin/h3531-session
  -> USB SYSTEM/MONITOR.APP supervisor
  -> short original-Monitor HDMI/HIFB initialization
  -> automatic Ethernet/DHCP
  -> Stage6.5D FullFrame Xfbdev
  -> Openbox + PCManFM + LXPanel
  -> normal X11 applications remain inside the same desktop session
```

The original Monitor remains as a fail-safe and as a hardware-initialization
helper. Legacy applications that still require exclusive `/dev/fb0` ownership
use an explicit compatibility handoff; they are no longer the default desktop
application model.

## Stage 6.6A goals

- one persistent LXDE session;
- one canonical desktop entry point: `START-DESKTOP.sh`;
- automatic DHCP at boot;
- Stage6.5D FullFrame repaint path retained to avoid drag/scroll trails;
- FBZX launched as an X11 window by default;
- H3531 AO/HDMI audio retained for FBZX;
- native framebuffer FBZX retained only as an explicit fallback;
- no `saveenv`, no SPI writes, no kernel replacement, no bootloader replacement.

## Repository layout

- `ports/stage66a/` — canonical desktop supervisor and runtime entry points.
- `ports/x11/` — shared HIFB/network/runtime helpers.
- `ports/fbzx/` — native fallback and Stage6.6A X11 FBZX build sources.
- `ports/nofrendo/` — current Nofrendo port.
- `ports/retroarch/` — current retained RetroArch source/configuration.
- `ports/frontend/` — GameFront source and assets.
- `.github/workflows/h3531-stage66a-persistent-desktop.yml` — canonical Stage6.6A package build.
- `tools/cleanup-obsolete-branches-stage66a.ps1` — ancestry-safe remote branch cleanup.

Target: Linux 3.0.8, ARMv7 EABI soft-float, 1280x720 HIFB/HDMI.
