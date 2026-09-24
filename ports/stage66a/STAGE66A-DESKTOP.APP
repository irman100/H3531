#!/bin/sh
# H3531 Stage6.6D4 self-healing persistent desktop supervisor.
# Original Monitor is retained as the proven HDMI/HIFB/input recovery layer.
# USB/RAM only: no saveenv, SPI writes, kernel or bootloader changes.

SYS=/mnt/usb/H3531/SYSTEM
BASE=/mnt/usb/H3531/APPS/x11-debian
ORIGINAL="$SYS/MONITOR.ORIGINAL.APP"
DISABLE="$SYS/DESKTOP.DISABLED"
DESKTOP="$BASE/START-DESKTOP.sh"
DISCOVER="$BASE/bin/h3531-input-discover"
INPUT_ENV=/var/h3531-input.env

BOOTLOG=/var/h3531-desktop-boot.log
MONLOG=/var/h3531-monitor-preinit.log

MONPID=

echo "========================================" >"$BOOTLOG"
echo "H3531 Stage6.6D4 Self-Healing Desktop" >>"$BOOTLOG"
echo "Dynamic evdev + Monitor recovery loop" >>"$BOOTLOG"
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

log()
{
    echo "$*" >>"$BOOTLOG"
}

monitor_warmup()
{
    SECONDS="$1"

    if [ -z "$MONPID" ] || ! kill -0 "$MONPID" 2>/dev/null; then
        log "ERROR: recovery Monitor is not alive"
        return 1
    fi

    log "Resuming original Monitor for ${SECONDS}s recovery warmup"
    kill -CONT "$MONPID" 2>/dev/null || return 1
    sleep "$SECONDS"

    kill -STOP "$MONPID" 2>/dev/null || return 1
    log "Original Monitor stopped again PID=$MONPID"
    sleep 1
    return 0
}

discover_input()
{
    rm -f "$INPUT_ENV" 2>/dev/null
    [ -x "$DISCOVER" ] || {
        log "ERROR: input discovery helper missing: $DISCOVER"
        return 1
    }

    "$DISCOVER" "$INPUT_ENV" >>"$BOOTLOG" 2>&1 || return 1
    [ -r "$INPUT_ENV" ] || return 1

    H3531_X11_KEYBD=
    H3531_X11_MOUSE=
    . "$INPUT_ENV"

    [ -n "$H3531_X11_KEYBD" ] || return 1
    [ -n "$H3531_X11_MOUSE" ] || return 1
    [ -c "$H3531_X11_KEYBD" ] || return 1
    [ -c "$H3531_X11_MOUSE" ] || return 1

    export H3531_X11_KEYBD H3531_X11_MOUSE
    log "Input discovery: keyboard=$H3531_X11_KEYBD mouse=$H3531_X11_MOUSE"
    return 0
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

cd "$BASE" || {
    echo "ERROR: cannot enter $BASE" >>"$BOOTLOG"
    fallback_monitor
    exit 73
}

while true; do
    if [ -f "$DISABLE" ]; then
        log "DESKTOP.DISABLED detected during supervisor loop"
        fallback_monitor
        exit 0
    fi

    if ! discover_input; then
        log "Input discovery failed; using Monitor recovery warmup"
        if ! monitor_warmup 3 || ! discover_input; then
                        log "Input recovery retry requested"
            sleep 2
            continue
        fi
    fi

    log "Starting desktop"
    H3531_LXDE_SECONDS=0 "$DESKTOP" >>"$BOOTLOG" 2>&1
    RC=$?
    log "Desktop exited rc=$RC"

    if [ -f "$DISABLE" ]; then
        fallback_monitor
        exit "$RC"
    fi

        log "Desktop self-heal cycle requested"

    # Live hardware proved that returning briefly to original Monitor makes
    # USB HID/input healthy again. Reuse that proven layer, then stop it before
    # handing framebuffer/input to a freshly discovered X session.
    if ! monitor_warmup 3; then
        log "Monitor recovery warmup failed"
        fallback_monitor
        exit 75
    fi
done
