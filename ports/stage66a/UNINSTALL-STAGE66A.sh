#!/bin/sh
# Restore the pre-Stage6.6A Monitor without touching SPI/rootfs.

SYS=/mnt/usb/H3531/SYSTEM
ACTIVE="$SYS/MONITOR.APP"
FALLBACK="$SYS/MONITOR.ORIGINAL.APP"
LOG=/var/h3531-stage66a-uninstall.log

: >"$LOG"

say() {
    echo "$*"
    echo "$*" >>"$LOG"
}

say "H3531 Stage6.6A uninstall"

[ -x "$FALLBACK" ] || {
    say "ERROR: $FALLBACK is missing or not executable"
    exit 20
}

cp "$FALLBACK" "$ACTIVE" || exit 21
chmod 755 "$ACTIVE" || exit 22

if [ -x /bin/busybox ]; then
    /bin/busybox sync >/dev/null 2>&1 || true
fi

say "Original Monitor restored."
say "Reboot to return to the previous shell."
exit 0
