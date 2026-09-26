# Stayplaytion MEM256K

MEM256K keeps the hardware-proven 256 MiB Linux / 256 MiB MMZ layout from
MEM256 and fixes startup ownership.

## Startup architecture

The PC UART helper controls U-Boot only:

1. load `zImage.img`;
2. load `H3531-MEM256K.IMG`;
3. set `initrd_high=0xffffffff`;
4. set `mem=256M`;
5. `bootm`;
6. immediately return to the normal full-duplex UART terminal.

The UART helper must not wait for a Linux prompt, mount USB, or execute a
Desktop supervisor.

Inside MEM256K, the RAM-root `/etc/profile` launches a detached background
worker once per boot. It waits for `/dev/sda1`, mounts it at `/mnt/usb`,
and execs `H3531/SYSTEM/STAGE66A-DESKTOP.APP`.

This preserves two independent processes:

- serial root shell: interactive and unaffected by Desktop Ctrl+C;
- Desktop supervisor: Monitor pre-init -> STOP Monitor -> LXDE.

The detached Desktop chain ignores SIGINT so UART Ctrl+C cannot terminate it.\nThe per-boot marker is `/var/sp`, and the autostart log is `/var/sp.log`.

## Memory invariants

The existing MEM256 loader is retained:

- Linux: first 256 MiB bank;
- MMZ anonymous: 0xC0000000, 128 MiB;
- MMZ ddr1: 0xC8000000, 128 MiB.

No saveenv, SPI writes, U-Boot replacement, or flash partition changes.
