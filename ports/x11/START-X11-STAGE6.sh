#!/bin/sh
# H3531 Stage6.0 - Xfbdev hardware proof launcher
# Safe USB/RAM test. Does not write U-Boot env or SPI.
#
# For the first terminal-launched test, stop the currently resident Monitor
# before running this script so it cannot compete for fb0/evdev:
#   kill -STOP <MONITOR_PID>
# Then run this script. After it exits:
#   kill -CONT <MONITOR_PID>

BASE=/mnt/usb/H3531/APPS/x11
XFBDEV="$BASE/bin/Xfbdev"
PROOF="$BASE/bin/H3531-X11-PROOF.APP"
XDPYINFO="$BASE/bin/xdpyinfo"

KEYBD="\${H3531_X11_KEYBD:-/dev/input/event1}"
MOUSE="\${H3531_X11_MOUSE:-/dev/input/event0}"
LOG=/var/h3531-stage6-xfbdev.log
PROOFLOG=/var/h3531-stage6-proof.log
DPYLOG=/var/h3531-stage6-xdpyinfo.log

echo "H3531 Stage6.0 Xfbdev proof"
echo "Xfbdev: $XFBDEV"
echo "keyboard: $KEYBD"
echo "mouse: $MOUSE"
echo "server log: $LOG"
echo "proof log: $PROOFLOG"

if [ ! -x "$XFBDEV" ]; then
    echo "ERROR: Xfbdev not found/executable"
    exit 10
fi
if [ ! -x "$PROOF" ]; then
    echo "ERROR: proof client not found/executable"
    exit 11
fi
if [ ! -c /dev/fb0 ]; then
    echo "ERROR: /dev/fb0 missing"
    exit 12
fi
if [ ! -c "$KEYBD" ]; then
    echo "ERROR: keyboard event node missing: $KEYBD"
    exit 13
fi
if [ ! -c "$MOUSE" ]; then
    echo "ERROR: mouse event node missing: $MOUSE"
    exit 14
fi

mkdir -p /var/h3531-x11 /tmp/.X11-unix 2>/dev/null
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0 2>/dev/null

export HOME=/var/h3531-x11
export TMPDIR=/var/tmp
export DISPLAY=:0

"$XFBDEV" :0 \
    -screen 1280x720x16 \
    -keybd "evdev,,device=$KEYBD" \
    -mouse "evdev,,device=$MOUSE" \
    -fp "$BASE/share/fonts/X11/misc" \
    -kb \
    -ac \
    -nolisten tcp \
    -noreset \
    >"$LOG" 2>&1 &
XPID=$!

i=0
while [ $i -lt 50 ]; do
    if [ -S /tmp/.X11-unix/X0 ]; then
        break
    fi
    if ! kill -0 "$XPID" 2>/dev/null; then
        echo "ERROR: Xfbdev exited during startup"
        cat "$LOG"
        exit 20
    fi
    sleep 1
    i=$((i+1))
done

if [ ! -S /tmp/.X11-unix/X0 ]; then
    echo "ERROR: X socket did not appear"
    kill "$XPID" 2>/dev/null
    wait "$XPID" 2>/dev/null
    cat "$LOG"
    exit 21
fi

echo "Xfbdev started pid=$XPID"

if [ -x "$XDPYINFO" ]; then
    "$XDPYINFO" >"$DPYLOG" 2>&1
    echo "xdpyinfo saved: $DPYLOG"
fi

"$PROOF" >"$PROOFLOG" 2>&1
RC=$?

echo "proof client exit=$RC"
kill "$XPID" 2>/dev/null
wait "$XPID" 2>/dev/null

echo "----- XFBDEV LOG -----"
cat "$LOG"
echo "----- PROOF LOG -----"
cat "$PROOFLOG"
echo "H3531 Stage6.0 proof finished"
exit "$RC"
