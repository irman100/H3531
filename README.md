# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

Current milestone: **0.5.2 Exclusive Graphics Input**.

Core architecture:

```text
h3531-input-init -> h3531-video-init -> storage/hotplug -> h3531-monitor -> applications
```

The Monitor owns the framebuffer while its UI is active. Native graphical applications temporarily receive exclusive ownership of `/dev/fb0` and evdev input, then the Monitor rebuilds its UI and reopens input after the child exits.

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
- 0.5.2 fixes graphics ownership in two places: Monitor closes its evdev clients while a graphical child runs, and both Monitor/SDL address the currently visible fbdev viewport using `xoffset`/`yoffset` instead of assuming page zero.
- HIFB clears use opaque black (`0x8000` in A1R5G5B5) so old surfaces cannot show through transparent pixels.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
