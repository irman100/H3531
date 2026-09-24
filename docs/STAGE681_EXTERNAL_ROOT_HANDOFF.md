# Stage6.8.1 — External Root Handoff

This stage proves the firmware/OS separation without writing internal SPI flash.

## Architecture proven by this stage

```
U-Boot
  -> zImage.img
  -> STAYFW.IMG (classic cramfs RAMDisk)
      -> /stayfw-init is PID 1
      -> mounts existing internal Hi3531 /usr firmware
      -> loads vendor modules
      -> input/video init
      -> 3s compatibility Monitor pre-init
      -> F2 rescue window
      -> scans USB/SSD for compatible Stayplaytion OS
      -> h3531-root-handoff (still PID 1)
      -> pivot_root
      -> external /sbin/init (PID 1)
```

The old firmware root remains read-only at `/oldroot` after handoff. This is deliberate for Stage6.8.1: it provides a recovery environment and avoids trying to unmount the executable backing the PID1 transition during the smoke test.

## Why pivot_root instead of switch_root

The current Hi3531 boot path uses a classic block RAM disk:

```
root=/dev/ram0 rootfstype=cramfs
```

This is not an initramfs rootfs. Stage6.8.1 therefore uses `pivot_root`, which matches the actual classic-initrd topology.

## Test OS format

The CI produces `STAYOS-TEST.img`, a read-only squashfs root image. Squashfs is selected because the proven vendor boot already mounts internal squashfs partitions, so filesystem support is known to exist in the current kernel.

The test image contains:

- `/etc/stayplaytion-os-release`
- a static BusyBox
- `/sbin/init`
- the framebuffer handoff status tool

The test external init displays:

```
STAYPLAYTION OS
EXTERNAL ROOT ACTIVE
FIRMWARE HANDOFF [OK]
```

## Safety

This stage does not:

- call `saveenv`;
- erase/write SPI;
- replace U-Boot;
- modify the internal partition table;
- write the external OS device from the target.

Writing `STAYOS-TEST.img` to a spare USB/SSD is performed on a development PC and destroys the previous contents of that selected removable device.

## Next step after hardware success

Stage6.8.2 will replace the temporary compatibility Monitor pre-init with a native minimal HDMI/HIFB initializer and then move toward a full external Desktop rootfs.
