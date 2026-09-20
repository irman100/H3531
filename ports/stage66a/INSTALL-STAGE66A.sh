#!/bin/sh
# Stage6.6A one-time USB installer.
# It never writes SPI, U-Boot environment, kernel or rootfs.

SYS=/mnt/usb/H3531/SYSTEM
ACTIVE="$SYS/MONITOR.APP"
FALLBACK="$SYS/MONITOR.ORIGINAL.APP"
STAGE66A="$SYS/STAGE66A-DESKTOP.APP"
LOG=/var/h3531-stage66a-install.log

echo "H3531 Stage6.6A install" >"$LOG"

[ -x "$STAGE66A" ] || {
    echo "ERROR: missing $STAGE66A" | tee -a "$LOG"
    exit 10
}

if [ ! -f "$FALLBACK" ]; then
    [ -f "$ACTIVE" ] || {
        echo "ERROR: current MONITOR.APP is missing; refusing activation" | tee -a "$LOG"
        exit 11
    }

    # Never manufacture a fallback from the Stage6.6A supervisor itself.
    if cmp -s "$ACTIVE" "$STAGE66A" 2>/dev/null; then
        echo "ERROR: Stage6.6A already occupies MONITOR.APP but no fallback exists" | tee -a "$LOG"
        exit 12
    fi

    echo "Saving current MONITOR.APP as MONITOR.ORIGINAL.APP" | tee -a "$LOG"
    cp -p "$ACTIVE" "$FALLBACK" || exit 13
    chmod 755 "$FALLBACK" 2>/dev/null || true
fi

[ -x "$FALLBACK" ] || {
    echo "ERROR: fallback Monitor is not executable" | tee -a "$LOG"
    exit 14
}

cp "$STAGE66A" "$ACTIVE" || exit 15
chmod 755 "$ACTIVE" || exit 16
sync

echo "Stage6.6A activated." | tee -a "$LOG"
echo "Reboot to enter the persistent desktop." | tee -a "$LOG"
exit 0
