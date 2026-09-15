#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage47_focus_performance.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.6 PixelStation sprite-frame UI active'
new_marker = 'Stage4.7 PixelStation focus-performance UI active'
if old_marker not in src:
    raise SystemExit("expected Stage4.6 marker not found")
src = src.replace(old_marker, new_marker, 1)

# More breathing room around the grid panel, while preserving carousel gaps because
# stage42_card_center_offset() already accounts for STAGE42_CARD_PAD.
replacements = {
    'static const int STAGE42_CARD_PAD = 5;': 'static const int STAGE42_CARD_PAD = 10;',
    'static const uint64_t STAGE42_GAME_ANIM_MS = 125;': 'static const uint64_t STAGE42_GAME_ANIM_MS = 95;',
    'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 125;': 'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 95;',
}
for old, new in replacements.items():
    if old not in src:
        raise SystemExit(f"Stage4.7 constant not found: {old}")
    src = src.replace(old, new, 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"start anchor not found: {start}")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"end anchor not found: {end}")
    return text[:a] + replacement.rstrip() + "\n\n" + text[b:]

# Cached scaler/converter: PNG sampling and RGB->A1R5G5B5 conversion happen once
# for each target size. During animation we only blit cached 16-bit pixels.
helpers = r'''struct Stage47ScaledAsset {
   int w = 0;
   int h = 0;
   bool opaque = false;
   std::vector<uint16_t> pixels;
   std::vector<unsigned char> alpha;
};

static std::map<std::string, Stage47ScaledAsset> stage47_scaled_assets;

static std::string stage47_scaled_key(const std::string &path, int w, int h, bool cover)
{
   return path + "#" + std::to_string(w) + "x" + std::to_string(h) + (cover ? "#c" : "#f");
}

static Stage47ScaledAsset *stage47_get_scaled_asset(const std::string &path,
      int w, int h, bool cover)
{
   if (w <= 0 || h <= 0) return nullptr;
   const std::string key = stage47_scaled_key(path, w, h, cover);
   auto found = stage47_scaled_assets.find(key);
   if (found != stage47_scaled_assets.end()) return &found->second;

   Stage45Asset *src = stage45_get_asset(path);
   if (!src || src->w <= 0 || src->h <= 0) return nullptr;

   if (stage47_scaled_assets.size() > 72) stage47_scaled_assets.clear();

   Stage47ScaledAsset out;
   out.w = w;
   out.h = h;
   out.pixels.assign((size_t)w * h, 0);
   out.alpha.assign((size_t)w * h, 0);

   const double sx = (double)w / src->w;
   const double sy = (double)h / src->h;
   const double scale = cover ? std::max(sx, sy) : std::min(sx, sy);
   const int dw = std::max(1, (int)std::lround(src->w * scale));
   const int dh = std::max(1, (int)std::lround(src->h * scale));
   const int ox = (w - dw) / 2;
   const int oy = (h - dh) / 2;
   bool opaque = true;

   for (int y = 0; y < h; ++y)
   {
      for (int x = 0; x < w; ++x)
      {
         const int rx = x - ox;
         const int ry = y - oy;
         if (rx < 0 || ry < 0 || rx >= dw || ry >= dh)
         {
            opaque = false;
            continue;
         }
         const int ix = std::min(src->w - 1, (int)((int64_t)rx * src->w / dw));
         const int iy = std::min(src->h - 1, (int)((int64_t)ry * src->h / dh));
         const unsigned char *p = src->rgba.data() + ((size_t)iy * src->w + ix) * 4U;
         const size_t pos = (size_t)y * w + x;
         out.pixels[pos] = pack1555(p[0], p[1], p[2]);
         out.alpha[pos] = p[3];
         if (p[3] < 250) opaque = false;
      }
   }
   out.opaque = opaque;
   auto inserted = stage47_scaled_assets.emplace(key, std::move(out));
   return &inserted.first->second;
}

static bool stage47_draw_cached_asset(Fb &fb, const std::string &path,
      int x, int y, int w, int h, bool cover, int opacity = 255)
{
   Stage47ScaledAsset *img = stage47_get_scaled_asset(path, w, h, cover);
   if (!img) return false;
   opacity = std::max(0, std::min(255, opacity));

   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      int left = std::max(0, -x);
      int right = std::min(w, (int)fb.w - x);
      if (left >= right) continue;
      uint16_t *dst = fb_row(fb, dy) + x + left;
      const uint16_t *sp = img->pixels.data() + (size_t)yy * w + left;
      const unsigned char *sa = img->alpha.data() + (size_t)yy * w + left;

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
         else
         {
            const int sr = ((*sp >> 10) & 31) * 255 / 31;
            const int sg = ((*sp >> 5) & 31) * 255 / 31;
            const int sb = (*sp & 31) * 255 / 31;
            *dst = stage46_blend1555(*dst, (unsigned char)sr, (unsigned char)sg,
                  (unsigned char)sb, (unsigned char)a);
         }
      }
   }
   return true;
}

struct Stage47CoverCache {
   std::string key;
   int w = 0;
   int h = 0;
   uint64_t stamp = 0;
   std::vector<uint16_t> pixels;
   std::vector<uint16_t> left;
   std::vector<uint16_t> right;
};

static std::vector<Stage47CoverCache> stage47_cover_cache;
static uint64_t stage47_cover_stamp = 1;
static const size_t STAGE47_COVER_CACHE_LIMIT = 36;

static Stage47CoverCache *stage47_get_cover(const std::string &path,
      int w, int h, int radius)
{
   if (path.empty() || w <= 0 || h <= 0) return nullptr;
   const std::string key = path + "#" + std::to_string(w) + "x" + std::to_string(h) +
         "#r" + std::to_string(radius);
   for (auto &e : stage47_cover_cache)
   {
      if (e.key == key)
      {
         e.stamp = stage47_cover_stamp++;
         return &e;
      }
   }

   Stage42Art *src = stage42_get_art(path);
   if (!src || src->w <= 0 || src->h <= 0) return nullptr;

   Stage47CoverCache fresh;
   fresh.key = key;
   fresh.w = w;
   fresh.h = h;
   fresh.stamp = stage47_cover_stamp++;
   fresh.pixels.resize((size_t)w * h);
   fresh.left.resize(h, 0);
   fresh.right.resize(h, (uint16_t)w);

   const double sx = (double)w / src->w;
   const double sy = (double)h / src->h;
   const double scale = std::max(sx, sy);
   const int dw = std::max(1, (int)std::lround(src->w * scale));
   const int dh = std::max(1, (int)std::lround(src->h * scale));
   const int ox = (w - dw) / 2;
   const int oy = (h - dh) / 2;

   for (int y = 0; y < h; ++y)
   {
      int left = 0, right = w;
      while (left < w && !stage45_round_contains(left, y, w, h, radius)) ++left;
      while (right > left && !stage45_round_contains(right - 1, y, w, h, radius)) --right;
      fresh.left[y] = (uint16_t)left;
      fresh.right[y] = (uint16_t)right;
      for (int x = left; x < right; ++x)
      {
         const int rx = x - ox;
         const int ry = y - oy;
         const int ix = std::max(0, std::min(src->w - 1, (int)((int64_t)rx * src->w / dw)));
         const int iy = std::max(0, std::min(src->h - 1, (int)((int64_t)ry * src->h / dh)));
         const unsigned char *p = src->rgba.data() + ((size_t)iy * src->w + ix) * 4U;
         fresh.pixels[(size_t)y * w + x] = pack1555(p[0], p[1], p[2]);
      }
   }

   if (stage47_cover_cache.size() >= STAGE47_COVER_CACHE_LIMIT)
   {
      auto it = std::min_element(stage47_cover_cache.begin(), stage47_cover_cache.end(),
            [](const Stage47CoverCache &a, const Stage47CoverCache &b) { return a.stamp < b.stamp; });
      if (it != stage47_cover_cache.end()) stage47_cover_cache.erase(it);
   }
   stage47_cover_cache.push_back(std::move(fresh));
   return &stage47_cover_cache.back();
}

static bool stage47_draw_game_cover_cached(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int radius)
{
   Stage47CoverCache *img = stage47_get_cover(path, w, h, radius);
   if (!img) return false;
   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      int left = img->left[yy];
      int right = img->right[yy];
      left = std::max(left, -x);
      right = std::min(right, (int)fb.w - x);
      if (left >= right) continue;
      memcpy(fb_row(fb, dy) + x + left,
             img->pixels.data() + (size_t)yy * w + left,
             (size_t)(right - left) * sizeof(uint16_t));
   }
   return true;
}

static float stage47_quantize_focus(float f)
{
   f = stage42_clamp01(f);
   return std::round(f * 4.0f) * 0.25f;
}

static bool stage47_draw_background_cached(Fb &fb)
{
   if (stage47_draw_cached_asset(fb, stage45_asset_path("stage45_bg.png"),
         0, 0, (int)fb.w, (int)fb.h, true, 255))
      return true;
   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, pack1555(2, 13, 29));
   return false;
}

static void stage47_draw_system_panel(Fb &fb, int x, int y, int w, int h,
      bool focused)
{
   const int margin = 12;
   stage47_draw_cached_asset(fb, stage46_ui_asset("system_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2, true, 255);
   stage47_draw_cached_asset(fb,
         stage46_ui_asset(focused ? "system_frame_focus.png" : "system_frame_idle.png"),
         x, y, w, h, false, focused ? 255 : 105);
}

static void stage47_draw_rom_panel(Fb &fb, int x, int y, int w, int h,
      bool focused)
{
   const int margin = 10;
   stage47_draw_cached_asset(fb, stage46_ui_asset("rom_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2, true, 255);
   stage47_draw_cached_asset(fb,
         stage46_ui_asset(focused ? "rom_frame_focus.png" : "rom_frame_idle.png"),
         x, y, w, h, false, focused ? 255 : 95);
}
'''

anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit("Stage4.7 system row anchor not found")
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
      const int content_h = selected ? 142 : 116;
      const int x = cx - w / 2;
      const int y = selected ? 99 : 111;

      if (selected)
      {
         const int frame_pad_x = 9;
         const int frame_pad_y = 7;
         stage47_draw_system_panel(fb, x - frame_pad_x, y - frame_pad_y,
               w + frame_pad_x * 2, content_h + frame_pad_y * 2,
               focus == FocusZone::Systems);
      }

      const std::string asset = stage45_controller_asset(s);
      bool drawn = false;
      if (!asset.empty())
      {
         const int bx = selected ? x + 15 : x + 5;
         const int by = selected ? y + 10 : y + 4;
         const int bw = selected ? w - 30 : w - 10;
         const int bh = selected ? 91 : 82;
         drawn = stage47_draw_cached_asset(fb, asset, bx, by, bw, bh, false, 255);
      }
      if (!drawn)
      {
         const SystemVisualSpec &visual = stage43_visual_spec(s);
         const uint16_t body = selected ? pack1555(196, 205, 216) : pack1555(103, 117, 135);
         const uint16_t ink = selected ? pack1555(21, 26, 34) : pack1555(36, 46, 60);
         const uint16_t accent = pack1555(visual.accent_r, visual.accent_g, visual.accent_b);
         stage43_draw_system_icon(fb, s, cx, y + 52, selected ? 1.10f : 0.88f, body, ink, accent);
      }

      const std::string label = stage42_system_short_name(s);
      const int scale = selected ? 2 : 1;
      const int tw = text_width(label, scale);
      const int ly = selected ? y + 114 : 228;
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
   const int card_center_y = 457;

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
      const int w = (int)std::lround(STAGE42_SMALL_W + (STAGE42_SELECTED_W - STAGE42_SMALL_W) * f);
      const int h = (int)std::lround(STAGE42_SMALL_H + (STAGE42_SELECTED_H - STAGE42_SMALL_H) * f);
      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;
      const int y = card_center_y - h / 2;
      const bool selected = std::fabs(cv.rel) < 0.50f;
      const int pad = STAGE42_CARD_PAD;
      const int fx = x - pad;
      const int fy = y - pad;
      const int fw = w + pad * 2;
      const int fh = h + pad * 2;

      /* Focus ownership is explicit: bright ROM only while Games owns focus.
       * All other ROM frames are deliberately dim. */
      stage47_draw_rom_panel(fb, fx, fy, fw, fh,
            selected && focus == FocusZone::Games);

      const int panel_margin = 10;
      const int art_gap_x = selected ? 10 : 8;
      const int brand_h = selected ? 23 : 19;
      const int art_x = fx + panel_margin + art_gap_x;
      const int art_y = fy + panel_margin + brand_h + 8;
      const int art_w = fw - (panel_margin + art_gap_x) * 2;
      const int art_h = fh - panel_margin * 2 - brand_h - 18;
      if (!stage47_draw_game_cover_cached(fb, g.image_path, art_x, art_y, art_w, art_h, 5))
         draw_fallback_card(fb, sys, art_x, art_y, art_w, art_h);

      const std::string brand = stage44_brand_name(sys);
      const int bw = text_width(brand, 1);
      draw_text(fb, fx + fw / 2 - bw / 2, fy + panel_margin + 6, brand, 1,
            selected ? text : dim);

      /* Frame is composited last so cover art can never extend over it. */
      stage47_draw_cached_asset(fb,
            stage46_ui_asset(selected && focus == FocusZone::Games ?
               "rom_frame_focus.png" : "rom_frame_idle.png"),
            fx, fy, fw, fh, false,
            selected && focus == FocusZone::Games ? 255 : 95);
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

# Use the cached fixed background, not the per-frame scaler.
if '   stage46_draw_background(fb);' not in src:
    raise SystemExit('Stage4.7 background call anchor not found')
src = src.replace('   stage46_draw_background(fb);', '   stage47_draw_background_cached(fb);', 1)

# Replace Stage4.6 proof lines with stronger Stage4.7 diagnostics.
layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE47_FOCUS systems-zone=>system-focus-sprite games-zone=>rom-focus-sprite idle-opacity=dim\\n");\n'
    '   printf("STAGE47_GEOMETRY rom-frame-pad=10 panel-margin=10 system-frame-extra=9x7\\n");\n'
    '   printf("STAGE47_PERF cached-background cached-ui-sprites cached-cover-resize quantized-scale anim=95ms\\n");\n'
)
if layout_ok not in src:
    raise SystemExit("Stage4.7 layout-test anchor not found")
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STAGE47_FOCUS_PERFORMANCE_PATCH_OK {src_path} -> {out_path}")
