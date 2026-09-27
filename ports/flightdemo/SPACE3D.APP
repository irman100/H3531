#!/bin/sh
APP_RUN=/mnt/usb/H3531/APPS/h3531-app-run
[ -x "$APP_RUN" ] || { echo "ERROR: missing application runner: $APP_RUN"; exit 10; }
exec "$APP_RUN" space3d window
