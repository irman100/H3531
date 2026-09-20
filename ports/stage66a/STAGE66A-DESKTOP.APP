#!/bin/sh
# H3531 Stage6.6A persistent desktop supervisor.
# USB/RAM only: no saveenv, SPI writes, kernel or bootloader changes.

SYS=/mnt/usb/H3531/SYSTEM
BASE=/mnt/usb/H3531/APPS/x11-debian
APPS=/mnt/usb/H3531/APPS

ORIGINAL="$SYS/MONITOR.ORIGINAL.APP"
DISABLE="$SYS/DESKTOP.DISABLED"
DESKTOP="$BASE/START-DESKTOP.sh"
DHCP="$BASE/bin/h3531-net-dhcp"

REQUEST=/var/h3531-app-request
APPFILE=/var/h3531-app-file
BOOTLOG=/var/h3531-desktop-boot.log
NETLOG=/var/h3531-desktop-network.log
APPLOG=/var/h3531-app-supervisor.log
MONLOG=/var/h3531-monitor-preinit.log

echo "========================================" >"$BOOTLOG"
echo "H3531 Stage6.6A Persistent Desktop" >>"$BOOTLOG"
echo "FullFrame + DHCP + X11 applications" >>"$BOOTLOG"
echo "========================================" >>"$BOOTLOG"

rm -f "$REQUEST" "$APPFILE"

# Remove only known transitional Stage6.x files left behind by overlay installs.
# User ROMs, games, application data, MONITOR.BAK and MONITOR.ORIGINAL.APP are
# deliberately untouched.
echo "Cleaning obsolete pre-6.6A runtime files" >>"$BOOTLOG"
rm -f "$BASE"/START-LXDE-STAGE6*.sh 2>/dev/null
rm -f "$BASE"/START-X11-*.sh 2>/dev/null
rm -f "$BASE"/STAGE6.VERSION 2>/dev/null
rm -f "$SYS"/MONITOR.V3.APP "$SYS"/MONITOR.V4.APP 2>/dev/null
rm -f "$SYS"/MONITOR.DESKTOP.PREV.APP "$SYS"/MONITOR.PREVIOUS.APP 2>/dev/null
rm -f "$APPS"/h3531-run.previous 2>/dev/null
rm -f "$APPS"/fbzx/FBZX.BIN 2>/dev/null
for OLD_STAGE_DOC in "$SYS"/docs/README_STAGE6_* "$SYS"/docs/SHA256SUMS_STAGE6_*; do
    [ -e "$OLD_STAGE_DOC" ] || continue
    case "$OLD_STAGE_DOC" in
        "$SYS"/docs/README_STAGE6_6A.txt|"$SYS"/docs/SHA256SUMS_STAGE6_6A.txt)
            ;;
        *)
            rm -f "$OLD_STAGE_DOC" 2>/dev/null
            ;;
    esac
done
rm -f "$SYS"/docs/README_DILLO* "$SYS"/docs/README_MIDORI* 2>/dev/null

if [ -f "$DISABLE" ]; then
    echo "DESKTOP.DISABLED -> original Monitor" >>"$BOOTLOG"
    exec "$ORIGINAL"
fi

[ -x "$ORIGINAL" ] || {
    echo "ERROR: original Monitor missing" >>"$BOOTLOG"
    exit 50
}
[ -x "$DESKTOP" ] || {
    echo "ERROR: Stage6.6A desktop launcher missing" >>"$BOOTLOG"
    exec "$ORIGINAL"
}

echo "Starting original Monitor for HDMI/HIFB initialization" >>"$BOOTLOG"
"$ORIGINAL" >"$MONLOG" 2>&1 &
MONPID=$!
echo "Original Monitor PID=$MONPID" >>"$BOOTLOG"

sleep 3
if ! kill -0 "$MONPID" 2>/dev/null; then
    echo "ERROR: original Monitor died during initialization" >>"$BOOTLOG"
    exec "$ORIGINAL"
fi
kill -STOP "$MONPID"
echo "Original Monitor stopped" >>"$BOOTLOG"
sleep 1

: >"$NETLOG"
if [ -x "$DHCP" ]; then
    N=1
    while [ "$N" -le 3 ]; do
        echo "DHCP attempt=$N" >>"$NETLOG"
        "$DHCP" eth0 >>"$NETLOG" 2>&1
        RC=$?
        if [ "$RC" -eq 0 ]; then
            echo "DHCP OK" >>"$NETLOG"
            break
        fi
        echo "DHCP failed rc=$RC" >>"$NETLOG"
        N=`expr "$N" + 1`
        sleep 2
    done
else
    echo "WARNING: DHCP helper missing: $DHCP" >>"$NETLOG"
fi

while true; do
    rm -f "$REQUEST" "$APPFILE"

    mkdir -p /var/h3531-lxde/Desktop 2>/dev/null
    if [ -f "$BASE/share/applications/h3531-fbzx.desktop" ]; then
        cp "$BASE/share/applications/h3531-fbzx.desktop"            /var/h3531-lxde/Desktop/FBZX.desktop
        chmod +x /var/h3531-lxde/Desktop/FBZX.desktop
    fi

    echo "Starting Stage6.6A persistent desktop" >>"$BOOTLOG"
    cd "$BASE" || {
        kill -CONT "$MONPID" 2>/dev/null
        wait "$MONPID"
        exit 51
    }

    H3531_LXDE_SECONDS=0 "$DESKTOP" >>"$BOOTLOG" 2>&1
    LXRC=$?
    echo "LXDE exited rc=$LXRC" >>"$BOOTLOG"

    # Normal X11 programs never reach this block because X stays alive.
    # Only explicit legacy framebuffer requests intentionally stop X.
    if [ -f "$REQUEST" ]; then
        APP="$(cat "$REQUEST" 2>/dev/null)"
        FILE=
        [ -f "$APPFILE" ] && FILE="$(cat "$APPFILE" 2>/dev/null)"
        rm -f "$REQUEST" "$APPFILE"

        echo "request=$APP file=$FILE" >>"$APPLOG"

        case "$APP" in
            fbzx-native)
                if [ -x "$APPS/fbzx/FBZX.APP" ]; then
                    cd "$APPS/fbzx"
                    if [ -n "$FILE" ]; then
                        SDL_VIDEODRIVER=h3531 SDL_FBDEV=/dev/fb0                             "$APPS/fbzx/FBZX.APP" "$FILE" >>"$APPLOG" 2>&1
                    else
                        SDL_VIDEODRIVER=h3531 SDL_FBDEV=/dev/fb0                             "$APPS/fbzx/FBZX.APP" >>"$APPLOG" 2>&1
                    fi
                else
                    echo "ERROR: native FBZX missing" >>"$APPLOG"
                fi
                ;;
            *)
                echo "ERROR: unknown legacy request=$APP" >>"$APPLOG"
                ;;
        esac

        # Legacy framebuffer apps may alter HIFB state.
        kill -CONT "$MONPID" 2>/dev/null
        sleep 3
        if kill -0 "$MONPID" 2>/dev/null; then
            kill -STOP "$MONPID"
            sleep 1
            continue
        fi
        echo "ERROR: original Monitor disappeared" >>"$APPLOG"
        exit 60
    fi

    echo "Unexpected LXDE exit -> original Monitor" >>"$BOOTLOG"
    kill -CONT "$MONPID" 2>/dev/null
    wait "$MONPID"
    exit "$LXRC"
done
