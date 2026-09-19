#!/bin/sh
BASE=/mnt/usb/H3531/APPS/x11-debian
PATH="$BASE/bin:/bin:/sbin:/usr/bin:/usr/sbin"
export PATH

IFACE="$1"
if [ -z "$IFACE" ]; then
  for p in /sys/class/net/*; do
    [ -e "$p" ] || continue
    n="${p##*/}"
    [ "$n" = "lo" ] && continue
    IFACE="$n"
    break
  done
fi

[ -n "$IFACE" ] || {
  echo "ERROR: no non-loopback interface found"
  exit 2
}

echo "H3531 manual DHCP test on interface: $IFACE"
echo "This changes runtime network state only; it does not write bootloader/SPI settings."

ifconfig "$IFACE" up 2>&1 || {
  echo "ERROR: cannot bring $IFACE up"
  exit 3
}

UDHCPC=
for p in /sbin/udhcpc /bin/udhcpc /usr/sbin/udhcpc /usr/bin/udhcpc; do
  if [ -x "$p" ]; then UDHCPC="$p"; break; fi
done

if [ -z "$UDHCPC" ]; then
  echo "No vendor udhcpc client was found."
  echo "Interface was only brought UP. Run h3531-net-info to inspect it."
  exit 4
fi

ACTION="$BASE/bin/h3531-udhcpc-action"
if [ ! -x "$ACTION" ]; then
  echo "ERROR: DHCP action helper missing: $ACTION"
  exit 5
fi

echo "Using vendor DHCP client: $UDHCPC"
echo "Using H3531 DHCP action helper: $ACTION"
echo "===== DHCP client output ====="

rm -f /var/h3531-dhcp.log /var/h3531-udhcpc-action.log
"$UDHCPC" -i "$IFACE" -q -n -s "$ACTION" >/var/h3531-dhcp.log 2>&1
rc=$?
cat /var/h3531-dhcp.log 2>&1 || true

echo
echo "===== DHCP action log ====="
cat /var/h3531-udhcpc-action.log 2>&1 || true

echo
echo "===== after DHCP attempt ====="
ifconfig "$IFACE" 2>&1 || true

echo
echo "===== kernel route table ====="
cat /proc/net/route 2>&1 || true

echo
echo "===== resolver ====="
cat /etc/resolv.conf 2>&1 || true

# Verify that a real default route exists. DHCP may assign an IPv4 address
# successfully while a broken vendor action script omits the gateway.
default_route=0
while read iface destination rest; do
  [ "$destination" = "00000000" ] && default_route=1
done </proc/net/route

if [ "$rc" -eq 0 ] && [ "$default_route" -ne 1 ]; then
  echo "ERROR: DHCP returned success but no default route was installed."
  exit 6
fi

exit "$rc"
