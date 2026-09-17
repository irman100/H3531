#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage425_media_scale_gba.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.24 V2 PixelStation SNES-label GBA-icon UI active'
new_marker = 'Stage4.25 PixelStation GBA-media baseline-scale UI active'
if old_marker not in src:
    raise SystemExit('Stage4.24 V2 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Dedicated physical media for Game Boy / Game Boy Advance. Stage4.24 only
# changed the GBA system/controller icon, so both still fell through to NES
# physical media. Keep NES/SNES/PS1 mappings untouched.
old_media = '''   if (n == "snes" || n == "supernes" || n == "sfc")
      return stage45_asset_path("media/cartridge_snes.png");
   if (n.find("atari") != std::string::npos)
      return stage45_asset_path("media/cartridge_atari.png");'''
new_media = '''   if (n == "snes" || n == "supernes" || n == "sfc")
      return stage45_asset_path("media/cartridge_snes.png");
   if (n == "gba" || n == "gameboyadvance")
      return stage45_asset_path("media/cartridge_gba.png");
   if (n == "gb" || n == "gbc" || n == "gameboy")
      return stage45_asset_path("media/cartridge_gameboy.png");
   if (n.find("atari") != std::string::npos)
      return stage45_asset_path("media/cartridge_atari.png");'''
if old_media not in src:
    raise SystemExit('physical-media mapping anchor not found')
src = src.replace(old_media, new_media, 1)

# Label planes are normalized to the actual cartridge image, so ROM art grows
# with the physical cartridge instead of staying at an old fixed size.
old_labels = '''   else if (n == "snes" || n == "supernes" || n == "sfc")
      { xp = 8; yp = 7; wp = 84; hp = 75; }
   else if (n.find("atari") != std::string::npos)
      { xp = 11; yp = 11; wp = 81; hp = 59; }'''
new_labels = '''   else if (n == "snes" || n == "supernes" || n == "sfc")
      { xp = 8; yp = 7; wp = 84; hp = 75; }
   else if (n == "gba" || n == "gameboyadvance")
      { xp = 14; yp = 29; wp = 72; hp = 56; }
   else if (n == "gb" || n == "gbc" || n == "gameboy")
      { xp = 14; yp = 30; wp = 72; hp = 55; }
   else if (n.find("atari") != std::string::npos)
      { xp = 11; yp = 11; wp = 81; hp = 59; }'''
if old_labels not in src:
    raise SystemExit('label-rectangle anchor not found')
src = src.replace(old_labels, new_labels, 1)

# Retune only small physical-media families. The renderer already aligns the
# last visible alpha row to one common floor. Increasing scale here therefore
# grows the cartridge UPWARD while preserving the exact bottom baseline.
# NES, SNES and PlayStation remain at scale 1.0.
start = src.find('static bool stage416_draw_media_contain(')
end = src.find('static Stage415LabelRect stage416_label_rect(', start)
if start < 0 or end < 0:
    raise SystemExit('Stage4.21 media contain function not found')
media_func = src[start:end]
old_scale = '   const double scale = std::min(sx, sy);'
new_scale = '''   const double base_scale = std::min(sx, sy);
   double stage425_boost = 1.0;
   const bool stage425_selected_size = box_h >= 230;
   if (path.find("media/cartridge_atari.png") != std::string::npos)
      stage425_boost = stage425_selected_size ? 1.16 : 1.10;
   else if (path.find("media/cartridge_megadrive.png") != std::string::npos)
      stage425_boost = stage425_selected_size ? 1.34 : 1.18;
   else if (path.find("media/cartridge_gba.png") != std::string::npos)
      stage425_boost = stage425_selected_size ? 1.14 : 1.08;
   else if (path.find("media/cartridge_gameboy.png") != std::string::npos)
      stage425_boost = stage425_selected_size ? 1.14 : 1.08;
   const double scale = base_scale * stage425_boost;'''
if old_scale not in media_func:
    raise SystemExit('cached media scale anchor not found')
media_func = media_func.replace(old_scale, new_scale, 1)
src = src[:start] + media_func + src[end:]

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE425_GBA_MEDIA dedicated-cartridge-gba label14-29-72-56\\n");\n'
    '   printf("STAGE425_GB_MEDIA dedicated-cartridge-gameboy label14-30-72-55\\n");\n'
    '   printf("STAGE425_SCALE selected Atari1.16 Sega1.34 GBA1.14 GB1.14 idle Atari1.10 Sega1.18 GBA1.08 GB1.08\\n");\n'
    '   printf("STAGE425_BASELINE last-visible-alpha-row shared-floor unchanged grow-upward NES-SNES-PS1-scale1\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE425_MEDIA_SCALE_GBA_PATCH_OK {src_path} -> {out_path}')
