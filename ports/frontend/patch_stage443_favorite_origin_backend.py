#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage443_favorite_origin_backend.py INPUT OUTPUT')

src = Path(sys.argv[1]).read_text(encoding='utf-8')
old = '''struct Game {
   std::string rom_path;
   std::string title;
   std::string image_path;
   std::string description;
};'''
new = '''struct Game {
   std::string rom_path;
   std::string title;
   std::string image_path;
   std::string description;

   /* Stage4.43: when a game is copied into the synthetic Favorites view,
    * preserve the real owning system so cartridge/case art and labels remain
    * console-correct. Empty for normal scanned library entries. */
   std::string origin_system;
   std::string origin_fullname;
   std::string origin_theme;
};'''
if old not in src:
    raise SystemExit('Stage4.43 Game struct anchor missing')
src = src.replace(old, new, 1)
for marker in ['origin_system', 'origin_fullname', 'origin_theme']:
    if marker not in src:
        raise SystemExit('missing Stage4.43 backend marker: ' + marker)
Path(sys.argv[2]).write_text(src, encoding='utf-8')
print('STAGE443_FAVORITE_ORIGIN_BACKEND_PATCH_OK')
