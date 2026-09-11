# H3531 Home Computer

Experimental home-computer environment for the HiSilicon Hi3531 / AHB70XXT16-3531 DVR board.

Current public milestone: **v0.7 FBZX Z80 demo**.

The goal is not to build only a ZX Spectrum machine. The project turns an old DVR platform into a small experimental home-computer environment: safe USB/RAM boot, own framebuffer monitor, FILES browser, BASIC, native ARM/SDL applications, and emulator experiments.

## Public article and quick start

- Russian article: [`docs/articles/h3531-home-computer-ru.md`](docs/articles/h3531-home-computer-ru.md)
- Quick start: [`docs/quickstart-v0.7-fbzx-z80-ru.md`](docs/quickstart-v0.7-fbzx-z80-ru.md)
- Release notes draft: [`docs/releases/v0.7-fbzx-z80-demo.md`](docs/releases/v0.7-fbzx-z80-demo.md)

## Core architecture

```text
h3531-input-init -> h3531-video-init -> storage/hotplug -> session supervisor -> h3531-monitor -> application
```

The Monitor owns the framebuffer and evdev only while its UI is active. Interactive graphical applications use an **exec session handoff**: the Monitor process is replaced by the application, so Monitor cannot continue reading input or repainting the framebuffer in the background. When the application exits, the boot session supervisor starts a fresh Monitor. FILES state is handed off through writable RAM so the file manager can resume after an app exits.

Current graphics paths:

```text
Matrix Brandy BASIC VI -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
Native ARM Linux SDL app -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
FBZX 3.1.0 Z80 snapshot demo -> SDL 1.2 H3531 backend -> /dev/fb0 -> HIFB -> VOU -> HDMI
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
- Windows Boot Kit 0.7 auto-intercept + full bidirectional UART terminal is physically proven. It remains a live terminal after Linux boot and does not use `saveenv` or SPI writes.
- FBZX 3.1.0 now runs on the physical board as an emulator experiment; `.Z80` snapshots can be opened from FILES through `FBZX.APP`.

## Quick Z80 demo layout

The public USB kit should have this layout on a FAT32 USB drive:

```text
/
├── H3531.IMG
├── zImage.img
├── h3531-video-init
├── h3531-input-init
├── FBZX.APP
├── keymap.bmp
├── spectrum-roms/
│   ├── 48.rom
│   └── if1-2.rom
└── Games/
    └── game.z80
```

ZX Spectrum ROM files and games are not committed here. Add them yourself according to your local legal/copyright situation.

## Safe boot rule

Do not use `saveenv`. Do not flash experimental images into SPI. The current workflow is intentionally USB/RAM-first so failed experiments do not brick the board.

## Continuation / new chat

Before continuing development in a new chat, read **[`docs/CHAT_HANDOFF.md`](docs/CHAT_HANDOFF.md)**. It records the current hardware facts, safe boot recipe, Boot Kit status, latest app fixes, and active follow-up tasks.

This repository contains project-owned source, patches, build scripts, tests and documentation. Vendor firmware/SDK material is not committed here unless its redistribution terms are known to permit it.
