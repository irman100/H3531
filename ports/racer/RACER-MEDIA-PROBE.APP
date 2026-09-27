#!/bin/sh
# Stayplaytion Racer Stage2 - read-only HiSilicon media ABI probe.
# Does not load modules, write flash, or modify configuration.
LOG=/var/h3531-racer-media-probe.log
exec >"$LOG" 2>&1

echo "=== RACER MEDIA ABI PROBE ==="
echo "kernel:"
cat /proc/version 2>/dev/null

echo
echo "=== MEDIA DEVICE NODES ==="
for p in /dev/hi_tde /dev/tde /dev/vdec /dev/vo /dev/sys /dev/mmz_userdev /dev/fb0 /dev/fb1 /dev/fb2
do
  if [ -e "$p" ]; then
    echo "FOUND $p"
    ls -l "$p" 2>/dev/null
  fi
done

echo
echo "=== PROC MEDIA DIRECTORIES ==="
for p in /proc/umap /proc/msp /proc/media-mem /proc/fb
do
  if [ -e "$p" ]; then
    echo "--- $p"
    ls -l "$p" 2>/dev/null
    [ -f "$p" ] && cat "$p" 2>/dev/null
  fi
done

echo
echo "=== MPP PROC DETAILS ==="
for p in /proc/umap/sys /proc/umap/vdec /proc/umap/vo /proc/umap/logmpp /proc/umap/vb
do
  if [ -f "$p" ]; then
    echo "--- $p"
    cat "$p" 2>/dev/null
  fi
done

echo
echo "=== CANDIDATE USERSPACE LIBRARIES ==="
for d in   /lib /usr/lib /usr/local/lib /opt/lib /app/lib   /mnt/mtd/lib /mnt/mtd/app/lib /mnt/mtd/lib64   /mnt/nand/lib /mnt/nand/app/lib   /home/lib /root/lib   /mnt/usb/H3531/LIB /mnt/usb/H3531/SYSTEM/lib
do
  [ -d "$d" ] || continue
  for f in "$d"/libmpi* "$d"/libtde* "$d"/libhigo* "$d"/libvdec* "$d"/libvo* "$d"/libhdmi*
  do
    [ -e "$f" ] || continue
    echo "FOUND_LIB $f"
    ls -l "$f" 2>/dev/null
  done
done

echo
echo "=== CANDIDATE MEDIA BINARIES ==="
for d in   /bin /sbin /usr/bin /usr/sbin /usr/local/bin   /app /opt/bin /mnt/mtd /mnt/mtd/app /mnt/mtd/bin   /mnt/nand /mnt/nand/app /root /home
do
  [ -d "$d" ] || continue
  for f in "$d"/*vdec* "$d"/*tde* "$d"/*mpi* "$d"/*player* "$d"/*sample* "$d"/*media*
  do
    [ -e "$f" ] || continue
    echo "FOUND_BIN $f"
    ls -l "$f" 2>/dev/null
  done
done

echo
echo "=== LOADED HISILICON MEDIA MODULES ==="
cat /proc/modules 2>/dev/null | while read line
do
  case "$line" in
    *hi3531_tde*|*hi3531_vdec*|*hi3531_vou*|*hifb*|*mmz*|*hi3531_sys*|*hi3531_hdmi*)
      echo "$line"
      ;;
  esac
done

echo
echo "=== END RACER MEDIA ABI PROBE ==="
