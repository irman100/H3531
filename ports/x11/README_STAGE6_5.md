# H3531 Stage6.5 — Desktop applications and network

Stage6.5 starts from the hardware-proven Stage6.4J LXDE Core instead of
rebuilding the desktop foundation.

## Stage6.5A scope

Desktop applications are added as on-demand programs only; nothing is
autostarted beyond the Stage6.4 desktop session:

- Leafpad
- Galculator
- GPicView
- LXTask
- LXAppearance
- LXInput
- ObConf
- Xarchiver
- Links2 in graphical X mode
- ePDFView
- mtPaint
- scrot

Network tooling is deliberately non-persistent:

- ifconfig, route, ip, ping
- wget and curl with packaged CA certificates
- h3531-net-info
- h3531-net-dhcp
- LXPanel netstatus monitor for eth0

The vendor system remains authoritative for Ethernet drivers and board-level
network initialization. Stage6.5A does not write bootloader environment,
firmware, SPI flash, or persistent vendor network configuration.

The interface `lo` is the loopback interface (localhost), not the physical
Ethernet connection. The physical interface is expected to be discovered on
hardware before any automatic network setup is considered.

Panel icon handling is kept separate from PCManFM icon-theme handling:
fixed panel launchers use explicit packaged image paths while PCManFM retains
its private GTK icon-theme override.
