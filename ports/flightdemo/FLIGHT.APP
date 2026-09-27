#!/bin/sh
# Manual launcher for Stayplaytion Flight Core.
# The generic app runner owns the native framebuffer lease.
APP_RUN=/mnt/usb/H3531/APPS/h3531-app-run
[ -x "$APP_RUN" ] || { echo "ERROR: missing application runner: $APP_RUN"; exit 10; }
exec "$APP_RUN" flightdemo window
