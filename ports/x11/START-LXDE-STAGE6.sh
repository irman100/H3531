#!/bin/sh
# H3531 Stage6.4D - integrated LXDE core session
# Xfbdev + HIFB alpha fix + Openbox + PCManFM desktop + LXPanel + LXTerminal
# Safe USB/RAM runtime: no saveenv, no SPI writes.

BASE=/mnt/usb/H3531/APPS/x11-debian
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"

XFBDEV="$BASE/bin/Xfbdev"
XKBCOMP="$BASE/bin/xkbcomp"
OPENBOX="$BASE/bin/openbox"
XSETROOT="$BASE/bin/xsetroot"
FCMATCH="$BASE/bin/fc-match"
HIFBALPHA="$BASE/bin/h3531-hifb-alpha"

PCMANFM="$BASE/bin/pcmanfm"
LXPANEL="$BASE/bin/lxpanel"
LXTERMINAL="$BASE/bin/lxterminal"

GDKQUERY="$BASE/bin/gdk-pixbuf-query-loaders"
GDKCSOURCE="$BASE/bin/gdk-pixbuf-csource"
GDKMODULEDIRFILE="$BASE/etc/gtk/gdk-pixbuf-module-dir"

KEYBD="${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="${H3531_X11_MOUSE:-/dev/input/event0}"
DURATION="${H3531_LXDE_SECONDS:-180}"
AUTOTERM="${H3531_LXDE_AUTOSTART_TERMINAL:-1}"

HOME_DIR=/var/h3531-lxde
LX_USER_PROFILE="$HOME_DIR/.config/lxpanel/LXDE"
PCMAN_USER_PROFILE="$HOME_DIR/.config/pcmanfm/LXDE"
LX_PACKAGED_PROFILE="$BASE/share/lxpanel/profile/LXDE"
PCMAN_PACKAGED_PROFILE="$BASE/etc/xdg/pcmanfm/LXDE"
MENU_CACHE_DIR="$BASE/lib/arm-linux-gnueabi/libmenu-cache1/libexec"
XLOG=/var/h3531-stage64-xfbdev.log
OLOG=/var/h3531-stage64-openbox.log
DLOG=/var/h3531-stage64-pcmanfm.log
PLOG=/var/h3531-stage64-lxpanel.log
TLOG=/var/h3531-stage64-lxterminal.log
GLOG=/var/h3531-stage64-gdk-pixbuf.log
FLOG=/var/h3531-stage64-fontconfig.log
ALOG=/var/h3531-stage64-hifb-alpha.log
RLOG=/var/h3531-stage64-xsetroot.log

FCONF=/var/h3531-fonts.conf
PANGORC=/var/h3531-pangorc
GDKLOADERS=/var/h3531-gdk-pixbuf.loaders
ALPHASAVE=/var/h3531-hifb-alpha.saved

FONTDIR="$BASE/share/fonts/truetype/dejavu"
PANGOVERFILE="$BASE/etc/pango/module-version"
PANGOMODULES="$BASE/etc/pango/pango.modules"
RCFILE="$BASE/etc/openbox/rc.xml"

echo "H3531 Stage6.4D LXDE Core Integration"
echo "keyboard=$KEYBD mouse=$MOUSE duration=${DURATION}s autostart-terminal=$AUTOTERM"
echo "IMPORTANT: resident Monitor must be STOPped before this session."

for f in "$LOADER" "$XFBDEV" "$XKBCOMP" "$OPENBOX" "$XSETROOT" "$FCMATCH"          "$HIFBALPHA" "$PCMANFM" "$LXPANEL" "$LXTERMINAL"; do
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

mkdir -p "$HOME_DIR" "$HOME_DIR/.cache" "$HOME_DIR/.config"          /var/lib/xkb /var/h3531-fontconfig-cache 2>/dev/null

ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || /sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 || {
    echo "ERROR: cannot configure loopback 127.0.0.1"
    exit 15
}

# LXDE was built for normal Debian paths. The package relocates those compiled
# paths to /var so the vendor read-only rootfs can map them back to USB.
mkdir -p /var/lib /var/share 2>/dev/null

if [ -L /var/lib/arm-linux-gnueabi ]; then
    rm -f /var/lib/arm-linux-gnueabi
elif [ -e /var/lib/arm-linux-gnueabi ]; then
    echo "ERROR: /var/lib/arm-linux-gnueabi already exists and is not a symlink"
    exit 16
fi
ln -s "$BASE/lib/arm-linux-gnueabi" /var/lib/arm-linux-gnueabi

# Wheezy libmenu-cache1 stores its helpers inside the multiarch tree:
# /usr/lib/arm-linux-gnueabi/libmenu-cache1/libexec.
# The package relocates /usr/lib/arm-linux-gnueabi to /var/lib/arm-linux-gnueabi,
# and that /var path already points at the USB multiarch tree above.
[ -x "$MENU_CACHE_DIR/menu-cached" ] || { echo "ERROR: menu-cached wrapper missing"; exit 37; }
[ -x "$MENU_CACHE_DIR/menu-cache-gen" ] || { echo "ERROR: menu-cache-gen wrapper missing"; exit 38; }
[ -x "$MENU_CACHE_DIR/menu-cached.real" ] || { echo "ERROR: menu-cached real ELF missing"; exit 39; }
[ -x "$MENU_CACHE_DIR/menu-cache-gen.real" ] || { echo "ERROR: menu-cache-gen real ELF missing"; exit 40; }


for d in applications desktop-directories icons pixmaps lxde lxpanel pcmanfm lxsession libfm mime themes menu; do
    if [ -d "$BASE/share/$d" ]; then
        if [ -L "/var/share/$d" ]; then
            rm -f "/var/share/$d"
        elif [ -e "/var/share/$d" ]; then
            echo "WARNING: /var/share/$d exists; leaving it unchanged"
            continue
        fi
        ln -s "$BASE/share/$d" "/var/share/$d"
    fi
done

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
export HOME="$HOME_DIR"
export XDG_CACHE_HOME="$HOME_DIR/.cache"
export XDG_CONFIG_HOME="$HOME_DIR/.config"
export XDG_CONFIG_DIRS="$BASE/etc/xdg"
export XDG_DATA_DIRS="$BASE/share:/var/share"
export XDG_CURRENT_DESKTOP=LXDE
export XDG_MENU_PREFIX=lxde-
export SHELL=/bin/sh
export LC_ALL=C
export LANG=C
export LANGUAGE=C
export FONTCONFIG_FILE="$FCONF"
export FONTCONFIG_PATH=/var
export PANGO_RC_FILE="$PANGORC"
export LD_LIBRARY_PATH="$LIBPATH"
export PATH="$BASE/bin:/bin:/sbin:/usr/bin:/usr/sbin"
[ -d "$BASE/lib/arm-linux-gnueabi/gio/modules" ] && export GIO_EXTRA_MODULES="$BASE/lib/arm-linux-gnueabi/gio/modules"

# LXPanel 0.5.x and PCManFM 0.9.x are most reliable here with explicit
# writable user profiles. Recreate them for this proof so stale configs from
# earlier sessions cannot shadow the packaged H3531 profile.
rm -rf "$LX_USER_PROFILE" "$PCMAN_USER_PROFILE" 2>/dev/null
mkdir -p "$LX_USER_PROFILE/panels" "$PCMAN_USER_PROFILE" || exit 29

[ -f "$LX_PACKAGED_PROFILE/config" ] || { echo "ERROR: packaged LXPanel config missing"; exit 30; }
[ -f "$LX_PACKAGED_PROFILE/panels/panel" ] || { echo "ERROR: packaged LXPanel panel missing"; exit 31; }
[ -f "$PCMAN_PACKAGED_PROFILE/pcmanfm.conf" ] || { echo "ERROR: packaged PCManFM profile missing"; exit 32; }

cp "$LX_PACKAGED_PROFILE/config" "$LX_USER_PROFILE/config" || exit 33
cp "$LX_PACKAGED_PROFILE/panels/panel" "$LX_USER_PROFILE/panels/panel" || exit 34
cp "$PCMAN_PACKAGED_PROFILE/pcmanfm.conf" "$PCMAN_USER_PROFILE/pcmanfm.conf" || exit 35

"$LOADER" --library-path "$LIBPATH" "$FCMATCH" "Sans:bold" >"$FLOG" 2>&1 || {
    echo "ERROR: fontconfig cannot resolve Sans:bold"
    cat "$FLOG"
    exit 17
}
echo "fontconfig: $(cat "$FLOG")"

# Build GdkPixbuf cache on the target when the packaged loader stack is present.
# Failure is logged but does not abort LXDE: text/file operations remain useful.
: >"$GLOG"
if [ -x "$GDKQUERY" ] && [ -x "$GDKCSOURCE" ] && [ -f "$GDKMODULEDIRFILE" ]; then
    GDKMODULEDIR="$(cat "$GDKMODULEDIRFILE")"
    export GDK_PIXBUF_MODULEDIR="$GDKMODULEDIR"
    unset GDK_PIXBUF_MODULE_FILE

    if "$LOADER" --library-path "$LIBPATH" "$GDKQUERY" "$GDKMODULEDIR"/*.so         >"$GDKLOADERS" 2>>"$GLOG" && [ -s "$GDKLOADERS" ]; then
        export GDK_PIXBUF_MODULE_FILE="$GDKLOADERS"
        if "$LOADER" --library-path "$LIBPATH" "$GDKCSOURCE"             "$BASE/share/pixmaps/h3531-lxde-test.xpm" >/dev/null 2>>"$GLOG"; then
            echo "gdk-pixbuf: target loader cache ready (XPM verified)"
        else
            echo "WARNING: GdkPixbuf XPM verification failed; continuing LXDE"
        fi
    else
        echo "WARNING: GdkPixbuf loader cache generation failed; continuing LXDE"
    fi
else
    echo "WARNING: GdkPixbuf runtime tools unavailable; continuing LXDE" >>"$GLOG"
fi

cat >/var/xkbcomp <<EOF
#!/bin/sh
echo "\$@" >/var/h3531-xkbcomp.args
exec "$LOADER" --library-path "$LIBPATH" "$XKBCOMP" -I"$BASE/share/X11/xkb" "\$@"
EOF
chmod 755 /var/xkbcomp

run_hifb_alpha()
{
    "$LOADER" --library-path "$LIBPATH" "$HIFBALPHA" "$@"
}

XPID=
OPID=
DPID=
PPID_H3531=
TPID=
ALPHA_ACTIVE=0

restore_alpha()
{
    if [ "$ALPHA_ACTIVE" = "1" ]; then
        echo "----- HIFB ALPHA RESTORE -----" >>"$ALOG"
        run_hifb_alpha restore /dev/fb0 "$ALPHASAVE" >>"$ALOG" 2>&1 || true
        ALPHA_ACTIVE=0
    fi
}

cleanup()
{
    "$PCMANFM" --profile LXDE --desktop-off >/dev/null 2>&1 || true
    [ -n "$TPID" ] && kill "$TPID" 2>/dev/null
    [ -n "$PPID_H3531" ] && kill "$PPID_H3531" 2>/dev/null
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    [ -n "$OPID" ] && kill "$OPID" 2>/dev/null
    [ -n "$XPID" ] && kill "$XPID" 2>/dev/null

    [ -n "$TPID" ] && wait "$TPID" 2>/dev/null
    [ -n "$PPID_H3531" ] && wait "$PPID_H3531" 2>/dev/null
    [ -n "$DPID" ] && wait "$DPID" 2>/dev/null
    [ -n "$OPID" ] && wait "$OPID" 2>/dev/null
    [ -n "$XPID" ] && wait "$XPID" 2>/dev/null

    restore_alpha
}
trap cleanup 1 2 15

: >"$ALOG"
echo "----- /proc/graphics/hifb0 BEFORE -----" >>"$ALOG"
cat /proc/graphics/hifb0 >>"$ALOG" 2>&1 || true

run_hifb_alpha restore-if-saved /dev/fb0 "$ALPHASAVE" >>"$ALOG" 2>&1 || {
    echo "ERROR: cannot recover previous HIFB alpha state"
    cat "$ALOG"
    exit 26
}

run_hifb_alpha push-opaque /dev/fb0 "$ALPHASAVE" >>"$ALOG" 2>&1 || {
    echo "ERROR: HIFB alpha opaque switch failed"
    cat "$ALOG"
    exit 27
}
ALPHA_ACTIVE=1

echo "HIFB alpha: opaque 255/255 enabled for LXDE session."

"$LOADER" --library-path "$LIBPATH" "$XFBDEV" :0   -fb /dev/fb0   -screen 1280x720x16   -keybd "evdev,,device=$KEYBD"   -mouse "evdev,,device=$MOUSE"   -fp "$BASE/share/fonts/X11/misc"   -xkbdir "$BASE/share/X11/xkb"   -softCursor   -nolisten unix   -nolock -ac -noreset   >"$XLOG" 2>&1 &
XPID=$!

sleep 3
kill -0 "$XPID" 2>/dev/null || {
    echo "ERROR: Xfbdev exited"
    cat "$XLOG"
    cleanup
    exit 20
}

"$LOADER" --library-path "$LIBPATH" "$XSETROOT"   -display "$DISPLAY" -solid "#F2F2F2" >"$RLOG" 2>&1 || true

"$LOADER" --library-path "$LIBPATH" "$OPENBOX"   --sm-disable --config-file "$RCFILE" >"$OLOG" 2>&1 &
OPID=$!

sleep 3
kill -0 "$OPID" 2>/dev/null || {
    echo "ERROR: Openbox exited"
    cat "$OLOG"
    cleanup
    exit 24
}

: >"$DLOG"
echo "PCManFM user profile: $PCMAN_USER_PROFILE/pcmanfm.conf" >>"$DLOG"
cat "$PCMAN_USER_PROFILE/pcmanfm.conf" >>"$DLOG" 2>&1

# PCManFM 0.9.x may daemonize and make the starter PID disappear. That is
# expected, so never use the starter PID as the health check. Always issue the
# separate --desktop command after giving the daemon a moment to initialize.
"$PCMANFM" --profile LXDE --daemon-mode >>"$DLOG" 2>&1 &
PCMAN_START_PID=$!
sleep 2
if kill -0 "$PCMAN_START_PID" 2>/dev/null; then
    echo "pcmanfm daemon starter still foreground pid=$PCMAN_START_PID" >>"$DLOG"
else
    echo "pcmanfm daemon starter exited/daemonized (expected)" >>"$DLOG"
fi

"$PCMANFM" --profile LXDE --desktop >>"$DLOG" 2>&1 &
DPID=$!
sleep 3
if kill -0 "$DPID" 2>/dev/null; then
    echo "pcmanfm desktop manager foreground pid=$DPID" >>"$DLOG"
else
    echo "pcmanfm desktop command returned; desktop request was delivered" >>"$DLOG"
    DPID=
fi

: >"$PLOG"
echo "LXPanel user profile: $LX_USER_PROFILE" >>"$PLOG"
cat "$LX_USER_PROFILE/config" >>"$PLOG" 2>&1
cat "$LX_USER_PROFILE/panels/panel" >>"$PLOG" 2>&1

"$LXPANEL" --profile LXDE >>"$PLOG" 2>&1 &
PPID_H3531=$!

sleep 4
if ! kill -0 "$PPID_H3531" 2>/dev/null; then
    echo "ERROR: LXPanel exited"
    cat "$PLOG"
    cleanup
    exit 28
fi

if [ "$AUTOTERM" = "1" ]; then
    "$LXTERMINAL" >"$TLOG" 2>&1 &
    TPID=$!
    sleep 4
    if ! kill -0 "$TPID" 2>/dev/null; then
        echo "WARNING: LXTerminal did not stay running"
        cat "$TLOG"
        TPID=
    fi
fi

echo "Stage6.4D LXDE core session is running."
echo "menu-cache helpers: /var/lib/arm-linux-gnueabi/libmenu-cache1/libexec -> USB wrappers"
echo "Expected: light desktop + LXPanel + Applications menu + file manager + LXTerminal."
echo "Right-click desktop and use panel/menu normally."
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

echo "----- OPENBOX LOG -----"
cat "$OLOG"
echo "----- PCMANFM LOG -----"
cat "$DLOG"
echo "----- LXPANEL LOG -----"
cat "$PLOG"
echo "----- LXTERMINAL LOG -----"
cat "$TLOG"
echo "----- GDK PIXBUF LOG -----"
cat "$GLOG"
echo "----- HIFB ALPHA LOG -----"
cat "$ALOG"
echo "----- XFBDEV LOG -----"
cat "$XLOG"
echo "H3531 Stage6.4D LXDE session finished"
