#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage48_perspective_polish.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.7 PixelStation focus-performance UI active'
new_marker = 'Stage4.8 PixelStation perspective-polish UI active'
if old_marker not in src:
    raise SystemExit("expected Stage4.7 marker not found")
src = src.replace(old_marker, new_marker, 1)

replacements = {
    'static const uint64_t STAGE42_GAME_ANIM_MS = 95;': 'static const uint64_t STAGE42_GAME_ANIM_MS = 128;',
    'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 95;': 'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 128;',
    'static const uint64_t STAGE42_FRAME_MS = 20;': 'static const uint64_t STAGE42_FRAME_MS = 16;',
    'static const size_t STAGE47_COVER_CACHE_LIMIT = 36;': 'static const size_t STAGE47_COVER_CACHE_LIMIT = 56;',
    'if (stage47_scaled_assets.size() > 72) stage47_scaled_assets.clear();': 'if (stage47_scaled_assets.size() > 128) stage47_scaled_assets.clear();',
}
for old, new in replacements.items():
    if old not in src:
        raise SystemExit(f"Stage4.8 constant not found: {old}")
    src = src.replace(old, new, 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"start anchor not found: {start}")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"end anchor not found: {end}")
    return text[:a] + replacement.rstrip() + "\n\n" + text[b:]

quant = r'''static float stage47_quantize_focus(float f)
{
   f = stage42_clamp01(f);
   return std::round(f * 8.0f) * 0.125f;
}
'''
src = replace_between(src, 'static float stage47_quantize_focus(float f)', 'static bool stage47_draw_background_cached', quant)

helpers = r'''struct Stage48ExactAsset {
   int w = 0;
   int h = 0;
   std::vector<uint16_t> pixels;
   std::vector<unsigned char> alpha;
};

static std::map<std::string, Stage48ExactAsset> stage48_exact_assets;

static Stage48ExactAsset *stage48_get_exact_asset(const std::string &path, int w, int h)
{
   if (w <= 0 || h <= 0) return nullptr;
   const std::string key = path + "#exact#" + std::to_string(w) + "x" + std::to_string(h);
   auto it = stage48_exact_assets.find(key);
   if (it != stage48_exact_assets.end()) return &it->second;
   Stage45Asset *src = stage45_get_asset(path);
   if (!src || src->w <= 0 || src->h <= 0) return nullptr;
   if (stage48_exact_assets.size() > 128) stage48_exact_assets.clear();

   Stage48ExactAsset out;
   out.w = w;
   out.h = h;
   out.pixels.resize((size_t)w * h);
   out.alpha.resize((size_t)w * h);
   for (int y = 0; y < h; ++y)
   {
      const int sy = std::min(src->h - 1, (int)((int64_t)y * src->h / h));
      for (int x = 0; x < w; ++x)
      {
         const int sx = std::min(src->w - 1, (int)((int64_t)x * src->w / w));
         const unsigned char *p = src->rgba.data() + ((size_t)sy * src->w + sx) * 4U;
         const size_t pos = (size_t)y * w + x;
         out.pixels[pos] = pack1555(p[0], p[1], p[2]);
         out.alpha[pos] = p[3];
      }
   }
   auto inserted = stage48_exact_assets.emplace(key, std::move(out));
   return &inserted.first->second;
}

static int stage48_row_shear(int row, int h, int shear)
{
   if (!shear || h <= 1) return 0;
   return (int)std::lround((double)shear * ((double)h * 0.5 - row) / h);
}

static bool stage48_draw_exact_asset(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int opacity = 255, int shear = 0)
{
   Stage48ExactAsset *img = stage48_get_exact_asset(path, w, h);
   if (!img) return false;
   opacity = std::max(0, std::min(255, opacity));
   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      const int sh = stage48_row_shear(yy, h, shear);
      for (int xx = 0; xx < w; ++xx)
      {
         const int dx = x + xx + sh;
         if (dx < 0 || dx >= (int)fb.w) continue;
         const size_t pos = (size_t)yy * w + xx;
         const int a = ((int)img->alpha[pos] * opacity + 127) / 255;
         if (a <= 0) continue;
         const uint16_t sp = img->pixels[pos];
         if (a >= 250) fb_row(fb, dy)[dx] = sp;
         else
         {
            const int sr = ((sp >> 10) & 31) * 255 / 31;
            const int sg = ((sp >> 5) & 31) * 255 / 31;
            const int sb = (sp & 31) * 255 / 31;
            uint16_t *dst = fb_row(fb, dy) + dx;
            *dst = stage46_blend1555(*dst, (unsigned char)sr, (unsigned char)sg,
                  (unsigned char)sb, (unsigned char)a);
         }
      }
   }
   return true;
}

static bool stage48_draw_cover_shear(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int radius, int shear)
{
   Stage47CoverCache *img = stage47_get_cover(path, w, h, radius);
   if (!img) return false;
   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      const int sh = stage48_row_shear(yy, h, shear);
      int left = std::max((int)img->left[yy], -x - sh);
      int right = std::min((int)img->right[yy], (int)fb.w - x - sh);
      if (left >= right) continue;
      memcpy(fb_row(fb, dy) + x + sh + left,
             img->pixels.data() + (size_t)yy * w + left,
             (size_t)(right - left) * sizeof(uint16_t));
   }
   return true;
}

static void stage48_draw_system_panel(Fb &fb, int x, int y, int w, int h, bool focused)
{
   const int margin = 14;
   stage48_draw_exact_asset(fb, stage46_ui_asset("system_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2, 255, 0);
   stage48_draw_exact_asset(fb,
         stage46_ui_asset(focused ? "system_frame_focus.png" : "system_frame_idle.png"),
         x, y, w, h, 255, 0);
}

static void stage48_draw_rom_panel(Fb &fb, int x, int y, int w, int h,
      bool focused, int shear)
{
   const int margin = 12;
   stage48_draw_exact_asset(fb, stage46_ui_asset("rom_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2, 255, shear);
   stage48_draw_exact_asset(fb,
         stage46_ui_asset(focused ? "rom_frame_focus.png" : "rom_frame_idle.png"),
         x, y, w, h, 255, shear);
}

static uint16_t stage48_blend_pixel(uint16_t dst, uint16_t src, int alpha)
{
   alpha = std::max(0, std::min(255, alpha));
   const int sr = ((src >> 10) & 31) * 255 / 31;
   const int sg = ((src >> 5) & 31) * 255 / 31;
   const int sb = (src & 31) * 255 / 31;
   return stage46_blend1555(dst, (unsigned char)sr, (unsigned char)sg,
         (unsigned char)sb, (unsigned char)alpha);
}

static void stage48_reflect_card(Fb &fb, int x, int y, int w, int h, int reflect_h)
{
   if (reflect_h <= 0) return;
   const int dst0 = y + h + 3;
   for (int r = 0; r < reflect_h; ++r)
   {
      const int sy = y + h - 1 - std::min(h - 1, r * 2);
      const int dy = dst0 + r;
      if (sy < 0 || sy >= (int)fb.h || dy < 0 || dy >= (int)fb.h) continue;
      const int alpha = std::max(0, 68 - r * 58 / std::max(1, reflect_h - 1));
      uint16_t *dst = fb_row(fb, dy);
      const uint16_t *sp = fb_row(fb, sy);
      const int left = std::max(0, x - 8);
      const int right = std::min((int)fb.w, x + w + 8);
      for (int xx = left; xx < right; ++xx)
         dst[xx] = stage48_blend_pixel(dst[xx], sp[xx], alpha);
   }
}
'''

anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit("Stage4.8 system row anchor not found")
src = src.replace(anchor, helpers + '\n\n' + anchor, 1)

system_row = r'''static void stage42_draw_system_row(Fb &fb, const std::vector<SystemDef> &systems,
      const std::vector<size_t> &vis, size_t visible_pos, FocusZone focus, float system_shift)
{
   if (vis.empty()) return;
   const int center = (int)fb.w / 2;
   const uint16_t text = pack1555(228, 240, 255);
   const uint16_t dim = pack1555(122, 151, 190);

   std::vector<Stage42SystemVisual> items;
   for (size_t pos = 0; pos < vis.size(); ++pos)
   {
      const float effective = (float)((int)pos - (int)visible_pos) + system_shift;
      if (std::fabs(effective) > 3.20f) continue;
      Stage42SystemVisual sv;
      sv.pos = pos;
      sv.rel = effective;
      sv.focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));
      items.push_back(sv);
   }
   std::sort(items.begin(), items.end(), [](const Stage42SystemVisual &a, const Stage42SystemVisual &b) {
      return a.focus < b.focus;
   });

   for (const auto &sv : items)
   {
      const SystemDef &s = systems[vis[sv.pos]];
      const float f = stage47_quantize_focus(sv.focus);
      const int w = (int)std::lround(STAGE42_SYSTEM_SMALL_W +
            (STAGE42_SYSTEM_SELECTED_W - STAGE42_SYSTEM_SMALL_W) * f);
      const int cx = center + (int)std::lround(stage42_system_center_offset(sv.rel));
      const bool selected = std::fabs(sv.rel) < 0.50f;
      const int content_h = selected ? 138 : 114;
      const int x = cx - w / 2;
      const int y = selected ? 101 : 112;

      if (selected)
      {
         const int frame_pad_x = 14;
         const int frame_pad_y = 11;
         stage48_draw_system_panel(fb, x - frame_pad_x, y - frame_pad_y,
               w + frame_pad_x * 2, content_h + frame_pad_y * 2,
               focus == FocusZone::Systems);
      }

      const std::string asset = stage45_controller_asset(s);
      bool drawn = false;
      if (!asset.empty())
      {
         const int bx = selected ? x + 15 : x + 5;
         const int by = selected ? y + 9 : y + 4;
         const int bw = selected ? w - 30 : w - 10;
         const int bh = selected ? 88 : 80;
         drawn = stage47_draw_cached_asset(fb, asset, bx, by, bw, bh, false, 255);
      }
      if (!drawn)
      {
         const SystemVisualSpec &visual = stage43_visual_spec(s);
         const uint16_t body = selected ? pack1555(196, 205, 216) : pack1555(103, 117, 135);
         const uint16_t ink = selected ? pack1555(21, 26, 34) : pack1555(36, 46, 60);
         const uint16_t accent = pack1555(visual.accent_r, visual.accent_g, visual.accent_b);
         stage43_draw_system_icon(fb, s, cx, y + 51, selected ? 1.08f : 0.87f, body, ink, accent);
      }

      const std::string label = stage42_system_short_name(s);
      const int scale = selected ? 2 : 1;
      const int tw = text_width(label, scale);
      const int ly = selected ? y + 111 : 227;
      draw_text(fb, cx - tw / 2, ly, label, scale,
            selected ? (focus == FocusZone::Systems ? text : dim) : dim);
   }
}
'''
src = replace_between(src, 'static void stage42_draw_system_row(', 'static void stage42_draw_games(', system_row)

games = r'''static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;

   const uint16_t text = pack1555(237, 246, 255);
   const uint16_t dim = pack1555(126, 151, 188);
   const int center = (int)fb.w / 2;
   const int card_center_y = 448;

   std::map<size_t, Stage42CardVisual> unique;
   for (int rel = -4; rel <= 4; ++rel)
   {
      const float effective = (float)rel + game_shift;
      if (std::fabs(effective) > 3.35f) continue;
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      const size_t idx = (size_t)gp;
      const float card_focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));
      auto it = unique.find(idx);
      if (it == unique.end() || stage42_should_replace_duplicate(it->second.rel, effective, game_shift))
      {
         Stage42CardVisual cv; cv.index = idx; cv.rel = effective; cv.focus = card_focus;
         unique[idx] = cv;
      }
   }

   std::vector<Stage42CardVisual> cards;
   for (auto &kv : unique) cards.push_back(kv.second);
   std::sort(cards.begin(), cards.end(), [](const Stage42CardVisual &a, const Stage42CardVisual &b) {
      return a.focus < b.focus;
   });

   for (const auto &cv : cards)
   {
      const Game &g = sys.games[cv.index];
      const float f = stage47_quantize_focus(cv.focus);
      int w = (int)std::lround(STAGE42_SMALL_W + (STAGE42_SELECTED_W - STAGE42_SMALL_W) * f);
      int h = (int)std::lround(STAGE42_SMALL_H + (STAGE42_SELECTED_H - STAGE42_SMALL_H) * f);
      const bool outer = std::fabs(cv.rel) >= 2.35f;
      if (outer) { w = w * 92 / 100; h = h * 97 / 100; }
      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;
      const int y = card_center_y - h / 2 + (outer ? 4 : 0);
      const bool selected = std::fabs(cv.rel) < 0.50f;
      const int pad = STAGE42_CARD_PAD;
      const int fx = x - pad;
      const int fy = y - pad;
      const int fw = w + pad * 2;
      const int fh = h + pad * 2;
      const int shear = outer ? (cv.rel < 0.0f ? 14 : -14) : 0;

      stage48_draw_rom_panel(fb, fx, fy, fw, fh,
            selected && focus == FocusZone::Games, shear);

      const int panel_margin = 12;
      const int art_gap_x = selected ? 9 : 7;
      const int brand_h = selected ? 23 : 19;
      const int art_x = fx + panel_margin + art_gap_x;
      const int art_y = fy + panel_margin + brand_h + 7;
      const int art_w = fw - (panel_margin + art_gap_x) * 2;
      const int art_h = fh - panel_margin * 2 - brand_h - 16;
      if (!stage48_draw_cover_shear(fb, g.image_path, art_x, art_y, art_w, art_h, 5, shear))
         draw_fallback_card(fb, sys, art_x, art_y, art_w, art_h);

      const std::string brand = stage44_brand_name(sys);
      const int bw = text_width(brand, 1);
      draw_text(fb, fx + fw / 2 - bw / 2, fy + panel_margin + 5, brand, 1,
            selected ? text : dim);

      stage48_draw_exact_asset(fb,
            stage46_ui_asset(selected && focus == FocusZone::Games ?
               "rom_frame_focus.png" : "rom_frame_idle.png"),
            fx, fy, fw, fh, 255, shear);

      stage48_reflect_card(fb, fx, fy, fw, fh, selected ? 20 : 14);
   }

   const Game &selected_game = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(selected_game.title, 2, 540);
   const int tw = text_width(title, 2);
   draw_text(fb, center - tw / 2, 610, title, 2,
         focus == FocusZone::Games ? text : dim);
   const std::string metadata = stage44_brand_name(sys) + "  *  " +
         std::to_string(game_pos + 1) + "/" + std::to_string(sys.games.size());
   const int mw = text_width(metadata, 1);
   draw_text(fb, center - mw / 2, 637, metadata, 1, dim);
}
'''
src = replace_between(src, 'static void stage42_draw_games(', 'static void stage42_draw_ui(', games)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE48_FRAME exact-stretch idle-focus-identical-geometry panel-margin-rom12-system14\\n");\n'
    '   printf("STAGE48_SMOOTH cadence=16ms anim=128ms focus-steps=8 cache-cover=56\\n");\n'
    '   printf("STAGE48_PERSPECTIVE outer-cards-shear=14 width=92pct reflection=scanline-fade\\n");\n'
)
if layout_ok not in src:
    raise SystemExit("Stage4.8 layout-test anchor not found")
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STAGE48_PERSPECTIVE_POLISH_PATCH_OK {src_path} -> {out_path}")
