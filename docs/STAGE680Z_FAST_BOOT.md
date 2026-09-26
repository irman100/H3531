# Stayplaytion Stage6.8.0Z Fast Boot

Stage6.8.0Z is a removable-media/RAM-only fast-start test path for the proven
256 MiB Linux + 256 MiB MMZ profile.

It does not replace the known-good `STAGE68-FIRMWARE.APP`. The new entry point is:

```sh
/mnt/usb/H3531/SYSTEM/STAGE68-FASTBOOT.APP
```

Critical-path changes:

- POST rescue window: 7000 ms -> 2000 ms.
- Original Monitor compatibility pre-init remains mandatory; it may stop early
  when HIFB reports `Show state: ON`, with the historical 3 s maximum retained.
- Ready splash: 1500 ms -> 500 ms.
- Probe-only external-OS scan is skipped on the fast critical path.
- DHCP/SNTP runs in the background in fast mode; LXDE remains usable offline.
- LXDE startup guard sleeps are shortened only when `H3531_FAST_BOOT=1`.
  Existing launchers retain their historical timings.

Safety:

- no `saveenv`
- no SPI writes
- no U-Boot replacement
- old `STAGE68-FIRMWARE.APP` remains the rollback path
- Linux/MMZ mapping is unchanged
