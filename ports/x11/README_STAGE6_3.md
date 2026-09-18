# H3531 Stage6.3 — Openbox light desktop

Stage6.0E/6.1 proved the X11 foundation on the real Hi3531.

Stage6.2 then exposed a product-level mismatch: Matchbox Window Manager is
optimized for handheld/kiosk/set-top-box behavior rather than a conventional
desktop. Its managed windows are intentionally constrained and it is therefore
a poor fit for the desired mouse-driven PC experience with freely movable and
resizable windows.

Stage6.3 keeps the proven Xfbdev/KDrive/XKB/font stack and replaces only the
window manager with Debian Wheezy Openbox.

First hardware goals:

- pure white root desktop
- light Openbox theme
- conventional decorated xterm window
- drag window by titlebar
- resize from edges/corners
- close/minimize/maximize buttons
- right-click root menu
- keyboard focus/input in Terminal

No Matchbox Desktop, Matchbox Panel, GTK desktop, PNG or GdkPixbuf are required
for this first Openbox proof.

Safety remains unchanged: USB/RAM only, no saveenv, no SPI writes.
