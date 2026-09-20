# H3531 Midori browser add-on

This workflow builds an additive Midori browser overlay for the H3531 USB
runtime. It deliberately targets Debian Wheezy armel so the browser and
WebKitGTK stack remain compatible with the board's old Linux 3.0.8 / EABI5
environment.

The package does not replace Xfbdev, Openbox, LXPanel, PCManFM, networking, or
the desktop launcher. It only adds Midori, its WebKitGTK runtime dependencies,
resources, certificates, and an LXDE menu entry.

Primary UART test:

    DISPLAY=127.0.0.1:0 /mnt/usb/H3531/APPS/x11-debian/bin/midori http://example.com

HTTPS test:

    DISPLAY=127.0.0.1:0 /mnt/usb/H3531/APPS/x11-debian/bin/midori https://example.com

No saveenv. No SPI writes.
