#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage44_pixelstation_theme.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.3 scalable pixel system icons active'
new_marker = 'Stage4.4 PixelStation night-coverflow theme active'
if old_marker not in src:
    raise SystemExit("expected Stage4.3 marker not found")
src = src.replace(old_marker, new_marker, 1)

# Fit seven-card cover-flow at 1280x720 while preserving a real 14px edge-to-edge gap.
replacements = {
    'static const int STAGE42_SMALL_W = 172;': 'static const int STAGE42_SMALL_W = 168;',
    'static const int STAGE42_SMALL_H = 260;': 'static const int STAGE42_SMALL_H = 252;',
    'static const int STAGE42_SELECTED_W = 256;': 'static const int STAGE42_SELECTED_W = 220;',
    'static const int STAGE42_SELECTED_H = 364;': 'static const int STAGE42_SELECTED_H = 328;',
    'static const int STAGE42_SYSTEM_GAP = 18;': 'static const int STAGE42_SYSTEM_GAP = 28;',
    'static const int STAGE42_SYSTEM_SMALL_W = 188;': 'static const int STAGE42_SYSTEM_SMALL_W = 176;',
    'static const int STAGE42_SYSTEM_SELECTED_W = 232;': 'static const int STAGE42_SYSTEM_SELECTED_W = 224;',
    'static const uint64_t STAGE42_GAME_ANIM_MS = 165;': 'static const uint64_t STAGE42_GAME_ANIM_MS = 170;',
    'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 165;': 'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 160;',
}
for old, new in replacements.items():
    if old not in src:
        raise SystemExit(f"constant not found: {old}")
    src = src.replace(old, new, 1)

# time() is available on the target old Linux and costs no new runtime dependency.
if '#include <ctime>' not in src:
    src = src.replace('#include <sys/time.h>\n', '#include <sys/time.h>\n#include <ctime>\n', 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"start anchor not found: {start}")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"end anchor not found: {end}")
    return text[:a] + replacement.rstrip() + "\n\n" + text[b:]

# Higher-fidelity pixel NES controller in the same visual family used by the theme.
nes_code = r'''static void stage42_draw_nes_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int w = (int)std::lround(122 * scale);
   const int h = (int)std::lround(55 * scale);
   const int x = cx - w / 2, y = cy - h / 2;
   const int edge = std::max(2, (int)std::lround(3 * scale));
   const uint16_t shell = pack1555(205, 208, 210);
   const uint16_t face  = pack1555(35, 38, 45);
   const uint16_t mid   = pack1555(105, 108, 113);
   const uint16_t red   = pack1555(222, 47, 55);

   /* stepped 8-bit shell, deliberately crisp instead of anti-aliased */
   fill_rect(fb, x + edge, y, w - edge * 2, h, shell);
   fill_rect(fb, x, y + edge, w, h - edge * 2, shell);
   frame_rect(fb, x, y, w, h, edge, ink);
   fill_rect(fb, x + (int)std::lround(7 * scale), y + (int)std::lround(8 * scale),
             w - (int)std::lround(14 * scale), h - (int)std::lround(16 * scale), face);

   const int dx = x + (int)std::lround(30 * scale);
   const int d = std::max(5, (int)std::lround(9 * scale));
   const int arm = std::max(18, (int)std::lround(29 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   fill_rect(fb, dx - d / 2 + 2, cy - d / 2 + 2, std::max(1, d - 4), std::max(1, d - 4), mid);

   const int panel_x = x + (int)std::lround(49 * scale);
   fill_rect(fb, panel_x, y + (int)std::lround(10 * scale),
             (int)std::lround(29 * scale), (int)std::lround(12 * scale), mid);
   fill_rect(fb, panel_x, y + (int)std::lround(25 * scale),
             (int)std::lround(29 * scale), (int)std::lround(5 * scale), shell);
   const int sw = std::max(6, (int)std::lround(11 * scale));
   const int sh = std::max(2, (int)std::lround(4 * scale));
   fill_rect(fb, panel_x + 1, y + (int)std::lround(36 * scale), sw, sh, shell);
   fill_rect(fb, panel_x + (int)std::lround(16 * scale), y + (int)std::lround(36 * scale), sw, sh, shell);

   const int r = std::max(4, (int)std::lround(7 * scale));
   stage42_fill_circle(fb, x + w - (int)std::lround(34 * scale), cy + (int)std::lround(5 * scale), r + 2, shell);
   stage42_fill_circle(fb, x + w - (int)std::lround(17 * scale), cy - (int)std::lround(2 * scale), r + 2, shell);
   stage42_fill_circle(fb, x + w - (int)std::lround(34 * scale), cy + (int)std::lround(5 * scale), r, red);
   stage42_fill_circle(fb, x + w - (int)std::lround(17 * scale), cy - (int)std::lround(2 * scale), r, red);
}
'''
src = replace_between(src, 'static void stage42_draw_nes_pad(', 'static void stage42_draw_sega_pad(', nes_code)

# Mega Drive / Genesis 3-button controller: sculpted body with two lower lobes,
# not the old oval placeholder.
md_code = r'''static void stage42_draw_sega_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const uint16_t shell = pack1555(37, 41, 50);
   const uint16_t shell_hi = pack1555(62, 67, 78);
   const uint16_t black = pack1555(15, 17, 22);
   const uint16_t blue = pack1555(72, 151, 235);
   const int rx = (int)std::lround(68 * scale);
   const int ry = (int)std::lround(24 * scale);

   /* main shoulder and the characteristic lower grips */
   stage42_fill_ellipse(fb, cx, cy - (int)std::lround(4 * scale), rx, ry, shell);
   stage42_fill_ellipse(fb, cx - (int)std::lround(43 * scale), cy + (int)std::lround(14 * scale),
                        (int)std::lround(25 * scale), (int)std::lround(22 * scale), shell);
   stage42_fill_ellipse(fb, cx + (int)std::lround(43 * scale), cy + (int)std::lround(14 * scale),
                        (int)std::lround(25 * scale), (int)std::lround(22 * scale), shell);
   fill_rect(fb, cx - (int)std::lround(53 * scale), cy - (int)std::lround(20 * scale),
             (int)std::lround(106 * scale), std::max(2, (int)std::lround(4 * scale)), shell_hi);

   /* recessed D-pad disc and cross */
   const int dx = cx - (int)std::lround(38 * scale);
   stage42_fill_circle(fb, dx, cy - (int)std::lround(2 * scale),
                       std::max(9, (int)std::lround(19 * scale)), black);
   const int d = std::max(5, (int)std::lround(8 * scale));
   const int arm = std::max(18, (int)std::lround(25 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2 - (int)std::lround(2 * scale), d, arm, shell_hi);
   fill_rect(fb, dx - arm / 2, cy - d / 2 - (int)std::lround(2 * scale), arm, d, shell_hi);

   /* three-button arc */
   const int r = std::max(4, (int)std::lround(6 * scale));
   const int bx = cx + (int)std::lround(23 * scale);
   stage42_fill_circle(fb, bx, cy + (int)std::lround(9 * scale), r + 2, black);
   stage42_fill_circle(fb, bx + (int)std::lround(17 * scale), cy + (int)std::lround(2 * scale), r + 2, black);
   stage42_fill_circle(fb, bx + (int)std::lround(32 * scale), cy - (int)std::lround(7 * scale), r + 2, black);
   stage42_fill_circle(fb, bx, cy + (int)std::lround(9 * scale), r, shell_hi);
   stage42_fill_circle(fb, bx + (int)std::lround(17 * scale), cy + (int)std::lround(2 * scale), r, shell_hi);
   stage42_fill_circle(fb, bx + (int)std::lround(32 * scale), cy - (int)std::lround(7 * scale), r, shell_hi);

   /* start button + tiny blue accent line, deliberately Sega-like but original */
   fill_rect(fb, cx - (int)std::lround(5 * scale), cy + (int)std::lround(15 * scale),
             std::max(7, (int)std::lround(13 * scale)), std::max(2, (int)std::lround(3 * scale)), black);
   fill_rect(fb, cx + (int)std::lround(11 * scale), cy - (int)std::lround(18 * scale),
             std::max(8, (int)std::lround(14 * scale)), std::max(2, (int)std::lround(3 * scale)), blue);
}
'''
src = replace_between(src, 'static void stage42_draw_sega_pad(', 'static void stage42_draw_generic_pad(', md_code)

helpers = r'''static uint16_t stage44_dim1555(uint16_t p, int num, int den)
{
   if (den <= 0) return p;
   int r = ((p >> 10) & 31) * num / den;
   int g = ((p >> 5) & 31) * num / den;
   int b = (p & 31) * num / den;
   return (uint16_t)(0x8000 | ((r & 31) << 10) | ((g & 31) << 5) | (b & 31));
}

static void stage44_draw_night_background(Fb &fb)
{
   const int w = (int)fb.w, h = (int)fb.h;
   const uint16_t sky0 = pack1555(4, 12, 30);
   const uint16_t sky1 = pack1555(7, 22, 48);
   const uint16_t sky2 = pack1555(8, 31, 61);
   const uint16_t ridge = pack1555(4, 16, 31);
   const uint16_t forest = pack1555(3, 12, 23);
   const uint16_t water = pack1555(4, 17, 34);
   const uint16_t moon = pack1555(139, 177, 214);
   fill_rect(fb, 0, 0, w, h, sky0);
   fill_rect(fb, 0, 56, w, 58, sky1);
   fill_rect(fb, 0, 114, w, 88, sky2);
   fill_rect(fb, 0, 202, w, h - 202, pack1555(3, 10, 22));

   /* deterministic stars: no rand(), therefore identical on every redraw */
   for (int i = 0; i < 46; ++i)
   {
      const int x = (i * 137 + 73) % std::max(1, w);
      const int y = 24 + ((i * 61 + 17) % 130);
      const int s = (i % 11 == 0) ? 2 : 1;
      fill_rect(fb, x, y, s, s, pack1555(92 + (i % 3) * 24, 135 + (i % 2) * 30, 190));
   }

   const int moon_x = w - 103;
   stage42_fill_circle(fb, moon_x, 86, 21, moon);
   stage42_fill_circle(fb, moon_x - 7, 80, 18, sky2);

   /* layered mountains */
   for (int x = 0; x < w; x += 18)
   {
      const int peak = 126 + ((x * 17 / 18) % 43);
      const int base = 204;
      for (int yy = peak; yy < base; ++yy)
      {
         const int d = yy - peak;
         const int half = std::min(33, d / 2 + 1);
         fill_rect(fb, x - half, yy, half * 2 + 1, 1, ridge);
      }
   }

   /* pine silhouettes */
   for (int x = 5; x < w; x += 24)
   {
      const int base = 228 + ((x / 24) % 3) * 4;
      const int th = 44 + ((x * 7) % 30);
      fill_rect(fb, x, base - th, 3, th, forest);
      for (int yy = 8; yy < th - 4; yy += 7)
      {
         const int half = std::min(15, yy / 3 + 2);
         fill_rect(fb, x - half, base - th + yy, half * 2 + 3, 2, forest);
      }
   }

   /* distant water strip with sparse reflections */
   fill_rect(fb, 0, 226, w, 48, water);
   for (int i = 0; i < 18; ++i)
   {
      const int x = (i * 89 + 43) % w;
      const int ww = 8 + (i % 4) * 5;
      fill_rect(fb, x, 234 + (i % 4) * 7, ww, 1, pack1555(13, 53, 86));
   }

   /* dark translucent-looking stage: achieved with solid 1555 bands */
   fill_rect(fb, 0, 276, w, h - 276, pack1555(3, 10, 21));
}

static std::string stage44_brand_name(const SystemDef &s)
{
   const std::string id = lower(s.name);
   if (id == "megadrive" || id == "genesis") return "MEGA DRIVE";
   if (id == "nes" || id == "famicom") return "NES";
   if (id == "snes" || id == "sfc") return "SUPER NINTENDO";
   if (id == "mastersystem") return "MASTER SYSTEM";
   if (id == "gamegear") return "GAME GEAR";
   if (id == "gb" || id == "gbc" || id == "gba") return "GAME BOY";
   if (id == "psx" || id == "ps1") return "PLAYSTATION";
   return stage42_system_short_name(s);
}

static std::string stage44_era_name(const SystemDef &s)
{
   const std::string id = lower(s.name);
   if (id == "megadrive" || id == "genesis" || id == "snes" || id == "sfc") return "16-BIT LEGENDS";
   if (id == "nes" || id == "famicom" || id == "mastersystem") return "8-BIT CLASSICS";
   if (id == "gb" || id == "gbc" || id == "gba" || id == "gamegear") return "HANDHELD CLASSICS";
   if (id == "psx" || id == "ps1") return "32-BIT ERA";
   return "RETRO LIBRARY";
}

static void stage44_draw_brand_badge(Fb &fb, const SystemDef &s, int cx, int y, int max_w, bool selected)
{
   const std::string brand = stage44_brand_name(s);
   const int scale = selected ? 2 : 1;
   const int tw = text_width(brand, scale);
   const int pad = selected ? 13 : 8;
   const int bw = std::min(max_w, tw + pad * 2);
   const int bh = selected ? 24 : 16;
   const int x = cx - bw / 2;
   const uint16_t bg = pack1555(5, 13, 27);
   const uint16_t line = selected ? pack1555(84, 192, 255) : pack1555(58, 85, 115);
   const uint16_t fg = selected ? pack1555(238, 246, 255) : pack1555(147, 169, 192);
   fill_rect(fb, x, y, bw, bh, bg);
   frame_rect(fb, x, y, bw, bh, selected ? 2 : 1, line);
   draw_text(fb, cx - tw / 2, y + (selected ? 5 : 4), brand, scale, fg);
}

static void stage44_reflect_rect(Fb &fb, int x, int y, int w, int h, int reflection_h)
{
   if (reflection_h <= 0 || !fb.mem) return;
   const int bottom = y + h - 1;
   for (int ry = 0; ry < reflection_h; ++ry)
   {
      const int sy = bottom - std::min(h - 1, ry * 2);
      const int dy = bottom + 1 + ry;
      if (sy < 0 || dy < 0 || sy >= (int)fb.h || dy >= (int)fb.h) continue;
      uint16_t *srcrow = fb_row(fb, sy);
      uint16_t *dstrow = fb_row(fb, dy);
      const int num = std::max(0, reflection_h - ry);
      for (int xx = 0; xx < w; ++xx)
      {
         const int px = x + xx;
         if (px < 0 || px >= (int)fb.w) continue;
         dstrow[px] = stage44_dim1555(srcrow[px], num, reflection_h * 4);
      }
   }
}

static void stage44_draw_glow_frame(Fb &fb, int x, int y, int w, int h, bool strong)
{
   const uint16_t glow0 = pack1555(17, 73, 118);
   const uint16_t glow1 = pack1555(32, 128, 205);
   const uint16_t glow2 = pack1555(82, 207, 255);
   frame_rect(fb, x - 7, y - 7, w + 14, h + 14, 1, glow0);
   frame_rect(fb, x - 4, y - 4, w + 8, h + 8, strong ? 2 : 1, glow1);
   frame_rect(fb, x - 1, y - 1, w + 2, h + 2, strong ? 3 : 2, glow2);
}

static void stage44_draw_footer_button(Fb &fb, int &x, int y, const char *key, const char *label, uint16_t c)
{
   stage42_fill_circle(fb, x + 10, y + 10, 10, c);
   const std::string k = key;
   const int kw = text_width(k, 1);
   draw_text(fb, x + 10 - kw / 2, y + 7, k, 1, pack1555(5, 13, 25));
   x += 27;
   draw_text(fb, x, y + 5, label, 1, pack1555(177, 197, 220));
   x += text_width(label, 1) + 30;
}

'''
anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit("system row anchor not found")
src = src.replace(anchor, helpers + anchor, 1)

system_row = r'''static void stage42_draw_system_row(Fb &fb, const std::vector<SystemDef> &systems,
      const std::vector<size_t> &vis, size_t visible_pos, FocusZone focus, float system_shift)
{
   if (vis.empty()) return;
   const int center = (int)fb.w / 2;
   const uint16_t dim = pack1555(126, 151, 179);
   const uint16_t text = pack1555(225, 238, 250);
   const uint16_t cyan = pack1555(75, 202, 255);

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
      const int h = (int)std::lround(101.0f + 23.0f * f);
      const int cx = center + (int)std::lround(stage42_system_center_offset(sv.rel));
      const int x = cx - w / 2;
      const int y = 82 + (int)std::lround((1.0f - f) * 10.0f);
      const bool selected = std::fabs(sv.rel) < 0.50f;

      /* only the selected system gets the luminous TV-style frame */
      if (selected)
      {
         fill_rect(fb, x, y, w, h, pack1555(5, 16, 33));
         stage44_draw_glow_frame(fb, x, y, w, h, focus == FocusZone::Systems);
      }

      const float icon_scale = 0.84f + f * 0.35f;
      const SystemVisualSpec &visual = stage43_visual_spec(s);
      const uint16_t body = selected ? pack1555(195, 202, 211) : pack1555(103, 117, 135);
      const uint16_t ink = selected ? pack1555(22, 27, 35) : pack1555(36, 46, 60);
      const uint16_t accent = selected
            ? pack1555(visual.accent_r, visual.accent_g, visual.accent_b)
            : pack1555(visual.accent_r / 2, visual.accent_g / 2, visual.accent_b / 2);
      stage43_draw_system_icon(fb, s, cx, y + 49, icon_scale, body, ink, accent);

      const std::string label = stage42_system_short_name(s);
      const int scale = selected ? 2 : 1;
      const int tw = text_width(label, scale);
      draw_text(fb, cx - tw / 2, y + h + 8, label, scale, selected ? text : dim);
   }

   if (focus == FocusZone::Systems)
      fill_rect(fb, center - 72, 238, 144, 3, cyan);
}
'''
src = replace_between(src, 'static void stage42_draw_system_row(', 'static void stage42_draw_games(', system_row)

games = r'''static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;

   const uint16_t case_bg = pack1555(8, 13, 22);
   const uint16_t case_inner = pack1555(12, 20, 32);
   const uint16_t border = pack1555(48, 72, 99);
   const uint16_t cyan = pack1555(75, 202, 255);
   const uint16_t text = pack1555(238, 245, 252);
   const uint16_t dim = pack1555(132, 153, 179);
   const int center = (int)fb.w / 2;
   const int card_center_y = 440;

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
         Stage42CardVisual cv;
         cv.index = idx;
         cv.rel = effective;
         cv.focus = card_focus;
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

      /* case shadow + plastic frame */
      fill_rect(fb, x + 6, y + 7, w + pad * 2, h + pad * 2, pack1555(1, 5, 12));
      fill_rect(fb, x - pad, y - pad, w + pad * 2, h + pad * 2, case_bg);
      frame_rect(fb, x - pad, y - pad, w + pad * 2, h + pad * 2, 2, selected ? cyan : border);
      fill_rect(fb, x, y, w, h, case_inner);

      /* system-branded top strip: replaces generic screenshot-card look */
      const int brand_h = selected ? 29 : 22;
      fill_rect(fb, x + 5, y + 5, w - 10, brand_h, pack1555(4, 11, 23));
      stage44_draw_brand_badge(fb, sys, x + w / 2, y + 7, w - 18, selected);

      const int art_y = y + brand_h + 9;
      const int art_h = h - brand_h - 39;
      fill_rect(fb, x + 7, art_y, w - 14, art_h, pack1555(3, 7, 14));
      if (!stage42_draw_art(fb, g.image_path, x + 7, art_y, w - 14, art_h))
         draw_fallback_card(fb, sys, x + 7, art_y, w - 14, art_h);

      /* lower brand plaque creates the console-box visual seen in the reference */
      stage44_draw_brand_badge(fb, sys, x + w / 2, y + h - 26, w - 24, false);

      if (selected)
         stage44_draw_glow_frame(fb, x - pad, y - pad, w + pad * 2, h + pad * 2,
                                 focus == FocusZone::Games);

      /* reflection is intentionally short and dark so it never competes with the title */
      if (selected || std::fabs(cv.rel) <= 2.1f)
         stage44_reflect_rect(fb, x - pad, y - pad, w + pad * 2, h + pad * 2, selected ? 34 : 22);
   }

   const Game &selected = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(selected.title, 2, 520);
   const int tw = text_width(title, 2);
   draw_text(fb, center - tw / 2, 613, title, 2, focus == FocusZone::Games ? text : dim);
   const std::string metadata = stage44_brand_name(sys) + "  *  " +
         std::to_string(game_pos + 1) + "/" + std::to_string(sys.games.size());
   const int mw = text_width(metadata, 1);
   draw_text(fb, center - mw / 2, 638, metadata, 1, dim);
}
'''
src = replace_between(src, 'static void stage42_draw_games(', 'static void stage42_draw_ui(', games)

ui = r'''static void stage42_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      FocusZone focus, float game_shift, float system_shift)
{
   const uint16_t text = pack1555(232, 243, 253);
   const uint16_t dim = pack1555(120, 147, 178);
   const uint16_t cyan = pack1555(75, 202, 255);
   stage44_draw_night_background(fb);

   /* PixelStation-like header, but project-branded. */
   draw_text(fb, 28, 19, "H3531 RETRO", 3, text);
   draw_text(fb, 30, 48, "GAMES  PAST  ALWAYS  PLAY", 1, dim);

   std::time_t tt = std::time(nullptr);
   std::tm *tmv = std::localtime(&tt);
   char timebuf[16] = "--:--";
   if (tmv) std::strftime(timebuf, sizeof(timebuf), "%H:%M", tmv);
   const int timew = text_width(timebuf, 2);
   draw_text(fb, (int)fb.w - timew - 38, 20, timebuf, 2, text);
   draw_text(fb, (int)fb.w - 190, 49, "RETROARCH / LIBRETRO", 1, dim);

   if (vis.empty())
   {
      draw_text(fb, 80, 300, "NO GAMES FOUND", 4, text);
      draw_text(fb, 82, 355, "Put ROMs in /mnt/usb/games/nes or /mnt/usb/games/md", 2, dim);
      return;
   }

   stage42_draw_system_row(fb, systems, vis, visible_pos, focus, system_shift);
   const SystemDef &sys = systems[vis[visible_pos]];

   /* thin divider + dynamic era mark just like the reference layout */
   fill_rect(fb, 76, 272, (int)fb.w - 152, 1, pack1555(48, 93, 132));
   const std::string era = stage44_era_name(sys);
   const int ew = text_width(era, 1);
   fill_rect(fb, (int)fb.w / 2 - ew / 2 - 13, 264, ew + 26, 18, pack1555(3, 10, 21));
   draw_text(fb, (int)fb.w / 2 - ew / 2, 268, era, 1, pack1555(159, 187, 219));

   stage42_draw_games(fb, sys, game_pos, game_shift, focus);

   const int footer_y = (int)fb.h - 54;
   fill_rect(fb, 0, footer_y, (int)fb.w, 54, pack1555(3, 12, 25));
   fill_rect(fb, 0, footer_y, (int)fb.w, 1, pack1555(47, 101, 144));

   int x = 34;
   const int by = footer_y + 16;
   stage44_draw_footer_button(fb, x, by, "<>", focus == FocusZone::Games ? "GAME" : "SYSTEM", cyan);
   stage44_draw_footer_button(fb, x, by, "A", focus == FocusZone::Games ? "LAUNCH" : "GAMES", pack1555(66, 220, 163));
   stage44_draw_footer_button(fb, x, by, "B", "EXIT", pack1555(239, 79, 92));
   stage44_draw_footer_button(fb, x, by, "Y", "RETROARCH", pack1555(240, 199, 72));
   draw_text(fb, (int)fb.w - 198, footer_y + 15, "GOOD GAMES", 1, dim);
   draw_text(fb, (int)fb.w - 198, footer_y + 30, "BRIGHTER TOMORROWS", 1, pack1555(72, 99, 130));
}
'''
src = replace_between(src, 'static void stage42_draw_ui(', 'static int stage42_layout_test()', ui)

# Theme-specific CI markers.
layout_marker = '   printf("PIXEL_ICON_STYLE original-programmatic no-external-theme-assets\\n");\n'
if layout_marker not in src:
    raise SystemExit("Stage4.3 layout marker missing")
extra_test = (
    '   printf("STAGE44_THEME pixelstation-night-coverflow procedural-background\\n");\n'
    '   printf("STAGE44_COVERS branded-case glow reflection seven-card-layout\\n");\n'
    '   printf("STAGE44_SYSTEMS custom-brand-badges stable-linear pixel-controllers\\n");\n'
)
src = src.replace(layout_marker, layout_marker + extra_test, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STAGE44_PIXELSTATION_THEME_PATCH_OK {src_path} -> {out_path}")
