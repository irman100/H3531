# Stayplaytion balanced memory profile — 256 MiB Linux / 256 MiB MMZ

## Goal

The AHB70XXT16-3531 board has 512 MiB physical DDR implemented as two
256-MiB banks in the vendor memory layout. The historical DVR configuration
gives Linux only 130 MiB from bank0 and gives MMZ the remaining 126 MiB of
bank0 plus all 256 MiB of bank1.

For Stayplaytion the DVR workload is no longer required. The balanced profile
therefore gives Linux the complete contiguous first bank and preserves the
complete second bank for HIFB/TDE/HiSilicon multimedia hardware.

## Target physical map

- Linux System RAM: `0x80000000..0x8fffffff` — 256 MiB.
- MMZ `anonymous`: `0xC0000000..0xC7ffffff` — 128 MiB.
- MMZ `ddr1`: `0xC8000000..0xCfffffff` — 128 MiB.
- Total physical DDR: 512 MiB.
- No overlapping ranges.

The two MMZ names are deliberately retained. Existing HIFB/TDE and other
vendor modules were written for the legacy MMZ layout and must not be forced
to use Linux System RAM.

## Safety / rollback

This profile is a removable-media RAM boot experiment:

- no `saveenv`;
- no SPI erase/write;
- no U-Boot replacement;
- no flash partition modification;
- the existing 130-MiB boot profile remains the rollback path.

The MEM256 RAMDisk changes its own RAM-root startup path so that, only when the
kernel command line contains `mem=256M`, module loading uses the balanced MMZ
map. With any other memory value it falls back to the original vendor
`/usr/etc/loadmod` path.

Do not use a plain `mem=256M` boot argument with the old unmodified RAMDisk:
the original `load3531` calculation would produce a zero-length first MMZ
zone. Likewise, do not set `mem=512M`; the two physical banks are not exposed
as one contiguous 512-MiB Linux range by the current vendor kernel setup.

## First hardware verification

After a successful MEM256 boot, verify:

```sh
cat /proc/cmdline
cat /proc/meminfo
cat /proc/iomem
cat /proc/graphics/hifb0
cat /proc/media-mem 2>/dev/null
free
```

Expected Linux-visible memory should be close to 256 MiB minus normal kernel
reservations. Then verify the desktop, HIFB output, audio, Stayplaytion and PS1.

Only after the balanced profile is hardware-proven should it replace the
historical 130-MiB profile as the normal boot path.
