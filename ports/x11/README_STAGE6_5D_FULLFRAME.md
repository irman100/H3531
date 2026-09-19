# H3531 Stage6.5D — Full-frame ShadowFB

Stage6.5B proved that simply enabling KDrive's software shadow framebuffer is
not enough: window dragging and browser/text scrolling still leave vertical
trails. The image is restored by minimize/restore, so the logical window
contents remain correct and a complete repaint succeeds.

Stage6.5D tests the next narrower hypothesis: HiFB/Xfbdev incremental damage
copies are unreliable, while full-screen transfers are reliable.

The new Xfbdev-shadow-full preserves ShadowFB but forces every shadow redisplay
to copy the complete root pixmap to the framebuffer.

Run:

    H3531_LXDE_SECONDS=0 ./START-LXDE-STAGE6-FULLFRAME.sh

The direct and Stage6.5B shadow launchers remain available as fallbacks.

No saveenv, no SPI writes, no kernel replacement.
