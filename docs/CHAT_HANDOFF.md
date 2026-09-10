# H3531 Home Computer — New Chat Handoff

Last updated: **2026-09-10**.

Use this file as the first source of truth when continuing the H3531 project in a new ChatGPT/Codex chat. It intentionally separates **physically proven** behavior from builds that still need board validation.

## Project identity

- Project: **H3531 Home Computer**.
- Board: **AHB70XXT16-3531 V2.01**.
- SoC: **HiSilicon Hi3531**, ARMv7, Linux sees 2 CPUs/SMP.
- Factory kernel kept for now: **Linux 3.0.8**.
- Main repository and sole source of truth: **https://github.com/irman100/H3531**.
- Clone URL: `https://github.com/irman100/H3531.git`.
- Main branch: `main`.
- Do **not** use the HuDu repository for H3531 development.

Main links:

- README: https://github.com/irman100/H3531/blob/main/README.md
- Release notes 0.6.1: https://github.com/irman100/H3531/blob/main/docs/releases/0.6.1.md
- SDL 1.2 H3531 video backend: https://github.com/irman100/H3531/blob/main/ports/sdl12/SDL_nullvideo.c
- SDL 1.2 H3531 input backend: https://github.com/irman100/H3531/blob/main/ports/sdl12/SDL_nullevents.c
- Native Pong source: https://github.com/irman100/H3531/blob/main/apps/native/h3531-sdl-pong.c
- BASIC Paddle source: https://github.com/irman100/H3531/blob/main/examples/basic/H3531-PADDLE.BAS
- Windows UART/boot terminal: https://github.com/irman100/H3531/tree/main/tools/windows

Main HEAD at this checkpoint before this documentation update was `d733c284d169096babc4c92190cd641738506b3d`.

## Non-negotiable safety rule

Experimental images are booted from USB into RAM through U-Boot.

**Never use `saveenv` during development and do not flash experimental RAMDisk builds into SPI.**

The known safe manual boot is:

```text
usb start
fatload usb 0:1 0x82000000 zImage.img
fatload usb 0:1 0x83000000 H3531.IMG
setenv initrd_high 0xffffffff
setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)
bootm 0x82000000 0x83000000
```

Do not replace `mem=130M` with 512M casually. The vendor memory/MMZ layout uses the remaining first bank and a second DDR bank for multimedia/video memory.

## Current proven architecture

```text
h3531-input-init
        -> h3531-video-init
        -> storage/hotplug
        -> session supervisor
        -> h3531-monitor
        -> application
```

For interactive graphical programs the lifecycle is now:

```text
Monitor owns screen + evdev
        -> launch app
        -> Monitor is replaced with execve()
        -> app exclusively owns screen + evdev
        -> app exits
        -> session supervisor starts a fresh Monitor
```

This is physically proven and solved the old framebuffer/input interference problem.

FILES resume state is stored in writable RAM and restored after a graphical app exits. This is physically proven as of 0.5.4.

## Hardware / framebuffer facts

- `/dev/fb0`: **1280x720, 16 bpp, A1R5G5B5 / ARGB1555**.
- line length: about 2560 bytes.
- frame size: 1280 x 720 x 2 = 1,843,200 bytes.
- alpha bit matters: `0x0000` is transparent black, `0x8000` is opaque black.
- proven graphics chain: `ARM userspace -> /dev/fb0 mmap -> HIFB -> VOU -> HDMI`.
- Sofia/dvrHelper are not required for this graphics path.
- USB keyboard, mouse, storage and an external USB 2.0 hub are physically proven.

## SDL 1.2 H3531 backend

Repository files:

- `ports/sdl12/SDL_nullvideo.c`
- `ports/sdl12/SDL_nullvideo.h`
- `ports/sdl12/SDL_nullevents.c`

Important behavior:

- logical app surface is 32-bit;
- physical framebuffer is converted to 16-bit A1R5G5B5;
- `rgb1555()` sets the alpha bit `0x8000`;
- cached X/Y scaling maps remove per-pixel divisions;
- visible framebuffer base respects `xoffset/yoffset`;
- active viewport is cleared to opaque black;
- `SDL_PREALLOC` is used;
- dirty rectangles are scaled to the physical framebuffer;
- evdev scans event devices dynamically and handles keyboard, relative mouse, wheel and buttons;
- current scan range is event0..63 with periodic rescan.

**Important for the current FBZX work:** the latest FBZX GitHub Actions run successfully completed the step `Build static SDL 1.2 with H3531 backend`. The current failure occurs in the later `Build FBZX 3.1.0 for H3531` step. Therefore do not assume SDL needs redesign until the exact FBZX compile/link error proves it.

## BASIC and native apps — proven milestones

Primary BASIC: **Matrix Brandy BASIC VI**.

Working graphics chain:

```text
Matrix Brandy -> SDL 1.2 H3531 backend -> /dev/fb0 -> HDMI
```

Matrix Brandy graphics (`MODE`, `GCOL`, `MOVE`, `DRAW`, etc.) are physically proven.

Key milestone history:

- 0.3.3: PTY line editor/history good.
- 0.3.5: file manager fixed and physically confirmed.
- 0.3.6: full checkpoint physically confirmed; reduced flicker and Open With.
- 0.4.0: graphics foundation, `.APP`, fbshow, Matrix Brandy as primary BASIC.
- 0.4.1: dynamic input/hot-plug/source viewer; physically proven.
- 0.5.1: cached framebuffer scaling; major physical speed improvement.
- 0.5.3: exec-based exclusive graphical session physically proven.
- 0.5.4: resumable FILES state physically proven.
- 0.6.0: both a graphical Matrix Brandy game and a native ARM C/SDL game launched and ran on the real Hi3531 board.
- 0.6.1: BASIC Paddle moved to widescreen MODE 71 and fixed old-paddle erase with `RECTANGLE FILL`; native Pong moved to 640x360 16:9.

Latest native Pong source fix: commit `8090bf3a0b9db9bcd12f24c977b42d660f15e39a` (`fix(pong): restore static background under moving objects`). It restores the clean static background under the old ball/paddles instead of painting flat black, so the centre dashed line, score and frame should survive sprite movement.

GitHub Actions run `34476022624` for that commit completed successfully. Its artifact is `H3531-APPS-0.6.0`, artifact id `10151517049`, digest `sha256:f6211f40b3d3dd134cf46cb02f1ee35e56ce5ac12d1e421bf2d0585cc0d52647`.

**Physical validation of the post-0.6.1 game fixes is still required.** The older 0.6.0 game paths themselves are physically proven.

## Windows Boot Kit 0.7 — physically proven

The user physically confirmed Boot Kit 0.7 works correctly and this item is considered finished.

Current Windows implementation lives in:

- `tools/windows/H3531-UART-BOOT.ps1`
- `tools/windows/START-H3531.cmd`
- `tools/windows/TERMINAL-ONLY-H3531.cmd`

Behavior:

1. `START-H3531.cmd` opens an isolated PowerShell UART terminal; `cmd.exe` does not remain in control of Ctrl+C.
2. The terminal listens for the physical U-Boot marker:
   `PHY 0x02: OUI = 0x01F0, Model = 0x0F, Rev = 0x01`
3. Immediately at that marker it sends sustained UART Ctrl+C (`0x03`) until `hisilicon #` appears.
4. It automatically runs USB start, loads `zImage.img` and `H3531.IMG`, sets temporary bootargs and executes `bootm`.
5. After Linux starts, the same window remains a full bidirectional UART terminal.
6. Ctrl+A..Ctrl+Z, arrows, navigation keys, F1..F12 and common ANSI/xterm sequences are forwarded.
7. Local-only shortcuts: Ctrl+Alt+H for help, Ctrl+Alt+Q to close.
8. `TERMINAL-ONLY-H3531.cmd` provides the same terminal without autoboot automation.

Boot Kit 0.7 ZIP SHA-256 from the generated checkpoint bundle:
`ad46327d0b294654b3a79756f5ad0ee9b0665502f3fe583053aae068465d7c5a`.

Files inside that bundle included:

- `H3531.IMG` 6,230,080 bytes, SHA-256 `4d8e46e45a1f9a350817cb1479d3e8777e759c906dae13888f354d77e7594bc3`.
- `zImage.img` 2,406,872 bytes, SHA-256 `f8d4f8d908a0211734c874816920db5b3c05baeaf89a2aa960ce17e8394ae9fa`.

Important: this `H3531.IMG` was assembled before the latest native Pong static-background fix (`8090bf3...`). Rebuild the RAMDisk later if the latest Pong fix is to be included in the boot image.

## Factory U-Boot findings relevant to boot automation

U-Boot reports itself as 2010.06-era vendor firmware.

Physically checked command set includes `bootm`, `run`, `setenv`, `fatload`, `fatls`, `usb`, `usbboot`, `go`, etc.

The factory U-Boot does **not** implement:

- `source`
- `autoscr`

Therefore `H3531.SCR` legacy script execution is not a valid approach on this board. The working solution is the Windows UART Boot Kit / manual commands.

## Current third-party emulator task: FBZX

Goal: port a real third-party ZX Spectrum emulator rather than another project-owned demo.

Chosen project: **FBZX 3.1.0**.

Work branch:
`feature/fbzx-h3531-realwork`

Branch HEAD at this checkpoint:
`2d8a2d8d25634735d397134755d3c64950072d94`
(`fix(fbzx): add SDL include parent for legacy headers`).

Important files on that branch:

- `.github/workflows/h3531-fbzx-build.yml`
- `ports/fbzx/build-h3531.sh`

Upstream FBZX revision intended to be pinned:
`981d48272e1cd04ce258e1060fd9574dd6bb4a60`.

Latest CI run:

- workflow: `H3531 FBZX Spectrum cross-build`
- run id: `34484813958`
- result: **failure**
- `Build static SDL 1.2 with H3531 backend`: **success**
- `Build FBZX 3.1.0 for H3531`: **failure**
- an always-upload artifact `H3531-FBZX-0.1` was created but was only about 339 bytes, so it is not a usable emulator binary.

Do not claim a finished `fbzx.APP` yet.

The current build script already:

- cross-compiles with `arm-linux-musleabi-g++`;
- targets ARMv7 soft-float;
- uses static/non-PIE linking;
- removes PulseAudio/ALSA desktop dependencies for the first test;
- uses the existing H3531 SDL 1.2 backend;
- adds the SDL include parent so old `#include <SDL/SDL.h>` can resolve.

One cleanup to notice: the shell script defaults to the exact pinned upstream commit, but the workflow currently passes `FBZX_REF=3.1.0`, overriding that default. Normalize this to the exact commit when continuing.

### Next FBZX work sequence

1. Inspect the exact compiler/linker error from run `34484813958` step `Build FBZX 3.1.0 for H3531`.
2. Fix the smallest compatibility issue.
3. Re-run CI repeatedly; do not stop after the first failed attempt.
4. Only change the H3531 SDL backend if the error or a later physical test demonstrates a missing SDL facility.
5. Produce a real ARM ELF `fbzx.APP` and verify with `file`/`readelf` and SHA-256.
6. First board test should be **without sound** and without commercial game ROMs/images.
7. Success criteria: Spectrum screen visible, USB keyboard works, emulator exits cleanly, session supervisor restores Monitor.

Possible later emulator candidates after FBZX: GNUBoy (Game Boy/GBC) and InfoNES (NES).

## New-chat starting prompt

Paste this into the new chat:

> Continue the **H3531 Home Computer** project. The sole source of truth is https://github.com/irman100/H3531 . First read `README.md` and `docs/CHAT_HANDOFF.md`. Do not use the HuDu repo. Hardware is AHB70XXT16-3531 V2.01 / HiSilicon Hi3531, Linux 3.0.8, ARMv7 soft-float, fixed 1280x720 ARGB1555 framebuffer. Never use `saveenv` and do not flash experimental images to SPI. Boot Kit 0.7 with full UART terminal/autoboot is physically proven and finished. The immediate task is to continue `feature/fbzx-h3531-realwork`: inspect failed Actions run `34484813958`, fix FBZX 3.1.0 cross-build, retry as many times as needed, and deliver a verified `fbzx.APP` for physical testing. Do not assume SDL is the problem: the H3531 SDL build step already passes. Preserve the exec-based exclusive app lifecycle.

## Priority after FBZX

After FBZX works on hardware:

1. rebuild the latest 0.6.1 RAMDisk so it actually contains the Pong background-restoration fix from `8090bf3...`;
2. physically verify corrected F9 Paddle and F10 Pong;
3. freeze that as a strong known-good checkpoint;
4. then consider GNUBoy / NES, audio, SATA SSD rootfs, and further vendor-module minimization.
