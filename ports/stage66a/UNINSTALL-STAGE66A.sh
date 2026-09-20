#!/bin/sh
# Restore the pre-Stage6.6A Monitor without touching SPI/rootfs.

SYS=/mnt/usb/H3531/SYSTEM
ACTIVE="$SYS/MONITOR.APP"
FALLBACK="$SYS/MONITOR.ORIGINAL.APP"
LOG=/var/h3531-stage66a-uninstall.log

echo "H3531 Stage6.6A uninstall" >"$LOG"

[ -x "$FALLBACK" ] || {
    echo "ERROR: $FALLBACK is missing or not executable" | tee -a "$LOG"
    exit 20
}

cp "$FALLBACK" "$ACTIVE" || exit 21
chmod 755 "$ACTIVE" || exit 22
sync

echo "Original Monitor restored." | tee -a "$LOG"
echo "Reboot to return to the previous shell." | tee -a "$LOG"
exit 0
