#!/bin/sh
# H3531 Stage6.2B - usable Matchbox desktop + panel + terminal
# Safe USB/RAM test: no saveenv, no SPI writes.
BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib"
XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
MATCHBOX="$BASE/bin/matchbox-window-manager"
DESKTOP="$BASE/bin/matchbox-desktop"
PANEL="$BASE/bin/matchbox-panel"
FCMATCH="$BASE/bin/fc-match"
GDKCSOURCE="$BASE/bin/gdk-pixbuf-csource"
FONTDIR="$BASE/share/fonts/truetype/dejavu"
PANGOVERFILE="$BASE/etc/pango/module-version"
PANGOMODULES="$BASE/etc/pango/pango.modules"
GDKLOADERS="$BASE/etc/gtk/gdk-pixbuf.loaders"
GDKMODULEDIRFILE="$BASE/etc/gtk/gdk-pixbuf-module-dir"
KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_DESKTOP_SECONDS:-120}"

XLOG=/var/h3531-stage62-xfbdev.log
WLOG=/var/h3531-stage62-wm.log
DLOG=/var/h3531-stage62-desktop.log
PLOG=/var/h3531-stage62-panel.log
FLOG=/var/h3531-stage62-fontconfig.log
GLOG=/var/h3531-stage62-gdk-pixbuf.log
FCONF=/var/h3531-fonts.conf
PANGORC=/var/h3531-pangorc
GTKRC=/var/h3531-gtkrc-2.0

echo "H3531 Stage6.2B Matchbox Desktop + Panel + Terminal"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s"
echo "IMPORTANT: resident Monitor must be STOPped before this test."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$MATCHBOX" "$DESKTOP" "$PANEL" "$FCMATCH" "$GDKCSOURCE" \
         "$BASE/bin/mb-applet-menu-launcher" "$BASE/bin/mb-applet-clock" "$BASE/bin/h3531-terminal"; do
    [ -x "$f" ] || { echo "ERROR: missing executable $f"; exit 10; }
done
[ -c /dev/fb0 ] || { echo "ERROR: /dev/fb0 missing"; exit 11; }
[ -c "$KEYBD" ] || { echo "ERROR: $KEYBD missing"; exit 12; }
[ -c "$MOUSE" ] || { echo "ERROR: $MOUSE missing"; exit 13; }
[ -f "$PANGOVERFILE" ] || { echo "ERROR: Pango module-version missing"; exit 18; }
[ -f "$PANGOMODULES" ] || { echo "ERROR: Pango module registry missing"; exit 19; }
[ -f "$GDKLOADERS" ] || { echo "ERROR: GdkPixbuf loader registry missing"; exit 24; }
[ -f "$GDKMODULEDIRFILE" ] || { echo "ERROR: GdkPixbuf module directory file missing"; exit 27; }

PANGOVER="$(cat "$PANGOVERFILE")"
PANGODIR="$BASE/lib/pango/$PANGOVER/modules"
GDKMODULEDIR="$(cat "$GDKMODULEDIRFILE")"
[ -f "$PANGODIR/pango-basic-fc.so" ] || { echo "ERROR: Pango basic FC module missing"; exit 22; }
[ -d "$GDKMODULEDIR" ] || { echo "ERROR: GdkPixbuf module directory missing"; exit 28; }

mkdir -p /var/h3531-x11 /var/lib/xkb /var/share /var/h3531-fontconfig-cache 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || \
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 14
}

rm -f /var/share/themes /var/share/matchbox /var/share/applications /var/share/pixmaps /var/share/icons 2>/dev/null
ln -s "$BASE/share/themes" /var/share/themes
ln -s "$BASE/share/matchbox" /var/share/matchbox
ln -s "$BASE/share/applications" /var/share/applications
ln -s "$BASE/share/pixmaps" /var/share/pixmaps
ln -s "$BASE/share/icons" /var/share/icons

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

cat >"$GTKRC" <<'EOF'
style "h3531"
{
  font_name = "Sans 14"
  bg[NORMAL] = "#D8D8D8"
  fg[NORMAL] = "#101010"
  bg[ACTIVE] = "#B8B8B8"
  fg[ACTIVE] = "#101010"
  bg[PRELIGHT] = "#EEEEEE"
  fg[PRELIGHT] = "#101010"
}
widget "*" style "h3531"
EOF

export DISPLAY=127.0.0.1:0
export HOME=/var/h3531-x11
export SHELL=/bin/sh
export LC_ALL=C
export FONTCONFIG_FILE="$FCONF"
export FONTCONFIG_PATH=/var
export PANGO_RC_FILE="$PANGORC"
export GTK2_RC_FILES="$GTKRC"
export GDK_PIXBUF_MODULE_FILE="$GDKLOADERS"
export GDK_PIXBUF_MODULEDIR="$GDKMODULEDIR"
export XDG_DATA_DIRS="$BASE/share"
export PATH="$BASE/bin:/bin:/sbin:/usr/bin:/usr/sbin"

"$LOADER" --library-path "$LIBPATH" "$FCMATCH" "Sans:bold" >"$FLOG" 2>&1 || {
    echo "ERROR: fontconfig cannot resolve Sans:bold"
    cat "$FLOG"
    exit 17
}
echo "fontconfig: $(cat "$FLOG")"

: >"$GLOG"
"$LOADER" --library-path "$LIBPATH" "$GDKCSOURCE" "$BASE/share/pixmaps/mbmenu.png" >/dev/null 2>>"$GLOG" || {
    echo "ERROR: GdkPixbuf cannot decode packaged PNG"
    cat "$GLOG"
    exit 29
}
"$LOADER" --library-path "$LIBPATH" "$GDKCSOURCE" "$BASE/share/pixmaps/xterm_48x48.xpm" >/dev/null 2>>"$GLOG" || {
    echo "ERROR: GdkPixbuf cannot decode packaged XPM"
    cat "$GLOG"
    exit 30
}
echo "gdk-pixbuf: PNG and XPM loaders OK"

cat >/var/xkbcomp <<EOF
#!/bin/sh
echo "\$@" >/var/h3531-xkbcomp.args
exec "$LOADER" --library-path "$LIBPATH" "$XKBCOMP" -I"$BASE/share/X11/xkb" "\$@"
EOF
chmod 755 /var/xkbcomp

XPID=
WPID=
DPID=
PPID_H3531=

cleanup()
{
    [ -n "$PPID_H3531" ] && kill "$PPID_H3531" 2>/dev/null
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    [ -n "$WPID" ] && kill "$WPID" 2>/dev/null
    [ -n "$XPID" ] && kill "$XPID" 2>/dev/null
    [ -n "$PPID_H3531" ] && wait "$PPID_H3531" 2>/dev/null
    [ -n "$DPID" ] && wait "$DPID" 2>/dev/null
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

"$LOADER" --library-path "$LIBPATH" "$DESKTOP" >"$DLOG" 2>&1 &
DPID=$!

sleep 3
kill -0 "$DPID" 2>/dev/null || { echo "ERROR: Matchbox Desktop exited"; cat "$DLOG"; cleanup; exit 25; }

"$LOADER" --library-path "$LIBPATH" "$PANEL" \
  -display "$DISPLAY" \
  --size 42 \
  --orientation north \
  --no-session \
  --default-apps mb-applet-menu-launcher,mb-applet-clock \
  >"$PLOG" 2>&1 &
PPID_H3531=$!

sleep 3
kill -0 "$PPID_H3531" 2>/dev/null || { echo "ERROR: Matchbox Panel exited"; cat "$PLOG"; cleanup; exit 26; }

echo "Stage6.2B desktop is running."
echo "Expected: undecorated desktop + top panel + menu/clock + Terminal launcher."
echo "Open Terminal and type: uname -a"
echo "DURATION=0 keeps the desktop running until interrupted."

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
echo "----- DESKTOP LOG -----"
cat "$DLOG"
echo "----- PANEL LOG -----"
cat "$PLOG"
echo "----- GDK PIXBUF LOG -----"
cat "$GLOG"
echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "H3531 Stage6.2B proof finished"
