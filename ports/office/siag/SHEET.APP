#!/bin/sh
# Stayplaytion / H3531 Siag Office - spreadsheet launcher.
BASE=/mnt/usb/H3531/APPS/x11-debian
OFFICE=/mnt/usb/H3531/APPS/siag
LOADER="$BASE/lib/ld-linux.so.3"
LIBPATH="$OFFICE/lib:$BASE/lib:$BASE/lib/arm-linux-gnueabi:$BASE/lib/arm-linux-gnueabi/lxpanel"
LOG=/var/h3531-siag-sheet.log

export DISPLAY=127.0.0.1:0
export HOME=/var/h3531-lxde
export XAPPLRESDIR="$OFFICE/lib/X11/app-defaults"

PROG=
for P in "$OFFICE/bin/siag" "$OFFICE/bin/xsiag"; do
    if [ -x "$P" ]; then PROG="$P"; break; fi
done

[ -n "$PROG" ] || {
    echo "ERROR: Siag spreadsheet binary not found" >"$LOG"
    exit 2
}
[ -x "$LOADER" ] || {
    echo "ERROR: X11 loader missing: $LOADER" >"$LOG"
    exit 3
}

echo "Stayplaytion Office Spreadsheet" >"$LOG"
echo "program=$PROG display=$DISPLAY" >>"$LOG"

if [ -n "$1" ]; then
    exec "$LOADER" --library-path "$LIBPATH" "$PROG" "$1" >>"$LOG" 2>&1
else
    exec "$LOADER" --library-path "$LIBPATH" "$PROG" >>"$LOG" 2>&1
fi
