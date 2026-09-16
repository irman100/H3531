#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage417_layout_sony_case.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.16 PixelStation user-media-assets UI active'
new_marker = 'Stage4.17 PixelStation denser-media Sony-case UI active'
if old_marker not in src:
    raise SystemExit('Stage4.16 marker not found')
src = src.replace(old_marker, new_marker, 1)

for name, value in {
    'STAGE42_CARD_GAP': 2,
    'STAGE42_SMALL_W': 236,
    'STAGE42_SMALL_H': 214,
    'STAGE42_SELECTED_W': 286,
    'STAGE42_SELECTED_H': 246,
}.items():
    pattern = rf'static const int {name} = \d+;'
    src, count = re.subn(pattern, f'static const int {name} = {value};', src, count=1)
    if count != 1:
        raise SystemExit(f'constant missing: {name}')

old_dy = 'const int dy = box_y + (box_h - dh) / 2;'
if old_dy not in src:
    raise SystemExit('Stage4.16 media vertical alignment anchor missing')
src = src.replace(old_dy, 'const int dy = box_y + box_h - dh;', 1)

old_reflect = 'const int dst0 = y + h + 3;'
if old_reflect not in src:
    raise SystemExit('Stage4.16 reflection anchor missing')
src = src.replace(old_reflect, 'const int dst0 = y + h + 1;', 1)

# Cropped user Sony jewel-case asset: the actual front-cover plane spans about
# x=13..85% and y=5..95%. Draw the ROM art into that full plane; the transparent
# case overlay is painted afterward, leaving the physical disc visible at right.
old_ps = '{ xp = 18; yp = 12; wp = 77; hp = 75; }'
new_ps = '{ xp = 13; yp = 5; wp = 72; hp = 90; }'
if old_ps not in src:
    raise SystemExit('PlayStation label rect anchor missing')
src = src.replace(old_ps, new_ps, 1)

old_system_block = '''      if (selected)\n      {\n         const int frame_pad_x = 14;\n         const int frame_pad_y = 11;\n         stage48_draw_system_panel(fb, x - frame_pad_x, y - frame_pad_y,\n               w + frame_pad_x * 2, content_h + frame_pad_y * 2,\n               focus == FocusZone::Systems);\n      }'''
new_system_block = '''      if (selected)\n      {\n         const int frame_pad_x = 14;\n         const int frame_pad_y = 11;\n         const int frame_x = x - frame_pad_x;\n         const int frame_y = y - frame_pad_y;\n         const int frame_w = w + frame_pad_x * 2;\n         const int frame_h = content_h + frame_pad_y * 2;\n         stage48_draw_system_panel(fb, frame_x, frame_y, frame_w, frame_h,\n               focus == FocusZone::Systems);\n         const uint16_t system_frame = focus == FocusZone::Systems ?\n               pack1555(72, 228, 255) : pack1555(62, 98, 132);\n         frame_rect(fb, frame_x, frame_y, frame_w, frame_h,\n               focus == FocusZone::Systems ? 3 : 2, system_frame);\n      }'''
if old_system_block not in src:
    raise SystemExit('Stage4.8 selected system-panel block missing')
src = src.replace(old_system_block, new_system_block, 1)

old_floor = 'fill_rect(fb, mr.x + mr.w / 4, STAGE413_CARD_FLOOR + 1, mr.w / 2, 3, active_line);'
new_floor = 'fill_rect(fb, mr.x, STAGE413_CARD_FLOOR + 1, mr.w, 4, active_line);'
if old_floor not in src:
    raise SystemExit('ROM focus-line anchor missing')
src = src.replace(old_floor, new_floor, 1)

old_block = '''      Stage416MediaRect mr;\n      const std::string media = stage415_media_asset(sys);\n      if (!stage416_draw_media_contain(fb, media, fx, fy, w, h, mr))\n      {\n         mr.x = fx; mr.y = fy; mr.w = w; mr.h = h;\n         fill_rect(fb, fx, fy, w, h, pack1555(18, 28, 42));\n      }\n\n      const Stage415LabelRect lr = stage416_label_rect(sys, mr);\n      stage415_draw_rom_label(fb, sys, g, lr, selected);'''
new_block = '''      Stage416MediaRect mr;\n      const std::string media = stage415_media_asset(sys);\n      if (!stage416_draw_media_contain(fb, media, fx, fy, w, h, mr))\n      {\n         mr.x = fx; mr.y = fy; mr.w = w; mr.h = h;\n         fill_rect(fb, fx, fy, w, h, pack1555(18, 28, 42));\n      }\n\n      const Stage415LabelRect lr = stage416_label_rect(sys, mr);\n      const std::string stage417_name = lower(sys.name);\n      const bool stage417_ps = stage417_name == \"psx\" || stage417_name == \"ps1\" ||\n                               stage417_name == \"playstation\";\n      stage415_draw_rom_label(fb, sys, g, lr, selected);\n      if (stage417_ps)\n      {\n         Stage416MediaRect overlay_rect;\n         stage416_draw_media_contain(fb, media, fx, fy, w, h, overlay_rect);\n      }'''
if old_block not in src:
    raise SystemExit('Stage4.16 media/label draw block missing')
src = src.replace(old_block, new_block, 1)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE417_GEOMETRY five-wide gap2 small236x214 selected286x246 edge-about20\\n");\n'
    '   printf("STAGE417_ROM_FOCUS full-media-width-underline4px\\n");\n'
    '   printf("STAGE417_SYSTEM_FRAME code-drawn bright-systems dim-games\\n");\n'
    '   printf("STAGE417_REFLECTION direct-adjacent one-to-one\\n");\n'
    '   printf("STAGE417_PLAYSTATION rom-cover-under-transparent-case disc-visible cover-rect13-5-72-90\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE417_LAYOUT_SONY_CASE_PATCH_OK {src_path} -> {out_path}')
