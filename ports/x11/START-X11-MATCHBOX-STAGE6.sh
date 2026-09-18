#!/bin/sh
# H3531 Stage6.1 - Debian Wheezy Xfbdev + Matchbox WM hardware proof
# Safe USB test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
MATCHBOX="$BASE/bin/matchbox-window-manager"
XMESSAGE="$BASE/bin/xmessage"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_X11_SECONDS:-60}"
XLOG=/var/h3531-stage6-xfbdev.log
MLOG=/var/h3531-stage6-matchbox.log
CLOG=/var/h3531-stage6-xmessage.log

echo "H3531 Stage6.1 Xfbdev + Matchbox hardware proof"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$MATCHBOX" "$XMESSAGE"; do
    if [ ! -x "$f" ]; then
        echo "ERROR: missing executable $f"
        exit 10
    fi
done
[ -c /dev/fb0 ] || { echo "ERROR: /dev/fb0 missing"; exit 11; }
[ -c "$KEYBD" ] || { echo "ERROR: $KEYBD missing"; exit 12; }
[ -c "$MOUSE" ] || { echo "ERROR: $MOUSE missing"; exit 13; }

mkdir -p /var/h3531-x11 /var/lib/xkb /var/share /var/matchbox 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || \
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 14
}

# Matchbox package paths are relocated at packaging time from /usr/share and
# /etc/matchbox to writable /var paths. Point those paths back to USB data.
ln -sf "$BASE/share/themes" /var/share/themes 2>/dev/null
ln -sf "$BASE/share/matchbox" /var/share/matchbox 2>/dev/null
if [ -f "$BASE/etc/matchbox/kbdconfig" ]; then
    cp "$BASE/etc/matchbox/kbdconfig" /var/matchbox/kbdconfig 2>/dev/null
fi

export DISPLAY=127.0.0.1:0
export HOME=/var/h3531-x11
export LC_ALL=C

cat >/var/xkbcomp <<EOF
#!/bin/sh
echo "\$@" >/var/h3531-xkbcomp.args
exec "$LOADER" --library-path "$LIBPATH" "$XKBCOMP" -I"$BASE/share/X11/xkb" "\$@"
EOF
chmod 755 /var/xkbcomp

"$LOADER" --library-path "$LIBPATH" "$XFBDEV" :0 \
  -fb /dev/fb0 \
  -screen 1280x720x16 \
  -keybd "evdev,,device=$KEYBD" \
  -mouse "evdev,,device=$MOUSE" \
  -fp "$BASE/share/fonts/X11/misc" \
  -xkbdir "$BASE/share/X11/xkb" \
  -softCursor \
  -nolisten unix \
  -nolock -ac -noreset \
  >"$XLOG" 2>&1 &
XPID=$!

sleep 3
if ! kill -0 "$XPID" 2>/dev/null; then
    echo "ERROR: Xfbdev exited"
    cat "$XLOG"
    exit 20
fi

"$LOADER" --library-path "$LIBPATH" "$MATCHBOX" \
  -display "$DISPLAY" \
  -theme Default \
  -use_titlebar yes \
  -use_cursor yes \
  -use_desktop_mode decorated \
  >"$MLOG" 2>&1 &
MPID=$!

sleep 3
if ! kill -0 "$MPID" 2>/dev/null; then
    echo "ERROR: Matchbox exited"
    cat "$MLOG"
    kill "$XPID" 2>/dev/null
    wait "$XPID" 2>/dev/null
    exit 21
fi

echo "Matchbox running pid=$MPID. Starting visible X11 test window..."
"$LOADER" --library-path "$LIBPATH" "$XMESSAGE" \
  -display "$DISPLAY" \
  -center \
  -buttons "Stage6.1 OK:0" \
  "H3531 Stage6.1 Matchbox hardware proof" \
  >"$CLOG" 2>&1 &
CPID=$!

echo "TEST: move mouse, verify cursor, click the window/button, use keyboard."
sleep "$DURATION"

kill "$CPID" 2>/dev/null
wait "$CPID" 2>/dev/null
kill "$MPID" 2>/dev/null
wait "$MPID" 2>/dev/null
kill "$XPID" 2>/dev/null
wait "$XPID" 2>/dev/null

echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "----- MATCHBOX LOG -----"
cat "$MLOG"
echo "xmessage log saved at $CLOG"
echo "H3531 Stage6.1 proof finished"
