#!/bin/sh
BASE=/mnt/usb/H3531/APPS/x11-debian
DIR=/mnt/usb/H3531/APPS/fbzx
PROFILE=/var/h3531-fbzx-128
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"

mkdir -p "$PROFILE" 2>/dev/null || exit 125

if [ ! -f "$PROFILE/.fbzx" ]; then
    cat >"$PROFILE/.fbzx" <<EOF
mode=1
issue=3
joystick=0
ay_sound=1
interface1=0
doublescan=1
volume=Q
bw=0
fast=0
turboload=1
mouse=0
EOF
fi

export DISPLAY=${DISPLAY:-127.0.0.1:0}
export HOME="$PROFILE"
export H3531_AUDIO=1
export SDL_VIDEODRIVER=x11
export LD_LIBRARY_PATH="$LIBPATH"
unset SDL_FBDEV
unset H3531_FBZX_FAST16

cd "$DIR" || exit 126
exec "$LOADER" --library-path "$LIBPATH" "$DIR/FBZX-X11.BIN" -ds -fs "$@"
