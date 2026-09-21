#!/bin/sh
# H3531 Stage6.6A persistent desktop supervisor.
# Keep this wrapper intentionally small and deterministic.
# USB/RAM only: no saveenv, SPI writes, kernel or bootloader changes.

SYS=/mnt/usb/H3531/SYSTEM
BASE=/mnt/usb/H3531/APPS/x11-debian
ORIGINAL="$SYS/MONITOR.ORIGINAL.APP"
DISABLE="$SYS/DESKTOP.DISABLED"
DESKTOP="$BASE/START-DESKTOP.sh"

BOOTLOG=/var/h3531-desktop-boot.log
MONLOG=/var/h3531-monitor-preinit.log

MONPID=

echo "========================================" >"$BOOTLOG"
echo "H3531 Stage6.6A Persistent Desktop" >>"$BOOTLOG"
echo "Single Monitor preinit + persistent LXDE" >>"$BOOTLOG"
echo "========================================" >>"$BOOTLOG"

if [ -f "$DISABLE" ]; then
    echo "DESKTOP.DISABLED -> original Monitor" >>"$BOOTLOG"
    exec "$ORIGINAL"
fi

[ -x "$ORIGINAL" ] || {
    echo "ERROR: original Monitor missing" >>"$BOOTLOG"
    exit 50
}

[ -x "$DESKTOP" ] || {
    echo "ERROR: Stage6.6A desktop launcher missing -> original Monitor" >>"$BOOTLOG"
    exec "$ORIGINAL"
}

fallback_monitor()
{
    echo "Falling back to original Monitor" >>"$BOOTLOG"

    if [ -n "$MONPID" ] && kill -0 "$MONPID" 2>/dev/null; then
        kill -CONT "$MONPID" 2>/dev/null
        wait "$MONPID"
        return
    fi

    exec "$ORIGINAL"
}

on_signal()
{
    echo "Supervisor interrupted" >>"$BOOTLOG"
    fallback_monitor
    exit 70
}

trap on_signal 1 2 15

echo "Starting original Monitor for HDMI/HIFB initialization" >>"$BOOTLOG"
"$ORIGINAL" >"$MONLOG" 2>&1 &
MONPID=$!
echo "Original Monitor PID=$MONPID" >>"$BOOTLOG"

sleep 3

if ! kill -0 "$MONPID" 2>/dev/null; then
    echo "ERROR: original Monitor exited during initialization" >>"$BOOTLOG"
    MONPID=
    fallback_monitor
    exit 71
fi

kill -STOP "$MONPID" 2>/dev/null || {
    echo "ERROR: cannot STOP original Monitor" >>"$BOOTLOG"
    fallback_monitor
    exit 72
}

echo "Original Monitor stopped PID=$MONPID" >>"$BOOTLOG"
sleep 1

echo "Starting Stage6.6A desktop" >>"$BOOTLOG"
cd "$BASE" || {
    echo "ERROR: cannot enter $BASE" >>"$BOOTLOG"
    fallback_monitor
    exit 73
}

H3531_LXDE_SECONDS=0 "$DESKTOP" >>"$BOOTLOG" 2>&1
RC=$?

echo "Desktop exited rc=$RC" >>"$BOOTLOG"

# A persistent desktop is the normal state. Any return is treated as a failure
# and we stay in the proven original Monitor instead of allowing h3531-session
# to create another preinit copy in a restart loop.
fallback_monitor
exit "$RC"
