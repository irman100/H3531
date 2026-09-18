#!/bin/sh
# H3531 Stage6.2D - robust Matchbox panel shell + terminal, no GTK desktop
# Safe USB/RAM test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
MATCHBOX="$BASE/bin/matchbox-window-manager"
PANEL="$BASE/bin/matchbox-panel"
FCMATCH="$BASE/bin/fc-match"
XSETROOT="$BASE/bin/xsetroot"
TERMINAL="$BASE/bin/h3531-terminal"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_DESKTOP_SECONDS:-180}"
AUTOTERM="${H3531_AUTOSTART_TERMINAL:-1}"

XLOG=/var/h3531-stage62-xfbdev.log
WLOG=/var/h3531-stage62-wm.log
PLOG=/var/h3531-stage62-panel.log
TLOG=/var/h3531-stage62-terminal.log
FLOG=/var/h3531-stage62-fontconfig.log
RLOG=/var/h3531-stage62-xsetroot.log
FCONF=/var/h3531-fonts.conf
PANGORC=/var/h3531-pangorc

FONTDIR="$BASE/share/fonts/truetype/dejavu"
PANGOVERFILE="$BASE/etc/pango/module-version"
PANGOMODULES="$BASE/etc/pango/pango.modules"

echo "H3531 Stage6.2D Matchbox Panel Shell + Terminal"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s autostart-terminal=$AUTOTERM"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$MATCHBOX" "$PANEL" "$FCMATCH" "$XSETROOT" \
         "$BASE/bin/h3531-terminal-applet" "$BASE/bin/mb-applet-clock" "$TERMINAL"; do
    [ -x "$f" ] || { echo "ERROR: missing executable $f"; exit 10; }
done
[ -c /dev/fb0 ] || { echo "ERROR: /dev/fb0 missing"; exit 11; }
[ -c "$KEYBD" ] || { echo "ERROR: $KEYBD missing"; exit 12; }
[ -c "$MOUSE" ] || { echo "ERROR: $MOUSE missing"; exit 13; }
[ -f "$PANGOVERFILE" ] || { echo "ERROR: Pango module-version missing"; exit 18; }
[ -f "$PANGOMODULES" ] || { echo "ERROR: Pango module registry missing"; exit 19; }

PANGOVER="$(cat "$PANGOVERFILE")"
PANGODIR="$BASE/lib/pango/$PANGOVER/modules"
[ -f "$PANGODIR/pango-basic-fc.so" ] || { echo "ERROR: Pango basic FC module missing"; exit 22; }

mkdir -p /var/h3531-x11 /var/lib/xkb /var/share /var/h3531-fontconfig-cache 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || \
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 14
}

rm -f /var/share/themes /var/share/matchbox /var/share/pixmaps 2>/dev/null
ln -s "$BASE/share/themes" /var/share/themes
ln -s "$BASE/share/matchbox" /var/share/matchbox
ln -s "$BASE/share/pixmaps" /var/share/pixmaps

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
export SHELL=/bin/sh
export LC_ALL=C
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
WPID=
PPID_H3531=
TPID=

cleanup()
{
    [ -n "$TPID" ] && kill "$TPID" 2>/dev/null
    [ -n "$PPID_H3531" ] && kill "$PPID_H3531" 2>/dev/null
    [ -n "$WPID" ] && kill "$WPID" 2>/dev/null
    [ -n "$XPID" ] && kill "$XPID" 2>/dev/null
    [ -n "$TPID" ] && wait "$TPID" 2>/dev/null
    [ -n "$PPID_H3531" ] && wait "$PPID_H3531" 2>/dev/null
    [ -n "$WPID" ] && wait "$WPID" 2>/dev/null
    [ -n "$XPID" ] && wait "$XPID" 2>/dev/null
}
trap cleanup 1 2 15

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
kill -0 "$XPID" 2>/dev/null || { echo "ERROR: Xfbdev exited"; cat "$XLOG"; exit 20; }

"$LOADER" --library-path "$LIBPATH" "$MATCHBOX" \
  -display "$DISPLAY" \
  -theme Default \
  -kbdconfig "$BASE/etc/matchbox/kbdconfig" \
  -use_titlebar yes \
  -use_cursor yes \
  -use_desktop_mode plain \
  >"$WLOG" 2>&1 &
WPID=$!

sleep 3
kill -0 "$WPID" 2>/dev/null || { echo "ERROR: Matchbox WM exited"; cat "$WLOG"; cleanup; exit 21; }

"$LOADER" --library-path "$LIBPATH" "$XSETROOT" \
  -display "$DISPLAY" -solid "#B0B0B0" >"$RLOG" 2>&1 || true

"$LOADER" --library-path "$LIBPATH" "$PANEL" \
  -display "$DISPLAY" \
  --size 48 \
  --orientation north \
  --no-session \
  --default-apps h3531-terminal-applet,mb-applet-clock \
  >"$PLOG" 2>&1 &
PPID_H3531=$!

sleep 4
kill -0 "$PPID_H3531" 2>/dev/null || { echo "ERROR: Matchbox Panel exited"; cat "$PLOG"; cleanup; exit 26; }

if [ "$AUTOTERM" = "1" ]; then
    "$TERMINAL" >"$TLOG" 2>&1 &
    TPID=$!
    sleep 4
    if ! kill -0 "$TPID" 2>/dev/null; then
        echo "ERROR: autostart Terminal exited"
        cat "$TLOG"
        cleanup
        exit 33
    fi
fi

echo "Stage6.2D shell is running."
echo "Expected: gray background + top panel + Terminal launcher + clock."
echo "A Terminal window should also open automatically."
echo "Type: uname -a"
echo "DURATION=0 keeps the shell running until interrupted."

if [ "$DURATION" = "0" ]; then
    while kill -0 "$XPID" 2>/dev/null && kill -0 "$WPID" 2>/dev/null; do
        sleep 5
    done
else
    sleep "$DURATION"
fi

cleanup
trap - 1 2 15

echo "----- WINDOW MANAGER LOG -----"
cat "$WLOG"
echo "----- PANEL LOG -----"
cat "$PLOG"
echo "----- TERMINAL LOG -----"
cat "$TLOG"
echo "----- XSETROOT LOG -----"
cat "$RLOG"
echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "H3531 Stage6.2D proof finished"
