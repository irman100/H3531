#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage422_atari_ps1_shell.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.21 PixelStation cached-silhouette smooth-render UI active'
new_marker = 'Stage4.22 PixelStation Atari-PlayStation shell UI active'
if old_marker not in src:
    raise SystemExit('Stage4.21 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Stage4.21 already computes system focus aura from the controller PNG alpha
# silhouette. Add Atari to that existing asset path; retain the PS mapping.
controller_anchor = '   if (n == "psx" || n == "ps1" || n == "playstation") return stage45_asset_path("controller_playstation.png");'
if controller_anchor not in src:
    raise SystemExit('PlayStation controller mapping anchor missing')
src = src.replace(controller_anchor,
    '   if (n.find("atari") != std::string::npos) return stage45_asset_path("controller_atari.png");\n' + controller_anchor,
    1)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE422_SYSTEMS top-row Atari-controller PlayStation-controller alpha-aura\\n");\n'
    '   printf("STAGE422_MEDIA Atari-cartridge PlayStation-jewel-case transparent-overlay-disc-visible\\n");\n'
    '   printf("STAGE422_PATHS primary-Games-Atari+Games-PS1 fallback-lowercase\\n");\n'
    '   printf("STAGE422_ART same-folder-basename-first png-jpg-jpeg-bmp legacy-subdirs\\n");\n'
    '   printf("STAGE422_PS cue-canonical referenced-bin-suppressed\\n");\n'
    '   printf("STAGE422_CACHE Stage4.21-scaled-assets+aura+cover preserved\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE422_ATARI_PS1_SHELL_PATCH_OK {src_path} -> {out_path}')
