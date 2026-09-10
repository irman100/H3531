# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

Current milestone: **0.5.4 Resumable File Manager**.

Core architecture:

```text
h3531-input-init -> h3531-video-init -> storage/hotplug -> session supervisor -> h3531-monitor -> application
```

The Monitor owns the framebuffer and evdev only while its UI is active. Interactive graphical applications use an **exec session handoff**: the Monitor process is replaced by the application, so Monitor cannot continue reading input or repainting the framebuffer in the background. When the application exits, the boot session supervisor starts a fresh Monitor.

0.5.4 adds a small one-shot session state handoff for FILES. Before an application launched from the file manager replaces Monitor, the current directory, selected entry and scroll position are saved in writable RAM. The fresh Monitor consumes and deletes that state file and immediately restores FILES at the previous location. This preserves exclusive ownership without making the old Monitor stay alive behind the application.

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
- 0.5.3 physically proved the exec-based exclusive graphics session model: screen cleanup and background Monitor interference are substantially improved.
- Physical testing of 0.5.3 showed an expected side effect: leaving an application also lost the file-manager view because the old Monitor process no longer existed.
- 0.5.4 keeps the exec model and restores FILES state after the graphical application exits; this release still needs physical validation on the board.
- Static `fbshow` currently retains the older hold-after path until the viewer itself gains an input loop.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
