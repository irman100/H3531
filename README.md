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


## Stage 6.6A installation model

The ReadyToCopy package is deliberately non-destructive: copying the `H3531`
directory to an existing USB drive does **not** overwrite the active vendor
`SYSTEM/MONITOR.APP`.

After copying the package, activate Stage6.6A once from the target shell:

```sh
/mnt/usb/H3531/SYSTEM/INSTALL-STAGE66A.sh
```

The installer first preserves the current executable Monitor as
`MONITOR.ORIGINAL.APP`, then installs the Stage6.6A persistent-desktop
supervisor as the active `MONITOR.APP`.

Rollback:

```sh
/mnt/usb/H3531/SYSTEM/UNINSTALL-STAGE66A.sh
```

The rollback restores `MONITOR.ORIGINAL.APP`. Neither script writes U-Boot
environment, SPI NOR, kernel, or root filesystem.

## Branch cleanup

Stage6.6A contains an ancestry-safe PowerShell cleanup tool:

```powershell
.\tools\cleanup-obsolete-branches-stage66a.ps1
```

The default mode is dry-run. It lists remote branches that are already
ancestors of Stage6.6A and preserves divergent branches with unique commits.

After Stage6.6A has passed hardware validation:

```powershell
.\tools\cleanup-obsolete-branches-stage66a.ps1 -Apply
```

Keep `feature/h3531-stage64-lxde` until Stage6.6A cold-boot/desktop/FBZX
acceptance is complete. It can be included in the later cleanup with
`-DropStage64`.


### Clean USB deployment

For Stage6.6A, do **not** merge the new desktop runtime into an old
`APPS/x11-debian` directory. Old browser/stage overlays can leave extra ELF
libraries and launchers behind even when the new files overwrite correctly.

With the board powered off and the USB storage connected to the PC:

1. Preserve the rest of `H3531/APPS` (FBZX, Nofrendo, RetroArch, GameFront,
   games and user ROM directories).
2. Delete only the existing `H3531/APPS/x11-debian` directory.
3. Copy the Stage6.6A `H3531` tree to the USB root.
4. Boot the board and run `/mnt/usb/H3531/SYSTEM/INSTALL-STAGE66A.sh` once.

This makes the desktop runtime deterministic while leaving user ROMs and the
native emulator directories untouched.
