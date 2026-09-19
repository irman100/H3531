#!/bin/sh
# BusyBox udhcpc action helper for the H3531 vendor rootfs.
# The stock script uses "broadcast +" with this old ifconfig. On this board
# that prints "+: Unknown host" and stops before installing the default route.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

IFCONFIG=/sbin/ifconfig
ROUTE=/sbin/route
[ -x "$IFCONFIG" ] || IFCONFIG=ifconfig
[ -x "$ROUTE" ] || ROUTE=route

LOG=/var/h3531-udhcpc-action.log
EVENT="$1"

{
  echo "event=$EVENT interface=$interface ip=$ip subnet=$subnet broadcast=$broadcast"
  echo "router=$router"
  echo "dns=$dns"
} >>"$LOG"

case "$EVENT" in
  deconfig)
    "$IFCONFIG" "$interface" 0.0.0.0 >>"$LOG" 2>&1 || true
    ;;

  bound|renew)
    if [ -n "$subnet" ]; then
      "$IFCONFIG" "$interface" "$ip" netmask "$subnet" >>"$LOG" 2>&1 || exit 10
    else
      "$IFCONFIG" "$interface" "$ip" >>"$LOG" 2>&1 || exit 10
    fi

    # Use the DHCP-provided broadcast only when it is a real address.
    # Never use the incompatible legacy "broadcast +" form.
    if [ -n "$broadcast" ]; then
      "$IFCONFIG" "$interface" broadcast "$broadcast" >>"$LOG" 2>&1 || true
    fi

    # Remove stale default routes on this interface, then install the router
    # option supplied by DHCP.
    "$ROUTE" del default dev "$interface" >>"$LOG" 2>&1 || true

    route_added=0
    for gw in $router; do
      if "$ROUTE" add default gw "$gw" dev "$interface" >>"$LOG" 2>&1; then
        echo "default-route=$gw interface=$interface" >>"$LOG"
        route_added=1
        break
      fi
    done

    if [ "$route_added" -ne 1 ]; then
      echo "ERROR: DHCP supplied no usable default gateway" >>"$LOG"
      exit 11
    fi

    if [ -n "$dns" ]; then
      : >/etc/resolv.conf
      for ns in $dns; do
        echo "nameserver $ns" >>/etc/resolv.conf
      done
    fi
    ;;

  *)
    echo "Ignoring udhcpc event: $EVENT" >>"$LOG"
    ;;
esac

exit 0
