#!/bin/sh
# H3531 Stage6.0D - Debian Wheezy armel Xfbdev hardware proof
# Safe USB test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XEV="$BASE/bin/xev"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_X11_SECONDS:-30}"
XLOG=/var/h3531-stage6-debian-xfbdev.log
ELOG=/var/h3531-stage6-debian-xev.log

echo "H3531 Stage6.0D Debian Wheezy Xfbdev proof"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XEV"; do
    if [ ! -x "$f" ]; then
        echo "ERROR: missing executable $f"
        exit 10
    fi
done
[ -c /dev/fb0 ] || { echo "ERROR: /dev/fb0 missing"; exit 11; }
[ -c "$KEYBD" ] || { echo "ERROR: $KEYBD missing"; exit 12; }
[ -c "$MOUSE" ] || { echo "ERROR: $MOUSE missing"; exit 13; }

mkdir -p /tmp/.X11-unix /var/h3531-x11 2>/dev/null
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0 2>/dev/null
export DISPLAY=:0
export HOME=/var/h3531-x11
export XAUTHORITY=/var/h3531-x11/.Xauthority

"$LOADER" --library-path "$LIBPATH" "$XFBDEV" :0 \
  -screen 1280x720x16 \
  -keybd "evdev,,device=$KEYBD" \
  -mouse "evdev,,device=$MOUSE" \
  -fp "$BASE/share/fonts/X11/misc" \
  -kb -ac -nolisten tcp -noreset \
  >"$XLOG" 2>&1 &
XPID=$!

for _wait in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -S /tmp/.X11-unix/X0 ] && break
    if ! kill -0 "$XPID" 2>/dev/null; then
        echo "ERROR: Xfbdev exited"
        cat "$XLOG"
        exit 20
    fi
    sleep 1
done
if [ ! -S /tmp/.X11-unix/X0 ]; then
    echo "ERROR: X socket did not appear"
    kill "$XPID" 2>/dev/null
    cat "$XLOG"
    exit 21
fi

echo "Xfbdev running pid=$XPID. Starting xev for ${DURATION}s..."
"$LOADER" --library-path "$LIBPATH" "$XEV" -display :0 \
  -geometry 760x460+240+120 >"$ELOG" 2>&1 &
EPID=$!

sleep "$DURATION"
kill "$EPID" 2>/dev/null
wait "$EPID" 2>/dev/null
kill "$XPID" 2>/dev/null
wait "$XPID" 2>/dev/null

echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "----- LAST XEV EVENTS -----"
tail -60 "$ELOG" 2>/dev/null || cat "$ELOG"
echo "H3531 Stage6.0D proof finished"
