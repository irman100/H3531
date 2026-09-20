# H3531 Dillo browser add-on

This package is intentionally additive. It is built against the current
Stage6.5B runtime layout and does not replace the desktop, X server, network
scripts, or launchers.

The CI workflow installs Debian Wheezy armel Dillo, computes its dynamic
library closure relative to the existing USB runtime, relocates Dillo's fixed
resource paths to USB-backed /var aliases, and produces an overlay ZIP.

Primary UART test:

    DISPLAY=127.0.0.1:0 /mnt/usb/H3531/APPS/x11-debian/bin/dillo http://example.com

No saveenv. No SPI writes.
