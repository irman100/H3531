#!/bin/sh
# H3531 Stage6.1G - Xfbdev + Matchbox + fontconfig + Pango module proof
# Safe USB test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
MATCHBOX="$BASE/bin/matchbox-window-manager"
XMESSAGE="$BASE/bin/xmessage"
FCMATCH="$BASE/bin/fc-match"
XSETROOT="$BASE/bin/xsetroot"
FONTDIR="$BASE/share/fonts/truetype/dejavu"
PANGOVERFILE="$BASE/etc/pango/module-version"
PANGOMODULES="$BASE/etc/pango/pango.modules"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_X11_SECONDS:-60}"
XLOG=/var/h3531-stage6-xfbdev.log
MLOG=/var/h3531-stage6-matchbox.log
CLOG=/var/h3531-stage6-xmessage.log
FLOG=/var/h3531-fontconfig-match.log
RLOG=/var/h3531-xsetroot.log
FCONF=/var/h3531-fonts.conf
PANGORC=/var/h3531-pangorc

echo "H3531 Stage6.1G Xfbdev + Matchbox + Pango proof"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$MATCHBOX" "$XMESSAGE" "$FCMATCH" "$XSETROOT"; do
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
[ -f "$PANGOVERFILE" ] || { echo "ERROR: Pango module-version missing"; exit 18; }
[ -f "$PANGOMODULES" ] || { echo "ERROR: Pango module registry missing"; exit 19; }

PANGOVER="$(cat "$PANGOVERFILE")"
PANGODIR="$BASE/lib/pango/$PANGOVER/modules"
PANGOBASIC="$PANGODIR/pango-basic-fc.so"
[ -f "$PANGOBASIC" ] || { echo "ERROR: Pango basic FC module missing"; exit 22; }

mkdir -p /var/h3531-x11 /var/lib/xkb /var/share /var/h3531-fontconfig-cache 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || \
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 14
}

# Matchbox DATADIR is relocated from /usr/share to writable /var/share.
rm -f /var/share/themes /var/share/matchbox 2>/dev/null
ln -s "$BASE/share/themes" /var/share/themes
ln -s "$BASE/share/matchbox" /var/share/matchbox

# Self-contained fontconfig on USB.
cat >"$FCONF" <<EOF
<?xml version="1.0"?>
<fontconfig>
  <dir>$FONTDIR</dir>
  <cachedir>/var/h3531-fontconfig-cache</cachedir>
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

# Self-contained Pango module registry on USB. Wheezy Pango loads its
# BasicScriptEngineFc module through ModuleFiles / ModulesPath.
cat >"$PANGORC" <<EOF
[Pango]
ModuleFiles=$PANGOMODULES
ModulesPath=$PANGODIR
EOF

export DISPLAY=127.0.0.1:0
export HOME=/var/h3531-x11
export LC_ALL=C
export FONTCONFIG_FILE="$FCONF"
export FONTCONFIG_PATH=/var
export PANGO_RC_FILE="$PANGORC"

"$LOADER" --library-path "$LIBPATH" "$FCMATCH" "Sans:bold" >"$FLOG" 2>&1 || {
    echo "ERROR: fontconfig cannot resolve Sans:bold"
    cat "$FLOG"
    exit 17
}
echo "fontconfig: $(cat "$FLOG")"
echo "pango module version: $PANGOVER"
echo "pango module registry: $PANGOMODULES"

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

# A light neutral root background makes the already-proven black X cursor
# clearly visible while we validate the WM/text path.
"$LOADER" --library-path "$LIBPATH" "$XSETROOT" \
  -display "$DISPLAY" -solid "#B0B0B0" >"$RLOG" 2>&1 || true

echo "Matchbox running pid=$MPID. Starting visible X11 test window..."
"$LOADER" --library-path "$LIBPATH" "$XMESSAGE" \
  -display "$DISPLAY" \
  -background white \
  -foreground black \
  -center \
  -buttons "Stage6.1G OK:0" \
  "H3531 Stage6.1G Matchbox + Pango hardware proof" \
  >"$CLOG" 2>&1 &
CPID=$!

echo "TEST: verify readable text, titlebar, black cursor on gray background, clicks and keyboard."
sleep "$DURATION"

kill "$CPID" 2>/dev/null
wait "$CPID" 2>/dev/null
kill "$MPID" 2>/dev/null
wait "$MPID" 2>/dev/null
kill "$XPID" 2>/dev/null
wait "$XPID" 2>/dev/null

echo "----- FONTCONFIG MATCH -----"
cat "$FLOG"
echo "----- XSETROOT LOG -----"
cat "$RLOG"
echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "----- MATCHBOX LOG -----"
cat "$MLOG"
echo "xmessage log saved at $CLOG"
echo "H3531 Stage6.1G proof finished"
