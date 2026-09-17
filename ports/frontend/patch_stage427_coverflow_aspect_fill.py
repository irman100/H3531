#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage427_coverflow_aspect_fill.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.25 PixelStation GBA-media baseline-scale UI active'
new_marker = 'Stage4.27 PixelStation coverflow-spacing aspect-fill UI active'
if old_marker not in src:
    raise SystemExit('Stage4.25 marker not found')
src = src.replace(old_marker, new_marker, 1)

# The GBA label percentages in Stage4.25 described the whole recessed panel,
# not the actual white sticker plane. The approved asset measures about
# x=19%, y=33%, w=62%, h=46%. Atari is also tightened to the inner label
# plane so ROM art never paints over the cartridge border/ridges.
old_gba = '{ xp = 14; yp = 29; wp = 72; hp = 56; }'
new_gba = '{ xp = 19; yp = 33; wp = 62; hp = 46; }'
if old_gba not in src:
    raise SystemExit('Stage4.25 GBA label rect missing')
src = src.replace(old_gba, new_gba, 1)

old_atari = '{ xp = 11; yp = 11; wp = 81; hp = 59; }'
new_atari = '{ xp = 14; yp = 14; wp = 74; hp = 55; }'
if old_atari not in src:
    raise SystemExit('Stage4.25 Atari label rect missing')
src = src.replace(old_atari, new_atari, 1)

# Retune only the requested small-media families. The Stage4.21 renderer
# anchors the last visible alpha row to the common floor, so these boosts grow
# upward while preserving the invisible-table baseline. NES/SNES/PS1 remain 1x.
src = src.replace('double stage425_boost = 1.0;', 'double stage427_boost = 1.0;', 1)
src = src.replace('const bool stage425_selected_size = box_h >= 230;',
                  'const bool stage427_selected_size = box_h >= 230;', 1)
src = src.replace('stage425_boost = stage425_selected_size ? 1.16 : 1.10;',
                  'stage427_boost = stage427_selected_size ? 1.30 : 1.20;', 1)
src = src.replace('stage425_boost = stage425_selected_size ? 1.34 : 1.18;',
                  'stage427_boost = stage427_selected_size ? 1.34 : 1.18;', 1)
src = src.replace('stage425_boost = stage425_selected_size ? 1.14 : 1.08;',
                  'stage427_boost = stage427_selected_size ? 1.22 : 1.14;', 1)
# The second 1.14/1.08 pair belongs to classic Game Boy; leave its visual size
# unchanged while renaming the variable family.
src = src.replace('stage425_boost = stage425_selected_size ? 1.14 : 1.08;',
                  'stage427_boost = stage427_selected_size ? 1.14 : 1.08;', 1)
src = src.replace('const double scale = base_scale * stage425_boost;',
                  'const double scale = base_scale * stage427_boost;', 1)
if 'stage425_boost' in src or 'stage425_selected_size' in src:
    raise SystemExit('stale Stage4.25 scale variable remains')

# Stage4.25 enlarged the physical silhouettes without widening their carousel
# centres. That is why wide Mega Drive cartridges overlap. Keep the selected
# item centred, but multiply the relative centre offsets per media family.
# The outer cards are intentionally allowed to leave the 1280-wide screen.
old_x = '      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;'
new_x = '''      const std::string stage427_name = lower(sys.name);
      double stage427_spacing = 1.0;
      if (stage427_name == "megadrive" || stage427_name == "genesis" ||
          stage427_name == "mastersystem" || stage427_name == "gamegear")
         stage427_spacing = 1.30;
      else if (stage427_name == "gba" || stage427_name == "gameboyadvance")
         stage427_spacing = 1.16;
      else if (stage427_name == "gb" || stage427_name == "gbc" || stage427_name == "gameboy")
         stage427_spacing = 1.08;
      else if (stage427_name.find("atari") != std::string::npos)
         stage427_spacing = 1.05;
      const int x = center + (int)std::lround(
            stage42_card_center_offset(cv.rel) * stage427_spacing) - w / 2;'''
if old_x not in src:
    raise SystemExit('game carousel x-position anchor missing')
src = src.replace(old_x, new_x, 1)

# Explicit source-crop cover cache. This is aspect-fill by construction:
# crop the source to the destination aspect ratio, then map that crop 1:1 to
# the sticker/case plane. There is no independent X/Y scale, so ROM art can
# never be squashed. Any excess is centre-cropped and every pixel is clipped
# to the requested label rectangle.
cover_helpers = r'''
struct Stage427CoverCache {
   std::string key;
   int w = 0, h = 0;
   uint64_t stamp = 0;
   std::vector<uint16_t> pixels;
   std::vector<uint16_t> left;
   std::vector<uint16_t> right;
};

static std::vector<Stage427CoverCache> stage427_cover_cache;
static uint64_t stage427_cover_stamp = 1;
static const size_t STAGE427_COVER_CACHE_LIMIT = 40;

static Stage427CoverCache *stage427_get_cover(const std::string &path,
      int w, int h, int radius)
{
   if (path.empty() || w <= 0 || h <= 0) return nullptr;
   const std::string key = path + "#s427#" + std::to_string(w) + "x" +
         std::to_string(h) + "#r" + std::to_string(radius);
   for (auto &e : stage427_cover_cache)
   {
      if (e.key == key)
      {
         e.stamp = stage427_cover_stamp++;
         return &e;
      }
   }

   Stage42Art *src_img = stage42_get_art(path);
   if (!src_img || src_img->w <= 0 || src_img->h <= 0) return nullptr;

   int crop_x = 0, crop_y = 0;
   int crop_w = src_img->w, crop_h = src_img->h;
   const double target_aspect = (double)w / (double)h;
   const double source_aspect = (double)src_img->w / (double)src_img->h;
   if (source_aspect > target_aspect)
   {
      crop_w = std::max(1, (int)std::lround(src_img->h * target_aspect));
      crop_w = std::min(crop_w, src_img->w);
      crop_x = (src_img->w - crop_w) / 2;
   }
   else if (source_aspect < target_aspect)
   {
      crop_h = std::max(1, (int)std::lround(src_img->w / target_aspect));
      crop_h = std::min(crop_h, src_img->h);
      crop_y = (src_img->h - crop_h) / 2;
   }

   Stage427CoverCache fresh;
   fresh.key = key;
   fresh.w = w;
   fresh.h = h;
   fresh.stamp = stage427_cover_stamp++;
   fresh.pixels.assign((size_t)w * h, 0);
   fresh.left.assign(h, 0);
   fresh.right.assign(h, (uint16_t)w);

   for (int y = 0; y < h; ++y)
   {
      int left = 0, right = w;
      while (left < w && !stage45_round_contains(left, y, w, h, radius)) ++left;
      while (right > left && !stage45_round_contains(right - 1, y, w, h, radius)) --right;
      fresh.left[y] = (uint16_t)left;
      fresh.right[y] = (uint16_t)right;
      const int sy = crop_y + std::min(crop_h - 1,
            (int)((int64_t)y * crop_h / h));
      for (int x = left; x < right; ++x)
      {
         const int sx = crop_x + std::min(crop_w - 1,
               (int)((int64_t)x * crop_w / w));
         const unsigned char *p = src_img->rgba.data() +
               ((size_t)sy * src_img->w + sx) * 4U;
         fresh.pixels[(size_t)y * w + x] = pack1555(p[0], p[1], p[2]);
      }
   }

   if (stage427_cover_cache.size() >= STAGE427_COVER_CACHE_LIMIT)
   {
      auto it = std::min_element(stage427_cover_cache.begin(), stage427_cover_cache.end(),
            [](const Stage427CoverCache &a, const Stage427CoverCache &b) {
               return a.stamp < b.stamp;
            });
      if (it != stage427_cover_cache.end()) stage427_cover_cache.erase(it);
   }
   stage427_cover_cache.push_back(std::move(fresh));
   return &stage427_cover_cache.back();
}

static bool stage427_draw_game_cover_cached(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int radius)
{
   Stage427CoverCache *img = stage427_get_cover(path, w, h, radius);
   if (!img) return false;
   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      int left = std::max((int)img->left[yy], -x);
      int right = std::min((int)img->right[yy], (int)fb.w - x);
      if (left >= right) continue;
      memcpy(fb_row(fb, dy) + x + left,
             img->pixels.data() + (size_t)yy * w + left,
             (size_t)(right - left) * sizeof(uint16_t));
   }
   return true;
}
'''
cover_anchor = 'static bool stage415_draw_rom_label(Fb &fb, const SystemDef &sys, const Game &g,'
if cover_anchor not in src:
    raise SystemExit('ROM label function anchor missing')
src = src.replace(cover_anchor, cover_helpers + '\n\n' + cover_anchor, 1)

old_cover_call = 'stage47_draw_game_cover_cached(fb, g.image_path, r.x, r.y, r.w, r.h, 2)'
new_cover_call = 'stage427_draw_game_cover_cached(fb, g.image_path, r.x, r.y, r.w, r.h, 2)'
if old_cover_call not in src:
    raise SystemExit('Stage4.21 cached cover call missing')
src = src.replace(old_cover_call, new_cover_call, 1)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE427_LABEL GBA19-33-62-46 ATARI14-14-74-55 PS1-aspect-fill-crop\\n");\n'
    '   printf("STAGE427_SCALE selected Atari1.30 Sega1.34 GBA1.22 idle Atari1.20 Sega1.18 GBA1.14\\n");\n'
    '   printf("STAGE427_SPACING Sega1.30 GBA1.16 GB1.08 Atari1.05 outer-clipping-allowed no-overlap-target\\n");\n'
    '   printf("STAGE427_COVER aspect-fill source-crop centered clip-to-label no-stretch\\n");\n'
    '   printf("STAGE427_BASELINE last-visible-alpha-row common-floor preserved grow-upward\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE427_COVERFLOW_ASPECT_FILL_PATCH_OK {src_path} -> {out_path}')
