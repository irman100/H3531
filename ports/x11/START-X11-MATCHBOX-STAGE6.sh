#!/bin/sh
# H3531 Stage6.1F - Debian Wheezy Xfbdev + Matchbox WM + USB fontconfig proof
# Safe USB test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
MATCHBOX="$BASE/bin/matchbox-window-manager"
XMESSAGE="$BASE/bin/xmessage"
FCMATCH="$BASE/bin/fc-match"
FONTDIR="$BASE/share/fonts/truetype/dejavu"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_X11_SECONDS:-60}"
XLOG=/var/h3531-stage6-xfbdev.log
MLOG=/var/h3531-stage6-matchbox.log
CLOG=/var/h3531-stage6-xmessage.log
FLOG=/var/h3531-fontconfig-match.log
FCONF=/var/h3531-fonts.conf

echo "H3531 Stage6.1F Xfbdev + Matchbox + fontconfig proof"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$MATCHBOX" "$XMESSAGE" "$FCMATCH"; do
    if [ ! -x "$f" ]; then
        echo "ERROR: missing executable $f"
        exit 10
    fi
done
[ -c /dev/fb0 ] || { echo "ERROR: /dev/fb0 missing"; exit 11; }
[ -c "$KEYBD" ] || { echo "ERROR: $KEYBD missing"; exit 12; }
[ -c "$MOUSE" ] || { echo "ERROR: $MOUSE missing"; exit 13; }
[ -f "$BASE/etc/matchbox/kbdconfig" ] || { echo "ERROR: Matchbox kbdconfig missing"; exit 15; }
[ -d "$FONTDIR" ] || { echo "ERROR: DejaVu font directory missing"; exit 16; }

mkdir -p /var/h3531-x11 /var/lib/xkb /var/share 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || \
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 14
}

# Matchbox DATADIR is relocated at packaging time from /usr/share to /var/share.
# /var is writable on the vendor rootfs, so expose USB theme/data there.
rm -f /var/share/themes /var/share/matchbox 2>/dev/null
ln -s "$BASE/share/themes" /var/share/themes
ln -s "$BASE/share/matchbox" /var/share/matchbox

# The vendor rootfs has no usable /etc/fonts configuration. Build a tiny,
# self-contained fontconfig file in writable /var and point it at USB DejaVu.
cat >"$FCONF" <<EOF
<?xml version="1.0"?>
<fontconfig>
  <dir>$FONTDIR</dir>
  <cache>/var/h3531-fonts.cache-2</cache>
  <alias>
    <family>Sans</family>
    <prefer><family>DejaVu Sans</family></prefer>
  </alias>
  <alias>
    <family>sans-serif</family>
    <prefer><family>DejaVu Sans</family></prefer>
  </alias>
</fontconfig>
EOF

export DISPLAY=127.0.0.1:0
export HOME=/var/h3531-x11
export LC_ALL=C
export FONTCONFIG_FILE="$FCONF"
export FONTCONFIG_PATH=/var

# Fail early with a useful log if the Pango/fontconfig path is not usable.
"$LOADER" --library-path "$LIBPATH" "$FCMATCH" "Sans:bold" >"$FLOG" 2>&1 || {
    echo "ERROR: fontconfig cannot resolve Sans:bold"
    cat "$FLOG"
    exit 17
}
echo "fontconfig: $(cat "$FLOG")"

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
  -kbdconfig "$BASE/etc/matchbox/kbdconfig" \
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
  -buttons "Stage6.1F OK:0" \
  "H3531 Stage6.1F Matchbox hardware proof" \
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

echo "----- FONTCONFIG MATCH -----"
cat "$FLOG"
echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "----- MATCHBOX LOG -----"
cat "$MLOG"
echo "xmessage log saved at $CLOG"
echo "H3531 Stage6.1F proof finished"
