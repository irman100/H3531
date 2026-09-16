#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage413.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.8 PixelStation perspective-polish UI active'
new_marker = 'Stage4.13 PixelStation library-features UI active'
if old_marker not in src:
    raise SystemExit('Stage4.8 marker not found')
src = src.replace(old_marker, new_marker, 1)

if '#include <fstream>\n' not in src:
    src = src.replace('#include <sys/time.h>\n', '#include <sys/time.h>\n#include <fstream>\n', 1)


def between(text, start, end, repl):
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit(f'anchor missing: {start} / {end}')
    return text[:a] + repl.rstrip() + '\n\n' + text[b:]

src = between(src, 'static float stage47_quantize_focus(float f)',
              'static bool stage47_draw_background_cached', r'''static float stage47_quantize_focus(float f)
{
   return stage42_clamp01(f);
}
''')

helpers = r'''static const char *STAGE413_USER_DIR = "/mnt/usb/H3531/USER/gamefront";
static const char *STAGE413_SETTINGS = "/mnt/usb/H3531/USER/gamefront/settings.cfg";
static const char *STAGE413_FAVORITES = "/mnt/usb/H3531/USER/gamefront/favorites.txt";
static const char *STAGE413_RA_CFG = "/mnt/usb/H3531/USER/gamefront/retroarch_override.cfg";
static const int STAGE413_CARD_FLOOR = 566;
static const int STAGE413_FOOTER = 666;

struct Stage413Settings {
   bool auto_save_load = true;
   bool rewind = false;
   int state_slot = 0;
};

static Stage413Settings stage413_settings;
static std::set<std::string> stage413_favorites;

static void stage413_dirs()
{
   mkdir("/mnt/usb/H3531/USER", 0755);
   mkdir(STAGE413_USER_DIR, 0755);
}

static bool stage413_bool(const std::string &v)
{
   const std::string s = lower(trim(v));
   return s == "1" || s == "true" || s == "yes" || s == "on";
}

static void stage413_load_settings()
{
   stage413_settings = Stage413Settings{};
   std::ifstream in(STAGE413_SETTINGS);
   std::string line;
   while (std::getline(in, line))
   {
      const size_t p = line.find('=');
      if (p == std::string::npos) continue;
      const std::string k = lower(trim(line.substr(0, p)));
      const std::string v = trim(line.substr(p + 1));
      if (k == "auto_save_load") stage413_settings.auto_save_load = stage413_bool(v);
      else if (k == "rewind") stage413_settings.rewind = stage413_bool(v);
      else if (k == "state_slot") stage413_settings.state_slot = std::max(0, std::min(9, atoi(v.c_str())));
   }
}

static void stage413_save_settings()
{
   stage413_dirs();
   std::ofstream out(STAGE413_SETTINGS, std::ios::trunc);
   if (!out) return;
   out << "auto_save_load=" << (stage413_settings.auto_save_load ? 1 : 0) << "\n";
   out << "rewind=" << (stage413_settings.rewind ? 1 : 0) << "\n";
   out << "state_slot=" << stage413_settings.state_slot << "\n";
}

static void stage413_write_ra_cfg()
{
   stage413_dirs();
   std::ofstream out(STAGE413_RA_CFG, std::ios::trunc);
   if (!out) return;
   out << "savestate_auto_save = \"" << (stage413_settings.auto_save_load ? "true" : "false") << "\"\n";
   out << "savestate_auto_load = \"" << (stage413_settings.auto_save_load ? "true" : "false") << "\"\n";
   out << "savestate_thumbnail_enable = \"true\"\n";
   out << "state_slot = \"" << stage413_settings.state_slot << "\"\n";
   out << "input_save_state = \"f2\"\n";
   out << "input_load_state = \"f4\"\n";
   out << "input_state_slot_decrease = \"f6\"\n";
   out << "input_state_slot_increase = \"f7\"\n";
   out << "input_rewind = \"r\"\n";
   out << "input_menu_toggle = \"f1\"\n";
   out << "rewind_enable = \"" << (stage413_settings.rewind ? "true" : "false") << "\"\n";
   out << "rewind_buffer_size = \"8\"\n";
   out << "rewind_granularity = \"1\"\n";
   out << "config_save_on_exit = \"false\"\n";
}

static void stage413_load_favorites()
{
   stage413_favorites.clear();
   std::ifstream in(STAGE413_FAVORITES);
   std::string line;
   while (std::getline(in, line))
   {
      line = trim(line);
      if (!line.empty()) stage413_favorites.insert(line);
   }
}

static void stage413_save_favorites()
{
   stage413_dirs();
   std::ofstream out(STAGE413_FAVORITES, std::ios::trunc);
   if (!out) return;
   for (const auto &p : stage413_favorites) out << p << "\n";
}

static bool stage413_favorite(const Game &g)
{
   return stage413_favorites.count(g.rom_path) != 0;
}

static void stage413_sort(SystemDef &sys)
{
   std::stable_sort(sys.games.begin(), sys.games.end(), [](const Game &a, const Game &b) {
      const bool af = stage413_favorites.count(a.rom_path) != 0;
      const bool bf = stage413_favorites.count(b.rom_path) != 0;
      if (af != bf) return af > bf;
      return lower(a.title) < lower(b.title);
   });
}

static void stage413_sort_all(std::vector<SystemDef> &systems)
{
   for (auto &s : systems) stage413_sort(s);
}

static size_t stage413_toggle_favorite(SystemDef &sys, size_t pos)
{
   if (sys.games.empty() || pos >= sys.games.size()) return 0;
   const std::string keep = sys.games[pos].rom_path;
   if (stage413_favorites.count(keep)) stage413_favorites.erase(keep);
   else stage413_favorites.insert(keep);
   stage413_save_favorites();
   stage413_sort(sys);
   for (size_t i = 0; i < sys.games.size(); ++i)
      if (sys.games[i].rom_path == keep) return i;
   return 0;
}

static std::string stage413_launch(const SystemDef &sys, const Game &game)
{
   stage413_write_ra_cfg();
   std::string cmd = launch_command(sys, game);
   const std::string rom = shell_quote(game.rom_path);
   const std::string extra = std::string("--appendconfig ") + shell_quote(STAGE413_RA_CFG) + " ";
   const size_t p = cmd.rfind(rom);
   if (p != std::string::npos) cmd.insert(p, extra);
   else cmd += " " + extra;
   return cmd;
}

static void stage413_star(Fb &fb, int x, int y, bool selected)
{
   static const char *p[7] = {"...#...", "...#...", "#######", ".#####.", "..###..", ".#...#.", "#.....#"};
   const int s = selected ? 2 : 1;
   const uint16_t c = selected ? pack1555(255, 220, 70) : pack1555(185, 169, 96);
   for (int yy = 0; yy < 7; ++yy)
      for (int xx = 0; xx < 7; ++xx)
         if (p[yy][xx] == '#') fill_rect(fb, x + xx * s, y + yy * s, s, s, c);
}

static void stage413_reflection(Fb &fb, int x, int y, int w, int h, bool selected)
{
   const int dst0 = y + h + 3;
   const int rh = std::max(0, STAGE413_FOOTER - dst0 - 3);
   if (rh <= 1) return;
   const int a0 = selected ? 58 : 42;
   for (int r = 0; r < rh; ++r)
   {
      const int sy = y + h - 1 - (int)((int64_t)r * (h - 1) / std::max(1, rh - 1));
      const int dy = dst0 + r;
      if (dy < 0 || dy >= (int)fb.h) continue;
      const int a = a0 * (rh - 1 - r) / std::max(1, rh - 1);
      uint16_t *dst = fb_row(fb, dy);
      const uint16_t *sp = fb_row(fb, sy);
      for (int xx = std::max(0, x); xx < std::min((int)fb.w, x + w); ++xx)
         dst[xx] = stage48_blend_pixel(dst[xx], sp[xx], a);
   }
}

static void stage413_menu_draw(Fb &fb, int item, const Game *game)
{
   const int x = 338, y = 132, w = 604, h = 430;
   const uint16_t bg = pack1555(5, 15, 31);
   const uint16_t hi = pack1555(17, 67, 104);
   const uint16_t line = pack1555(69, 221, 255);
   const uint16_t text = pack1555(234, 245, 255);
   const uint16_t dim = pack1555(134, 158, 183);
   fill_rect(fb, x, y, w, h, bg);
   frame_rect(fb, x, y, w, h, 2, line);
   draw_text(fb, x + 24, y + 18, "QUICK SETTINGS", 2, text);

   std::vector<std::string> rows;
   rows.push_back("BACK TO LIBRARY");
   rows.push_back(std::string("FAVORITE            ") + (game && stage413_favorite(*game) ? "YES" : "NO"));
   rows.push_back(std::string("AUTO SAVE / LOAD    ") + (stage413_settings.auto_save_load ? "ON" : "OFF"));
   rows.push_back(std::string("REWIND              ") + (stage413_settings.rewind ? "ON" : "OFF"));
   rows.push_back(std::string("SAVE STATE SLOT      ") + std::to_string(stage413_settings.state_slot));
   rows.push_back("RETROARCH FULL MENU");
   rows.push_back("SAVE SETTINGS & BACK");

   for (int i = 0; i < 7; ++i)
   {
      const int yy = y + 70 + i * 39;
      if (i == item) { fill_rect(fb, x + 14, yy - 6, w - 28, 31, hi); fill_rect(fb, x + 14, yy - 6, 4, 31, line); }
      draw_text(fb, x + 30, yy + 2, rows[(size_t)i], 2, i == item ? text : dim);
   }
   draw_text(fb, x + 24, y + h - 57, "IN GAME: F2 SAVE  F4 LOAD  F6/F7 SLOT  R REWIND  F1 MENU", 1, dim);
   draw_text(fb, x + 24, y + h - 33, "UP/DOWN SELECT  LEFT/RIGHT CHANGE  ENTER APPLY  ESC/F1 BACK", 1, dim);
}
'''

anchor = 'static void stage42_draw_system_row('
if anchor not in src:
    raise SystemExit('system-row anchor missing')
src = src.replace(anchor, helpers + '\n\n' + anchor, 1)

src = src.replace('const int y = selected ? 101 : 112;', 'const int y = selected ? 66 : 82;', 1)
src = src.replace('const int ly = selected ? y + 111 : 227;', 'const int ly = selected ? y + 111 : y + 96;', 1)

games = r'''static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;
   const uint16_t text = pack1555(237, 246, 255);
   const uint16_t dim = pack1555(126, 151, 188);
   const int center = (int)fb.w / 2;

   std::map<size_t, Stage42CardVisual> unique;
   for (int rel = -4; rel <= 4; ++rel)
   {
      const float er = (float)rel + game_shift;
      if (std::fabs(er) > 3.35f) continue;
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
      const int margin = 12;

      stage48_draw_exact_asset(fb, stage46_ui_asset("rom_panel_bg.png"),
            fx + margin, fy + margin, fw - margin * 2, fh - margin * 2, 255, 0);

      const int art_gap = selected ? 9 : 7;
      const int brand_h = selected ? 23 : 19;
      const int ax = fx + margin + art_gap;
      const int ay = fy + margin + brand_h + 7;
      const int aw = fw - (margin + art_gap) * 2;
      const int ah = fh - margin * 2 - brand_h - 16;
      if (!stage48_draw_cover_shear(fb, g.image_path, ax, ay, aw, ah, 5, 0)) draw_fallback_card(fb, sys, ax, ay, aw, ah);

      const std::string brand = stage44_brand_name(sys);
      draw_text(fb, fx + fw / 2 - text_width(brand, 1) / 2, fy + margin + 5, brand, 1, selected ? text : dim);
      if (stage413_favorite(g)) stage413_star(fb, fx + fw - (selected ? 31 : 20), fy + 18, selected);

      stage48_draw_exact_asset(fb, stage46_ui_asset(selected && focus == FocusZone::Games ?
            "rom_frame_focus.png" : "rom_frame_idle.png"), fx, fy, fw, fh, 255, 0);
      stage413_reflection(fb, fx, fy, fw, fh, selected);
   }

   const Game &g = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(g.title, 2, 540);
   draw_text(fb, center - text_width(title, 2) / 2, 580, title, 2, focus == FocusZone::Games ? text : dim);
   const std::string meta = stage44_brand_name(sys) + "  *  " + std::to_string(game_pos + 1) + "/" + std::to_string(sys.games.size());
   draw_text(fb, center - text_width(meta, 1) / 2, 608, meta, 1, dim);
}
'''
src = between(src, 'static void stage42_draw_games(', 'static void stage42_draw_ui(', games)

src = src.replace('fill_rect(fb, 72, 285, (int)fb.w - 144, 1, pack1555(53, 109, 163));',
                  'fill_rect(fb, 72, 258, (int)fb.w - 144, 1, pack1555(53, 109, 163));', 1)
src = src.replace('fill_rect(fb, (int)fb.w / 2 - ew / 2 - 12, 278, ew + 24, 16, pack1555(2, 9, 20));',
                  'fill_rect(fb, (int)fb.w / 2 - ew / 2 - 12, 251, ew + 24, 16, pack1555(2, 9, 20));', 1)
src = src.replace('draw_text(fb, (int)fb.w / 2 - ew / 2, 281, era, 1, pack1555(164, 192, 230));',
                  'draw_text(fb, (int)fb.w / 2 - ew / 2, 254, era, 1, pack1555(164, 192, 230));', 1)
src = src.replace('const int footer_y = 656;', 'const int footer_y = STAGE413_FOOTER;', 1)
old_footer = '''   draw_text(fb, 40, footer_y + 16, "NAVIGATE", 1, dim);\n   draw_text(fb, 208, footer_y + 16, "A  LAUNCH", 1, dim);\n   draw_text(fb, 378, footer_y + 16, "B  BACK", 1, dim);\n   draw_text(fb, 518, footer_y + 16, "Y  SYSTEM MENU", 1, dim);\n   draw_text(fb, 738, footer_y + 16, "OPTIONS", 1, dim);'''
new_footer = '''   if (focus == FocusZone::Games)\n      draw_text(fb, 92, footer_y + 16, "LEFT/RIGHT GAME   UP SYSTEMS   ENTER PLAY   F1 MENU   F5 RESCAN   ESC EXIT", 1, dim);\n   else\n      draw_text(fb, 92, footer_y + 16, "LEFT/RIGHT SYSTEM   DOWN GAMES   ENTER GAMES   F1 MENU   ESC EXIT", 1, dim);'''
if old_footer not in src:
    raise SystemExit('footer block missing')
src = src.replace(old_footer, new_footer, 1)

modal = r'''static void stage413_quick_menu(Fb &physical, Input &in, Stage42Backbuffer &back,
      std::vector<SystemDef> &systems, size_t visible_pos, size_t &game_pos, FocusZone focus)
{
   int item = 0;
   bool open = true;
   while (open)
   {
      auto vis = visible_systems(systems);
      if (vis.empty()) return;
      if (visible_pos >= vis.size()) visible_pos = 0;
      SystemDef &sys = systems[vis[visible_pos]];
      if (!sys.games.empty() && game_pos >= sys.games.size()) game_pos = 0;
      const Game *g = sys.games.empty() ? nullptr : &sys.games[game_pos];

      stage42_draw_ui(back.fb, systems, visible_pos, vis, game_pos, focus, 0.0f, 0.0f);
      stage413_menu_draw(back.fb, item, g);
      stage42_present(physical, back);

      const Action a = input_poll(in);
      switch (a)
      {
         case Action::PrevSystem: item = item ? item - 1 : 6; break;
         case Action::NextSystem: item = (item + 1) % 7; break;
         case Action::PrevGame:
         case Action::NextGame:
         {
            const int dir = a == Action::PrevGame ? -1 : 1;
            if (item == 1 && !sys.games.empty()) game_pos = stage413_toggle_favorite(sys, game_pos);
            else if (item == 2) stage413_settings.auto_save_load = !stage413_settings.auto_save_load;
            else if (item == 3) stage413_settings.rewind = !stage413_settings.rewind;
            else if (item == 4) stage413_settings.state_slot = (stage413_settings.state_slot + dir + 10) % 10;
            stage413_save_settings(); stage413_write_ra_cfg();
            break;
         }
         case Action::Launch:
            if (item == 0) open = false;
            else if (item == 1 && !sys.games.empty()) game_pos = stage413_toggle_favorite(sys, game_pos);
            else if (item == 2) stage413_settings.auto_save_load = !stage413_settings.auto_save_load;
            else if (item == 3) stage413_settings.rewind = !stage413_settings.rewind;
            else if (item == 4) stage413_settings.state_slot = (stage413_settings.state_slot + 1) % 10;
            else if (item == 5)
            {
               run_external(physical, in, std::string(kRetroArchMenu));
               if (!back.init(physical)) return;
            }
            else if (item == 6) open = false;
            stage413_save_settings(); stage413_write_ra_cfg();
            break;
         case Action::ServiceMenu:
         case Action::Exit: open = false; break;
         default: break;
      }
      usleep(10000);
   }
}
'''
src = src.replace('static int stage42_layout_test()', modal + '\n\nstatic int stage42_layout_test()', 1)

old_scan = '   scan_all(systems);\n\n   Fb physical;'
new_scan = '''   stage413_dirs();\n   stage413_load_settings();\n   stage413_load_favorites();\n   stage413_write_ra_cfg();\n   scan_all(systems);\n   stage413_sort_all(systems);\n\n   Fb physical;'''
if old_scan not in src:
    raise SystemExit('main startup block missing')
src = src.replace(old_scan, new_scan, 1)

src = src.replace('run_external(physical, in, launch_command(s, s.games[game_pos]));',
                  'run_external(physical, in, stage413_launch(s, s.games[game_pos]));', 1)

old_service = '''         case Action::ServiceMenu:\n            game_anim = Stage42GameAnim{};\n            system_anim = Stage42SystemAnim{};\n            run_external(physical, in, std::string(kRetroArchMenu));\n            if (!back.init(physical)) return 5;\n            redraw = true;\n            break;'''
new_service = '''         case Action::ServiceMenu:\n            game_anim = Stage42GameAnim{};\n            system_anim = Stage42SystemAnim{};\n            stage413_quick_menu(physical, in, back, systems, visible_pos, game_pos, focus);\n            redraw = true;\n            break;'''
if old_service not in src:
    raise SystemExit('service-menu block missing')
src = src.replace(old_service, new_service, 1)

src = src.replace('         case Action::Rescan:\n            scan_all(systems);',
                  '         case Action::Rescan:\n            scan_all(systems);\n            stage413_sort_all(systems);', 1)

layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE413_LAYOUT systems-up card-floor=566 footer=666 bottom-aligned\\n");\n'
    '   printf("STAGE413_REFLECTION full-card-mirror-to-footer alpha-fade\\n");\n'
    '   printf("STAGE413_MENU favorite autosave-load rewind state-slot retroarch-menu\\n");\n'
    '   printf("STAGE413_FAVORITES persistent-per-system favorites-first star-badge\\n");\n'
    '   printf("STAGE413_RETROARCH appendconfig F2-save F4-load R-rewind F1-menu\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout marker missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE413_LIBRARY_FEATURES_PATCH_OK {src_path} -> {out_path}')
