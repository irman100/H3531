#!/bin/sh
BASE=/mnt/usb/H3531/APPS/x11-debian
DIR=/mnt/usb/H3531/APPS/fbzx
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"
export DISPLAY=${DISPLAY:-127.0.0.1:0}
export HOME=/var/h3531-lxde
export H3531_AUDIO=1
export SDL_VIDEODRIVER=x11
export LD_LIBRARY_PATH="$LIBPATH"
unset SDL_FBDEV
unset H3531_FBZX_FAST16
cd "$DIR" || exit 126
# -ds disables FBZX simulated CRT black scanlines.
exec "$LOADER" --library-path "$LIBPATH" "$DIR/FBZX-X11.BIN" -ds "$@"
