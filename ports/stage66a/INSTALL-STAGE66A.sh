#!/bin/sh
# Stage6.6A one-time USB installer.
# It never writes SPI, U-Boot environment, kernel or rootfs.

SYS=/mnt/usb/H3531/SYSTEM
ACTIVE="$SYS/MONITOR.APP"
FALLBACK="$SYS/MONITOR.ORIGINAL.APP"
STAGE66A="$SYS/STAGE66A-DESKTOP.APP"
NETCFG="$SYS/NETWORK.CFG"
NETDEFAULT="$SYS/NETWORK.CFG.DEFAULT"
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

# Create a device-local network config once. Updates never overwrite it.
if [ ! -f "$NETCFG" ]; then
    if [ -f "$NETDEFAULT" ]; then
        cp "$NETDEFAULT" "$NETCFG" || exit 17
    else
        cat >"$NETCFG" <<EOF
MODE=dhcp
INTERFACE=eth0
EOF
    fi
    say "Created persistent network config: $NETCFG"
else
    say "Keeping existing network config: $NETCFG"
fi

# Clear diagnostic emergency-disable marker when the user explicitly installs.
rm -f "$SYS/DESKTOP.DISABLED"

cp "$STAGE66A" "$ACTIVE" || exit 15
chmod 755 "$ACTIVE" || exit 16

if [ -x /bin/busybox ]; then
    /bin/busybox sync >/dev/null 2>&1 || true
fi

say "Stage6.6A activated."
say "Network mode: DHCP on eth0 (persistent policy, fresh lease each boot)."
say "Reboot to enter the persistent desktop."
exit 0
