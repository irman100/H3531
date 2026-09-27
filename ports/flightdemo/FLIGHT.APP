#!/bin/sh
# Stayplaytion Flight Core native framebuffer launcher.
RUN=/mnt/usb/H3531/APPS/h3531-native-run
BIN=/mnt/usb/H3531/APPS/flightdemo/FLIGHT.BIN

[ -x "$RUN" ] || { echo "ERROR: missing native lease bridge: $RUN"; exit 10; }
[ -x "$BIN" ] || { echo "ERROR: missing flight binary: $BIN"; exit 11; }

exec "$RUN" flightdemo "$BIN"
