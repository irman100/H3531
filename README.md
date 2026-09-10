# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

Current milestone: **0.6.0 Games & Native SDL Apps**.

Core architecture:

```text
h3531-input-init -> h3531-video-init -> storage/hotplug -> session supervisor -> h3531-monitor -> application
```

The Monitor owns the framebuffer and evdev only while its UI is active. Interactive graphical applications use an **exec session handoff**: the Monitor process is replaced by the application, so Monitor cannot continue reading input or repainting the framebuffer in the background. When the application exits, the boot session supervisor starts a fresh Monitor. FILES state is handed off through writable RAM so the file manager can resume after an app exits.

Current graphics paths:

```text
Matrix Brandy BASIC VI -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
Native ARM Linux SDL app -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
```

Target environment: Linux 3.0.8, ARMv7 EABI soft-float, fixed 1280x720 16-bit A1R5G5B5/ARGB1555 framebuffer.

## Current status

- Direct HIFB graphics: physically proven.
- USB keyboard, mouse, mass storage and external USB hub: physically proven.
- Matrix Brandy BASIC VI graphics through the custom SDL 1.2 H3531 backend: physically proven.
- 0.5.1 removed per-pixel 64-bit divisions from the framebuffer scaler and made BASIC substantially more responsive on the physical board.
- 0.5.3 physically proved the exec-based exclusive graphics session model: screen cleanup and background Monitor interference are substantially improved.
- 0.5.4 physically proved resumable FILES state after graphical applications exit.
- 0.6.0 adds an original Matrix Brandy graphical game (`H3531-PADDLE.BAS`) and the first general-purpose native ARM Linux SDL application built against the same H3531 backend (`h3531-sdl-pong.APP`).
- Monitor 0.6.0 adds `GAMES`, `PADDLE` and `PONG`; F9 launches the BASIC game and F10 launches native SDL Pong.

## 0.6.0 controls

- `F9` / `PADDLE`: Matrix Brandy paddle game. Arrow keys or A/D move; Q exits the game.
- `F10` / `PONG`: native Linux/SDL Pong. Arrow keys or A/D move, mouse also controls the paddle; Esc or Q exits.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
