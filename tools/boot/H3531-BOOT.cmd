# H3531 Home Computer stable RAM boot script
# Build to H3531.SCR with tools/boot/make_uboot_script.py
# Files expected in FAT root:
#   zImage.img
#   H3531.IMG
#   H3531.SCR
#
# IMPORTANT: intentionally does not call saveenv and does not write flash.

echo ========================================
echo H3531 HOME COMPUTER - RAM BOOT
echo ========================================
echo Loading Linux kernel...
fatload usb 0:1 0x82000000 zImage.img
echo Loading current H3531 RAMDisk...
fatload usb 0:1 0x83000000 H3531.IMG
setenv initrd_high 0xffffffff
setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)
echo Starting H3531 Home Computer...
bootm 0x82000000 0x83000000
