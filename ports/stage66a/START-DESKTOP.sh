#!/bin/sh
# H3531 Stage6.6A canonical persistent desktop entry point.
# Network bootstrap lives here deliberately: whether the desktop is started by
# the boot supervisor or manually for diagnostics, Ethernet is configured first.

BASE=/mnt/usb/H3531/APPS/x11-debian
SYS=/mnt/usb/H3531/SYSTEM
DHCP="$BASE/bin/h3531-net-dhcp"
NETCFG="$SYS/NETWORK.CFG"
NETLOG=/var/h3531-desktop-network.log

IFACE=eth0
MODE=dhcp

if [ -f "$NETCFG" ]; then
    while IFS='=' read -r key value; do
        case "$key" in
            MODE) MODE="$value" ;;
            INTERFACE) IFACE="$value" ;;
        esac
    done <"$NETCFG"
fi

network_has_default_route()
{
    while read iface destination gateway rest; do
        if [ "$iface" = "$IFACE" ] && [ "$destination" = "00000000" ]; then
            return 0
        fi
    done </proc/net/route
    return 1
}

: >"$NETLOG"
echo "Stage6.6A network bootstrap mode=$MODE interface=$IFACE" >>"$NETLOG"

case "$MODE" in
    off)
        echo "Network bootstrap disabled by NETWORK.CFG" >>"$NETLOG"
        ;;
    dhcp|"")
        if network_has_default_route; then
            echo "Default route already present; keeping current network state" >>"$NETLOG"
        elif [ -x "$DHCP" ]; then
            N=1
            while [ "$N" -le 3 ]; do
                echo "DHCP attempt=$N" >>"$NETLOG"
                "$DHCP" "$IFACE" >>"$NETLOG" 2>&1
                RC=$?
                if [ "$RC" -eq 0 ] && network_has_default_route; then
                    echo "DHCP OK" >>"$NETLOG"
                    break
                fi
                echo "DHCP failed rc=$RC" >>"$NETLOG"
                N=`expr "$N" + 1`
                sleep 2
            done
        else
            echo "WARNING: DHCP helper missing: $DHCP" >>"$NETLOG"
        fi
        ;;
    *)
        echo "WARNING: unsupported network mode '$MODE'; desktop will continue" >>"$NETLOG"
        ;;
esac

export H3531_X11_FULLFRAME_FB=1
exec "$BASE/libexec/h3531-lxde-core" "$@"
