# Stayplaytion Firmware removable-media development boot
# Stage6.8 prototype. This script never writes U-Boot environment or SPI flash.
#
# Files expected in FAT root:
#   zImage.img
#   H3531.IMG
#   STAY68.SCR
#
# H3531.IMG remains the current firmware-development RAM disk for Stage6.8.0.

echo ========================================
echo STAYPLAYTION FIRMWARE DEVELOPMENT BOOT
echo ========================================
echo Loading firmware kernel...
fatload usb 0:1 0x82000000 zImage.img
echo Loading firmware RAM disk...
fatload usb 0:1 0x83000000 H3531.IMG
setenv initrd_high 0xffffffff
setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)
echo Starting Stayplaytion Firmware...
bootm 0x82000000 0x83000000
