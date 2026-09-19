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

echo "Using vendor DHCP client: $UDHCPC"
echo "===== DHCP client output ====="

# The vendor rootfs has no tee(1). Redirect to a writable /var log first,
# then print it back to UART. Keeping udhcpc out of a broken pipe is
# important: otherwise it may terminate before sending a DHCP DISCOVER.
"$UDHCPC" -i "$IFACE" -q -n >/var/h3531-dhcp.log 2>&1
rc=$?
cat /var/h3531-dhcp.log 2>&1 || true

echo
echo "===== after DHCP attempt ====="
ifconfig "$IFACE" 2>&1 || true

echo
echo "===== kernel route table ====="
cat /proc/net/route 2>&1 || true

echo
echo "===== resolver ====="
cat /etc/resolv.conf 2>&1 || true

exit $rc
