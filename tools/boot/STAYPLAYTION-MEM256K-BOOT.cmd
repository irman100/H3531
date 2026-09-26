# Stayplaytion MEM256K manual U-Boot sequence
# Safe RAM/removable-media boot. No saveenv. No SPI writes.
usb start
fatload usb 0:1 0x82000000 zImage.img
fatload usb 0:1 0x83000000 H3531-MEM256K.IMG
setenv initrd_high 0xffffffff
setenv bootargs mem=256M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)
bootm 0x82000000 0x83000000
