# H3531 Stage6.4 — LXDE Core Integration

Stage6.4 stops treating the desktop as a sequence of single-application proofs.

The proven hardware/runtime foundation is preserved:

- Hi3531 HIFB ARGB1555 temporary alpha fix
- Xfbdev 1.12.4, exact hardware-proven binary
- KDrive evdev mouse/keyboard
- XKB
- Openbox conventional window management
- USB armel runtime

On top of that, Stage6.4 integrates the LXDE core as one session:

- PCManFM: desktop + file manager
- LXPanel: applications menu, taskbar, tray/clock/profile plugins
- LXTerminal: terminal emulator
- standard XDG .desktop application menu
- writable HOME/XDG state in /var

This is an adapted LXDE session rather than a normal Debian boot. The vendor
rootfs is read-only and does not provide Debian's /lib loader, system D-Bus,
PAM or init stack. ELF applications are therefore wrapped through the packaged
armel loader and all mutable state is redirected to /var.

GdkPixbuf loader cache is generated on the target. Image-loader failure is
logged but is not allowed to block the whole desktop session; the goal is to
keep file/text functionality available while icon support is hardened.

Browser integration is intentionally Stage6.5 because modern TLS/rendering is
the only major desktop component that requires a separate compatibility choice.

Safety:
- USB/RAM only
- no saveenv
- no SPI writes
- HIFB alpha state restored automatically on session exit
