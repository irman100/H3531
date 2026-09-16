#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage415_cartridge_media_v2.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.13 PixelStation library-features UI active'
new_marker = 'Stage4.15 PixelStation cartridge-media UI active'
if old_marker not in src:
    raise SystemExit('Stage4.13 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Stage4.13 inherits geometry through several previous patches. Normalize by
# symbol name instead of depending on a historical numeric value.
for name, value in {
    'STAGE42_SMALL_W': 190,
    'STAGE42_SMALL_H': 250,
    'STAGE42_SELECTED_W': 238,
    'STAGE42_SELECTED_H': 286,
}.items():
    pattern = rf'static const int {name} = \d+;'
    src, count = re.subn(pattern, f'static const int {name} = {value};', src, count=1)
    if count != 1:
        raise SystemExit(f'geometry constant missing: {name}')


def between(text, start, end, repl):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + repl.rstrip() + '\n\n' + text[b:]

reflection = r'''static void stage413_reflection(Fb &fb, int x, int y, int w, int h, bool selected)
{
   const int dst0 = y + h + 3;
   const int max_h = std::max(0, STAGE413_FOOTER - dst0 - 2);
   const int rh = std::min(h, max_h);
   if (rh <= 1) return;
   const int a0 = selected ? 92 : 64;
   for (int r = 0; r < rh; ++r)
   {
      const int sy = y + h - 1 - r;
      const int dy = dst0 + r;
      if (sy < y || dy < 0 || dy >= (int)fb.h) break;
      const int a = a0 * (rh - r) / std::max(1, rh);
      uint16_t *dst = fb_row(fb, dy);
      const uint16_t *sp = fb_row(fb, sy);
      for (int xx = std::max(0, x); xx < std::min((int)fb.w, x + w); ++xx)
         dst[xx] = stage48_blend_pixel(dst[xx], sp[xx], a);
   }
}
'''
src = between(src, 'static void stage413_reflection(', 'static void stage413_menu_draw(', reflection)

helpers = r'''struct Stage415LabelRect {
   int x = 0, y = 0, w = 0, h = 0;
};

static std::string stage415_media_asset(const SystemDef &sys)
{
   const std::string n = lower(sys.name);
   if (n == "nes" || n == "famicom" || n == "dendy")
      return stage45_asset_path("media/cartridge_nes.png");
   if (n == "megadrive" || n == "genesis" || n == "mastersystem" || n == "gamegear")
      return stage45_asset_path("media/cartridge_megadrive.png");
   if (n == "snes" || n == "supernes" || n == "sfc")
      return stage45_asset_path("media/cartridge_snes.png");
   if (n.find("atari") != std::string::npos)
      return stage45_asset_path("media/cartridge_atari.png");
   if (n == "psx" || n == "ps1" || n == "playstation")
      return stage45_asset_path("media/case_playstation.png");
   return stage45_asset_path("media/cartridge_nes.png");
}

static std::string stage415_system_label(const SystemDef &sys)
{
   const std::string n = lower(sys.name);
   if (n == "nes" || n == "famicom" || n == "dendy") return "NES";
   if (n == "megadrive" || n == "genesis") return "MEGA DRIVE";
   if (n == "mastersystem") return "MASTER SYSTEM";
   if (n == "gamegear") return "GAME GEAR";
   if (n == "snes" || n == "supernes" || n == "sfc") return "SNES";
   if (n.find("atari") != std::string::npos) return "ATARI";
   if (n == "psx" || n == "ps1" || n == "playstation") return "PLAYSTATION";
   return ascii_safe(sys.fullname.empty() ? sys.name : sys.fullname);
}

static Stage415LabelRect stage415_label_rect(const SystemDef &sys, int x, int y, int w, int h)
{
   const std::string n = lower(sys.name);
   int xp = 14, yp = 25, wp = 72, hp = 32;
   if (n == "nes" || n == "famicom" || n == "dendy")
      { xp = 13; yp = 29; wp = 74; hp = 30; }
   else if (n == "megadrive" || n == "genesis" || n == "mastersystem" || n == "gamegear")
      { xp = 13; yp = 25; wp = 74; hp = 35; }
   else if (n == "snes" || n == "supernes" || n == "sfc")
      { xp = 14; yp = 25; wp = 72; hp = 31; }
   else if (n.find("atari") != std::string::npos)
      { xp = 16; yp = 22; wp = 68; hp = 31; }
   else if (n == "psx" || n == "ps1" || n == "playstation")
      { xp = 17; yp = 14; wp = 65; hp = 62; }
   Stage415LabelRect r;
   r.x = x + w * xp / 100;
   r.y = y + h * yp / 100;
   r.w = std::max(24, w * wp / 100);
   r.h = std::max(24, h * hp / 100);
   return r;
}

static void stage415_fallback_label(Fb &fb, const SystemDef &sys, const Game &g,
      const Stage415LabelRect &r, bool selected)
{
   const uint16_t bg = pack1555(7, 18, 36);
   const uint16_t line = selected ? pack1555(70, 225, 255) : pack1555(92, 101, 112);
   const uint16_t text = selected ? pack1555(239, 248, 255) : pack1555(190, 197, 207);
   fill_rect(fb, r.x, r.y, r.w, r.h, bg);
   frame_rect(fb, r.x, r.y, r.w, r.h, 1, line);
   std::string system = stage42_ellipsize_px(stage415_system_label(sys), 1, r.w - 12);
   const std::string raw_rom = stem_of(g.rom_path);
   std::string rom = stage42_ellipsize_px(raw_rom, 1, r.w - 12);
   const int sy = r.y + std::max(6, r.h / 3 - 4);
   const int ry = r.y + std::max(18, (r.h * 2) / 3 - 4);
   draw_text(fb, r.x + (r.w - text_width(system, 1)) / 2, sy, system, 1, text);
   draw_text(fb, r.x + (r.w - text_width(rom, 1)) / 2, ry, rom, 1, text);
}

static bool stage415_draw_rom_label(Fb &fb, const SystemDef &sys, const Game &g,
      const Stage415LabelRect &r, bool selected)
{
   if (!g.image_path.empty() &&
       stage45_draw_game_cover(fb, g.image_path, r.x, r.y, r.w, r.h, 2))
      return true;
   stage415_fallback_label(fb, sys, g, r, selected);
   return false;
}
'''
anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit('system-row anchor missing in Stage4.15')
src = src.replace(anchor, helpers + '\n\n' + anchor, 1)

games = r'''static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;
   const uint16_t text = pack1555(237, 246, 255);
   const uint16_t dim = pack1555(126, 151, 188);
   const int center = (int)fb.w / 2;

   std::map<size_t, Stage42CardVisual> unique;
   for (int rel = -3; rel <= 3; ++rel)
   {
      const float er = (float)rel + game_shift;
      if (std::fabs(er) > 2.65f) continue;
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      const size_t idx = (size_t)gp;
      Stage42CardVisual cv; cv.index = idx; cv.rel = er;
      cv.focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(er)));
      auto it = unique.find(idx);
      if (it == unique.end() || stage42_should_replace_duplicate(it->second.rel, er, game_shift)) unique[idx] = cv;
   }

   std::vector<Stage42CardVisual> cards;
   for (auto &kv : unique) cards.push_back(kv.second);
   std::sort(cards.begin(), cards.end(), [](const Stage42CardVisual &a, const Stage42CardVisual &b) { return a.focus < b.focus; });

   for (const auto &cv : cards)
   {
      const Game &g = sys.games[cv.index];
      const float f = stage47_quantize_focus(cv.focus);
      const int w = (int)std::lround(STAGE42_SMALL_W + (STAGE42_SELECTED_W - STAGE42_SMALL_W) * f);
      const int h = (int)std::lround(STAGE42_SMALL_H + (STAGE42_SELECTED_H - STAGE42_SMALL_H) * f);
      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;
      const bool selected = std::fabs(cv.rel) < 0.50f;
      const int pad = STAGE42_CARD_PAD;
      const int fw = w + pad * 2, fh = h + pad * 2;
      const int fx = x - pad, fy = STAGE413_CARD_FLOOR - fh;

      const std::string media = stage415_media_asset(sys);
      const bool media_ok = stage48_draw_exact_asset(fb, media, fx, fy, fw, fh, 255, 0);
      if (!media_ok)
         stage48_draw_exact_asset(fb, stage46_ui_asset("rom_panel_bg.png"), fx + 9, fy + 9, fw - 18, fh - 18, 255, 0);

      const Stage415LabelRect lr = stage415_label_rect(sys, fx, fy, fw, fh);
      stage415_draw_rom_label(fb, sys, g, lr, selected);
      if (stage413_favorite(g)) stage413_star(fb, fx + fw - (selected ? 31 : 20), fy + 18, selected);
      stage48_draw_exact_asset(fb, stage46_ui_asset(selected && focus == FocusZone::Games ?
            "rom_frame_focus.png" : "rom_frame_idle.png"), fx, fy, fw, fh, 255, 0);
      stage413_reflection(fb, fx, fy, fw, fh, selected);
   }

   const Game &g = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(g.title, 2, 560);
   draw_text(fb, center - text_width(title, 2) / 2, 580, title, 2, focus == FocusZone::Games ? text : dim);
   const std::string meta = stage415_system_label(sys) + "  *  " + std::to_string(game_pos + 1) + "/" + std::to_string(sys.games.size());
   draw_text(fb, center - text_width(meta, 1) / 2, 608, meta, 1, dim);
}
'''
src = between(src, 'static void stage42_draw_games(', 'static void stage42_draw_ui(', games)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE415_MEDIA five-wide system-specific-cartridge-or-case\\n");\n'
    '   printf("STAGE415_LABEL rom-art-overlay fallback-system-plus-rom-name\\n");\n'
    '   printf("STAGE415_REFLECTION one-to-one-mirror clipped-at-footer alpha-selected92-idle64\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE415_CARTRIDGE_MEDIA_PATCH_OK {src_path} -> {out_path}')
