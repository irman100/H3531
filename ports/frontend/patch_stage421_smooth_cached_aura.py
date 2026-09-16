#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.20 PixelStation silhouette-aura UI active'
new_marker = 'Stage4.21 PixelStation cached-silhouette smooth-60fps UI active'
if old_marker not in src:
    raise SystemExit('Stage4.20 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Smooth frame pacing: ~60 Hz, enough animation samples to avoid the five-step
# feel of Stage4.20 while staying responsive.
for name, value in {
    'STAGE42_GAME_ANIM_MS': 128,
    'STAGE42_SYSTEM_ANIM_MS': 128,
    'STAGE42_FRAME_MS': 16,
}.items():
    src, n = re.subn(rf'static const uint64_t {name} = \d+;',
                     f'static const uint64_t {name} = {value};', src, count=1)
    if n != 1:
        raise SystemExit(f'constant missing: {name}')

old_quant = '''static float stage47_quantize_focus(float f)\n{\n   f = stage42_clamp01(f);\n   return std::round(f * 4.0f) * 0.25f;\n}'''
new_quant = '''static float stage47_quantize_focus(float f)\n{\n   f = stage42_clamp01(f);\n   return std::round(f * 8.0f) * 0.125f;\n}'''
if old_quant not in src:
    raise SystemExit('Stage4.20 focus quantizer not found')
src = src.replace(old_quant, new_quant, 1)

# Late cartridge-media patches had regressed to the uncached cover scaler.
old_cover = 'stage45_draw_game_cover(fb, g.image_path, r.x, r.y, r.w, r.h, 2)'
new_cover = 'stage47_draw_game_cover_cached(fb, g.image_path, r.x, r.y, r.w, r.h, 2)'
if old_cover not in src:
    raise SystemExit('ROM label cover scaler anchor missing')
src = src.replace(old_cover, new_cover, 1)

struct_anchor = '''struct Stage416MediaRect {\n   int x = 0, y = 0, w = 0, h = 0;\n};'''
if struct_anchor not in src:
    raise SystemExit('Stage416MediaRect anchor missing')

helpers = r'''

struct Stage421AlphaBounds {
   int left = 0, top = 0, right = -1, bottom = -1;
};

static Stage421AlphaBounds stage421_alpha_bounds(const Stage47ScaledAsset *img, int threshold = 8)
{
   Stage421AlphaBounds b;
   if (!img || img->w <= 0 || img->h <= 0) return b;
   b.left = img->w; b.top = img->h; b.right = -1; b.bottom = -1;
   for (int y = 0; y < img->h; ++y)
      for (int x = 0; x < img->w; ++x)
         if (img->alpha[(size_t)y * img->w + x] >= threshold)
         {
            b.left = std::min(b.left, x); b.right = std::max(b.right, x);
            b.top = std::min(b.top, y); b.bottom = std::max(b.bottom, y);
         }
   if (b.right < b.left || b.bottom < b.top)
      b = Stage421AlphaBounds{};
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
static const size_t STAGE421_AURA_CACHE_LIMIT = 40;

static Stage421AuraLayer *stage421_get_aura(const std::string &path,
      int w, int h, int outer, int strength)
{
   if (path.empty() || w <= 0 || h <= 0 || outer <= 0) return nullptr;
   const std::string key = path + "#" + std::to_string(w) + "x" + std::to_string(h) +
         "#o" + std::to_string(outer) + "#s" + std::to_string(strength);
   auto found = stage421_aura_cache.find(key);
   if (found != stage421_aura_cache.end())
   {
      found->second.stamp = stage421_aura_stamp++;
      return &found->second;
   }

   Stage47ScaledAsset *src = stage47_get_scaled_asset(path, w, h, false);
   if (!src) return nullptr;
   const int aw = w + outer * 2;
   const int ah = h + outer * 2;
   const uint16_t INF = 0x3fff;
   std::vector<uint16_t> dist((size_t)aw * ah, INF);
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
         if (src->alpha[(size_t)y * w + x] >= 18)
            dist[(size_t)(y + outer) * aw + (x + outer)] = 0;

   // Two-pass 3/4 chamfer distance transform. This replaces Stage4.20's
   // repeated multi-radius alpha probes on every animation frame.
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

   Stage421AuraLayer out;
   out.w = aw; out.h = ah; out.pad = outer; out.stamp = stage421_aura_stamp++;
   out.alpha.assign((size_t)aw * ah, 0);
   const int maxd = outer * 3;
   for (size_t i = 0; i < dist.size(); ++i)
   {
      const int d = dist[i];
      if (d > maxd) continue;
      const int remain = maxd - d;
      int a = strength * remain * remain / std::max(1, maxd * maxd);
      out.alpha[i] = (unsigned char)std::max(0, std::min(235, a));
   }

   if (stage421_aura_cache.size() >= STAGE421_AURA_CACHE_LIMIT)
   {
      auto it = std::min_element(stage421_aura_cache.begin(), stage421_aura_cache.end(),
            [](const auto &a, const auto &b) { return a.second.stamp < b.second.stamp; });
      if (it != stage421_aura_cache.end()) stage421_aura_cache.erase(it);
   }
   auto inserted = stage421_aura_cache.emplace(key, std::move(out));
   return &inserted.first->second;
}

static void stage421_draw_aura(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int outer, int strength)
{
   Stage421AuraLayer *aura = stage421_get_aura(path, w, h, outer, strength);
   if (!aura) return;
   const int ox = x - aura->pad;
   const int oy = y - aura->pad;
   const uint16_t hot = pack1555(87, 231, 255);
   const uint16_t cool = pack1555(23, 107, 228);
   for (int yy = 0; yy < aura->h; ++yy)
   {
      const int dy = oy + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dy);
      for (int xx = 0; xx < aura->w; ++xx)
      {
         const int dx = ox + xx;
         if (dx < 0 || dx >= (int)fb.w) continue;
         const int av = aura->alpha[(size_t)yy * aura->w + xx];
         if (av < 4) continue;
         row[dx] = stage48_blend_pixel(row[dx], av > 125 ? hot : cool, av);
      }
   }
}

static void stage421_present_rows(Fb &physical, const Stage42Backbuffer &back,
      int y0, int y1)
{
   if (!physical.mem || !back.fb.mem) return;
   y0 = std::max(0, y0);
   y1 = std::min((int)physical.h, y1);
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

# Cached cartridge/case scaling + visible-alpha floor alignment.
def between(text, start, end, replacement):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + replacement.rstrip() + '\n\n' + text[b:]

media_func = r'''static bool stage416_draw_media_contain(Fb &fb, const std::string &path,
      int box_x, int box_y, int box_w, int box_h, Stage416MediaRect &out)
{
   Stage45Asset *src_img = stage45_get_asset(path);
   if (!src_img || src_img->w <= 0 || src_img->h <= 0 || box_w <= 0 || box_h <= 0)
      return false;

   const double sx = (double)box_w / src_img->w;
   const double sy = (double)box_h / src_img->h;
   const double s = std::min(sx, sy);
   const int dw = std::max(1, (int)std::lround(src_img->w * s));
   const int dh = std::max(1, (int)std::lround(src_img->h * s));
   const int dx = box_x + (box_w - dw) / 2;
   Stage47ScaledAsset *scaled = stage47_get_scaled_asset(path, dw, dh, false);
   if (!scaled) return false;

   const Stage421AlphaBounds bounds = stage421_alpha_bounds(scaled, 8);
   const int visible_bottom = bounds.bottom >= 0 ? bounds.bottom : dh - 1;
   const int visible_floor = box_y + box_h - 1;
   const int dy = visible_floor - visible_bottom;
   stage421_blit_scaled(fb, scaled, dx, dy, 255);

   out.x = dx; out.y = dy; out.w = dw; out.h = dh;
   return true;
}'''
src = between(src, 'static bool stage416_draw_media_contain(', 'static Stage415LabelRect stage416_label_rect(', media_func)

# Replace expensive Stage4.20 per-frame media aura with the cached narrower contour.
old_media_call = 'stage420_media_aura(fb, media, mr, 218);'
new_media_call = 'stage421_draw_aura(fb, media, mr.x, mr.y, mr.w, mr.h, 18, 220);'
if old_media_call not in src:
    raise SystemExit('Stage4.20 media aura call missing')
src = src.replace(old_media_call, new_media_call, 1)

# System controller aura now uses the exact PNG alpha silhouette. Also align the
# visible alpha center of NES/SEGA/etc. to one shared horizontal axis.
old_system_aura = '''      if (selected && focus == FocusZone::Systems)\n      {\n         const float stage420_scale = 0.76f + f * 0.34f;\n         const int stage420_pad_cy = y + 35;\n         stage420_system_aura(fb, s, cx, stage420_pad_cy, stage420_scale, 210);\n      }\n\n      const std::string asset = stage45_controller_asset(s);\n      bool drawn = false;\n      if (!asset.empty())\n      {\n         const int bx = selected ? x + 15 : x + 5;\n         const int by = selected ? y + 10 : y + 4;\n         const int bw = selected ? w - 30 : w - 10;\n         const int bh = selected ? 91 : 82;\n         drawn = stage47_draw_cached_asset(fb, asset, bx, by, bw, bh, false, 255);\n      }'''
new_system_aura = '''      const std::string asset = stage45_controller_asset(s);\n      bool drawn = false;\n      if (!asset.empty())\n      {\n         const int margin_x = (int)std::lround(5.0f + 10.0f * f);\n         const int bw = std::max(24, w - margin_x * 2);\n         const int bh = (int)std::lround(82.0f + 9.0f * f);\n         const int bx = cx - bw / 2;\n         int by = y + (int)std::lround(4.0f + 6.0f * f);\n         Stage47ScaledAsset *ctrl = stage47_get_scaled_asset(asset, bw, bh, false);\n         if (ctrl)\n         {\n            const Stage421AlphaBounds ab = stage421_alpha_bounds(ctrl, 18);\n            if (ab.bottom >= ab.top)\n            {\n               const int visible_cy = (ab.top + ab.bottom) / 2;\n               const int shared_cy = 154;\n               by = shared_cy - visible_cy;\n            }\n            if (selected && focus == FocusZone::Systems)\n               stage421_draw_aura(fb, asset, bx, by, bw, bh, 18, 220);\n            drawn = stage421_blit_scaled(fb, ctrl, bx, by, 255);\n         }\n      }'''
if old_system_aura not in src:
    raise SystemExit('Stage4.20 system aura/draw block missing')
src = src.replace(old_system_aura, new_system_aura, 1)

# Smooth the system card geometry itself instead of jumping at the selected threshold.
src = src.replace('const int content_h = selected ? 142 : 116;',
                  'const int content_h = (int)std::lround(116.0f + 26.0f * f);', 1)
src = src.replace('const int y = selected ? 99 : 111;',
                  'const int y = (int)std::lround(111.0f - 12.0f * f);', 1)

# Partial framebuffer present during animation: game scrolling does not need to
# rewrite the static top ~280 rows, while system changes update both bands.
old_present = '         stage42_present(physical, back);'
new_present = '''         if (game_active && !system_active)\n            stage421_present_rows(physical, back, 286, 680);\n         else if (system_active)\n            stage421_present_rows(physical, back, 72, 680);\n         else\n            stage42_present(physical, back);'''
if old_present not in src:
    raise SystemExit('present call missing')
src = src.replace(old_present, new_present, 1)

src = src.replace('      usleep(animating ? 4000 : 10000);',
                  '      usleep(animating ? 1500 : 8000);', 1)

# Diagnostics.
layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE421_AURA cached-chamfer alpha-silhouette outer18 strength220 media+systems\\n");\n'
    '   printf("STAGE421_SMOOTH frame16ms anim128ms focus-steps=8 controller-visible-center=154\\n");\n'
    '   printf("STAGE421_PERF cached-media-resize cached-rom-cover partial-present game286-680 system72-680\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE421_SMOOTH_CACHED_AURA_PATCH_OK {src_path} -> {out_path}')
