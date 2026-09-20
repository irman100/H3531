#!/bin/sh
# Stage6.6A one-time USB installer.
# It never writes SPI, U-Boot environment, kernel or rootfs.

SYS=/mnt/usb/H3531/SYSTEM
ACTIVE="$SYS/MONITOR.APP"
FALLBACK="$SYS/MONITOR.ORIGINAL.APP"
STAGE66A="$SYS/STAGE66A-DESKTOP.APP"
LOG=/var/h3531-stage66a-install.log

: >"$LOG"

say() {
    echo "$*"
    echo "$*" >>"$LOG"
}

is_stage66a_supervisor() {
    file="$1"
    [ -f "$file" ] || return 1
    while IFS= read -r line; do
        case "$line" in
            *"H3531 Stage6.6A persistent desktop supervisor."*)
                return 0
                ;;
        esac
    done <"$file"
    return 1
}

say "H3531 Stage6.6A install"

[ -x "$STAGE66A" ] || {
    say "ERROR: missing $STAGE66A"
    exit 10
}

if [ ! -f "$FALLBACK" ]; then
    [ -f "$ACTIVE" ] || {
        say "ERROR: current MONITOR.APP is missing; refusing activation"
        exit 11
    }

    # Never manufacture a fallback from the Stage6.6A supervisor itself.
    # Use only ash built-ins here: the vendor rootfs does not provide every
    # standard utility as a standalone command.
    if is_stage66a_supervisor "$ACTIVE"; then
        say "ERROR: Stage6.6A already occupies MONITOR.APP but no fallback exists"
        exit 12
    fi

    say "Saving current MONITOR.APP as MONITOR.ORIGINAL.APP"
    cp "$ACTIVE" "$FALLBACK" || exit 13
    chmod 755 "$FALLBACK" 2>/dev/null || true
fi

[ -x "$FALLBACK" ] || {
    say "ERROR: fallback Monitor is not executable"
    exit 14
}

cp "$STAGE66A" "$ACTIVE" || exit 15
chmod 755 "$ACTIVE" || exit 16

# Some H3531 images do not expose a standalone 'sync' command. If the BusyBox
# multi-call binary provides the applet, use it; otherwise installation remains
# valid and the normal filesystem/reboot path will flush writes.
if [ -x /bin/busybox ]; then
    /bin/busybox sync >/dev/null 2>&1 || true
fi

say "Stage6.6A activated."
say "Reboot to enter the persistent desktop."
exit 0
