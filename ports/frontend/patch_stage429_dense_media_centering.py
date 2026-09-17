#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage429_dense_media_centering.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.27 PixelStation coverflow-spacing aspect-fill UI active'
new_marker = 'Stage4.29 PixelStation dense-media centered-label UI active'
if old_marker not in src:
    raise SystemExit('Stage4.27 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Atari: move the ROM-art plane slightly right/down so the art is visually
# centred inside the physical recessed sticker plane. Keep strict clipping.
old_atari = '{ xp = 14; yp = 14; wp = 74; hp = 55; }'
new_atari = '{ xp = 15; yp = 16; wp = 74; hp = 54; }'
if old_atari not in src:
    raise SystemExit('Stage4.27 Atari label rect missing')
src = src.replace(old_atari, new_atari, 1)

# Media silhouette tuning from the physical photos:
# - GBA cards were visually too far apart, so side cards grow more than centre.
# - Mega Drive keeps the deliberate edge overflow, but cards grow until gaps are minimal.
# - Atari selected grows only a little; idle cards grow substantially so the side
#   cartridges no longer look miniature next to the centre card.
repls = [
    ('stage427_boost = stage427_selected_size ? 1.30 : 1.20;',
     'stage427_boost = stage427_selected_size ? 1.34 : 1.34;'),
    ('stage427_boost = stage427_selected_size ? 1.34 : 1.18;',
     'stage427_boost = stage427_selected_size ? 1.42 : 1.30;'),
    ('stage427_boost = stage427_selected_size ? 1.22 : 1.14;',
     'stage427_boost = stage427_selected_size ? 1.25 : 1.22;'),
]
for old, new in repls:
    if old not in src:
        raise SystemExit('Stage4.27 scale anchor missing: ' + old)
    src = src.replace(old, new, 1)

# Carousel density. Sega deliberately preserves the wide/overflow composition;
# its gap reduction comes primarily from larger media. GBA and Atari are pulled
# substantially closer together. Five visible cards remain the stable model.
old = '         stage427_spacing = 1.30;'
new = '         stage427_spacing = 1.30;'
if old not in src:
    raise SystemExit('Sega spacing anchor missing')
# Keep Sega centre spacing exactly as Stage4.27; larger silhouettes close the gap.

old_gba_spacing = '         stage427_spacing = 1.16;'
new_gba_spacing = '         stage427_spacing = 1.02;'
if old_gba_spacing not in src:
    raise SystemExit('GBA spacing anchor missing')
src = src.replace(old_gba_spacing, new_gba_spacing, 1)

old_gb_spacing = '         stage427_spacing = 1.08;'
new_gb_spacing = '         stage427_spacing = 1.06;'
if old_gb_spacing not in src:
    raise SystemExit('GB spacing anchor missing')
src = src.replace(old_gb_spacing, new_gb_spacing, 1)

old_atari_spacing = '         stage427_spacing = 1.05;'
new_atari_spacing = '         stage427_spacing = 0.94;'
if old_atari_spacing not in src:
    raise SystemExit('Atari spacing anchor missing')
src = src.replace(old_atari_spacing, new_atari_spacing, 1)

# Remove the half-pixel top/left bias when an odd number of source pixels is
# discarded by aspect-fill. This keeps the crop mathematically centred.
old_crop_x = '      crop_x = (src_img->w - crop_w) / 2;'
new_crop_x = '      crop_x = (src_img->w - crop_w + 1) / 2;'
old_crop_y = '      crop_y = (src_img->h - crop_h) / 2;'
new_crop_y = '      crop_y = (src_img->h - crop_h + 1) / 2;'
if old_crop_x not in src or old_crop_y not in src:
    raise SystemExit('Stage4.27 centre-crop anchors missing')
src = src.replace(old_crop_x, new_crop_x, 1)
src = src.replace(old_crop_y, new_crop_y, 1)

# Replace Stage4.27 layout-report strings so host/QEMU tests describe the final
# values actually compiled into the binary.
old_reports = [
    'STAGE427_LABEL GBA19-33-62-46 ATARI14-14-74-55 PS1-aspect-fill-crop',
    'STAGE427_SCALE selected Atari1.30 Sega1.34 GBA1.22 idle Atari1.20 Sega1.18 GBA1.14',
    'STAGE427_SPACING Sega1.30 GBA1.16 GB1.08 Atari1.05 outer-clipping-allowed no-overlap-target',
    'STAGE427_COVER aspect-fill source-crop centered clip-to-label no-stretch',
]
new_reports = [
    'STAGE429_LABEL GBA19-33-62-46 ATARI15-16-74-54 PS1-aspect-fill-crop',
    'STAGE429_SCALE selected Atari1.34 Sega1.42 GBA1.25 idle Atari1.34 Sega1.30 GBA1.22',
    'STAGE429_SPACING Sega1.30 GBA1.02 GB1.06 Atari0.94 five-wide edge-overflow-allowed',
    'STAGE429_COVER aspect-fill true-center-crop strict-label-clip no-stretch',
]
for old, new in zip(old_reports, new_reports):
    if old not in src:
        raise SystemExit('Stage4.27 report anchor missing: ' + old)
    src = src.replace(old, new, 1)

# Add a dedicated marker before LAYOUT_TEST_OK for easy binary/runtime checks.
needle = '   printf("STAGE427_BASELINE last-visible-alpha-row common-floor preserved grow-upward\\n");\n'
insert = needle + '   printf("STAGE429_DENSITY physical-photo-tuned GBA-closer Sega-larger Atari-denser\\n");\n'
if needle not in src:
    raise SystemExit('baseline report anchor missing')
src = src.replace(needle, insert, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE429_DENSE_MEDIA_CENTERING_PATCH_OK {src_path} -> {out_path}')
