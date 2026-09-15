#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage46_sprite_ui.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.5 PixelStation reference-fidelity assets active'
new_marker = 'Stage4.6 PixelStation sprite-frame UI active'
if old_marker not in src:
    raise SystemExit("expected Stage4.5 marker not found")
src = src.replace(old_marker, new_marker, 1)

# Lighter cover-flow and shorter transitions. The artwork/panel/frame hierarchy now
# comes from PNG sprites, so the CPU no longer builds multi-pass procedural frames.
replacements = {
    'static const int STAGE42_SMALL_W = 164;': 'static const int STAGE42_SMALL_W = 150;',
    'static const int STAGE42_SMALL_H = 266;': 'static const int STAGE42_SMALL_H = 244;',
    'static const int STAGE42_SELECTED_W = 224;': 'static const int STAGE42_SELECTED_W = 208;',
    'static const int STAGE42_SELECTED_H = 294;': 'static const int STAGE42_SELECTED_H = 284;',
    'static const int STAGE42_SYSTEM_GAP = 40;': 'static const int STAGE42_SYSTEM_GAP = 34;',
    'static const int STAGE42_SYSTEM_SMALL_W = 190;': 'static const int STAGE42_SYSTEM_SMALL_W = 178;',
    'static const int STAGE42_SYSTEM_SELECTED_W = 232;': 'static const int STAGE42_SYSTEM_SELECTED_W = 224;',
    'static const uint64_t STAGE42_GAME_ANIM_MS = 170;': 'static const uint64_t STAGE42_GAME_ANIM_MS = 125;',
    'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 160;': 'static const uint64_t STAGE42_SYSTEM_ANIM_MS = 125;',
}
for old, new in replacements.items():
    if old not in src:
        raise SystemExit(f"Stage4.6 constant not found: {old}")
    src = src.replace(old, new, 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"start anchor not found: {start}")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"end anchor not found: {end}")
    return text[:a] + replacement.rstrip() + "\n\n" + text[b:]

helpers = r'''static std::string stage46_ui_asset(const char *name)
{
   return stage45_asset_path((std::string("ui/") + name).c_str());
}

static uint16_t stage46_blend1555(uint16_t dst, unsigned char sr, unsigned char sg,
      unsigned char sb, unsigned char alpha)
{
   if (alpha == 0) return dst;
   if (alpha >= 250) return pack1555(sr, sg, sb);
   const int dr = ((dst >> 10) & 31) * 255 / 31;
   const int dg = ((dst >> 5) & 31) * 255 / 31;
   const int db = (dst & 31) * 255 / 31;
   const int a = alpha;
   const int ia = 255 - a;
   return pack1555((sr * a + dr * ia) / 255,
                   (sg * a + dg * ia) / 255,
                   (sb * a + db * ia) / 255);
}

static bool stage46_draw_asset_alpha(Fb &fb, const std::string &path,
      int x, int y, int w, int h, bool cover)
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
         const int dx = x + xx;
         if (dx < 0 || dx >= (int)fb.w) continue;
         const int rx = dx - ox;
         const int ry = dy - oy;
         if (rx < 0 || ry < 0 || rx >= dw || ry >= dh) continue;
         const int src_x = std::min(img->w - 1, (int)((int64_t)rx * img->w / dw));
         const int src_y = std::min(img->h - 1, (int)((int64_t)ry * img->h / dh));
         const unsigned char *p = img->rgba.data() + ((size_t)src_y * img->w + src_x) * 4U;
         if (p[3]) row[dx] = stage46_blend1555(row[dx], p[0], p[1], p[2], p[3]);
      }
   }
   return true;
}

static bool stage46_draw_background(Fb &fb)
{
   /* Stage4.6 intentionally has exactly one background source. */
   if (stage46_draw_asset_alpha(fb, stage45_asset_path("stage45_bg.png"),
         0, 0, (int)fb.w, (int)fb.h, true))
      return true;
   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, pack1555(2, 13, 29));
   return false;
}

static void stage46_draw_system_panel(Fb &fb, int x, int y, int w, int h,
      bool bright_focus)
{
   const int margin = std::max(6, w / 30);
   stage46_draw_asset_alpha(fb, stage46_ui_asset("system_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2, true);
   stage46_draw_asset_alpha(fb,
         stage46_ui_asset(bright_focus ? "system_frame_focus.png" : "system_frame_idle.png"),
         x, y, w, h, false);
}

static void stage46_draw_rom_panel(Fb &fb, int x, int y, int w, int h,
      bool bright_focus)
{
   const int margin = std::max(6, w / 28);
   stage46_draw_asset_alpha(fb, stage46_ui_asset("rom_panel_bg.png"),
         x + margin, y + margin, w - margin * 2, h - margin * 2, true);
   stage46_draw_asset_alpha(fb,
         stage46_ui_asset(bright_focus ? "rom_frame_focus.png" : "rom_frame_idle.png"),
         x, y, w, h, false);
}
'''

anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit("Stage4.6 system row anchor not found")
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
      const float f = sv.focus;
      const int w = (int)std::lround(STAGE42_SYSTEM_SMALL_W +
            (STAGE42_SYSTEM_SELECTED_W - STAGE42_SYSTEM_SMALL_W) * f);
      const int cx = center + (int)std::lround(stage42_system_center_offset(sv.rel));
      const bool selected = std::fabs(sv.rel) < 0.50f;
      const int frame_h = selected ? 148 : 122;
      const int x = cx - w / 2;
      const int y = selected ? 96 : 108;

      /* Only the selected system gets a panel/frame. The frame is bright only
       * when the Systems zone owns focus; otherwise it deliberately uses the
       * dim sprite so the ROM focus below is unambiguous. */
      if (selected)
         stage46_draw_system_panel(fb, x, y, w, frame_h, focus == FocusZone::Systems);

      const std::string asset = stage45_controller_asset(s);
      bool drawn = false;
      if (!asset.empty())
      {
         const int bx = selected ? x + 16 : x + 5;
         const int by = selected ? y + 12 : y + 5;
         const int bw = selected ? w - 32 : w - 10;
         const int bh = selected ? 96 : 86;
         drawn = stage45_draw_asset(fb, asset, bx, by, bw, bh, false, 0);
      }
      if (!drawn)
      {
         const SystemVisualSpec &visual = stage43_visual_spec(s);
         const uint16_t body = selected ? pack1555(196, 205, 216) : pack1555(103, 117, 135);
         const uint16_t ink = selected ? pack1555(21, 26, 34) : pack1555(36, 46, 60);
         const uint16_t accent = pack1555(visual.accent_r, visual.accent_g, visual.accent_b);
         stage43_draw_system_icon(fb, s, cx, y + 54, selected ? 1.12f : 0.90f, body, ink, accent);
      }

      const std::string label = stage42_system_short_name(s);
      const int scale = selected ? 2 : 1;
      const int tw = text_width(label, scale);
      const int ly = selected ? y + 118 : 228;
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
      const float f = cv.focus;
      const int w = (int)std::lround(STAGE42_SMALL_W + (STAGE42_SELECTED_W - STAGE42_SMALL_W) * f);
      const int h = (int)std::lround(STAGE42_SMALL_H + (STAGE42_SELECTED_H - STAGE42_SMALL_H) * f);
      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;
      const int y = card_center_y - h / 2;
      const bool selected = f > 0.5f;
      const int pad = STAGE42_CARD_PAD;
      const int fx = x - pad;
      const int fy = y - pad;
      const int fw = w + pad * 2;
      const int fh = h + pad * 2;

      /* Sprite panel first, then art, then frame. No procedural round-frame and
       * no second bottom brand badge. */
      stage46_draw_rom_panel(fb, fx, fy, fw, fh,
            selected && focus == FocusZone::Games);

      const int margin = std::max(7, fw / 28);
      const int brand_h = selected ? 25 : 21;
      const int art_x = fx + margin + 2;
      const int art_y = fy + margin + brand_h + 3;
      const int art_w = fw - margin * 2 - 4;
      const int art_h = fh - margin * 2 - brand_h - 8;
      if (!stage45_draw_game_cover(fb, g.image_path, art_x, art_y, art_w, art_h, 4))
         draw_fallback_card(fb, sys, art_x, art_y, art_w, art_h);

      /* The top system badge is retained; the duplicated bottom badge from
       * Stage4.5 is intentionally gone. */
      const std::string brand = stage44_brand_name(sys);
      const int bscale = 1;
      const int bw = text_width(brand, bscale);
      draw_text(fb, fx + fw / 2 - bw / 2, fy + margin + 7, brand, bscale,
            selected ? text : dim);

      /* Re-draw frame after cover so its transparent PNG edge is always crisp. */
      stage46_draw_asset_alpha(fb,
            stage46_ui_asset(selected && focus == FocusZone::Games ?
               "rom_frame_focus.png" : "rom_frame_idle.png"),
            fx, fy, fw, fh, false);
   }

   const Game &selected = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(selected.title, 2, 540);
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

ui = r'''static void stage42_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      FocusZone focus, float game_shift, float system_shift)
{
   const uint16_t text = pack1555(227, 240, 255);
   const uint16_t dim = pack1555(128, 157, 199);
   const uint16_t footer = pack1555(4, 17, 36);
   const uint16_t accent = pack1555(75, 202, 255);

   /* One fixed background only. Foreground state changes; background does not. */
   stage46_draw_background(fb);

   draw_text(fb, 36, 24, "PIXELSTATION", 2, text);
   draw_text(fb, 37, 55, "GAMES PAST ALWAYS PLAY", 1, dim);

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

   /* Raised footer: no longer glued to the physical bottom edge. */
   const int footer_y = 656;
   fill_rect(fb, 0, footer_y, (int)fb.w, 46, footer);
   fill_rect(fb, 0, footer_y, (int)fb.w, 1, accent);
   draw_text(fb, 40, footer_y + 16, "NAVIGATE", 1, dim);
   draw_text(fb, 208, footer_y + 16, "A  LAUNCH", 1, dim);
   draw_text(fb, 378, footer_y + 16, "B  BACK", 1, dim);
   draw_text(fb, 518, footer_y + 16, "Y  SYSTEM MENU", 1, dim);
   draw_text(fb, 738, footer_y + 16, "OPTIONS", 1, dim);
}
'''
src = replace_between(src, 'static void stage42_draw_ui(', 'static int stage42_layout_test()', ui)

# Stage4.6 proof markers for host/QEMU CI.
layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE46_SPRITE_UI png-frames grid-panels single-background alpha-blend\\n");\n'
    '   printf("STAGE46_FOCUS systems-bright-only-when-focused roms-bright-only-when-focused\\n");\n'
    '   printf("STAGE46_GEOMETRY divider_y=285 card_center_y=457 title_y=610 footer_y=656\\n");\n'
    '   printf("STAGE46_ASSETS ui/system_frame_idle.png ui/system_frame_focus.png ui/rom_frame_idle.png ui/rom_frame_focus.png\\n");\n'
)
if layout_ok not in src:
    raise SystemExit("Stage4.6 layout-test marker anchor not found")
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STAGE46_SPRITE_UI_PATCH_OK {src_path} -> {out_path}")
