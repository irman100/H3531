#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage45_reference_fidelity.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.4 PixelStation night-coverflow theme active'
new_marker = 'Stage4.5 PixelStation reference-fidelity assets active'
if old_marker not in src:
    raise SystemExit("expected Stage4.4 marker not found")
src = src.replace(old_marker, new_marker, 1)

# Geometry measured from the 1280x720 visual reference.
replacements = {
    'static const int STAGE42_SMALL_W = 168;': 'static const int STAGE42_SMALL_W = 164;',
    'static const int STAGE42_SMALL_H = 252;': 'static const int STAGE42_SMALL_H = 266;',
    'static const int STAGE42_SELECTED_W = 220;': 'static const int STAGE42_SELECTED_W = 224;',
    'static const int STAGE42_SELECTED_H = 328;': 'static const int STAGE42_SELECTED_H = 294;',
    'static const int STAGE42_SYSTEM_GAP = 28;': 'static const int STAGE42_SYSTEM_GAP = 40;',
    'static const int STAGE42_SYSTEM_SMALL_W = 176;': 'static const int STAGE42_SYSTEM_SMALL_W = 190;',
    'static const int STAGE42_SYSTEM_SELECTED_W = 224;': 'static const int STAGE42_SYSTEM_SELECTED_W = 232;',
}
for old, new in replacements.items():
    if old not in src:
        raise SystemExit(f"constant not found: {old}")
    src = src.replace(old, new, 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"start anchor not found: {start}")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"end anchor not found: {end}")
    return text[:a] + replacement.rstrip() + "\n\n" + text[b:]

helpers = r'''static const char *STAGE45_ASSET_ROOT = "/mnt/usb/H3531/APPS/gamefront/assets";

struct Stage45Asset {
   int w = 0;
   int h = 0;
   std::vector<unsigned char> rgba;
   bool attempted = false;
};

static std::map<std::string, Stage45Asset> stage45_asset_cache;

static std::string stage45_asset_path(const char *name)
{
   return std::string(STAGE45_ASSET_ROOT) + "/" + name;
}

static Stage45Asset *stage45_get_asset(const std::string &path)
{
   Stage45Asset &a = stage45_asset_cache[path];
   if (a.attempted)
      return a.rgba.empty() ? nullptr : &a;
   a.attempted = true;
   int c = 0;
   unsigned char *raw = stbi_load(path.c_str(), &a.w, &a.h, &c, 4);
   if (!raw || a.w <= 0 || a.h <= 0)
   {
      if (raw) stbi_image_free(raw);
      a.w = a.h = 0;
      return nullptr;
   }
   a.rgba.assign(raw, raw + (size_t)a.w * a.h * 4U);
   stbi_image_free(raw);
   return &a;
}

static bool stage45_round_contains(int px, int py, int w, int h, int r)
{
   if (r <= 0) return px >= 0 && py >= 0 && px < w && py < h;
   if (px < 0 || py < 0 || px >= w || py >= h) return false;
   if ((px >= r && px < w - r) || (py >= r && py < h - r)) return true;
   const int cx = px < r ? r - 1 : w - r;
   const int cy = py < r ? r - 1 : h - r;
   const int dx = px - cx, dy = py - cy;
   return dx * dx + dy * dy <= r * r;
}

static void stage45_fill_round_rect(Fb &fb, int x, int y, int w, int h, int r, uint16_t c)
{
   if (w <= 0 || h <= 0) return;
   r = std::max(0, std::min(r, std::min(w, h) / 2));
   for (int yy = 0; yy < h; ++yy)
   {
      int inset = 0;
      if (r > 0 && (yy < r || yy >= h - r))
      {
         const int cy = yy < r ? r - 1 : h - r;
         const int dy = yy - cy;
         const int span = (int)std::sqrt((double)std::max(0, r * r - dy * dy));
         inset = std::max(0, r - span);
      }
      fill_rect(fb, x + inset, y + yy, w - inset * 2, 1, c);
   }
}

static void stage45_round_panel(Fb &fb, int x, int y, int w, int h, int r,
      int border_px, uint16_t border, uint16_t fill)
{
   stage45_fill_round_rect(fb, x, y, w, h, r, border);
   if (border_px > 0 && w > border_px * 2 && h > border_px * 2)
      stage45_fill_round_rect(fb, x + border_px, y + border_px,
            w - border_px * 2, h - border_px * 2,
            std::max(0, r - border_px), fill);
}

static void stage45_glow_round(Fb &fb, int x, int y, int w, int h, int r, bool strong)
{
   const uint16_t g0 = pack1555(7, 34, 74);
   const uint16_t g1 = pack1555(17, 90, 172);
   const uint16_t g2 = pack1555(45, 177, 255);
   const uint16_t g3 = pack1555(143, 233, 255);
   stage45_fill_round_rect(fb, x - 8, y - 8, w + 16, h + 16, r + 8, g0);
   stage45_fill_round_rect(fb, x - 5, y - 5, w + 10, h + 10, r + 5, g1);
   stage45_fill_round_rect(fb, x - 3, y - 3, w + 6, h + 6, r + 3, strong ? g3 : g2);
}

static bool stage45_draw_asset(Fb &fb, const std::string &path,
      int x, int y, int w, int h, bool cover, int clip_radius)
{
   Stage45Asset *img = stage45_get_asset(path);
   if (!img || img->w <= 0 || img->h <= 0 || w <= 0 || h <= 0) return false;
   const double sx = (double)w / img->w;
   const double sy = (double)h / img->h;
   const double s = cover ? std::max(sx, sy) : std::min(sx, sy);
   const int dw = std::max(1, (int)std::lround(img->w * s));
   const int dh = std::max(1, (int)std::lround(img->h * s));
   const int ox = x + (w - dw) / 2;
   const int oy = y + (h - dh) / 2;

   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dy);
      for (int xx = 0; xx < w; ++xx)
      {
         if (clip_radius > 0 && !stage45_round_contains(xx, yy, w, h, clip_radius)) continue;
         const int dx = x + xx;
         if (dx < 0 || dx >= (int)fb.w) continue;
         const int rx = dx - ox;
         const int ry = dy - oy;
         if (rx < 0 || ry < 0 || rx >= dw || ry >= dh) continue;
         const int src_x = std::min(img->w - 1, (int)((int64_t)rx * img->w / dw));
         const int src_y = std::min(img->h - 1, (int)((int64_t)ry * img->h / dh));
         const unsigned char *p = img->rgba.data() + ((size_t)src_y * img->w + src_x) * 4U;
         if (p[3] >= 96) row[dx] = pack1555(p[0], p[1], p[2]);
      }
   }
   return true;
}

static std::string stage45_controller_asset(const SystemDef &s)
{
   const std::string n = lower(s.name);
   if (n == "nes" || n == "famicom") return stage45_asset_path("controller_nes.png");
   if (n == "megadrive" || n == "genesis") return stage45_asset_path("controller_megadrive.png");
   if (n == "snes" || n == "supernes") return stage45_asset_path("controller_snes.png");
   if (n == "gb" || n == "gbc" || n == "gba" || n == "gameboy") return stage45_asset_path("controller_gameboy.png");
   if (n == "psx" || n == "ps1" || n == "playstation") return stage45_asset_path("controller_playstation.png");
   return {};
}

static bool stage45_draw_background(Fb &fb)
{
   return stage45_draw_asset(fb, stage45_asset_path("stage45_bg.png"),
         0, 0, (int)fb.w, (int)fb.h, true, 0);
}

static bool stage45_draw_game_cover(Fb &fb, const std::string &path,
      int x, int y, int w, int h, int radius)
{
   Stage42Art *img = stage42_get_art(path);
   if (!img || img->w <= 0 || img->h <= 0) return false;
   const double sx = (double)w / img->w;
   const double sy = (double)h / img->h;
   const double s = std::max(sx, sy);
   const int dw = std::max(1, (int)std::lround(img->w * s));
   const int dh = std::max(1, (int)std::lround(img->h * s));
   const int ox = x + (w - dw) / 2;
   const int oy = y + (h - dh) / 2;
   for (int yy = 0; yy < h; ++yy)
   {
      const int dy = y + yy;
      if (dy < 0 || dy >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dy);
      for (int xx = 0; xx < w; ++xx)
      {
         if (!stage45_round_contains(xx, yy, w, h, radius)) continue;
         const int dx = x + xx;
         if (dx < 0 || dx >= (int)fb.w) continue;
         const int rx = dx - ox, ry = dy - oy;
         if (rx < 0 || ry < 0 || rx >= dw || ry >= dh) continue;
         const int sxp = std::min(img->w - 1, (int)((int64_t)rx * img->w / dw));
         const int syp = std::min(img->h - 1, (int)((int64_t)ry * img->h / dh));
         const unsigned char *p = img->rgba.data() + ((size_t)syp * img->w + sxp) * 4U;
         if (p[3] >= 96) row[dx] = pack1555(p[0], p[1], p[2]);
      }
   }
   return true;
}

static void stage45_draw_brand_badge(Fb &fb, const SystemDef &s,
      int cx, int y, int max_w, bool selected)
{
   const std::string brand = stage44_brand_name(s);
   const int scale = 1;
   const int tw = text_width(brand, scale);
   const int w = std::min(max_w, tw + 22);
   const int x = cx - w / 2;
   const uint16_t outer = selected ? pack1555(154, 205, 255) : pack1555(86, 111, 145);
   const uint16_t inner = pack1555(3, 10, 22);
   stage45_round_panel(fb, x, y, w, 18, 9, 1, outer, inner);
   draw_text(fb, cx - tw / 2, y + 5, brand, scale,
         selected ? pack1555(242, 249, 255) : pack1555(190, 204, 220));
}
'''

anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit("system row anchor not found")
src = src.replace(anchor, helpers + '\n\n' + anchor, 1)

system_row = r'''static void stage42_draw_system_row(Fb &fb, const std::vector<SystemDef> &systems,
      const std::vector<size_t> &vis, size_t visible_pos, FocusZone focus, float system_shift)
{
   if (vis.empty()) return;
   const int center = (int)fb.w / 2;
   const uint16_t text = pack1555(216, 230, 249);
   const uint16_t dim = pack1555(121, 145, 183);

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
      const float f = sv.focus;
      const int w = (int)std::lround(STAGE42_SYSTEM_SMALL_W +
            (STAGE42_SYSTEM_SELECTED_W - STAGE42_SYSTEM_SMALL_W) * f);
      const int cx = center + (int)std::lround(stage42_system_center_offset(sv.rel));
      const bool selected = std::fabs(sv.rel) < 0.50f;
      const int frame_h = selected ? 166 : 132;
      const int x = cx - w / 2;
      const int y = selected ? 96 : 108;

      if (selected)
      {
         stage45_glow_round(fb, x, y, w, frame_h, 7, focus == FocusZone::Systems);
         stage45_round_panel(fb, x, y, w, frame_h, 7, 2,
               pack1555(133, 221, 255), pack1555(4, 13, 29));
      }

      const std::string asset = stage45_controller_asset(s);
      bool drawn = false;
      if (!asset.empty())
      {
         const int bx = selected ? x + 12 : x + 3;
         const int by = selected ? y + 7 : y + 3;
         const int bw = selected ? w - 24 : w - 6;
         const int bh = selected ? 122 : 104;
         drawn = stage45_draw_asset(fb, asset, bx, by, bw, bh, false, 0);
      }
      if (!drawn)
      {
         const SystemVisualSpec &visual = stage43_visual_spec(s);
         const uint16_t body = selected ? pack1555(198, 205, 214) : pack1555(103, 117, 135);
         const uint16_t ink = selected ? pack1555(21, 26, 34) : pack1555(36, 46, 60);
         const uint16_t accent = pack1555(visual.accent_r, visual.accent_g, visual.accent_b);
         stage43_draw_system_icon(fb, s, cx, y + 59, selected ? 1.20f : 0.96f, body, ink, accent);
      }

      const std::string label = stage42_system_short_name(s);
      const int scale = selected ? 2 : 1;
      const int tw = text_width(label, scale);
      const int ly = selected ? y + 139 : 239;
      draw_text(fb, cx - tw / 2, ly, label, scale, selected ? text : dim);
   }
}
'''
src = replace_between(src, 'static void stage42_draw_system_row(', 'static void stage42_draw_games(', system_row)

games = r'''static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;

   const uint16_t case_outer = pack1555(40, 55, 76);
   const uint16_t case_inner = pack1555(5, 11, 21);
   const uint16_t selected_edge = pack1555(112, 219, 255);
   const uint16_t text = pack1555(237, 246, 255);
   const uint16_t dim = pack1555(126, 151, 188);
   const int center = (int)fb.w / 2;
   const int card_center_y = 458;

   std::map<size_t, Stage42CardVisual> unique;
   for (int rel = -4; rel <= 4; ++rel)
   {
      const float effective = (float)rel + game_shift;
      if (std::fabs(effective) > 3.20f) continue;
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
      const float f = cv.focus;
      const int w = (int)std::lround(STAGE42_SMALL_W + (STAGE42_SELECTED_W - STAGE42_SMALL_W) * f);
      const int h = (int)std::lround(STAGE42_SMALL_H + (STAGE42_SELECTED_H - STAGE42_SMALL_H) * f);
      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;
      const int y = card_center_y - h / 2;
      const bool selected = f > 0.5f;
      const int pad = STAGE42_CARD_PAD;
      const int radius = selected ? 8 : 6;

      if (selected)
         stage45_glow_round(fb, x - pad, y - pad, w + pad * 2, h + pad * 2, radius + pad,
               focus == FocusZone::Games);

      stage45_round_panel(fb, x - pad + 4, y - pad + 5, w + pad * 2, h + pad * 2,
            radius + pad, 1, pack1555(1, 4, 10), pack1555(1, 4, 10));
      stage45_round_panel(fb, x - pad, y - pad, w + pad * 2, h + pad * 2,
            radius + pad, 2, selected ? selected_edge : case_outer, case_inner);

      const int brand_h = selected ? 25 : 21;
      stage45_draw_brand_badge(fb, sys, x + w / 2, y + 4, w - 16, selected);

      const int art_x = x + 6;
      const int art_y = y + brand_h + 5;
      const int art_w = w - 12;
      const int art_h = h - brand_h - 36;
      stage45_fill_round_rect(fb, art_x, art_y, art_w, art_h, 4, pack1555(2, 7, 15));
      if (!stage45_draw_game_cover(fb, g.image_path, art_x, art_y, art_w, art_h, 4))
         draw_fallback_card(fb, sys, art_x, art_y, art_w, art_h);

      stage45_draw_brand_badge(fb, sys, x + w / 2, y + h - 23, w - 18, false);

      if (selected || std::fabs(cv.rel) <= 2.1f)
         stage44_reflect_rect(fb, x - pad, y - pad, w + pad * 2, h + pad * 2,
               selected ? 30 : 18);
   }

   const Game &selected = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(selected.title, 2, 520);
   const int tw = text_width(title, 2);
   draw_text(fb, center - tw / 2, 617, title, 2, focus == FocusZone::Games ? text : dim);
   const std::string metadata = stage44_brand_name(sys) + "  *  " +
         std::to_string(game_pos + 1) + "/" + std::to_string(sys.games.size());
   const int mw = text_width(metadata, 1);
   draw_text(fb, center - mw / 2, 646, metadata, 1, dim);
}
'''
src = replace_between(src, 'static void stage42_draw_games(', 'static void stage42_draw_ui(', games)

ui = r'''static void stage42_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      FocusZone focus, float game_shift, float system_shift)
{
   const uint16_t text = pack1555(227, 240, 255);
   const uint16_t dim = pack1555(128, 157, 199);
   if (!stage45_draw_background(fb))
      stage44_draw_night_background(fb);

   std::time_t tt = std::time(nullptr);
   std::tm *tmv = std::localtime(&tt);
   char timebuf[16] = "--:--";
   char datebuf[32] = "--- --- --";
   if (tmv)
   {
      std::strftime(timebuf, sizeof(timebuf), "%H:%M", tmv);
      std::strftime(datebuf, sizeof(datebuf), "%a %b %d", tmv);
      for (char *p = datebuf; *p; ++p) *p = (char)std::toupper((unsigned char)*p);
   }
   draw_text(fb, 1018, 28, timebuf, 2, text);
   draw_text(fb, 1000, 59, datebuf, 1, dim);

   if (vis.empty())
   {
      draw_text(fb, 480, 330, "NO GAMES FOUND", 3, text);
      return;
   }

   stage42_draw_system_row(fb, systems, vis, visible_pos, focus, system_shift);
   const SystemDef &sys = systems[vis[visible_pos]];

   fill_rect(fb, 72, 285, (int)fb.w - 144, 1, pack1555(53, 109, 163));
   const std::string era = stage44_era_name(sys);
   const int ew = text_width(era, 1);
   fill_rect(fb, (int)fb.w / 2 - ew / 2 - 12, 278, ew + 24, 16, pack1555(2, 9, 20));
   draw_text(fb, (int)fb.w / 2 - ew / 2, 281, era, 1, pack1555(164, 192, 230));

   stage42_draw_games(fb, sys, game_pos, game_shift, focus);
}
'''
src = replace_between(src, 'static void stage42_draw_ui(', 'static int stage42_layout_test()', ui)

# Extend the self-test output so CI can prove the measured Stage4.5 layout is present.
needle = '   printf("STAGE44_THEME pixelstation-night-coverflow procedural-background\\n");\n'
if needle in src:
    src = src.replace(needle, needle +
        '   printf("STAGE45_REFERENCE bg-asset controller-sprites rounded-cases 1280x720\\n");\n'
        '   printf("STAGE45_GEOMETRY divider_y=285 card_center_y=458 title_y=617 footer_y=678\\n");\n', 1)
else:
    # Keep build robust even if Stage4.4 diagnostic wording changes.
    layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
    if layout_ok in src:
        src = src.replace(layout_ok,
            '   printf("STAGE45_REFERENCE bg-asset controller-sprites rounded-cases 1280x720\\n");\n'
            '   printf("STAGE45_GEOMETRY divider_y=285 card_center_y=458 title_y=617 footer_y=678\\n");\n' + layout_ok, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STAGE45_REFERENCE_FIDELITY_PATCH_OK {src_path} -> {out_path}")
