#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_final.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.20 PixelStation silhouette-aura UI active'
new_marker = 'Stage4.21 PixelStation cached-silhouette smooth-render UI active'
if old_marker not in src:
    raise SystemExit('Stage4.20 marker not found')
src = src.replace(old_marker, new_marker, 1)

# The final Stage4.13 chain already provides continuous focus interpolation.
# Preserve it. Normalize only the timing targets explicitly.
for name, value in {
    'STAGE42_GAME_ANIM_MS': 128,
    'STAGE42_SYSTEM_ANIM_MS': 128,
    'STAGE42_FRAME_MS': 16,
}.items():
    src, n = re.subn(rf'static const uint64_t {name} = \d+;',
                     f'static const uint64_t {name} = {value};', src, count=1)
    if n != 1:
        raise SystemExit(f'timing constant missing: {name}')

# Late media patches reintroduced the uncached cover scaler. Put ROM labels
# back on the Stage4.7 pre-scaled cover cache.
slow_cover = 'stage45_draw_game_cover(fb, g.image_path, r.x, r.y, r.w, r.h, 2)'
fast_cover = 'stage47_draw_game_cover_cached(fb, g.image_path, r.x, r.y, r.w, r.h, 2)'
if slow_cover not in src:
    raise SystemExit('slow ROM cover draw anchor missing')
src = src.replace(slow_cover, fast_cover, 1)

struct_anchor = '''struct Stage416MediaRect {
   int x = 0, y = 0, w = 0, h = 0;
};'''
if struct_anchor not in src:
    raise SystemExit('Stage416MediaRect anchor missing')

helpers = r'''

struct Stage421AlphaBounds {
   int left = 0, top = 0, right = -1, bottom = -1;
};

static std::map<std::string, Stage421AlphaBounds> stage421_bounds_cache;

static Stage421AlphaBounds stage421_get_bounds(const std::string &path,
      int w, int h, int threshold = 8)
{
   const std::string key = path + "#bounds#" + std::to_string(w) + "x" +
         std::to_string(h) + "#t" + std::to_string(threshold);
   auto found = stage421_bounds_cache.find(key);
   if (found != stage421_bounds_cache.end()) return found->second;

   Stage421AlphaBounds b;
   Stage47ScaledAsset *img = stage47_get_scaled_asset(path, w, h, false);
   if (!img || img->w <= 0 || img->h <= 0) return b;
   b.left = img->w; b.top = img->h; b.right = -1; b.bottom = -1;
   for (int y = 0; y < img->h; ++y)
      for (int x = 0; x < img->w; ++x)
         if (img->alpha[(size_t)y * img->w + x] >= threshold)
         {
            b.left = std::min(b.left, x); b.right = std::max(b.right, x);
            b.top = std::min(b.top, y); b.bottom = std::max(b.bottom, y);
         }
   if (b.right < b.left || b.bottom < b.top) b = Stage421AlphaBounds{};
   if (stage421_bounds_cache.size() > 192) stage421_bounds_cache.clear();
   stage421_bounds_cache[key] = b;
   return b;
}

static bool stage421_blit_scaled(Fb &fb, const Stage47ScaledAsset *img,
      int x, int y, int opacity = 255)
{
   if (!img || img->w <= 0 || img->h <= 0) return false;
   opacity = std::max(0, std::min(255, opacity));
   for (int yy = 0; yy < img->h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      int left = std::max(0, -x);
      int right = std::min(img->w, (int)fb.w - x);
      if (left >= right) continue;
      uint16_t *dst = fb_row(fb, dy) + x + left;
      const uint16_t *sp = img->pixels.data() + (size_t)yy * img->w + left;
      const unsigned char *sa = img->alpha.data() + (size_t)yy * img->w + left;
      if (img->opaque && opacity == 255)
      {
         memcpy(dst, sp, (size_t)(right - left) * sizeof(uint16_t));
         continue;
      }
      for (int xx = left; xx < right; ++xx, ++dst, ++sp, ++sa)
      {
         const int a = ((int)*sa * opacity + 127) / 255;
         if (a <= 0) continue;
         if (a >= 250) *dst = *sp;
         else *dst = stage48_blend_pixel(*dst, *sp, a);
      }
   }
   return true;
}

struct Stage421AuraLayer {
   int w = 0, h = 0, pad = 0;
   uint64_t stamp = 0;
   std::vector<unsigned char> alpha;
};

static std::map<std::string, Stage421AuraLayer> stage421_aura_cache;
static uint64_t stage421_aura_stamp = 1;
static const size_t STAGE421_AURA_CACHE_LIMIT = 48;

static Stage421AuraLayer *stage421_get_aura(const std::string &path,
      int w, int h, int outer, int strength)
{
   if (path.empty() || w <= 0 || h <= 0 || outer <= 0 || strength <= 0)
      return nullptr;
   const std::string key = path + "#aura#" + std::to_string(w) + "x" +
         std::to_string(h) + "#o" + std::to_string(outer) + "#s" +
         std::to_string(strength);
   auto found = stage421_aura_cache.find(key);
   if (found != stage421_aura_cache.end())
   {
      found->second.stamp = stage421_aura_stamp++;
      return &found->second;
   }

   Stage47ScaledAsset *src_img = stage47_get_scaled_asset(path, w, h, false);
   if (!src_img) return nullptr;
   const int aw = w + outer * 2;
   const int ah = h + outer * 2;
   const uint16_t INF = 0x3fff;
   std::vector<uint16_t> dist((size_t)aw * ah, INF);
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
         if (src_img->alpha[(size_t)y * w + x] >= 18)
            dist[(size_t)(y + outer) * aw + x + outer] = 0;

   // 3/4 chamfer distance transform: compute the expanded silhouette once per
   // asset+size, never probe multiple alpha neighbours on every frame.
   for (int y = 0; y < ah; ++y)
      for (int x = 0; x < aw; ++x)
      {
         uint16_t &d = dist[(size_t)y * aw + x];
         if (x > 0) d = std::min<uint16_t>(d, dist[(size_t)y * aw + x - 1] + 3);
         if (y > 0) d = std::min<uint16_t>(d, dist[(size_t)(y - 1) * aw + x] + 3);
         if (x > 0 && y > 0) d = std::min<uint16_t>(d, dist[(size_t)(y - 1) * aw + x - 1] + 4);
         if (x + 1 < aw && y > 0) d = std::min<uint16_t>(d, dist[(size_t)(y - 1) * aw + x + 1] + 4);
      }
   for (int y = ah - 1; y >= 0; --y)
      for (int x = aw - 1; x >= 0; --x)
      {
         uint16_t &d = dist[(size_t)y * aw + x];
         if (x + 1 < aw) d = std::min<uint16_t>(d, dist[(size_t)y * aw + x + 1] + 3);
         if (y + 1 < ah) d = std::min<uint16_t>(d, dist[(size_t)(y + 1) * aw + x] + 3);
         if (x + 1 < aw && y + 1 < ah) d = std::min<uint16_t>(d, dist[(size_t)(y + 1) * aw + x + 1] + 4);
         if (x > 0 && y + 1 < ah) d = std::min<uint16_t>(d, dist[(size_t)(y + 1) * aw + x - 1] + 4);
      }

   Stage421AuraLayer layer;
   layer.w = aw; layer.h = ah; layer.pad = outer; layer.stamp = stage421_aura_stamp++;
   layer.alpha.assign((size_t)aw * ah, 0);
   const int maxd = outer * 3;
   for (size_t i = 0; i < dist.size(); ++i)
   {
      const int d = dist[i];
      if (d > maxd) continue;
      const int remain = maxd - d;
      const int a = strength * remain * remain / std::max(1, maxd * maxd);
      layer.alpha[i] = (unsigned char)std::max(0, std::min(235, a));
   }

   if (stage421_aura_cache.size() >= STAGE421_AURA_CACHE_LIMIT)
   {
      auto oldest = std::min_element(stage421_aura_cache.begin(), stage421_aura_cache.end(),
            [](const std::pair<const std::string, Stage421AuraLayer> &a,
               const std::pair<const std::string, Stage421AuraLayer> &b) {
               return a.second.stamp < b.second.stamp;
            });
      if (oldest != stage421_aura_cache.end()) stage421_aura_cache.erase(oldest);
   }
   auto inserted = stage421_aura_cache.emplace(key, std::move(layer));
   return &inserted.first->second;
}

static void stage421_draw_aura(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int outer, int strength)
{
   Stage421AuraLayer *aura = stage421_get_aura(path, w, h, outer, strength);
   if (!aura) return;
   const int ox = x - aura->pad;
   const int oy = y - aura->pad;
   const uint16_t hot = pack1555(90, 235, 255);
   const uint16_t cool = pack1555(23, 109, 230);

   // Draw in 2x2 pixel blocks: one alpha lookup/blend decision for four output
   // pixels. This matches the pixel-art style and cuts aura overhead sharply.
   for (int yy = 0; yy < aura->h; yy += 2)
      for (int xx = 0; xx < aura->w; xx += 2)
      {
         const int a = aura->alpha[(size_t)yy * aura->w + xx];
         if (a < 4) continue;
         const uint16_t glow = a > 125 ? hot : cool;
         for (int by = 0; by < 2; ++by)
         {
            const int dy = oy + yy + by;
            if (dy < 0 || dy >= (int)fb.h) continue;
            uint16_t *row = fb_row(fb, dy);
            for (int bx = 0; bx < 2; ++bx)
            {
               const int dx = ox + xx + bx;
               if (dx < 0 || dx >= (int)fb.w) continue;
               row[dx] = stage48_blend_pixel(row[dx], glow, a);
            }
         }
      }
}

static void stage421_present_rows(Fb &physical, const Stage42Backbuffer &back,
      int y0, int y1)
{
   if (!physical.mem || !back.fb.mem) return;
   y0 = std::max(0, y0); y1 = std::min((int)physical.h, y1);
   if (y0 >= y1) return;
   const size_t row_bytes = (size_t)physical.w * 2U;
   for (int y = y0; y < y1; ++y)
      memcpy(physical.mem + (size_t)y * physical.stride,
             back.fb.mem + (size_t)y * back.fb.stride, row_bytes);
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
}
'''
src = src.replace(struct_anchor, struct_anchor + helpers, 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f'start anchor missing: {start}')
    b = text.find(end, a + len(start))
    if b < 0:
        raise SystemExit(f'end anchor missing: {end}')
    return text[:a] + replacement.rstrip() + '\n\n' + text[b:]

# Rebuild the cartridge/case draw path around Stage4.7's scaled-asset cache.
media_func = r'''static bool stage416_draw_media_contain(Fb &fb, const std::string &path,
      int box_x, int box_y, int box_w, int box_h, Stage416MediaRect &out)
{
   Stage45Asset *source = stage45_get_asset(path);
   if (!source || source->w <= 0 || source->h <= 0 || box_w <= 0 || box_h <= 0)
      return false;
   const double sx = (double)box_w / source->w;
   const double sy = (double)box_h / source->h;
   const double scale = std::min(sx, sy);
   const int dw = std::max(1, (int)std::lround(source->w * scale));
   const int dh = std::max(1, (int)std::lround(source->h * scale));
   const int dx = box_x + (box_w - dw) / 2;

   Stage47ScaledAsset *scaled = stage47_get_scaled_asset(path, dw, dh, false);
   if (!scaled) return false;
   const Stage421AlphaBounds bounds = stage421_get_bounds(path, dw, dh, 8);
   const int visible_bottom = bounds.bottom >= 0 ? bounds.bottom : dh - 1;
   const int visible_floor = box_y + box_h - 1;
   const int dy = visible_floor - visible_bottom;
   stage421_blit_scaled(fb, scaled, dx, dy, 255);

   out.x = dx; out.y = dy; out.w = dw; out.h = dh;
   return true;
}'''
src = replace_between(src, 'static bool stage416_draw_media_contain(',
                      'static Stage415LabelRect stage416_label_rect(', media_func)

# Narrower cached cartridge/case aura: 27px -> 18px.
old_media_aura = 'stage420_media_aura(fb, media, mr, 218);'
if old_media_aura not in src:
    raise SystemExit('Stage4.20 media aura call missing')
src = src.replace(old_media_aura,
                  'stage421_draw_aura(fb, media, mr.x, mr.y, mr.w, mr.h, 18, 220);', 1)

# Smooth the system item geometry itself, preserving Stage4.13's final range.
if 'const int content_h = selected ? 138 : 114;' not in src:
    raise SystemExit('final Stage4.20 content_h anchor missing')
src = src.replace('const int content_h = selected ? 138 : 114;',
                  'const int content_h = (int)std::lround(114.0f + 24.0f * f);', 1)
if 'const int y = selected ? 66 : 82;' not in src:
    raise SystemExit('final Stage4.20 system y anchor missing')
src = src.replace('const int y = selected ? 66 : 82;',
                  'const int y = (int)std::lround(82.0f - 16.0f * f);', 1)

# Replace Stage4.20's geometric system aura and controller draw with the exact
# controller-PNG alpha silhouette. The visual alpha center follows the original
# interpolation path (126 -> 119), so NES/SEGA sit on the same visual axis.
system_a = src.find('      if (selected && focus == FocusZone::Systems)')
system_b = src.find('      if (!drawn)', system_a + 1)
if system_a < 0 or system_b < 0:
    raise SystemExit('system aura structural anchors missing')
new_system = r'''      const std::string asset = stage45_controller_asset(s);
      bool drawn = false;
      if (!asset.empty())
      {
         const int margin_x = (int)std::lround(5.0f + 10.0f * f);
         const int bw = std::max(24, w - margin_x * 2);
         const int bh = std::max(24, (int)std::lround(80.0f + 8.0f * f));
         const int bx = cx - bw / 2;
         int by = y + (int)std::lround(4.0f + 5.0f * f);
         Stage47ScaledAsset *ctrl = stage47_get_scaled_asset(asset, bw, bh, false);
         if (ctrl)
         {
            const Stage421AlphaBounds bounds = stage421_get_bounds(asset, bw, bh, 18);
            if (bounds.bottom >= bounds.top)
            {
               const int visible_cy = (bounds.top + bounds.bottom) / 2;
               const int shared_cy = (int)std::lround(126.0f - 7.0f * f);
               by = shared_cy - visible_cy;
            }
            if (selected && focus == FocusZone::Systems)
               stage421_draw_aura(fb, asset, bx, by, bw, bh, 18, 220);
            drawn = stage421_blit_scaled(fb, ctrl, bx, by, 255);
         }
      }
'''
src = src[:system_a] + new_system.rstrip() + '\n' + src[system_b:]

# Copy only the framebuffer bands that can actually change during an animation.
# Game scrolling leaves the top systems area untouched. System scrolling starts
# at y=40 to include the controller aura safely.
redraw_block = '''         stage42_draw_ui(back.fb, systems, visible_pos, vis, game_pos, focus,
                         game_shift, system_shift);
         stage42_present(physical, back);'''
new_redraw_block = '''         stage42_draw_ui(back.fb, systems, visible_pos, vis, game_pos, focus,
                         game_shift, system_shift);
         if (game_active && !system_active)
            stage421_present_rows(physical, back, 286, 680);
         else if (system_active)
            stage421_present_rows(physical, back, 40, 680);
         else
            stage42_present(physical, back);'''
if redraw_block not in src:
    raise SystemExit('main redraw/present block missing')
src = src.replace(redraw_block, new_redraw_block, 1)

if '      usleep(animating ? 4000 : 10000);' not in src:
    raise SystemExit('main loop sleep anchor missing')
src = src.replace('      usleep(animating ? 4000 : 10000);',
                  '      usleep(animating ? 1500 : 8000);', 1)

# Stage4.20 geometry-based aura helpers remain compiled but are no longer called.
# Keeping them avoids touching unrelated proven code and has zero runtime cost.
layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE421_AURA cached-chamfer alpha-silhouette outer18 strength220 media+systems block2x2\\n");\n'
    '   printf("STAGE421_SYSTEM_ALIGN alpha-center path126-to119 geometry114-to138 y82-to66\\n");\n'
    '   printf("STAGE421_PERF cached-alpha-bounds cached-media-resize cached-rom-cover partial-present game286-680 system40-680\\n");\n'
    '   printf("STAGE421_TIMING frame16ms anim128ms continuous-focus sleep1.5ms\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE421_SMOOTH_CACHED_AURA_FINAL_PATCH_OK {src_path} -> {out_path}')
