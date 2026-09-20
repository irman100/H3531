# H3531 Stage 6.6A — Persistent Desktop

Stage 6.6A changes the system model from "desktop as one fullscreen application"
to a persistent X11/LXDE desktop.

## Goals

- Boot directly into the proven Stage6.5D FullFrame LXDE session.
- Bring Ethernet/DHCP up automatically during boot.
- Keep Xfbdev/Openbox/PCManFM/LXPanel alive while normal desktop applications run.
- Run FBZX as an X11 window by default so closing the emulator does not destroy
  open folders or restart the desktop.
- Keep the proven native H3531 framebuffer FBZX as an explicit compatibility
  fallback.
- Keep legacy framebuffer-app handoff in the supervisor for applications that
  have not yet been ported to X11.

## Boot ownership

```
h3531-session
  -> SYSTEM/MONITOR.APP (Stage6.6A supervisor)
       -> MONITOR.ORIGINAL.APP for short HDMI/HIFB initialization
       -> STOP original Monitor
       -> automatic DHCP
       -> Stage6.5D FullFrame LXDE
```

The original Monitor is retained as a fail-safe. No saveenv, SPI NOR write,
kernel replacement, or bootloader modification is performed.

## FBZX modes

Default:

```
PCManFM / Open With / file association
  -> h3531-fbzx.desktop
  -> FBZX-X11.APP
  -> FBZX-X11.BIN
  -> SDL 1.2 X11 window
```

Compatibility fallback:

```
h3531-run fbzx-native [game]
  -> supervisor releases X11
  -> native FBZX.APP owns /dev/fb0
  -> FullFrame LXDE is restored afterwards
```

## Canonical active workflows

Stage6.6A intentionally removes old iterative workflow files from the active
branch. Git history still contains them.

The active branch keeps only the current workflows needed for:
- Stage6.6A persistent desktop
- native FBZX fallback
- Nofrendo v11.1
- latest retained RetroArch builds
- latest retained GameFront build
- base apps/basic builds

Hardware acceptance for 6.6A:
1. cold boot reaches FullFrame LXDE with network already configured;
2. double-clicking a Spectrum file opens FBZX in a normal X11 window;
3. closing FBZX leaves PCManFM folders and LXPanel alive;
4. no Monitor flash or X11 restart occurs for X11 FBZX;
5. HDMI audio remains functional through the H3531 AO backend.
