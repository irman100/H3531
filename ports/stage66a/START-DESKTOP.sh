#!/bin/sh
# H3531 Stage6.6A canonical persistent desktop entry point.
# Network bootstrap lives here deliberately: whether the desktop is started by
# the boot supervisor or manually for diagnostics, Ethernet is configured first.

BASE=/mnt/usb/H3531/APPS/x11-debian
SYS=/mnt/usb/H3531/SYSTEM
DHCP="$BASE/bin/h3531-net-dhcp"
NETCFG="$SYS/NETWORK.CFG"
NETLOG=/var/h3531-desktop-network.log
RTCSYNC="$BASE/bin/h3531-rtc-sync"
RTCLOG=/var/h3531-rtc.log
RTCPID=/var/h3531-rtc-sync.pid
TZCFG="$SYS/TIMEZONE.CFG"

# Stage6.8.0T: keep CLOCK_REALTIME/RTC in UTC, but present local civil time.
# Default local zone is fixed UTC+3. POSIX TZ signs are reversed, so MSK-3 = UTC+3.
TZ_VALUE='MSK-3'
if [ -f "$TZCFG" ]; then
    while IFS='=' read -r key value; do
        case "$key" in
            TZ) [ -n "$value" ] && TZ_VALUE="$value" ;;
        esac
    done <"$TZCFG"
fi
export TZ="$TZ_VALUE"

# Stage6.8.0P: battery-backed RTC integration.
# The frontend reads normal CLOCK_REALTIME; synchronize Linux from RTC before
# drawing the UI, then keep a lightweight watcher so later NTP/manual clock
# corrections are persisted back into the battery clock.
if [ -x "$RTCSYNC" ]; then
    : >"$RTCLOG"
    "$RTCSYNC" load >>"$RTCLOG" 2>&1 || true

    RTC_WATCH_ACTIVE=0
    if [ -r "$RTCPID" ]; then
        OLD_RTC_PID="$(cat "$RTCPID" 2>/dev/null)"
        if [ -n "$OLD_RTC_PID" ] && kill -0 "$OLD_RTC_PID" 2>/dev/null; then
            RTC_WATCH_ACTIVE=1
            echo "[RTC] existing watch pid=$OLD_RTC_PID" >>"$RTCLOG"
        else
            rm -f "$RTCPID" 2>/dev/null
        fi
    fi

    if [ "$RTC_WATCH_ACTIVE" != "1" ]; then
        "$RTCSYNC" watch >>"$RTCLOG" 2>&1 &
        echo "$!" >"$RTCPID"
        echo "[RTC] watch started pid=$!" >>"$RTCLOG"
    fi
else
    : >"$RTCLOG"
    echo "[RTC] helper missing: $RTCSYNC" >>"$RTCLOG"
fi

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

network_bootstrap()
{
    case "$MODE" in
        off)
            echo "Network bootstrap disabled by NETWORK.CFG" >>"$NETLOG"
            ;;
        dhcp|"")
            if network_has_default_route; then
                echo "Default route already present; keeping current network state" >>"$NETLOG"
            elif [ -x "$DHCP" ]; then
                DHCP_OK=0
                for LABEL in first second third; do
                    echo "DHCP attempt=$LABEL" >>"$NETLOG"
                    "$DHCP" "$IFACE" >>"$NETLOG" 2>&1
                    RC=$?
                    if [ "$RC" -eq 0 ] && network_has_default_route; then
                        echo "DHCP OK" >>"$NETLOG"
                        DHCP_OK=1
                        break
                    fi
                    echo "DHCP failed rc=$RC" >>"$NETLOG"
                    sleep 2
                done
                [ "$DHCP_OK" = "1" ] || echo "WARNING: DHCP unavailable; desktop will continue offline" >>"$NETLOG"
            else
                echo "WARNING: DHCP helper missing: $DHCP" >>"$NETLOG"
            fi
            ;;
        *)
            echo "WARNING: unsupported network mode '$MODE'; desktop will continue" >>"$NETLOG"
            ;;
    esac

    # DHCP only configures the network; it does not set CLOCK_REALTIME.
    # Start SNTP after the network attempt so it can use a newly acquired route.
    if [ -x "$RTCSYNC" ] && [ "$MODE" != "off" ]; then
        "$RTCSYNC" net >>"$RTCLOG" 2>&1 &
        echo "[RTC] network time sync started pid=$!" >>"$RTCLOG"
    fi
}

# Stage6.8.0Z: the desktop is useful offline, so Fast Boot never waits for DHCP.
# The normal launcher keeps the historical synchronous behavior for rollback.
if [ "${H3531_FAST_BOOT:-0}" = "1" ]; then
    network_bootstrap &
    echo "Fast boot network bootstrap pid=$!" >>"$NETLOG"
else
    network_bootstrap
fi

APPSCAN="$BASE/bin/h3531-appscan-desktop"
if [ -x "$APPSCAN" ]; then
    "$APPSCAN" >/var/h3531-appscan.log 2>&1 || {
        echo "WARNING: application scan failed; continuing desktop" >>"$NETLOG"
    }
fi

export H3531_X11_FULLFRAME_FB=1
exec "$BASE/libexec/h3531-lxde-core" "$@"
