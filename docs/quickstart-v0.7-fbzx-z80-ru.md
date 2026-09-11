# H3531 Home Computer - быстрый запуск USB Kit

## Важно

Не используйте `saveenv` и не прошивайте экспериментальные образы в SPI Flash. Этот набор рассчитан на безопасный запуск из USB/RAM.

## Что положить на USB-флешку

В корень FAT32-флешки скопируйте:

```text
H3531.IMG
zImage.img
h3531-video-init
h3531-input-init
FBZX.APP
keymap.bmp
spectrum-roms/
Games/
```

В `spectrum-roms/` нужно положить свои ROM-файлы:

```text
spectrum-roms/48.rom
spectrum-roms/if1-2.rom
```

ROM-файлы и игры не включены в архив по юридическим причинам.

В `Games/` положите тестовый файл ZX Spectrum snapshot:

```text
Games/game.z80
```

## Ручная USB/RAM загрузка через U-Boot

```text
usb start
fatload usb 0:1 0x82000000 zImage.img
fatload usb 0:1 0x83000000 H3531.IMG
setenv initrd_high 0xffffffff
setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)
bootm 0x82000000 0x83000000
```

Не выполняйте `saveenv`.

## Проверка после загрузки

1. Дождитесь запуска H3531 Monitor.
2. Откройте FILES.
3. Перейдите в папку `Games/`.
4. Выберите файл `*.z80`.
5. Нажмите Enter.
6. Monitor должен передать управление `FBZX.APP`, и snapshot должен запуститься.

## Текущее состояние

Подтвержден запуск именно `.Z80` из FILES. `.TAP/.TZX`, автозагрузка кассет, звук и другие эмуляторы - перспективные направления.
