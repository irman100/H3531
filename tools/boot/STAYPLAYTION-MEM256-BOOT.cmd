# Stayplaytion balanced 512 MiB memory profile
# AHB70XXT16-3531 physical map:
#   Linux: bank0 0x80000000..0x8fffffff = 256 MiB
#   MMZ:   bank1 0xC0000000..0xcfffffff = 256 MiB
# Safe removable-media/RAM boot only. No saveenv. No SPI writes.

echo ========================================
echo STAYPLAYTION - BALANCED MEMORY 256/256
echo ========================================
echo Loading Linux kernel...
fatload usb 0:1 0x82000000 zImage.img
echo Loading MEM256 RAMDisk...
fatload usb 0:1 0x83000000 H3531-MEM256.IMG
setenv initrd_high 0xffffffff
setenv bootargs mem=256M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)
echo Linux RAM: 256 MiB - MMZ: 256 MiB
echo Starting Stayplaytion MEM256...
bootm 0x82000000 0x83000000
