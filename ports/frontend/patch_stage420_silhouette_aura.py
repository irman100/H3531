#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage420_silhouette_aura.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.19 PixelStation focus-aura UI active'
new_marker = 'Stage4.20 PixelStation silhouette-aura UI active'
if old_marker not in src:
    raise SystemExit('Stage4.19 marker not found')
src = src.replace(old_marker, new_marker, 1)


def between(text, start, end, repl):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + repl.rstrip() + '\n\n' + text[b:]

helper = r'''static inline bool stage420_media_opaque(Stage42Art *img,
      const Stage416MediaRect &m, int px, int py)
{
   if (!img || m.w <= 0 || m.h <= 0 || px < m.x || py < m.y ||
       px >= m.x + m.w || py >= m.y + m.h)
      return false;
   int sx = (int)((int64_t)(px - m.x) * img->w / std::max(1, m.w));
   int sy = (int)((int64_t)(py - m.y) * img->h / std::max(1, m.h));
   sx = std::max(0, std::min(img->w - 1, sx));
   sy = std::max(0, std::min(img->h - 1, sy));
   const size_t off = ((size_t)sy * img->w + (size_t)sx) * 4U + 3U;
   return img->rgba[off] >= 18;
}

static void stage420_media_aura(Fb &fb, const std::string &path,
      const Stage416MediaRect &m, int strength)
{
   Stage42Art *img = stage42_get_art(path);
   if (!img || m.w <= 0 || m.h <= 0 || strength <= 0) return;

   static const int radii[] = {0, 4, 8, 13, 19, 27};
   const int outer = radii[5];
   const int x0 = std::max(0, m.x - outer);
   const int y0 = std::max(0, m.y - outer);
   const int x1 = std::min((int)fb.w - 1, m.x + m.w - 1 + outer);
   const int y1 = std::min((int)fb.h - 1, m.y + m.h - 1 + outer);
   const uint16_t inner = pack1555(92, 236, 255);
   const uint16_t mid   = pack1555(43, 168, 255);
   const uint16_t outer_c = pack1555(20, 94, 216);

   for (int py = y0; py <= y1; py += 2)
   {
      for (int px = x0; px <= x1; px += 2)
      {
         int ring = -1;
         for (int ri = 0; ri < 6 && ring < 0; ++ri)
         {
            const int r = radii[ri];
            if (r == 0)
            {
               if (stage420_media_opaque(img, m, px, py)) ring = 0;
               continue;
            }
            const int d = (r * 7) / 10;
            if (stage420_media_opaque(img, m, px + r, py) ||
                stage420_media_opaque(img, m, px - r, py) ||
                stage420_media_opaque(img, m, px, py + r) ||
                stage420_media_opaque(img, m, px, py - r) ||
                stage420_media_opaque(img, m, px + d, py + d) ||
                stage420_media_opaque(img, m, px - d, py + d) ||
                stage420_media_opaque(img, m, px + d, py - d) ||
                stage420_media_opaque(img, m, px - d, py - d))
               ring = ri;
         }
         if (ring < 0) continue;

         static const int weights[] = {255, 235, 205, 165, 115, 68};
         int a = strength * weights[ring] / 255;
         a = std::max(0, std::min(235, a));
         const uint16_t glow = ring <= 1 ? inner : (ring <= 3 ? mid : outer_c);

         for (int by = 0; by < 2; ++by)
         {
            const int yy = py + by;
            if (yy < 0 || yy >= (int)fb.h) continue;
            uint16_t *row = fb_row(fb, yy);
            for (int bx = 0; bx < 2; ++bx)
            {
               const int xx = px + bx;
               if (xx < 0 || xx >= (int)fb.w) continue;
               row[xx] = stage48_blend_pixel(row[xx], glow, a);
            }
         }
      }
   }
}

static void stage420_system_aura(Fb &fb, const SystemDef &s,
      int cx, int cy, float scale, int strength)
{
   const bool nes = stage42_is_nes(s);
   const bool sega = stage42_is_sega(s);
   const int core_w = std::max(34, (int)std::lround((nes ? 112.0f : sega ? 124.0f : 116.0f) * scale));
   const int core_h = std::max(22, (int)std::lround((nes ? 42.0f : sega ? 54.0f : 50.0f) * scale));
   const int pad = std::max(18, (int)std::lround(24.0f * scale));
   const int x0 = std::max(0, cx - core_w / 2 - pad);
   const int x1 = std::min((int)fb.w - 1, cx + core_w / 2 + pad);
   const int y0 = std::max(0, cy - core_h / 2 - pad);
   const int y1 = std::min((int)fb.h - 1, cy + core_h / 2 + pad);
   const uint16_t inner = pack1555(96, 240, 255);
   const uint16_t outer_c = pack1555(27, 113, 232);

   const int hw = std::max(1, core_w / 2);
   const int hh = std::max(1, core_h / 2);

   for (int py = y0; py <= y1; py += 2)
   {
      for (int px = x0; px <= x1; px += 2)
      {
         const int ax = std::abs(px - cx);
         const int ay = std::abs(py - cy);
         double outside = 0.0;
         if (nes)
         {
            const int ox = std::max(0, ax - hw);
            const int oy = std::max(0, ay - hh);
            outside = std::sqrt((double)ox * ox + (double)oy * oy);
         }
         else
         {
            const double nx = (double)ax / hw;
            const double ny = (double)ay / hh;
            const double q = std::sqrt(nx * nx + ny * ny);
            outside = q <= 1.0 ? 0.0 : (q - 1.0) * std::min(hw, hh);
         }
         if (outside > pad) continue;
         const double t = 1.0 - outside / std::max(1, pad);
         int a = (int)std::lround(strength * t * t);
         if (a < 4) continue;
         a = std::min(235, a);
         const uint16_t glow = outside < pad * 0.38 ? inner : outer_c;
         for (int by = 0; by < 2; ++by)
         {
            const int yy = py + by;
            if (yy < 0 || yy >= (int)fb.h) continue;
            uint16_t *row = fb_row(fb, yy);
            for (int bx = 0; bx < 2; ++bx)
            {
               const int xx = px + bx;
               if (xx < 0 || xx >= (int)fb.w) continue;
               row[xx] = stage48_blend_pixel(row[xx], glow, a);
            }
         }
      }
   }
}
'''
src = between(src, 'static void stage419_focus_aura(', 'static void stage42_draw_system_row(', helper)

old_system = '''      if (selected && focus == FocusZone::Systems)
      {
         stage419_focus_aura(fb, x, y, w, content_h, 34, 24, 86);
      }'''
new_system = '''      if (selected && focus == FocusZone::Systems)
      {
         const float stage420_scale = 0.76f + f * 0.34f;
         const int stage420_pad_cy = y + 35;
         stage420_system_aura(fb, s, cx, stage420_pad_cy, stage420_scale, 210);
      }'''
if old_system not in src:
    raise SystemExit('Stage4.19 system aura block missing')
src = src.replace(old_system, new_system, 1)

old_media = '''      if (selected && focus == FocusZone::Games)
      {
         stage419_focus_aura(fb, mr.x, mr.y, mr.w, mr.h, 24, 18, 74);
         if (stage419_media_ok)
         {
            Stage416MediaRect stage419_restore;
            stage416_draw_media_contain(fb, media, fx, fy, w, h, stage419_restore);
         }
      }'''
new_media = '''      if (selected && focus == FocusZone::Games)
      {
         stage420_media_aura(fb, media, mr, 218);
         if (stage419_media_ok)
         {
            Stage416MediaRect stage420_restore;
            stage416_draw_media_contain(fb, media, fx, fy, w, h, stage420_restore);
         }
      }'''
if old_media not in src:
    raise SystemExit('Stage4.19 media aura block missing')
src = src.replace(old_media, new_media, 1)

src = src.replace('   printf("STAGE419_AURA systems86 media74 pixel-radial-backlight\\n");\n', '', 1)
layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE420_AURA media-alpha-silhouette rings0-4-8-13-19-27 strength218\\n");\n'
    '   printf("STAGE420_SYSTEM_AURA NES-box SEGA-ellipse shared-pad-center strength210\\n");\n'
    '   printf("STAGE420_FOCUS brighter contour-aware no-frame no-panel no-underline\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE420_SILHOUETTE_AURA_PATCH_OK {src_path} -> {out_path}')
