# H3531 Native Racer — Vice City world experiment

**Статус: experimental / work in progress.** Это не полный порт reVC и пока не законченная играбельная Vice City.

Исходники и инструменты: [ports/racer в development branch](https://github.com/irman100/H3531/tree/feature/h3531-stage68-desktop-input-polish/ports/racer).

## Цель

Racer — отдельный лёгкий нативный 3D-движок для HiSilicon Hi3531. Вместо переноса всего reVC целиком в нём реализуются только необходимые механизмы: импорт мира, object LOD, streaming, collision, камера, транспорт и управление. Ориентир — поведение оригинальной Vice City и архитектурные решения reVC, но runtime остаётся собственным и компактным.

## Целевая платформа

- HiSilicon Hi3531, dual Cortex-A9 / ARMv7.
- Linux 3.0.8.
- HIFB 1280×720, 16-bit.
- Внутренний render 640×360 с точным увеличением 2×.
- CPU software rasterizer и dual-core разделение работы.
- ARM softfp / VFPv3-d16; NEON не включать без отдельного подтверждения аппаратной поддержки.

Обычного 3D GPU в этой платформе не предполагается: текстурированные треугольники и Z-buffer — задача CPU.

## Локальный импорт Vice City

Импортёр читает ресурсы из локальной установки игры и из уже распакованной папки ресурсов. Типовые источники:

- default.dat, gta_vc.dat;
- IDE/IPL — модели, экземпляры и world transforms;
- DFF — геометрия и atomic LOD;
- TXD/TXDP — текстуры и наследование словарей;
- COL, включая models/coll;
- gta3.img и/или извлечённые файлы из models/gta3;
- handling.cfg, surface.dat;
- локальные SFX и radio source files для audio importer.

World pack использует локальные данные пользователя. Vehicle importer отдельно проверяет оригинальные игровые ресурсы, чтобы не подменять stock Oceanic модифицированной моделью.

### Сборка мира на Windows

Пример для стандартных локальных путей:

    cd /d E:/H3531/SYSTEM/tools/vc-import
    BUILD_VC_FULL_WORLD.cmd "E:/Games/GTA Vice City" "E:/Games/GTA Vice City/models/gta3"
    py -3 vc_full_pack_local_audit.py --pack-dir build/vc-full-pack
    DEPLOY_VC_FULL_WORLD.cmd

Если менялись только RACER.BIN, управление или аудио runtime, полный rebuild мира обычно не нужен. Пересобирать страницы следует при изменении импортёра, формата world pack или самих локальных игровых ресурсов.

### Локальные автомобильные и аудиоресурсы

Оригинальный Oceanic и его handling импортируются из локальной установки игры. Локальные звуковые данные собираются отдельно:

    cd /d E:/H3531/SYSTEM/tools/vc-import
    BUILD_VC_OCEANIC_ONLY.cmd "E:/Games/GTA Vice City"
    BUILD_VC_AUDIO.cmd "E:/Games/GTA Vice City"

Сборка аудио может создавать PCM-файлы для двигателя, шин, приземления, ударов и доступных радиостанций. Наличие файлов и успешный CI не доказывают, что весь микс уже принят на реальном устройстве.

## World streaming и LOD

Текущая архитектура использует:

- VFW1 — paged world / streaming;
- VCO2 — object metadata и связь треугольников с объектами/atomic LOD;
- gta-object-lod-v4 — выбор одного LOD целого объекта по исходным дистанциям;
- локальные страницы геометрии и collision;
- загрузку областей вокруг автомобиля и освобождение ненужных областей.

Старый triangle-distance fade не используется как основной механизм LOD. LOD должен переключать целый объект между его штатными atomic tiers.

## Рендер и производительность

Уже реализованы в development branch:

- object-level frustum culling до transform;
- кеши преобразования и screen projection общих вершин;
- dual-A9 adaptive raster split;
- scanline-span raster вместо перебора всего bounding box;
- аппаратный TDE для финального 2× scaling/present, не для 3D rasterization.

В плотных городских сценах CPU raster остаётся существенным bottleneck. Нельзя считать фиксированное разрешение, дальность прорисовки или физику уже окончательно оптимизированными.

## Автомобильная физика

Импортируются оригинальные характеристики из handling.cfg и матрица сцепления поверхностей из surface.dat. Runtime использует четыре suspension/wheel contacts, отдельные продольные и боковые tyre forces, wheel states, steering, braking и transmission logic.

При этом физика и управление автомобиля остаются незавершёнными. На аппаратных тестах возникали проблемы с потерей контактов, подъёмом кузова и тягой. Поэтому README не заявляет, что автомобиль сейчас стабильно управляется или что Racer уже готов к полноценной игре. Любые дальнейшие изменения нужно проверять по реальному /var/h3531-native-app.log, а не только по QEMU selftest.

## Аудио

Audio backend использует штатный HiSilicon AO transport, 48 kHz, signed 16-bit mono PCM. Импортёр может локально получить SFX и радиоданные из пользовательской установки Vice City. Ни оригинальные Rockstar audio files, ни готовые PCM-файлы не должны попадать в GitHub или CI artifacts.

## Запуск и диагностика на устройстве

Runtime находится в:

- /mnt/usb/H3531/APPS/racer/RACER.APP
- /mnt/usb/H3531/APPS/racer/RACER.BIN

Основной лог:

    cat /var/h3531-native-app.log

Для аппаратной диагностики полезны PERF, VC_WHEELS, VC_DYN, PAD_KEY, audio startup и streaming markers. QEMU/ARM CI проверяет сборку и выбранные регрессии, но не заменяет поездку на реальной плате.

## Авторские права и локальные данные

GTA Vice City и её игровые ресурсы принадлежат правообладателям. Этот репозиторий не распространяет DFF, TXD, COL, IMG, игровые аудиофайлы, музыку или готовый мир. Пользователь должен самостоятельно предоставить локальную копию игры и запускать импортёр у себя. Проект независимый и не связан с Rockstar Games.
