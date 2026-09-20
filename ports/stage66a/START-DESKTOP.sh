#!/bin/sh
# H3531 Stage6.6A canonical persistent desktop entry point.
BASE=/mnt/usb/H3531/APPS/x11-debian
export H3531_X11_FULLFRAME_FB=1
exec "$BASE/libexec/h3531-lxde-core" "$@"
