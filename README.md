# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

Current milestone: **0.6.1 Game Cleanup & Widescreen**.

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
- 0.5.3 physically proved the exec-based exclusive graphics session model.
- 0.5.4 physically proved resumable FILES state after graphical applications exit.
- 0.6.0 physically proved both the Matrix Brandy game path and the first native ARM Linux SDL game path.
- 0.6.1 fixes the BASIC paddle erase routine (`RECTANGLE FILL`), uses `CLG` for graphics clearing, moves the BASIC game to Matrix Brandy widescreen MODE 71, and changes native SDL Pong to a 640x360 16:9 logical surface that scales exactly 2x to 1280x720.

## Controls

- `F9` / `PADDLE`: Matrix Brandy paddle game. Arrow keys or A/D move; Q exits.
- `F10` / `PONG`: native Linux/SDL Pong. Arrow keys or A/D move, mouse also controls the paddle; Esc or Q exits.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
