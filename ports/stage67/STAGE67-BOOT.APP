#!/bin/sh
# Stayplaytion Stage6.7 direct boot manager.
# Normal path does NOT start the vendor Monitor. F2 or a critical failure
# enters the original Monitor rescue environment.

SYS=/mnt/usb/H3531/SYSTEM
BASE=/mnt/usb/H3531/APPS/x11-debian
ORIGINAL="$SYS/MONITOR.ORIGINAL.APP"
DISABLE="$SYS/DESKTOP.DISABLED"
DESKTOP="$BASE/START-DESKTOP.sh"
DISCOVER="$BASE/bin/h3531-input-discover"
BOOTSCREEN="$BASE/bin/h3531-boot-screen"
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"
INPUT_ENV=/var/h3531-input.env
BOOTLOG=/var/h3531-boot-manager.log

: >"$BOOTLOG"

log()
{
    echo "$*" >>"$BOOTLOG"
}

rescue()
{
    log "Entering original Monitor rescue mode"
    exec "$ORIGINAL"
}

discover_input()
{
    rm -f "$INPUT_ENV" 2>/dev/null
    [ -x "$DISCOVER" ] || return 1
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
    return 0
}

[ -x "$ORIGINAL" ] || {
    log "ERROR: original Monitor missing"
    exit 50
}

if [ -f "$DISABLE" ]; then
    log "DESKTOP.DISABLED -> rescue Monitor"
    rescue
fi

[ -x "$DESKTOP" ] || {
    log "ERROR: desktop launcher missing"
    rescue
}

[ -x "$BOOTSCREEN" ] || {
    log "ERROR: boot screen helper missing"
    rescue
}

log "Stayplaytion Stage6.7 direct boot"
"$LOADER" --library-path "$LIBPATH" "$BOOTSCREEN" 4000 >>"$BOOTLOG" 2>&1
MENU_RC=$?

case "$MENU_RC" in
    2)
        log "F2 requested rescue Monitor"
        rescue
        ;;
    0)
        ;;
    *)
        log "Boot preflight failed rc=$MENU_RC"
        rescue
        ;;
esac

if ! discover_input; then
    log "Input discovery failed before direct desktop start"
    rescue
fi

cd "$BASE" || {
    log "ERROR: cannot enter $BASE"
    rescue
}

log "Starting desktop directly without Monitor warmup"
H3531_LXDE_SECONDS=0 "$DESKTOP" >>"$BOOTLOG" 2>&1
RC=$?
log "Desktop exited rc=$RC; switching to rescue Monitor"
rescue
