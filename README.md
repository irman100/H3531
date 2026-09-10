# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

Current milestone: **0.5.3 Exclusive Graphics Session**.

Core architecture:

```text
h3531-input-init -> h3531-video-init -> storage/hotplug -> session supervisor -> h3531-monitor -> application
```

The Monitor owns the framebuffer and evdev only while its UI is active. In 0.5.3, an interactive graphical application is started with an **exec session handoff**: the Monitor process is replaced by the application instead of remaining alive as a parent process. When the application exits, the boot session supervisor starts a fresh Monitor.

Current graphics path:

```text
Matrix Brandy BASIC VI -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
```

Target environment: Linux 3.0.8, ARMv7 EABI soft-float, fixed 1280x720 16-bit A1R5G5B5/ARGB1555 framebuffer.

## Current status

- Direct HIFB graphics: physically proven.
- USB keyboard, mouse, mass storage and external USB hub: physically proven.
- Matrix Brandy BASIC VI graphics through the custom SDL 1.2 H3531 backend: physically proven.
- 0.5.1 removed per-pixel 64-bit divisions from the framebuffer scaler and made BASIC substantially more responsive on the physical board.
- 0.5.2 attempted exclusive input by closing Monitor evdev descriptors while a graphical child ran; physical testing still showed background Monitor/input conflicts and incomplete screen cleanup.
- 0.5.3 changes the process model instead of only closing descriptors: interactive graphical applications replace Monitor with `execve()`, and a supervisor restarts Monitor after the app exits.
- 0.5.3 also wipes the full mapped HIFB framebuffer to opaque black on graphics-session transitions and on fresh Monitor startup, so stale pixels from previous applications should not survive outside the application's logical viewport.
- Static `fbshow` currently retains the older hold-after path until the viewer itself gains an input loop.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
