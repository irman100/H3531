#!/bin/sh
# Stayplaytion Stage6.7.3 Framebuffer Ownership Diagnostic Boot Manager.
# Normal user-visible path is Boot Loader -> Desktop.
# Original Monitor receives exclusive /dev/fb0 ownership during compatibility pre-init.

SYS=/mnt/usb/H3531/SYSTEM
BASE=/mnt/usb/H3531/APPS/x11-debian
ORIGINAL="$SYS/MONITOR.ORIGINAL.APP"
DISABLE="$SYS/DESKTOP.DISABLED"
DESKTOP="$BASE/START-DESKTOP.sh"
DISCOVER="$BASE/bin/h3531-input-discover"
BOOTSCREEN="$BASE/bin/h3531-boot-screen"
HIFBALPHA="$BASE/bin/h3531-hifb-alpha"
BOOT_ALPHA_SAVE=/var/h3531-boot-alpha.saved
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"
INPUT_ENV=/var/h3531-input.env
BOOTLOG=/var/h3531-boot-manager.log
MONLOG=/var/h3531-monitor-preinit.log

BOOT_ALPHA_ACTIVE=0
MONPID=

: >"$BOOTLOG"
: >"$MONLOG"

log()
{
    echo "$*" >>"$BOOTLOG"
}

log_hifb_state()
{
    TAG="$1"
    echo "===== HIFB $TAG =====" >>"$BOOTLOG"
    cat /proc/graphics/hifb0 >>"$BOOTLOG" 2>&1 || true
    if [ -x "$HIFBALPHA" ]; then
        "$LOADER" --library-path "$LIBPATH"             "$HIFBALPHA" status /dev/fb0 >>"$BOOTLOG" 2>&1 || true
    fi
}

restore_boot_alpha()
{
    if [ "$BOOT_ALPHA_ACTIVE" = "1" ] && [ -x "$HIFBALPHA" ]; then
        "$LOADER" --library-path "$LIBPATH"             "$HIFBALPHA" restore /dev/fb0 "$BOOT_ALPHA_SAVE" >>"$BOOTLOG" 2>&1 || true
        BOOT_ALPHA_ACTIVE=0
    fi
}

rescue()
{
    restore_boot_alpha
    log "Entering original Monitor rescue mode"

    if [ -n "$MONPID" ] && kill -0 "$MONPID" 2>/dev/null; then
        kill -CONT "$MONPID" 2>/dev/null || true
        wait "$MONPID"
        exit $?
    fi

    exec "$ORIGINAL"
}

on_signal()
{
    log "Boot manager interrupted"
    rescue
}

trap on_signal 1 2 15

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
    log "Input discovery: keyboard=$H3531_X11_KEYBD mouse=$H3531_X11_MOUSE"
    return 0
}

start_monitor_preinit()
{
    log "Starting original Monitor exclusive compatibility pre-init"
    log_hifb_state "BEFORE MONITOR PREINIT"

    # Stage6.7.3 diagnostic: do NOT keep h3531-boot-screen open while
    # MONITOR.ORIGINAL.APP initializes HIFB. D6.4 proved the Monitor-first,
    # single-owner framebuffer sequence on hardware. Stage6.7.2 violated it by
    # opening /dev/fb0 concurrently from the warmup mask every 100 ms.
    "$ORIGINAL" >"$MONLOG" 2>&1 &
    MONPID=$!
    log "Original Monitor exclusive pre-init PID=$MONPID"

    sleep 4

    if ! kill -0 "$MONPID" 2>/dev/null; then
        log "ERROR: original Monitor exited during exclusive compatibility pre-init"
        MONPID=
        return 1
    fi

    kill -STOP "$MONPID" 2>/dev/null || {
        log "ERROR: cannot STOP original Monitor after compatibility pre-init"
        return 1
    }

    sleep 1
    log "Original Monitor exclusive compatibility pre-init complete; stopped PID=$MONPID"
    log_hifb_state "AFTER MONITOR PREINIT"

    # Repaint Stayplaytion only after Monitor has released active execution.
    "$LOADER" --library-path "$LIBPATH" "$BOOTSCREEN" ready 1500 >>"$BOOTLOG" 2>&1 || true
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

log "Stayplaytion Stage6.7.3 framebuffer-ownership diagnostic boot"

if [ -x "$HIFBALPHA" ]; then
    "$LOADER" --library-path "$LIBPATH"         "$HIFBALPHA" restore-if-saved /dev/fb0 "$BOOT_ALPHA_SAVE" >>"$BOOTLOG" 2>&1 || true

    if "$LOADER" --library-path "$LIBPATH"         "$HIFBALPHA" push-opaque /dev/fb0 "$BOOT_ALPHA_SAVE" >>"$BOOTLOG" 2>&1; then
        BOOT_ALPHA_ACTIVE=1
        log "Boot framebuffer alpha forced opaque"
    else
        log "WARNING: cannot force boot framebuffer alpha opaque"
    fi
fi

log_hifb_state "COLD BOOT"

"$LOADER" --library-path "$LIBPATH" "$BOOTSCREEN" menu 7000 >>"$BOOTLOG" 2>&1
MENU_RC=$?
log "Boot menu exited rc=$MENU_RC"

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

log "Releasing boot-screen alpha ownership before Monitor pre-init"
restore_boot_alpha

if ! start_monitor_preinit; then
    log "Compatibility pre-init failed"
    rescue
fi

if ! discover_input; then
    log "Input discovery failed after Monitor compatibility pre-init"
    rescue
fi

cd "$BASE" || {
    log "ERROR: cannot enter $BASE"
    rescue
}

log "Starting desktop after exclusive Monitor compatibility pre-init"
H3531_LXDE_SECONDS=0 "$DESKTOP" >>"$BOOTLOG" 2>&1
RC=$?
log "Desktop exited rc=$RC; resuming rescue Monitor"
rescue
