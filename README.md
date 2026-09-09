# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 board.

Current milestone: **0.5.x Graphical BASIC**.

Core architecture:

```text
h3531-input-init -> h3531-video-init -> storage/hotplug -> h3531-monitor -> applications
```

The Monitor owns the framebuffer while its UI is active. Native graphical applications temporarily receive exclusive ownership of `/dev/fb0`, then the Monitor rebuilds its UI after the child exits.

Current graphics path:

```text
Matrix Brandy BASIC VI -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
```

Target environment: Linux 3.0.8, ARMv7 EABI soft-float, fixed 1280x720 16-bit ARGB1555 framebuffer.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
