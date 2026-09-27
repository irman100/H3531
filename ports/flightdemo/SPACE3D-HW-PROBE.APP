#!/bin/sh
# Space3D hardware capability probe. Read-only: no module loads, no writes.
LOG=/var/h3531-space3d-hw-probe.log
exec >"$LOG" 2>&1

echo "=== SPACE3D HW PROBE ==="
echo "kernel:"
cat /proc/version 2>/dev/null

echo
echo "=== DEVICES ==="
for p in   /dev/tde /dev/hi_tde /dev/vdec /dev/vo /dev/sys /dev/mmz_userdev   /dev/hi3531_tde /dev/hi_tde2 /dev/hifb0 /dev/fb0
do
  if [ -e "$p" ]; then
    echo "FOUND $p"
    ls -l "$p" 2>/dev/null
  fi
done

echo
echo "=== LIBRARIES ==="
for d in /lib /usr/lib /usr/local/lib /mnt/usb/H3531/APPS/*/runtime
do
  [ -d "$d" ] || continue
  for f in "$d"/libtde* "$d"/libmpi* "$d"/libive* "$d"/libvdec* "$d"/libvo*
  do
    [ -e "$f" ] || continue
    echo "FOUND $f"
    ls -l "$f" 2>/dev/null
  done
done

echo
echo "=== MODULES ==="
cat /proc/modules 2>/dev/null

echo
echo "=== MEDIA MEMORY ==="
cat /proc/media-mem 2>/dev/null

echo
echo "=== FB ==="
cat /proc/fb 2>/dev/null

echo
echo "=== END SPACE3D HW PROBE ==="
