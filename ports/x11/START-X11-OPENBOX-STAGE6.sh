#!/bin/sh
# H3531 Stage6.3A - Openbox desktop with temporary HIFB ARGB1555 alpha fix
# Safe USB/RAM test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
OPENBOX="$BASE/bin/openbox"
XSETROOT="$BASE/bin/xsetroot"
FCMATCH="$BASE/bin/fc-match"
TERMINAL="$BASE/bin/h3531-openbox-terminal"
HIFBALPHA="$BASE/bin/h3531-hifb-alpha"
RCFILE="$BASE/etc/openbox/rc.xml"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_OPENBOX_SECONDS:-180}"

XLOG=/var/h3531-stage63-xfbdev.log
OLOG=/var/h3531-stage63-openbox.log
TLOG=/var/h3531-stage63-terminal.log
RLOG=/var/h3531-stage63-xsetroot.log
FLOG=/var/h3531-stage63-fontconfig.log
ALOG=/var/h3531-stage63-hifb-alpha.log
FCONF=/var/h3531-fonts.conf
PANGORC=/var/h3531-pangorc
ALPHASAVE=/var/h3531-hifb-alpha.saved

FONTDIR="$BASE/share/fonts/truetype/dejavu"
PANGOVERFILE="$BASE/etc/pango/module-version"
PANGOMODULES="$BASE/etc/pango/pango.modules"

echo "H3531 Stage6.3A Openbox + HIFB opaque ARGB1555"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$OPENBOX" "$XSETROOT" "$FCMATCH" "$TERMINAL" "$HIFBALPHA"; do
    [ -x "$f" ] || { echo "ERROR: missing executable $f"; exit 10; }
done
[ -c /dev/fb0 ] || { echo "ERROR: /dev/fb0 missing"; exit 11; }
[ -c "$KEYBD" ] || { echo "ERROR: $KEYBD missing"; exit 12; }
[ -c "$MOUSE" ] || { echo "ERROR: $MOUSE missing"; exit 13; }
[ -f "$RCFILE" ] || { echo "ERROR: Openbox rc.xml missing"; exit 14; }
[ -f "$PANGOVERFILE" ] || { echo "ERROR: Pango module-version missing"; exit 18; }
[ -f "$PANGOMODULES" ] || { echo "ERROR: Pango module registry missing"; exit 19; }

PANGOVER="$(cat "$PANGOVERFILE")"
PANGODIR="$BASE/lib/pango/$PANGOVER/modules"
[ -f "$PANGODIR/pango-basic-fc.so" ] || { echo "ERROR: Pango basic FC module missing"; exit 22; }

mkdir -p /var/h3531-x11 /var/h3531-x11/.cache /var/h3531-x11/.config          /var/lib/xkb /var/h3531-fontconfig-cache 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || /sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 15
}

cat >"$FCONF" <<EOF
<?xml version="1.0"?>
<fontconfig>
  <dir>$FONTDIR</dir>
  <cachedir>/var/h3531-fontconfig-cache</cachedir>
  <alias><family>Sans</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>Monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
</fontconfig>
EOF

cat >"$PANGORC" <<EOF
[Pango]
ModuleFiles=$PANGOMODULES
ModulesPath=$PANGODIR
EOF

export DISPLAY=127.0.0.1:0
export HOME=/var/h3531-x11
export XDG_CACHE_HOME=/var/h3531-x11/.cache
export XDG_CONFIG_HOME=/var/h3531-x11/.config
export SHELL=/bin/sh
export LC_ALL=C
export LANG=C
export LANGUAGE=C
export FONTCONFIG_FILE="$FCONF"
export FONTCONFIG_PATH=/var
export PANGO_RC_FILE="$PANGORC"
export XDG_DATA_DIRS="$BASE/share"
export PATH="$BASE/bin:/bin:/sbin:/usr/bin:/usr/sbin"

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

XPID=
OPID=
TPID=
ALPHA_ACTIVE=0

restore_alpha()
{
    if [ "$ALPHA_ACTIVE" = "1" ]; then
        echo "----- HIFB ALPHA RESTORE -----" >>"$ALOG"
        "$HIFBALPHA" restore /dev/fb0 "$ALPHASAVE" >>"$ALOG" 2>&1 || true
        ALPHA_ACTIVE=0
    fi
}

cleanup()
{
    [ -n "$TPID" ] && kill "$TPID" 2>/dev/null
    [ -n "$OPID" ] && kill "$OPID" 2>/dev/null
    [ -n "$XPID" ] && kill "$XPID" 2>/dev/null
    [ -n "$TPID" ] && wait "$TPID" 2>/dev/null
    [ -n "$OPID" ] && wait "$OPID" 2>/dev/null
    [ -n "$XPID" ] && wait "$XPID" 2>/dev/null
    restore_alpha
}
trap cleanup 1 2 15

: >"$ALOG"
echo "----- /proc/graphics/hifb0 BEFORE -----" >>"$ALOG"
cat /proc/graphics/hifb0 >>"$ALOG" 2>&1 || true

"$HIFBALPHA" restore-if-saved /dev/fb0 "$ALPHASAVE" >>"$ALOG" 2>&1 || {
    echo "ERROR: cannot recover previous HIFB alpha state"
    cat "$ALOG"
    exit 26
}

"$HIFBALPHA" push-opaque /dev/fb0 "$ALPHASAVE" >>"$ALOG" 2>&1 || {
    echo "ERROR: HIFB alpha ioctl compatibility/opaque switch failed"
    cat "$ALOG"
    exit 27
}
ALPHA_ACTIVE=1

echo "----- /proc/graphics/hifb0 OPAQUE -----" >>"$ALOG"
cat /proc/graphics/hifb0 >>"$ALOG" 2>&1 || true

echo "HIFB alpha switched temporarily to opaque 255/255."
echo "Expected /proc: Alpha Enable ON, AlphaChannel Enable ON, Alpha0/Alpha1 255,255."

"$LOADER" --library-path "$LIBPATH" "$XFBDEV" :0   -fb /dev/fb0   -screen 1280x720x16   -keybd "evdev,,device=$KEYBD"   -mouse "evdev,,device=$MOUSE"   -fp "$BASE/share/fonts/X11/misc"   -xkbdir "$BASE/share/X11/xkb"   -softCursor   -nolisten unix   -nolock -ac -noreset   >"$XLOG" 2>&1 &
XPID=$!

sleep 3
if ! kill -0 "$XPID" 2>/dev/null; then
    echo "ERROR: Xfbdev exited"
    cat "$XLOG"
    cleanup
    exit 20
fi

"$LOADER" --library-path "$LIBPATH" "$XSETROOT"   -display "$DISPLAY" -solid "#FFFFFF" >"$RLOG" 2>&1 || {
    echo "ERROR: xsetroot failed"
    cat "$RLOG"
    cleanup
    exit 23
}

"$LOADER" --library-path "$LIBPATH" "$OPENBOX"   --sm-disable --config-file "$RCFILE" >"$OLOG" 2>&1 &
OPID=$!

sleep 4
if ! kill -0 "$OPID" 2>/dev/null; then
    echo "ERROR: Openbox exited"
    cat "$OLOG"
    cleanup
    exit 24
fi

"$LOADER" --library-path "$LIBPATH" "$XSETROOT"   -display "$DISPLAY" -solid "#FFFFFF" >>"$RLOG" 2>&1 || true

"$TERMINAL" >"$TLOG" 2>&1 &
TPID=$!

sleep 4
if ! kill -0 "$TPID" 2>/dev/null; then
    echo "ERROR: Terminal exited"
    cat "$TLOG"
    cleanup
    exit 25
fi

echo "Stage6.3A Openbox desktop is running."
echo "Expected NOW: truly WHITE desktop + light movable/resizable Terminal."
echo "HIFB alpha will be restored automatically on exit."
echo "DURATION=0 keeps the session running until interrupted."

if [ "$DURATION" = "0" ]; then
    while kill -0 "$XPID" 2>/dev/null && kill -0 "$OPID" 2>/dev/null; do
        sleep 5
    done
else
    sleep "$DURATION"
fi

cleanup
trap - 1 2 15

echo "----- HIFB ALPHA LOG -----"
cat "$ALOG"
echo "----- OPENBOX LOG -----"
cat "$OLOG"
echo "----- TERMINAL LOG -----"
cat "$TLOG"
echo "----- XSETROOT LOG -----"
cat "$RLOG"
echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "H3531 Stage6.3A proof finished"
