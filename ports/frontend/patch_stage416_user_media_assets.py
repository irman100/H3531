#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage416_user_media_assets.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.15 PixelStation cartridge-media UI active'
new_marker = 'Stage4.16 PixelStation user-media-assets UI active'
if old_marker not in src:
    raise SystemExit('Stage4.15 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Wider, denser five-item cartridge row. The physical cartridge artwork itself
# is the card chrome now, so there is no extra sprite-frame padding.
for name, value in {
    'STAGE42_CARD_GAP': 6,
    'STAGE42_CARD_PAD': 0,
    'STAGE42_SMALL_W': 218,
    'STAGE42_SMALL_H': 204,
    'STAGE42_SELECTED_W': 268,
    'STAGE42_SELECTED_H': 232,
}.items():
    pattern = rf'static const int {name} = \d+;'
    src, count = re.subn(pattern, f'static const int {name} = {value};', src, count=1)
    if count != 1:
        raise SystemExit(f'constant missing: {name}')


def between(text, start, end, repl):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + repl.rstrip() + '\n\n' + text[b:]

helpers = r'''struct Stage416MediaRect {
   int x = 0, y = 0, w = 0, h = 0;
};

static bool stage416_draw_media_contain(Fb &fb, const std::string &path,
      int box_x, int box_y, int box_w, int box_h, Stage416MediaRect &out)
{
   Stage42Art *img = stage42_get_art(path);
   if (!img || img->w <= 0 || img->h <= 0 || box_w <= 0 || box_h <= 0)
      return false;

   const double sx = (double)box_w / (double)img->w;
   const double sy = (double)box_h / (double)img->h;
   const double s = std::min(sx, sy);
   const int dw = std::max(1, (int)std::lround(img->w * s));
   const int dh = std::max(1, (int)std::lround(img->h * s));
   const int dx = box_x + (box_w - dw) / 2;
   const int dy = box_y + (box_h - dh) / 2;

   for (int yy = 0; yy < dh; ++yy)
   {
      const int syy = (int)((int64_t)yy * img->h / dh);
      const int dyy = dy + yy;
      if (dyy < 0 || dyy >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dyy);
      for (int xx = 0; xx < dw; ++xx)
      {
         const int dxx = dx + xx;
         if (dxx < 0 || dxx >= (int)fb.w) continue;
         const int sxx = (int)((int64_t)xx * img->w / dw);
         const unsigned char *p = img->rgba.data() + ((size_t)syy * img->w + sxx) * 4U;
         if (p[3] < 8) continue;
         const uint16_t fg = pack1555(p[0], p[1], p[2]);
         row[dxx] = p[3] >= 245 ? fg : stage48_blend_pixel(row[dxx], fg, p[3]);
      }
   }

   out.x = dx; out.y = dy; out.w = dw; out.h = dh;
   return true;
}

static Stage415LabelRect stage416_label_rect(const SystemDef &sys, const Stage416MediaRect &m)
{
   const std::string n = lower(sys.name);
   int xp = 10, yp = 22, wp = 75, hp = 65; // NES / Dendy large face label
   if (n == "megadrive" || n == "genesis" || n == "mastersystem" || n == "gamegear")
      { xp = 13; yp = 22; wp = 74; hp = 58; }
   else if (n == "snes" || n == "supernes" || n == "sfc")
      { xp = 10; yp = 20; wp = 80; hp = 33; }
   else if (n.find("atari") != std::string::npos)
      { xp = 11; yp = 11; wp = 81; hp = 59; }
   else if (n == "psx" || n == "ps1" || n == "playstation")
      { xp = 18; yp = 12; wp = 77; hp = 75; }

   Stage415LabelRect r;
   r.x = m.x + m.w * xp / 100;
   r.y = m.y + m.h * yp / 100;
   r.w = std::max(24, m.w * wp / 100);
   r.h = std::max(24, m.h * hp / 100);
   return r;
}
'''
anchor = 'static void stage42_draw_games('
if anchor not in src:
    raise SystemExit('draw-games anchor missing')
src = src.replace(anchor, helpers + '\n\n' + anchor, 1)

games = r'''static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;
   const uint16_t text = pack1555(237, 246, 255);
   const uint16_t dim = pack1555(126, 151, 188);
   const uint16_t active_line = pack1555(78, 232, 255);
   const int center = (int)fb.w / 2;

   std::map<size_t, Stage42CardVisual> unique;
   for (int rel = -3; rel <= 3; ++rel)
   {
      const float er = (float)rel + game_shift;
      if (std::fabs(er) > 2.70f) continue;
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      const size_t idx = (size_t)gp;
      Stage42CardVisual cv; cv.index = idx; cv.rel = er;
      cv.focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(er)));
      auto it = unique.find(idx);
      if (it == unique.end() || stage42_should_replace_duplicate(it->second.rel, er, game_shift))
         unique[idx] = cv;
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
      const bool selected = std::fabs(cv.rel) < 0.50f;
      const int fx = x;
      const int fy = STAGE413_CARD_FLOOR - h;

      Stage416MediaRect mr;
      const std::string media = stage415_media_asset(sys);
      if (!stage416_draw_media_contain(fb, media, fx, fy, w, h, mr))
      {
         mr.x = fx; mr.y = fy; mr.w = w; mr.h = h;
         fill_rect(fb, fx, fy, w, h, pack1555(18, 28, 42));
      }

      const Stage415LabelRect lr = stage416_label_rect(sys, mr);
      stage415_draw_rom_label(fb, sys, g, lr, selected);

      if (stage413_favorite(g))
         stage413_star(fb, mr.x + mr.w - (selected ? 27 : 18), mr.y + 10, selected);

      // No neon frame: the physical media silhouette is the card. A short
      // floor marker indicates game focus without covering the cartridge.
      if (selected && focus == FocusZone::Games)
         fill_rect(fb, mr.x + mr.w / 4, STAGE413_CARD_FLOOR + 1, mr.w / 2, 3, active_line);

      stage413_reflection(fb, mr.x, mr.y, mr.w, mr.h, selected);
   }

   const Game &g = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(g.title, 2, 560);
   draw_text(fb, center - text_width(title, 2) / 2, 580, title, 2,
             focus == FocusZone::Games ? text : dim);
   const std::string meta = stage415_system_label(sys) + "  *  " +
                            std::to_string(game_pos + 1) + "/" +
                            std::to_string(sys.games.size());
   draw_text(fb, center - text_width(meta, 1) / 2, 608, meta, 1, dim);
}
'''
src = between(src, 'static void stage42_draw_games(', 'static void stage42_draw_ui(', games)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE416_MEDIA exact-user-assets contain-no-stretch five-wide\\n");\n'
    '   printf("STAGE416_LABEL system-specific-overlay NES10-22-75-65 MD13-22-74-58 SNES10-20-80-33 ATARI11-11-81-59 PS18-12-77-75\\n");\n'
    '   printf("STAGE416_FRAME none physical-media-silhouette focus-floor-marker\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE416_USER_MEDIA_PATCH_OK {src_path} -> {out_path}')
