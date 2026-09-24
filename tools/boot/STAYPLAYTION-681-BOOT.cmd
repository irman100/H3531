# Stayplaytion Stage6.8.1 external-root handoff boot
# Safe removable-media test: no saveenv and no SPI writes.
#
# FAT boot device files:
#   zImage.img
#   STAYFW.IMG
#   STAY681.SCR
#
# A second USB/SSD should contain STAYOS-TEST.img written as a raw whole-device
# squashfs image, or an equivalent compatible Stayplaytion OS squashfs root.

echo ========================================
echo STAYPLAYTION FIRMWARE 6.8.1
echo EXTERNAL ROOT HANDOFF TEST
echo ========================================

usb start
echo Loading Linux kernel...
fatload usb 0:1 0x82000000 zImage.img
echo Loading Stayplaytion firmware RAM disk...
fatload usb 0:1 0x83000000 STAYFW.IMG

setenv initrd_high 0xffffffff
setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro init=/stayfw-init mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)

echo Starting Stayplaytion firmware PID1...
bootm 0x82000000 0x83000000
