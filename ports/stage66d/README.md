# H3531 Stage6.6D Live Native Lease

Stage6.6C proved persistent LXDE survives native framebuffer applications,
but SIGSTOP freezes X input/hotplug ownership and does not guarantee a full
physical framebuffer repaint.

Stage6.6D keeps X/LXDE alive. XInput disables Evdev keyboard/mouse so KDrive
releases EVIOCGRAB. Xfbdev-shadow-live suppresses only shadow->fb copies while
the native app owns /dev/fb0. On exit the bridge restores fb mode/alpha/pixels,
re-enables X input, removes the lease marker, and xrefresh triggers FullFrame.
