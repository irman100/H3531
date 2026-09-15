#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage411_trapezoid.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.8 PixelStation perspective-polish UI active'
new_marker = 'Stage4.11 PixelStation side-trapezoid UI active'
if old_marker not in src:
    raise SystemExit("expected Stage4.8 marker not found")
src = src.replace(old_marker, new_marker, 1)

helpers = r'''
static int stage411_column_inset(int column, int w, int side, int max_inset)
{
   if (!side || w <= 1 || max_inset <= 0) return 0;
   const double u = (double)column / (double)(w - 1);
   /* side < 0: left card, outer edge is left and inner edge is right.
    * side > 0: right card, outer edge is right and inner edge is left. */
   const double inner_weight = side < 0 ? u : (1.0 - u);
   return (int)std::lround(inner_weight * max_inset);
}

static bool stage411_draw_exact_asset_trapezoid(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int opacity, int side, int max_inset)
{
   Stage48ExactAsset *img = stage48_get_exact_asset(path, w, h);
   if (!img) return false;
   opacity = std::max(0, std::min(255, opacity));

   for (int xx = 0; xx < w; ++xx)
   {
      const int inset = stage411_column_inset(xx, w, side, max_inset);
      const int dh = std::max(1, h - inset * 2);
      const int dx = x + xx;
      if (dx < 0 || dx >= (int)fb.w) continue;

      for (int ddy = 0; ddy < dh; ++ddy)
      {
         const int dy = y + inset + ddy;
         if (dy < 0 || dy >= (int)fb.h) continue;
         const int sy = std::min(h - 1, (int)((int64_t)ddy * h / dh));
         const size_t pos = (size_t)sy * w + xx;
         const int a = ((int)img->alpha[pos] * opacity + 127) / 255;
         if (a <= 0) continue;
         const uint16_t sp = img->pixels[pos];
         uint16_t *dst = fb_row(fb, dy) + dx;
         if (a >= 250) *dst = sp;
         else
         {
            const int sr = ((sp >> 10) & 31) * 255 / 31;
            const int sg = ((sp >> 5) & 31) * 255 / 31;
            const int sb = (sp & 31) * 255 / 31;
            *dst = stage46_blend1555(*dst, (unsigned char)sr, (unsigned char)sg,
                  (unsigned char)sb, (unsigned char)a);
         }
      }
   }
   return true;
}

static bool stage411_draw_cover_trapezoid(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int radius, int side, int max_inset)
{
   Stage47CoverCache *img = stage47_get_cover(path, w, h, radius);
   if (!img) return false;

   for (int xx = 0; xx < w; ++xx)
   {
      const int inset = stage411_column_inset(xx, w, side, max_inset);
      const int dh = std::max(1, h - inset * 2);
      const int dx = x + xx;
      if (dx < 0 || dx >= (int)fb.w) continue;

      for (int ddy = 0; ddy < dh; ++ddy)
      {
         const int dy = y + inset + ddy;
         if (dy < 0 || dy >= (int)fb.h) continue;
         const int sy = std::min(h - 1, (int)((int64_t)ddy * h / dh));
         if (xx < (int)img->left[sy] || xx >= (int)img->right[sy]) continue;
         fb_row(fb, dy)[dx] = img->pixels[(size_t)sy * w + xx];
      }
   }
   return true;
}

static void stage411_draw_rom_panel(Fb &fb, int x, int y, int w, int h,
      int side, int max_inset)
{
   const int margin = 12;
   const int panel_inset = std::max(0, max_inset - 2);
   /* The panel helper intentionally draws only the grid background.
    * The frame is composited once, after cover art and labels. */
   stage411_draw_exact_asset_trapezoid(fb, stage46_ui_asset("rom_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2,
         255, side, panel_inset);
}

static void stage411_reflect_card(Fb &fb, int x, int y, int w, int h,
      int reflect_h, int side, int max_inset)
{
   if (reflect_h <= 0) return;
   for (int xx = 0; xx < w; ++xx)
   {
      const int col_inset = stage411_column_inset(xx, w, side, max_inset);
      const int usable_h = std::max(1, h - col_inset * 2);
      const int dx = x + xx;
      if (dx < 0 || dx >= (int)fb.w) continue;

      for (int r = 0; r < reflect_h; ++r)
      {
         const int sy = y + h - 1 - col_inset -
               std::min(usable_h - 1, r * 2);
         const int dy = y + h + 3 - col_inset + r;
         if (sy < 0 || sy >= (int)fb.h || dy < 0 || dy >= (int)fb.h) continue;
         const int alpha = std::max(0, 72 - r * 68 / std::max(1, reflect_h - 1));
         uint16_t *dst = fb_row(fb, dy) + dx;
         const uint16_t sp = fb_row(fb, sy)[dx];
         *dst = stage48_blend_pixel(*dst, sp, alpha);
      }
   }
}
'''

anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit('Stage4.11 insertion anchor not found')
src = src.replace(anchor, helpers + '\n\n' + anchor, 1)

old_geom = '''      const bool outer = std::fabs(cv.rel) >= 2.35f;\n      if (outer) { w = w * 92 / 100; h = h * 97 / 100; }\n      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;\n      const int y = card_center_y - h / 2 + (outer ? 4 : 0);'''
new_geom = '''      const bool outer = std::fabs(cv.rel) >= 2.35f;\n      if (outer) w = w * 90 / 100;\n      const int outer_nudge = outer ? (cv.rel < 0.0f ? -8 : 8) : 0;\n      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2 + outer_nudge;\n      const int y = card_center_y - h / 2;'''
if old_geom not in src:
    raise SystemExit('Stage4.11 outer geometry anchor not found')
src = src.replace(old_geom, new_geom, 1)

old_setup = '''      const int shear = outer ? (cv.rel < 0.0f ? 14 : -14) : 0;\n\n      stage48_draw_rom_panel(fb, fx, fy, fw, fh,\n            selected && focus == FocusZone::Games, shear);'''
new_setup = '''      const int trapezoid_side = outer ? (cv.rel < 0.0f ? -1 : 1) : 0;\n      const int trapezoid_inset = outer ? 24 : 0;\n\n      stage411_draw_rom_panel(fb, fx, fy, fw, fh,\n            trapezoid_side, trapezoid_inset);'''
if old_setup not in src:
    raise SystemExit('Stage4.11 panel call anchor not found')
src = src.replace(old_setup, new_setup, 1)

old_cover = '''      if (!stage48_draw_cover_shear(fb, g.image_path, art_x, art_y, art_w, art_h, 5, shear))\n         draw_fallback_card(fb, sys, art_x, art_y, art_w, art_h);'''
new_cover = '''      if (!stage411_draw_cover_trapezoid(fb, g.image_path, art_x, art_y, art_w, art_h, 5,\n            trapezoid_side, std::max(0, trapezoid_inset - 5)))\n         draw_fallback_card(fb, sys, art_x, art_y, art_w, art_h);'''
if old_cover not in src:
    raise SystemExit('Stage4.11 cover call anchor not found')
src = src.replace(old_cover, new_cover, 1)

old_frame = '''      stage48_draw_exact_asset(fb,\n            stage46_ui_asset(selected && focus == FocusZone::Games ?\n               "rom_frame_focus.png" : "rom_frame_idle.png"),\n            fx, fy, fw, fh, 255, shear);\n\n      stage48_reflect_card(fb, fx, fy, fw, fh, selected ? 20 : 14);'''
new_frame = '''      stage411_draw_exact_asset_trapezoid(fb,\n            stage46_ui_asset(selected && focus == FocusZone::Games ?\n               "rom_frame_focus.png" : "rom_frame_idle.png"),\n            fx, fy, fw, fh, 255, trapezoid_side, trapezoid_inset);\n\n      stage411_reflect_card(fb, fx, fy, fw, fh,\n            selected ? 34 : (outer ? 26 : 20), trapezoid_side, trapezoid_inset);'''
if old_frame not in src:
    raise SystemExit('Stage4.11 final frame anchor not found')
src = src.replace(old_frame, new_frame, 1)

layout_anchor = '   printf("LAYOUT_TEST_OK\\n");\n'
layout_markers = (
    '   printf("STAGE411_TRAPEZOID outer-edge-full inner-edge-inset=24 no-shear\\n");\n'
    '   printf("STAGE411_FRAME single-final-frame panel-background-only\\n");\n'
    '   printf("STAGE411_REFLECTION selected=34 outer=26 side=20\\n");\n'
)
if layout_anchor not in src:
    raise SystemExit('Stage4.11 layout anchor not found')
src = src.replace(layout_anchor, layout_markers + layout_anchor, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE411_TRAPEZOID_PATCH_OK {src_path} -> {out_path}')
