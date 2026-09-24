# Stayplaytion Firmware Architecture Contract

**Stage:** 6.8 prototype  
**Safety baseline:** Stage6.7.3 commit `62e3725f8f3c7f8745b9e5be71fd65ef13428df2`

## Goal

Turn the current Hi3531 appliance into a computer with a permanent firmware layer and a replaceable operating-system disk.

The firmware is BIOS/UEFI-like from the user's point of view, but the implementation is intentionally simpler and safer for Hi3531:

```
Hi3531 BootROM
  -> existing U-Boot
  -> permanent Stayplaytion firmware kernel + tiny initramfs
  -> hardware POST / rescue / boot-device discovery
  -> external Stayplaytion OS root filesystem
  -> Desktop / Stayplaytion
```

The existing U-Boot and the first flash boot area are not replaced during Stage6.8 development.

## Non-negotiable safety rules

1. No `saveenv` during prototype stages.
2. No SPI erase/write commands.
3. No replacement of the existing U-Boot.
4. F2 rescue remains available.
5. `MONITOR.ORIGINAL.APP` remains intact until native HDMI/HIFB initialization is proven.
6. Stage6.7.3 exclusive framebuffer ownership is preserved: firmware POST closes `/dev/fb0` before compatibility Monitor pre-init.
7. Every new boot mechanism is proven from removable media before any internal-flash migration.

## Firmware responsibilities

The permanent firmware layer owns:

- early boot policy;
- hardware POST;
- HDMI/HIFB initialization;
- keyboard and mouse discovery;
- USB/storage discovery;
- boot-device selection;
- recovery entry;
- validation of a compatible Stayplaytion OS;
- mounting and handing off to the selected OS;
- a minimal firmware log under `/run` or RAM.

The firmware must not own desktop applications, user files, emulator content, office software, or normal OS packages.

## Operating-system responsibilities

The replaceable OS disk owns:

- root filesystem;
- X/LXDE or future desktop shell;
- Stayplaytion frontend;
- applications and emulators;
- networking userland;
- persistent settings and user data.

## External OS disk contract

A compatible root partition contains at minimum:

```
/etc/stayplaytion-os-release
/sbin/init
/bin
/lib
/usr
/var
```

Prototype marker example:

```
STAYPLAYTION_OS=1
NAME=Stayplaytion OS
VERSION=0.1
ARCH=armel
SOC=hi3531-v100
```

During Stage6.8.0 the firmware only probes for this marker and reports it. It does not switch root yet.

Later firmware stages will prefer a filesystem label such as `STAYOS`, but the marker file remains the compatibility contract.

## Planned disk layout

Recommended removable SSD/USB layout:

```
Partition 1  FAT16/FAT32   STAYBOOT   optional recovery/update files
Partition 2  Linux fs     STAYOS     operating-system root filesystem
Partition 3  Linux fs     STAYDATA   optional persistent user data
```

The final firmware must still be able to boot a one-partition development image.

## Boot states

```
POWER ON
  -> Firmware POST
  -> F2 rescue window
  -> video/input/storage init
  -> scan boot devices
      -> valid Stayplaytion OS -> mount -> handoff
      -> no valid OS -> recovery screen / bundled fallback
```

## Stage plan

### Stage6.8.0 — Firmware POST Prototype

- real CPU/RAM/video/input POST;
- preserve proven Stage6.7.3 video handoff;
- read-only scan for an external Stayplaytion OS;
- continue into bundled Desktop;
- no root switch;
- no internal flash writes.

### Stage6.8.1 — External Rootfs Handoff Prototype

- build a tiny firmware initramfs;
- mount a prepared external root filesystem;
- transfer `/proc`, `/sys`, `/dev`;
- hand off to external `/sbin/init`;
- fall back to recovery on failure.

### Stage6.8.2 — Native Video Pre-init

- identify the minimum HDMI/HIFB operations currently performed by Monitor;
- replace normal-boot Monitor dependency with a target-compatible native initializer;
- keep Monitor only for F2 rescue.

### Stage6.8.3 — Firmware Media Image

- produce a reproducible firmware image and OS-disk image;
- verify cold boot, missing-disk recovery, keyboard selection and power-loss safety.

### Internal-flash migration

Only after all removable-media tests pass:

- inventory exact SPI partitions and recovery route;
- back up flash;
- verify external programmer/recovery path;
- migrate only the minimal proven firmware payload;
- preserve existing bootloader and rescue path.

## Stage6.8.0 success criteria

A cold boot must:

1. show `STAYPLAYTION FIRMWARE`;
2. display real RAM and framebuffer mode;
3. report keyboard/mouse/storage status;
4. keep F2 rescue functional;
5. perform exclusive compatibility video pre-init;
6. reach the existing Desktop;
7. log external OS probe results to `/var/h3531-firmware.log`;
8. perform zero persistent flash writes.
