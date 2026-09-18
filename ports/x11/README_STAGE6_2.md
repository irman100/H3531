# H3531 Stage6.2 — usable Matchbox desktop

Stage6.1 hardware proof established the foundation:

- Xfbdev on /dev/fb0
- KDrive evdev mouse and keyboard
- XKB
- visible movable cursor
- Matchbox Window Manager
- real managed window and close button
- fontconfig + DejaVu
- Wheezy Pango BasicScriptEngineFc without runtime errors

Stage6.2 stops doing cursor/window demos and adds useful shell functionality.

## First usable desktop

The first Stage6.2 image contains:

- Matchbox Window Manager
- Matchbox Desktop application launcher
- Matchbox Panel at the top
- application menu
- clock
- Terminal launcher
- xterm as the first interactive application

Matchbox Desktop reads standard .desktop entries, so later applications can be
added without changing the window-manager architecture. ZX Spectrum, file
management and H3531-specific tools can therefore become normal launchable
applications in subsequent iterations.

## Safety

USB/RAM only. No saveenv. No SPI writes.
Resident MONITOR.APP must be STOPped before testing and CONTinued afterwards.
