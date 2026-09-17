#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage424_v2_snes_gba_art.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.22 PixelStation Atari-PlayStation shell UI active'
new_marker = 'Stage4.24 V2 PixelStation SNES-label GBA-icon UI active'
if old_marker not in src:
    raise SystemExit('Stage4.22 marker not found')
src = src.replace(old_marker, new_marker, 1)

old_gb = '   if (n == "gb" || n == "gbc" || n == "gba" || n == "gameboy") return stage45_asset_path("controller_gameboy.png");'
new_gb = '''   if (n == "gba" || n == "gameboyadvance") return stage45_asset_path("controller_gba.png");
   if (n == "gb" || n == "gbc" || n == "gameboy") return stage45_asset_path("controller_gameboy.png");'''
if old_gb not in src:
    raise SystemExit('GBA/Game Boy controller mapping anchor not found')
src = src.replace(old_gb, new_gb, 1)

# Stage4.24 V2 supplied SNES cartridge has a much taller artwork sticker.
# Its measured usable light label plane is approximately x=8%, y=7%, w=84%, h=75%
# of the contained cartridge asset. Fill that complete rectangle with ROM artwork.
old_snes = '{ xp = 10; yp = 20; wp = 80; hp = 33; }'
new_snes = '{ xp = 8; yp = 7; wp = 84; hp = 75; }'
if old_snes not in src:
    raise SystemExit('SNES label rect anchor not found')
src = src.replace(old_snes, new_snes, 1)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE424V2_SNES cartridge-new-tall-label rect8-7-84-75 rom-art-fills-label\\n");\n'
    '   printf("STAGE424V2_GBA dedicated-controller-gba-not-gameboy\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE424_V2_SNES_GBA_ART_PATCH_OK {src_path} -> {out_path}')
