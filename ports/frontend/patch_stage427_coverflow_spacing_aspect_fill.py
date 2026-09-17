#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage427_coverflow_spacing_aspect_fill.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.25 PixelStation GBA-media baseline-scale UI active'
new_marker = 'Stage4.27 PixelStation coverflow-spacing aspect-fill UI active'
if old_marker not in src:
    raise SystemExit('Stage4.25 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Re-measured from the actual installed PNG silhouettes. The GBA white label
# begins at about 19/33 percent and spans about 62/46 percent. Atari artwork
# stays inside the cartridge's inner recessed face rather than covering its rim.
old_gba = '{ xp = 14; yp = 29; wp = 72; hp = 56; }'
new_gba = '{ xp = 19; yp = 33; wp = 62; hp = 46; }'
if old_gba not in src:
    raise SystemExit('Stage4.25 GBA label rect missing')
src = src.replace(old_gba, new_gba, 1)

old_atari = '{ xp = 11; yp = 11; wp = 81; hp = 59; }'
new_atari = '{ xp = 14; yp = 15; wp = 76; hp = 55; }'
if old_atari not in src:
    raise SystemExit('Stage4.25 Atari label rect missing')
src = src.replace(old_atari, new_atari, 1)

# Increase only the two families requested in the latest hardware review.
# Baseline behaviour remains unchanged because Stage4.21 aligns the last
# visible alpha row to the common floor after scaling.
old_scale_block = '''   double stage425_boost = 1.0;
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
new_scale_block = '''   double stage427_boost = 1.0;
   const bool stage427_selected_size = box_h >= 230;
   if (path.find("media/cartridge_atari.png") != std::string::npos)
      stage427_boost = stage427_selected_size ? 1.28 : 1.20;
   else if (path.find("media/cartridge_megadrive.png") != std::string::npos)
      stage427_boost = stage427_selected_size ? 1.34 : 1.18;
   else if (path.find("media/cartridge_gba.png") != std::string::npos)
      stage427_boost = stage427_selected_size ? 1.24 : 1.16;
   else if (path.find("media/cartridge_gameboy.png") != std::string::npos)
      stage427_boost = stage427_selected_size ? 1.14 : 1.08;
   const double scale = base_scale * stage427_boost;'''
if old_scale_block not in src:
    raise SystemExit('Stage4.25 media boost block missing')
src = src.replace(old_scale_block, new_scale_block, 1)

# Replace the cached label scaler with an explicit source-crop implementation.
# It always preserves source aspect ratio, center-crops what cannot fit and
# writes strictly inside the target label plane. This fixes GBA/Atari spill and
# prevents PlayStation covers from ever being stretched/squashed.
old_label_draw = '''static bool stage415_draw_rom_label(Fb &fb, const SystemDef &sys, const Game &g,
      const Stage415LabelRect &r, bool selected)
{
   if (!g.image_path.empty() &&
       stage47_draw_game_cover_cached(fb, g.image_path, r.x, r.y, r.w, r.h, 2))
      return true;
   stage415_fallback_label(fb, sys, g, r, selected);
   return false;
}'''
new_label_draw = r'''static bool stage427_draw_cover_fill_clip(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int radius)
{
   Stage42Art *img = stage42_get_art(path);
   if (!img || img->w <= 0 || img->h <= 0 || w <= 0 || h <= 0)
      return false;

   int crop_x = 0, crop_y = 0, crop_w = img->w, crop_h = img->h;
   const int64_t src_cross = (int64_t)img->w * h;
   const int64_t dst_cross = (int64_t)img->h * w;
   if (src_cross > dst_cross)
   {
      crop_w = std::max(1, (int)((int64_t)img->h * w / h));
      crop_w = std::min(crop_w, img->w);
      crop_x = (img->w - crop_w) / 2;
   }
   else if (src_cross < dst_cross)
   {
      crop_h = std::max(1, (int)((int64_t)img->w * h / w));
      crop_h = std::min(crop_h, img->h);
      crop_y = (img->h - crop_h) / 2;
   }

   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dy);
      const int sy = crop_y + std::min(crop_h - 1,
            (int)((int64_t)yy * crop_h / h));
      for (int xx = 0; xx < w; ++xx)
      {
         if (radius > 0 && !stage45_round_contains(xx, yy, w, h, radius))
            continue;
         const int dx = x + xx;
         if (dx < 0 || dx >= (int)fb.w) continue;
         const int sx = crop_x + std::min(crop_w - 1,
               (int)((int64_t)xx * crop_w / w));
         const unsigned char *p = img->rgba.data() +
               ((size_t)sy * img->w + sx) * 4U;
         if (p[3] < 8) continue;
         const uint16_t fg = pack1555(p[0], p[1], p[2]);
         row[dx] = p[3] >= 245 ? fg : stage48_blend_pixel(row[dx], fg, p[3]);
      }
   }
   return true;
}

static bool stage415_draw_rom_label(Fb &fb, const SystemDef &sys, const Game &g,
      const Stage415LabelRect &r, bool selected)
{
   if (!g.image_path.empty() &&
       stage427_draw_cover_fill_clip(fb, g.image_path, r.x, r.y, r.w, r.h, 2))
      return true;
   stage415_fallback_label(fb, sys, g, r, selected);
   return false;
}'''
if old_label_draw not in src:
    raise SystemExit('Stage4.21 cached ROM-label draw function missing')
src = src.replace(old_label_draw, new_label_draw, 1)

# Wider physical media must not overlap. Move centres farther apart per family,
# not by shrinking the media. Extreme items are deliberately allowed offscreen.
spacing_helper = r'''static double stage427_media_spacing_factor(const SystemDef &sys)
{
   const std::string n = lower(sys.name);
   if (n == "megadrive" || n == "genesis" || n == "mastersystem" || n == "gamegear")
      return 1.30;
   if (n == "gba" || n == "gameboyadvance")
      return 1.13;
   if (n == "gb" || n == "gbc" || n == "gameboy")
      return 1.10;
   if (n.find("atari") != std::string::npos)
      return 1.04;
   return 1.0;
}

'''
games_anchor = 'static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,'
if games_anchor not in src:
    raise SystemExit('draw-games function missing')
src = src.replace(games_anchor, spacing_helper + games_anchor, 1)

gstart = src.find(games_anchor)
gend = src.find('static void stage42_draw_ui(', gstart)
if gstart < 0 or gend < 0:
    raise SystemExit('draw-games range missing')
games = src[gstart:gend]
old_x = '      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;'
new_x = '''      const double stage427_spacing = stage427_media_spacing_factor(sys);
      const int x = center + (int)std::lround(
            stage42_card_center_offset(cv.rel) * stage427_spacing) - w / 2;'''
if old_x not in games:
    raise SystemExit('game-card centre anchor missing')
games = games.replace(old_x, new_x, 1)
src = src[:gstart] + games + src[gend:]

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE427_LABELS GBA19-33-62-46 ATARI14-15-76-55 PS13-5-72-90 clip=strict\\n");\n'
    '   printf("STAGE427_ASPECT_FILL center-crop preserve-ratio no-stretch all-ROM-labels\\n");\n'
    '   printf("STAGE427_SCALE selected Atari1.28 Sega1.34 GBA1.24 idle Atari1.20 Sega1.18 GBA1.16 baseline-unchanged\\n");\n'
    '   printf("STAGE427_SPACING Sega1.30 GBA1.13 GB1.10 Atari1.04 outer-items-may-offscreen no-overlap-target\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE427_COVERFLOW_SPACING_ASPECT_FILL_PATCH_OK {src_path} -> {out_path}')
