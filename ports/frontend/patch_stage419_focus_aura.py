#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage419_focus_aura.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.18 PixelStation grounded-media glass-reflection UI active'
new_marker = 'Stage4.19 PixelStation focus-aura UI active'
if old_marker not in src:
    raise SystemExit('Stage4.18 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Stage4.19 removes all cursor/box chrome from focused items. Focus is indicated
# only by a soft cyan/blue backlight aura rendered behind the real object.
helper = r'''static void stage419_focus_aura(Fb &fb, int x, int y, int w, int h,
      int pad_x, int pad_y, int strength)
{
   if (w <= 0 || h <= 0 || strength <= 0) return;

   const int cx = x + w / 2;
   const int cy = y + h / 2;
   const int rx = std::max(1, w / 2 + pad_x);
   const int ry = std::max(1, h / 2 + pad_y);
   const int x0 = std::max(0, cx - rx);
   const int x1 = std::min((int)fb.w - 1, cx + rx);
   const int y0 = std::max(0, cy - ry);
   const int y1 = std::min((int)fb.h - 1, cy + ry);
   const int64_t rx2 = (int64_t)rx * rx;
   const int64_t ry2 = (int64_t)ry * ry;
   const uint16_t inner = pack1555(76, 224, 255);
   const uint16_t outer = pack1555(30, 118, 224);

   // 2x2 blocks keep the effect cheap on Hi3531 and visually consistent with
   // the pixel-art UI. No rectangular backing is drawn: only a radial fade.
   for (int yy = y0; yy <= y1; yy += 2)
   {
      const int64_t dy = (int64_t)yy - cy;
      const int64_t ny = (dy * dy * 65536LL) / ry2;
      if (ny >= 65536LL) continue;
      for (int xx = x0; xx <= x1; xx += 2)
      {
         const int64_t dx = (int64_t)xx - cx;
         const int64_t d2 = ny + (dx * dx * 65536LL) / rx2;
         if (d2 >= 65536LL) continue;

         const int64_t fall = 65536LL - d2;
         int a = (int)((int64_t)strength * fall * fall / (65536LL * 65536LL));
         if (a < 3) continue;
         const uint16_t glow = d2 < 24576LL ? inner : outer;

         for (int by = 0; by < 2; ++by)
         {
            const int py = yy + by;
            if (py < 0 || py >= (int)fb.h) continue;
            uint16_t *row = fb_row(fb, py);
            for (int bx = 0; bx < 2; ++bx)
            {
               const int px = xx + bx;
               if (px < 0 || px >= (int)fb.w) continue;
               row[px] = stage48_blend_pixel(row[px], glow, a);
            }
         }
      }
   }
}
'''
anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit('system-row anchor missing')
src = src.replace(anchor, helper + '\n\n' + anchor, 1)

old_system = '''      if (selected)
      {
         const int frame_pad_x = 14;
         const int frame_pad_y = 11;
         const int frame_x = x - frame_pad_x;
         const int frame_y = y - frame_pad_y;
         const int frame_w = w + frame_pad_x * 2;
         const int frame_h = content_h + frame_pad_y * 2;
         stage48_draw_system_panel(fb, frame_x, frame_y, frame_w, frame_h,
               focus == FocusZone::Systems);
         const uint16_t system_frame = focus == FocusZone::Systems ?
               pack1555(72, 228, 255) : pack1555(62, 98, 132);
         frame_rect(fb, frame_x, frame_y, frame_w, frame_h,
               focus == FocusZone::Systems ? 3 : 2, system_frame);
      }'''
new_system = '''      if (selected && focus == FocusZone::Systems)
      {
         stage419_focus_aura(fb, x, y, w, content_h, 34, 24, 86);
      }'''
if old_system not in src:
    raise SystemExit('Stage4.17 system frame/panel block missing')
src = src.replace(old_system, new_system, 1)

# Remove the ROM underline/cursor completely.
old_line_decl = '   const uint16_t active_line = pack1555(78, 232, 255);\n'
if old_line_decl in src:
    src = src.replace(old_line_decl, '', 1)

old_cursor = '''      if (selected && focus == FocusZone::Games)
         fill_rect(fb, mr.x, STAGE413_CARD_FLOOR + 1, mr.w, 4, active_line);'''
if old_cursor not in src:
    raise SystemExit('Stage4.17/4.18 ROM underline block missing')
src = src.replace(old_cursor, '', 1)

# To get a true backlight, first obtain the exact contained-media rectangle,
# then paint the aura, then redraw the cartridge/case above it. This avoids a
# rectangular panel while preserving transparent silhouette edges.
old_media = '''      Stage416MediaRect mr;
      const std::string media = stage415_media_asset(sys);
      if (!stage416_draw_media_contain(fb, media, fx, fy, w, h, mr))
      {
         mr.x = fx; mr.y = fy; mr.w = w; mr.h = h;
         fill_rect(fb, fx, fy, w, h, pack1555(18, 28, 42));
      }

      const Stage415LabelRect lr = stage416_label_rect(sys, mr);'''
new_media = '''      Stage416MediaRect mr;
      const std::string media = stage415_media_asset(sys);
      const bool stage419_media_ok = stage416_draw_media_contain(fb, media, fx, fy, w, h, mr);
      if (!stage419_media_ok)
      {
         mr.x = fx; mr.y = fy; mr.w = w; mr.h = h;
         fill_rect(fb, fx, fy, w, h, pack1555(18, 28, 42));
      }

      if (selected && focus == FocusZone::Games)
      {
         stage419_focus_aura(fb, mr.x, mr.y, mr.w, mr.h, 24, 18, 74);
         if (stage419_media_ok)
         {
            Stage416MediaRect stage419_restore;
            stage416_draw_media_contain(fb, media, fx, fy, w, h, stage419_restore);
         }
      }

      const Stage415LabelRect lr = stage416_label_rect(sys, mr);'''
if old_media not in src:
    raise SystemExit('Stage4.18 media draw block missing')
src = src.replace(old_media, new_media, 1)

# Remove the now-obsolete Stage4.18 cursor diagnostic marker.
src = src.replace('   printf("STAGE418_CURSOR floor+1 full-selected-media-width 4px\\n");\n', '', 1)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE419_FOCUS aura-only no-frame no-panel no-underline\\n");\n'
    '   printf("STAGE419_AURA systems86 media74 pixel-radial-backlight\\n");\n'
    '   printf("STAGE419_GLASS preserve-stage418 reflection-gap\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE419_FOCUS_AURA_PATCH_OK {src_path} -> {out_path}')
