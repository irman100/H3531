# H3531 Stage6.5B — Shadow framebuffer repaint experiment

The hardware symptom is now narrowly defined:

- the desktop is stable while idle;
- vertical trails/echo appear while dragging windows;
- the same class of corruption appears while scrolling content.

Those two operations strongly exercise X server region copying. The current
KDrive fbdev server uses the physical packed-pixel framebuffer directly in
normal orientation. Stage6.5B keeps that proven binary unchanged and adds a
second source-rebuilt Xfbdev that forces KDrive's existing software shadow
framebuffer.

This is an A/B hardware experiment, not a destructive replacement.

Direct mode remains available:

    H3531_LXDE_SECONDS=0 ./START-LXDE-STAGE6.sh

Shadow test:

    H3531_LXDE_SECONDS=0 ./START-LXDE-STAGE6-SHADOW.sh

If dragging and scrolling trails disappear in shadow mode, the fault is
localized to direct framebuffer copy/update behavior rather than PCManFM,
Openbox, LXPanel, themes, or application repaint logic.

No saveenv. No SPI writes. No kernel replacement.
