#!/bin/sh
# Stayplaytion Stage6.8.0 Firmware POST Prototype.
# Safety rule: no saveenv, no SPI writes, no bootloader replacement.
# This stage preserves the proven Stage6.7.3 exclusive framebuffer ownership.

SYS=/mnt/usb/H3531/SYSTEM
BASE=/mnt/usb/H3531/APPS/x11-debian
ORIGINAL="$SYS/MONITOR.ORIGINAL.APP"
DISABLE="$SYS/DESKTOP.DISABLED"
DESKTOP="$BASE/START-DESKTOP.sh"
DISCOVER="$BASE/bin/h3531-input-discover"
OSPROBE="$BASE/bin/h3531-os-probe"
BOOTSCREEN="$BASE/bin/h3531-boot-screen"
HIFBALPHA="$BASE/bin/h3531-hifb-alpha"
BOOT_ALPHA_SAVE=/var/h3531-boot-alpha.saved
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"
INPUT_ENV=/var/h3531-input.env
BOOTLOG=/var/h3531-firmware.log
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
        "$LOADER" --library-path "$LIBPATH" "$HIFBALPHA" status /dev/fb0 >>"$BOOTLOG" 2>&1 || true
    fi
}

restore_boot_alpha()
{
    if [ "$BOOT_ALPHA_ACTIVE" = "1" ] && [ -x "$HIFBALPHA" ]; then
        "$LOADER" --library-path "$LIBPATH" "$HIFBALPHA" restore /dev/fb0 "$BOOT_ALPHA_SAVE" >>"$BOOTLOG" 2>&1 || true
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
    log "Firmware manager interrupted"
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
    log "Starting exclusive compatibility video pre-init"
    log_hifb_state "BEFORE VIDEO PREINIT"

    "$ORIGINAL" >"$MONLOG" 2>&1 &
    MONPID=$!
    log "Compatibility Monitor PID=$MONPID"

    sleep 3

    if ! kill -0 "$MONPID" 2>/dev/null; then
        log "ERROR: compatibility Monitor exited during pre-init"
        MONPID=
        return 1
    fi

    kill -STOP "$MONPID" 2>/dev/null || {
        log "ERROR: cannot STOP compatibility Monitor"
        return 1
    }

    sleep 1
    log "Compatibility video pre-init complete; Monitor stopped PID=$MONPID"
    log_hifb_state "AFTER VIDEO PREINIT"

    "$LOADER" --library-path "$LIBPATH" "$BOOTSCREEN" ready 1500 >>"$BOOTLOG" 2>&1 || true
    return 0
}

probe_external_os()
{
    [ -x "$OSPROBE" ] || {
        log "OS probe helper not installed; continuing legacy Desktop"
        return 1
    }

    log "Scanning external storage for Stayplaytion OS marker"
    "$OSPROBE" >>"$BOOTLOG" 2>&1
    RC=$?
    if [ "$RC" -eq 0 ]; then
        log "External Stayplaytion OS detected (probe-only stage; not switching root yet)"
        return 0
    fi
    log "No external Stayplaytion OS detected; using bundled Desktop"
    return 1
}

[ -x "$ORIGINAL" ] || { log "ERROR: original Monitor missing"; exit 50; }
[ -x "$DESKTOP" ] || { log "ERROR: desktop launcher missing"; rescue; }
[ -x "$BOOTSCREEN" ] || { log "ERROR: firmware POST helper missing"; rescue; }

log "Stayplaytion Stage6.8.0 Firmware POST Prototype"
log "Safety: SPI/U-Boot remain untouched"

if [ -x "$HIFBALPHA" ]; then
    "$LOADER" --library-path "$LIBPATH" "$HIFBALPHA" restore-if-saved /dev/fb0 "$BOOT_ALPHA_SAVE" >>"$BOOTLOG" 2>&1 || true
    if "$LOADER" --library-path "$LIBPATH" "$HIFBALPHA" push-opaque /dev/fb0 "$BOOT_ALPHA_SAVE" >>"$BOOTLOG" 2>&1; then
        BOOT_ALPHA_ACTIVE=1
        log "Firmware POST framebuffer alpha forced opaque"
    fi
fi

log_hifb_state "FIRMWARE POST"
"$LOADER" --library-path "$LIBPATH" "$BOOTSCREEN" menu 7000 >>"$BOOTLOG" 2>&1
MENU_RC=$?
log "Firmware POST exited rc=$MENU_RC"

case "$MENU_RC" in
    2) log "F2 requested rescue Monitor"; rescue ;;
    0) ;;
    *) log "Firmware preflight failed rc=$MENU_RC"; rescue ;;
esac

log "Releasing firmware POST framebuffer before compatibility video pre-init"
restore_boot_alpha

if ! start_monitor_preinit; then
    log "Compatibility video pre-init failed"
    rescue
fi

if ! discover_input; then
    log "Input discovery failed after video pre-init"
    rescue
fi

probe_external_os || true

cd "$BASE" || { log "ERROR: cannot enter $BASE"; rescue; }

log "Starting bundled Desktop (Stage6.8.0 probe-only handoff)"
H3531_LXDE_SECONDS=0 "$DESKTOP" >>"$BOOTLOG" 2>&1
RC=$?
log "Desktop exited rc=$RC; resuming rescue Monitor"
rescue
