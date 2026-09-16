#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage418_media_floor_glass.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.17 PixelStation denser-media Sony-case UI active'
new_marker = 'Stage4.18 PixelStation grounded-media glass-reflection UI active'
if old_marker not in src:
    raise SystemExit('Stage4.17 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Align the LAST VISIBLE (non-transparent) row of every cartridge/case to one
# shared floor line. This prevents differently-shaped assets (e.g. Mega Drive)
# from appearing to float above the NES row merely because their PNG canvas has
# transparent bottom padding.
old_dy = 'const int dy = box_y + box_h - dh;'
new_dy = r'''int stage418_src_bottom = img->h - 1;
   for (; stage418_src_bottom > 0; --stage418_src_bottom)
   {
      bool any = false;
      const size_t row_base = (size_t)stage418_src_bottom * (size_t)img->w * 4U;
      for (int sxp = 0; sxp < img->w; ++sxp)
      {
         if (img->rgba[row_base + (size_t)sxp * 4U + 3U] >= 8)
         {
            any = true;
            break;
         }
      }
      if (any) break;
   }
   const int stage418_visible_rows = std::max(1,
         (int)(((int64_t)(stage418_src_bottom + 1) * dh + img->h - 1) / img->h));
   const int stage418_visible_floor = box_y + box_h - 1;
   const int dy = stage418_visible_floor - (stage418_visible_rows - 1);'''
if old_dy not in src:
    raise SystemExit('Stage4.17 bottom-alignment anchor missing')
src = src.replace(old_dy, new_dy, 1)


def between(text, start, end, repl):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + repl.rstrip() + '\n\n' + text[b:]

# The cursor occupies floor+1..floor+4. Reflection starts at floor+7: one row
# between physical media and cursor, then a 2px visual gap under the cursor,
# giving a thick-glass / glossy-table effect instead of a glued-on mirror.
reflection = r'''static void stage413_reflection(Fb &fb, int x, int y, int w, int h, bool selected)
{
   const int dst0 = STAGE413_CARD_FLOOR + 7;
   const int src_bottom = std::min(y + h - 1, STAGE413_CARD_FLOOR - 1);
   const int src_h = std::max(1, src_bottom - y + 1);
   const int max_h = std::max(0, STAGE413_FOOTER - dst0 - 2);
   const int rh = std::min(src_h, max_h);
   if (rh <= 1) return;
   const int a0 = selected ? 92 : 64;
   for (int r = 0; r < rh; ++r)
   {
      const int sy = src_bottom - r;
      const int dy = dst0 + r;
      if (sy < y || dy < 0 || dy >= (int)fb.h) break;
      const int a = a0 * (rh - r) / std::max(1, rh);
      uint16_t *dst = fb_row(fb, dy);
      const uint16_t *sp = fb_row(fb, sy);
      for (int xx = std::max(0, x); xx < std::min((int)fb.w, x + w); ++xx)
         dst[xx] = stage48_blend_pixel(dst[xx], sp[xx], a);
   }
}
'''
src = between(src, 'static void stage413_reflection(', 'static void stage413_menu_draw(', reflection)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE418_MEDIA_FLOOR visible-alpha-bottom=floor-1 all-systems\\n");\n'
    '   printf("STAGE418_CURSOR floor+1 full-selected-media-width 4px\\n");\n'
    '   printf("STAGE418_GLASS reflection-start=floor+7 cursor-gap=2px one-to-one\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE418_MEDIA_FLOOR_GLASS_PATCH_OK {src_path} -> {out_path}')
