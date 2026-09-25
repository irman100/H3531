#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage441_favorites_search.py INPUT OUTPUT')

src = Path(sys.arvg[1]).read_text(encoding='utf-8')

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0: raise SystemExit('function not found: ' + signature)
    brace = text.find('{', start)
    if brace < 0: raise SystemExit('opening brace not found: ' + signature)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == '{': depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[:start] + replacement.rstrip() + text[i+1:]
    raise SystemExit('unterminated function: ' + signature)

helpers = r'''
static bool stage441_is_favorites(const SystemDef &s)
{
   return lower(s.name) == "favorites";
}

static void stage441_draw_big_star(Fb &fb, int cx, int cy, bool selected)
{
   /* Lightweight 11x11 pixel star; scaled primitives avoid image assets. */
   static const char *p[11] = {
      ".....#.....",
      "....###....",
      "....###....",
      "###########",
      ".#########.",
      "..#######..",
      "...#####...",
      "..###.###..",
      ".##.....##.",
      "##.......##",
      "#.........#"
   };
   const int s = selected ? 6 : 4;
   const uint16_t c = selected ? pack1555(255, 220, 70) : pack1555(205, 182, 82);
   const int w = 11 * s;
   const int h = 11 * s;
   for (int yy = 0; yy < 11; ++yy)
      for (int xx = 0; xx < 11; ++xx)
         if (p[yy][xx] == '#')
            fill_rect(fb, cx - w/2 + xx*s, cy - h/2 + yy*s, s, s, c);
}

static void stage441_rebuild_favorites(std::vector<SystemDef> &systems)
{
   /* Remove an older synthetic view first. */
   for (auto it = systems.begin(); it != systems.end(); )
   {
      if (stage441_is_favorites(*it)) it = systems.erase(it);
      else ++it;
   }

   SystemDef fav;
   fav.name = "favorites";
   fav.fullname = "FAVORITES";
   fav.path = "/mnt/usb/H3531/USER/gamefront";
   fav.theme = "favorites";

   for (const auto &s : systems)
      for (const auto &g : s.games)
         if (stage413_favorite(g))
            fav.games.push_back(g);

   std::stable_sort(fav.games.begin(), fav.games.end(), [](const Game &a, const Game &b) {
      return lower(a.title) < lower(b.title);
   });
   systems.insert(systems.begin(), fav);
}

static bool stage441_resolve_original(const std::vector<SystemDef> &systems,
      const std::string &rom, size_t &sys_index, size_t &game_index)
{
   for (size_t si = 0; si < systems.size(); ++si)
   {
      if (stage441_is_favorites(systems[si])) continue;
      for (size_t gi = 0; gi < systems[si].games.size(); ++gi)
      {
         if (systems[si].games[gi].rom_path == rom)
         {
            sys_index = si;
            game_index = gi;
            return true;
         }
      }
   }
   return false;
}

static size_t stage441_visible_pos_for_system(const std::vector<SystemDef> &systems,
      size_t sys_index)
{
   const auto vis = visible_systems(systems);
   for (size_t p = 0; p < vis.size(); ++p)
      if (vis[p] == sys_index) return p;
   return 0;
}

static void stage441_toggle_path(std::vector<SystemDef> &systems,
      const std::string &rom)
{
   if (stage413_favorites.count(rom)) stage413_favorites.erase(rom);
   else stage413_favorites.insert(rom);
   stage413_save_favorites();

   for (auto &s : systems)
      if (!stage441_is_favorites(s)) stage413_sort(s);
   stage441_rebuild_favorites(systems);
}

struct Stage441SearchHit {
   size_t system = 0;
   size_t game = 0;
};

static std::vector<Stage441SearchHit> stage441_search_hits(
      const std::vector<SystemDef> &systems, const std::string &query)
{
   std::vector<Stage441SearchHit> out;
   const std::string q = lower(trim(query));
   if (q.empty()) return out;

   for (size_t si = 0; si < systems.size(); ++si)
   {
      if (stage441_is_favorites(systems[si])) continue;
      for (size_t gi = 0; gi < systems[si].games.size(); ++gi)
      {
         const Game &g = systems[si].games[gi];
         const std::string hay = lower(g.title + " " + stem_of(g.rom_path) + " " +
               systems[si].fullname + " " + systems[si].name);
         if (hay.find(q) != std::string::npos)
            out.push_back(Stage441SearchHit{si, gi});
      }
   }
   std::stable_sort(out.begin(), out.end(), [&](const Stage441SearchHit &a,
         const Stage441SearchHit &b) {
      return lower(systems[a.system].games[a.game].title) <
             lower(systems[b.system].games[b.game].title);
   });
   return out;
}

static char stage441_key_char(unsigned code)
{
   switch (code)
   {
      case KEY_A: return 'A'; case KEY_B: return 'B'; case KEY_C: return 'C';
      case KEY_D: return 'D'; case KEY_E: return 'E'; case KEY_F: return 'F';
      case KEY_G: return 'G'; case KEY_H: return 'H'; case KEY_I: return 'I';
      case KEY_J: return 'J'; case KEY_K: return 'K'; case KEY_L: return 'L';
      case KEY_M: return 'M'; case KEY_N: return 'N'; case KEY_O: return 'O';
      case KEY_P: return 'P'; case KEY_Q: return 'Q'; case KEY_R: return 'R';
      case KEY_S: return 'S'; case KEY_T: return 'T'; case KEY_U: return 'U';
      case KEY_V: return 'V'; case KEY_W: return 'W'; case KEY_X: return 'X';
      case KEY_Y: return 'Y'; case KEY_Z: return 'Z';
      case KEY_0: return '0'; case KEY_1: return '1'; case KEY_2: return '2';
      case KEY_3: return '3'; case KEY_4: return '4'; case KEY_5: return '5';
      case KEY_6: return '6'; case KEY_7: return '7'; case KEY_8: return '8';
      case KEY_9: return '9'; case KEY_SPACE: return ' ';
      default: return 0;
   }
}

static void stage441_draw_search(Fb &fb, const std::vector<SystemDef> &systems,
      const std::string &query, int key_index, bool result_mode, int result_index)
{
   const uint16_t bg = pack1555(4, 13, 28);
   const uint16_t panel = pack1555(10, 25, 46);
   const uint16_t line = pack1555(75, 155, 205);
   const uint16_t text = pack1555(238, 246, 255);
   const uint16_t dim = pack1555(135, 158, 185);
   const uint16_t hi = pack1555(34, 91, 138);
   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);
   fill_rect(fb, 65, 55, 1150, 610, panel);
   frame_rect(fb, 65, 55, 1150, 610, 2, line);
   draw_text(fb, 100, 82, "SEARCH GAMES", 3, text);

   std::string shown = query.empty() ? "TYPE WITH PAD OR KEYBOARD..." : query;
   draw_text(fb, 100, 135, stage42_ellipsize_px(shown, 2, 620), 2,
         query.empty() ? dim : text);

   static const char *keys = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
   const int cols = 6;
   for (int i = 0; i < 36; ++i)
   {
      const int row = i / cols, col = i % cols;
      const int x = 100 + col * 78;
      const int y = 205 + row * 47;
      if (!result_mode && i == key_index) fill_rect(fb, x - 9, y - 9, 48, 38, hi);
      char label[2] = {keys[i], 0};
      draw_text(fb, x, y, label, 2, text);
   }
   draw_text(fb, 100, 500, "A ADD   B BACK   START/Y RESULTS   KEYBOARD ALSO WORKS", 1, dim);
   draw_text(fb, 100, 525, "LEFT/RIGHT/UP/DOWN MOVE   F3 SEARCH", 1, dim);

   const auto hits = stage441_search_hits(systems, query);
   const int rx = 650, ry = 150;
   draw_text(fb, rx, 112, "RESULTS", 2, text);
   if (hits.empty())
      draw_text(fb, rx, ry, query.empty() ? "ENTER A QUERY" : "NO MATCHES", 2, dim);
   else
   {
      const int visible = std::min(9, (int)hits.size());
      int start = 0;
      if (result_index >= visible) start = result_inde²È="25É¥¹œ•É„€ôÍÑ…”ÐÑ}•É…}¹…µ”¡ÍåÌ¤ìœœœ°(œœœ€€½¹ÍÐÍÑèéÍÑÉ¥¹œ•É„€ôÍÑ…”ÐÐÅ}¥Í}™…Ù½É¥Ñ•Ì¡ÍåÌ¤€ü€‰e=UHY=I%QLˆ€èÍÑ…”ÐÑ}•É…}¹…µ”¡ÍåÌ¤ìœœœ°€Ä¤((Œ½½Ñ•È‘½Õµ•¹ÑÌ½¹ÑÉ½±±•Èµ™¥ÉÍÐÍ¡•±°‰¥¹‘¥¹Ì¸)ÍÉŒ€ôÍÉŒ¹É•Á±…” (œœœ€€€€€‘É…Ý}Ñ•áÐ¡™ˆ°€äÈ°™½½Ñ•É}ä€¬€ÄØ°€‰1P½I%!P5€€U@MeMQ5L€€9QHA1d€€Ä59T€€ÔIM8€€Ma%Pˆ°€Ä°‘¥´¤ìœœœ°(œœœ€€€€€‘É…Ý}Ñ•áÐ¡™ˆ°€ÐÐ°™½½Ñ•É}ä€¬€ÄÐ°€‰MQ%,½A9X€€0Ä½HÄMeMQ4€€0È½HÈY=I%Q€€dMI €€A1d€€MQIP59Tˆ°€Ä°‘¥´¤ìœœœ°€Ä¤)ÍÉŒ€ôÍÉŒ¹É•Á±…” (œœœ€€€€€‘É…Ý}Ñ•áÐ¡™ˆ°€äÈ°™½½Ñ•É}ä€¬€ÄØ°€‰1P½I%!PMeMQ4€€=]85L€€9QH5L€€Ä59T€€Ma%Pˆ°€Ä°‘¥´¤ìœœœ°(œœœ€€€€€‘É…Ý}Ñ•áÐ¡™ˆ°€ÐÐ°™½½Ñ•É}ä€¬€ÄÐ°€‰MQ%,½A9X€€0Ä½HÄMeMQ4€€0È½HÈY=I%Q€€dMI €€M1P€€	,ˆ°€Ä°‘¥´¤ìœœœ°€Ä¤((ŒMÑ…”Ð¸ÐÄµ…É­•È¸)ÍÉŒ€ôÍÉŒ¹É•Á±…” MÑ…”Ð¸Ìä‘…ÁÑ¥Ù”…¹…±½œ…á¥Ì…ÁÑÕÉ”…Ñ¥Ù”œ°(€€€€€€€€€€€€€€€€€€MÑ…”Ð¸ÐÄ…Ù½É¥Ñ•Ì!½µ”M•…É …¹½¹ÑÉ½±±•È9…Ù¥…Ñ¥½¸…Ñ¥Ù”œ°€Ä¤((ŒMÑ…ÉÑÕÀèµ…Ñ•É¥…±¥é”Íå¹Ñ¡•Ñ¥Œ…Ù½É¥Ñ•Ì‰•™½É”½Á•¹¥¹œÑ¡”U$¸)½±‘}ÍÑ…ÉÐ€ô€œœœ€€Í…¹}…±°¡ÍåÍÑ•µÌ¤íq¸€€ÍÑ…”ÐÄÍ}Í½ÉÑ}…±°¡ÍåÍÑ•µÌ¤ìœœœ)¹•Ý}ÍÑ…ÉÐ€ô€œœœ€€Í…¹}…±°¡ÍåÍÑ•µÌ¤íq¸€€ÍÑ…”ÐÄÍ}Í½ÉÑ}…±°¡ÍåÍÑ•µÌ¤íq¸€€ÍÑ…”ÐÐÅ}É•‰Õ¥±‘}™…Ù½É¥Ñ•Ì¡ÍåÍÑ•µÌ¤ìœœœ)¥˜½±‘}ÍÑ…ÉÐ¹½Ð¥¸ÍÉŒèÉ…¥Í”MåÍÑ•µá¥Ð ÍÑ…ÉÑÕÀÍ…¸…¹¡½Èµ¥ÍÍ¥¹œœ¤)ÍÉŒ€ôÍÉŒ¹É•Á±…”¡½±‘}ÍÑ…ÉÐ°¹•Ý}ÍÑ…ÉÐ°€Ä¤((Œ5…¥¸…Ñ¥½¸ÍÝ¥Ñ …‘‘¥Ñ¥½¹Ì¸¥É•ÐÍ¡½Õ±‘•ÉÌÁ…”ÍåÍÑ•µÌÉ•…É‘±•ÍÌ½˜™½ÕÌ¸)¹••‘±”€ô€œœœ€€€€€€€€…Í”Ñ¥½¸èéAÉ•ÙMåÍÑ•´éq¸€€€€€€€€€€€¥˜€¡™½ÕÌ€ôô½ÕÍi½¹”èé…µ•Ì¤ì™½ÕÌ€ô½ÕÍi½¹”èéMåÍÑ•µÌìÉ•‘É…Ü€ôÑÉÕ”ìõq¸€€€€€€€€€€€‰É•…¬íq¹q¸€€€€€€€€…Í”Ñ¥½¸èé9•áÑMåÍÑ•´éq¸€€€€€€€€€€€¥˜€¡™½ÕÌ€ôô½ÕÍi½¹”èéMåÍÑ•µÌ¤ì™½ÕÌ€ô½ÕÍi½¹”èé…µ•ÌìÉ•‘É…Ü€ôÑÉÕ”ìõq¸€€€€€€€€€€€‰É•…¬ìœœœ)É•Á±…•µ•¹Ð€ô€œœœ€€€€€€€€…Í”Ñ¥½¸èéAÉ•ÙMåÍÑ•´éq¸€€€€€€€€€€€¥˜€¡™½ÕÌ€ôô½ÕÍi½¹”èé…µ•Ì¤ì™½ÕÌ€ô½ÕÍi½¹”èéMåÍÑ•µÌìÉ•‘É…Ü€ôÑÉÕ”ìõq¸€€€€€€€€€€€‰É•…¬íq¹q¸€€€€€€€€…Í”Ñ¥½¸èé9•áÑMåÍÑ•´éq¸€€€€€€€€€€€¥˜€¡™½ÕÌ€ôô½ÕÍi½¹”èéMåÍÑ•µÌ¤ì™½ÕÌ€ô½ÕÍi½¹”èé…µ•ÌìÉ•‘É…Ü€ôÑÉÕ”ìõq¸€€€€€€€€€€€‰É•…¬íq¹q¸€€€€€€€€…Í”Ñ¥½¸èéAÉ•ÙMåÍÑ•µ¥É•Ðéq¸€€€€€€€€€€€¥˜€ …Ù¥Ì¹•µÁÑä ¤€˜˜€…ÍåÍÑ•µ}…Ñ¥Ù”¥q¸€€€€€€€€€€€íq¸€€€€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ôÙ¥Í¥‰±•}Á½Ì€üÙ¥Í¥‰±•}Á½Ì€´€Ä€èÙ¥Ì¹Í¥é” ¤€´€Äíq¸€€€€€€€€€€€€€€…µ•}Á½Ì€ô€Àíq¸€€€€€€€€€€€€€€™½ÕÌ€ô½ÕÍi½¹”èé…µ•Ìíq¸€€€€€€€€€€€€€€ÍåÍÑ•µ}…¹¥´¹‘¥È€ô€´Äíq¸€€€€€€€€€€€€€€ÍåÍÑ•µ}…¹¥´¹ÍÑ…ÉÐ€ôÍÑ…”ÐÉ}¹½Ý}µÌ ¤íq¸€€€€€€€€€€€€€€É•‘É…Ü€ôÑÉÕ”íq¸€€€€€€€€€€€õq¸€€€€€€€€€€€‰É•…¬íq¹q¸€€€€€€€€…Í”Ñ¥½¸èé9•áÑMåÍÑ•µ¥É•Ðéq¸€€€€€€€€€€€¥˜€ …Ù¥Ì¹•µÁÑä ¤€˜˜€…ÍåÍÑ•µ}…Ñ¥Ù”¥q¸€€€€€€€€€€€íq¸€€€€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ô€¡Ù¥Í¥‰±•}Á½Ì€¬€Ä¤€”Ù¥Ì¹Í¥é” ¤íq¸€€€€€€€€€€€€€€…µ•}Á½Ì€ô€Àíq¸€€€€€€€€€€€€€€™½ÕÌ€ô½ÕÍi½¹”èé…µ•Ìíq¸€€€€€€€€€€€€€€ÍåÍÑ•µ}…¹¥´¹‘¥È€ô€Äíq¸€€€€€€€€€€€€€€ÍåÍÑ•µ}…¹¥´¹ÍÑ…ÉÐ€ôÍÑ…”ÐÉ}¹½Ý}µÌ ¤íq¸€€€€€€€€€€€€€€É•‘É…Ü€ôÑÉÕ”íq¸€€€€€€€€€€€õq¸€€€€€€€€€€€‰É•…¬íq¹q¸€€€€€€€€…Í”Ñ¥½¸èéQ½±•…Ù½É¥Ñ”éq¸€€€€€€€€€€€¥˜€ …Ù¥Ì¹•µÁÑä ¤¥q¸€€€€€€€€€€€íq¸€€€€€€€€€€€€€€MåÍÑ•µ•˜€™Ì€ôÍåÍÑ•µÍmÙ¥ÍmÙ¥Í¥‰±•}Á½Íutíq¸€€€€€€€€€€€€€€¥˜€ …Ì¹…µ•Ì¹•µÁÑä ¤€˜˜…µ•}Á½Ì€ðÌ¹…µ•Ì¹Í¥é” ¤¥q¸€€€€€€€€€€€€€€íq¸€€€€€€€€€€€€€€€€€½¹ÍÐ‰½½°Ý…Í}™…Ù½É¥Ñ•Ì€ôÍÑ…”ÐÐÅ}¥Í}™…Ù½É¥Ñ•Ì¡Ì¤íq¸€€€€€€€€€€€€€€€€€½¹ÍÐÍÑèéÍÑÉ¥¹œÕÉÉ•¹Ñ}É½´€ôÌ¹…µ•Ím…µ•}Á½Ít¹É½µ}Á…Ñ íq¸€€€€€€€€€€€€€€€€€½¹ÍÐÍÑèéÍÑÉ¥¹œÕÉÉ•¹Ñ}ÍåÍÑ•µ}¹…µ”€ôÌ¹¹…µ”íq¸€€€€€€€€€€€€€€€€€ÍÑ…”ÐÐÅ}Ñ½±•}Á…Ñ ¡ÍåÍÑ•µÌ°ÕÉÉ•¹Ñ}É½´¤íq¸€€€€€€€€€€€€€€€€€…ÕÑ¼¹•Ý}Ù¥Ì€ôÙ¥Í¥‰±•}ÍåÍÑ•µÌ¡ÍåÍÑ•µÌ¤íq¸€€€€€€€€€€€€€€€€€¥˜€¡Ý…Í}™…Ù½É¥Ñ•Ì¥q¸€€€€€€€€€€€€€€€€€íq¸€€€€€€€€€€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ô€Àíq¸€€€€€€€€€€€€€€€€€€€€¥˜€ …ÍåÍÑ•µÍlÁt¹…µ•Ì¹•µÁÑä ¤€˜˜…µ•}Á½Ì€øôÍåÍÑ•µÍlÁt¹…µ•Ì¹Í¥é” ¤¥q¸€€€€€€€€€€€€€€€€€€€€€€€…µ•}Á½Ì€ôÍåÍÑ•µÍlÁt¹…µ•Ì¹Í¥é” ¤€´€Äíq¸€€€€€€€€€€€€€€€€€€€€•±Í”¥˜€¡ÍåÍÑ•µÍlÁt¹…µ•Ì¹•µÁÑä ¤¤…µ•}Á½Ì€ô€Àíq¸€€€€€€€€€€€€€€€€€õq¸€€€€€€€€€€€€€€€€€•±Í•q¸€€€€€€€€€€€€€€€€€íq¸€€€€€€€€€€€€€€€€€€€€Í¥é•}ÐÉ•Í½±Ù•‘}Í¤€ô€À°É•Í½±Ù•‘}¤€ô€Àíq¸€€€€€€€€€€€€€€€€€€€€¥˜€¡ÍÑ…”ÐÐÅ}É•Í½±Ù•}½É¥¥¹…°¡ÍåÍÑ•µÌ°ÕÉÉ•¹Ñ}É½´°É•Í½±Ù•‘}Í¤°É•Í½±Ù•‘}¤¤¥q¸€€€€€€€€€€€€€€€€€€€€íq¸€€€€€€€€€€€€€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ôÍÑ…”ÐÐÅ}Ù¥Í¥‰±•}Á½Í}™½É}ÍåÍÑ•´¡ÍåÍÑ•µÌ°É•Í½±Ù•‘}Í¤¤íq¸€€€€€€€€€€€€€€€€€€€€€€€…µ•}Á½Ì€ôÉ•Í½±Ù•‘}¤íq¸€€€€€€€€€€€€€€€€€€€€õq¸€€€€€€€€€€€€€€€€€õq¸€€€€€€€€€€€€€€€€€É•‘É…Ü€ôÑÉÕ”íq¸€€€€€€€€€€€€€€õq¸€€€€€€€€€€€õq¸€€€€€€€€€€€‰É•…¬íq¹q¸€€€€€€€€…Í”Ñ¥½¸èéM•…É éq¸€€€€€€€€íq¸€€€€€€€€€€€Í¥é•}ÐÑ…É•Ñ}ÍåÍÑ•´€ô€À°Ñ…É•Ñ}…µ”€ô€Àíq¸€€€€€€€€€€€…µ•}…¹¥´€ôMÑ…”ÐÉ…µ•¹¥µíôíq¸€€€€€€€€€€€ÍåÍÑ•µ}…¹¥´€ôMÑ…”ÐÉMåÍÑ•µ¹¥µíôíq¸€€€€€€€€€€€¥˜€¡ÍÑ…”ÐÐÅ}Í•…É¡}‘¥…±½œ¡Á¡åÍ¥…°°¥¸°‰…¬°ÍåÍÑ•µÌ°Ñ…É•Ñ}ÍåÍÑ•´°Ñ…É•Ñ}…µ”¤¥q¸€€€€€€€€€€€íq¸€€€€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ôÍÑ…”ÐÐÅ}Ù¥Í¥‰±•}Á½Í}™½É}ÍåÍÑ•´¡ÍåÍÑ•µÌ°Ñ…É•Ñ}ÍåÍÑ•´¤íq¸€€€€€€€€€€€€€€…µ•}Á½Ì€ôÑ…É•Ñ}…µ”íq¸€€€€€€€€€€€€€€™½ÕÌ€ô½ÕÍi½¹”èé…µ•Ìíq¸€€€€€€€€€€€õq¸€€€€€€€€€€€É•‘É…Ü€ôÑÉÕ”íq¸€€€€€€€€€€€‰É•…¬íq¸€€€€€€€€ôœœœ)¥˜¹••‘±”¹½Ð¥¸ÍÉŒèÉ…¥Í”MåÍÑ•µá¥Ð µ…¥¸¹…Ù¥…Ñ¥½¸…Ñ¥½¸…¹¡½Èµ¥ÍÍ¥¹œœ¤)ÍÉŒ€ôÍÉŒ¹É•Á±…”¡¹••‘±”°É•Á±…•µ•¹Ð°€Ä¤((Œ…Ù½É¥Ñ•Ì±…Õ¹ É•Í½±Ù•Ì‰…¬Ñ¼Ñ¡”½Ý¹¥¹œÉ•…°ÍåÍÑ•´¸)½±‘}±…Õ¹ €ô€œœœ€€€€€€€€€€€€€€€€€ÉÕ¹}•áÑ•É¹…°¡Á¡åÍ¥…°°¥¸°ÍÑ…”ÐÄÍ}±…Õ¹ ¡Ì°Ì¹…µ•Ím…µ•}Á½Ít¤¤íq¸€€€€€€€€€€€€€€€€€¥˜€ …‰…¬¹¥¹¥Ð¡Á¡åÍ¥…°¤¤É•ÑÕÉ¸€Ôìœœœ)¹•Ý}±…Õ¹ €ô€œœœ€€€€€€€€€€€€€€€€€¥˜€¡ÍÑ…”ÐÐÅ}¥Í}™…Ù½É¥Ñ•Ì¡Ì¤¥q¸€€€€€€€€€€€€€€€€€íq¸€€€€€€€€€€€€€€€€€€€€Í¥é•}Ð½Ý¹•É}Í¤€ô€À°½Ý¹•É}¤€ô€Àíq¸€€€€€€€€€€€€€€€€€€€€¥˜€¡ÍÑ…”ÐÐÅ}É•Í½±Ù•}½É¥¥¹…°¡ÍåÍÑ•µÌ°Ì¹…µ•Ím…µ•}Á½Ít¹É½µ}Á…Ñ °½Ý¹•É}Í¤°½Ý¹•É}¤¤¥q¸€€€€€€€€€€€€€€€€€€€€€€€ÉÕ¹}•áÑ•É¹…°¡Á¡åÍ¥…°°¥¸°ÍÑ…”ÐÄÍ}±…Õ¹ ¡ÍåÍÑ•µÍm½Ý¹•É}Í¥t°ÍåÍÑ•µÍm½Ý¹•É}Í¥t¹…µ•Ím½Ý¹•É}¥t¤¤íq¸€€€€€€€€€€€€€€€€€õq¸€€€€€€€€€€€€€€€€€•±Í•q¸€€€€€€€€€€€€€€€€€€€€ÉÕ¹}•áÑ•É¹…°¡Á¡åÍ¥…°°¥¸°ÍÑ…”ÐÄÍ}±…Õ¹ ¡Ì°Ì¹…µ•Ím…µ•}Á½Ít¤¤íq¸€€€€€€€€€€€€€€€€€¥˜€ …‰…¬¹¥¹¥Ð¡Á¡åÍ¥…°¤¤É•ÑÕÉ¸€Ôìœœœ)¥˜½±‘}±…Õ¹ ¹½Ð¥¸ÍÉŒèÉ…¥Í”MåÍÑ•µá¥Ð ±…Õ¹ …¹¡½Èµ¥ÍÍ¥¹œœ¤)ÍÉŒ€ôÍÉŒ¹É•Á±…”¡½±‘}±…Õ¹ °¹•Ý}±…Õ¹ °€Ä¤((ŒI•Í…¸ÁÉ•Í•ÉÙ•ÌÍå¹Ñ¡•Ñ¥Œ…Ù½É¥Ñ•Ì…¹É•ÑÕÉ¹Ì¡½µ”Ñ¡•É”¸)½±‘}É•Í…¸€ô€œœœ€€€€€€€€€€€Í…¹}…±°¡ÍåÍÑ•µÌ¤íq¸€€€€€€€€€€€ÍÑ…”ÐÄÍ}Í½ÉÑ}…±°¡ÍåÍÑ•µÌ¤íq¸€€€€€€€€€€€ÍÑ…”ÐÉ}±•…É}…ÉÑ}…¡” ¤íq¸€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ô€Àìœœœ)¹•Ý}É•Í…¸€ô€œœœ€€€€€€€€€€€€¼¨É½ÀÍå¹Ñ¡•Ñ¥Œ…Ù½É¥Ñ•Ì‰•™½É”Í…¹¹¥¹œÉ•…°ÍåÍÑ•µÌ¸€¨½q¸€€€€€€€€€€€™½È€¡…ÕÑ¼¥Ð€ôÍåÍÑ•µÌ¹‰•¥¸ ¤ì¥Ð€„ôÍåÍÑ•µÌ¹•¹ ¤ì€¥q¸€€€€€€€€€€€€€€¥˜€¡ÍÑ…”ÐÐÅ}¥Í}™…Ù½É¥Ñ•Ì ©¥Ð¤¤¥Ð€ôÍåÍÑ•µÌ¹•É…Í”¡¥Ð¤ì•±Í”€¬­¥Ðíq¸€€€€€€€€€€€Í…¹}…±°¡ÍåÍÑ•µÌ¤íq¸€€€€€€€€€€€ÍÑ…”ÐÄÍ}Í½ÉÑ}…±°¡ÍåÍÑ•µÌ¤íq¸€€€€€€€€€€€ÍÑ…”ÐÐÅ}É•‰Õ¥±‘}™…Ù½É¥Ñ•Ì¡ÍåÍÑ•µÌ¤íq¸€€€€€€€€€€€ÍÑ…”ÐÉ}±•…É}…ÉÑ}…¡” ¤íq¸€€€€€€€€€€€Ù¥Í¥‰±•}Á½Ì€ô€Àìœœœ)¥˜½±‘}É•Í…¸¹½Ð¥¸ÍÉŒèÉ…¥Í”MåÍÑ•µá¥Ð É•Í…¸…¹¡½Èµ¥ÍÍ¥¹œœ¤)ÍÉŒ€ôÍÉŒ¹É•Á±…”¡½±‘}É•Í…¸°¹•Ý}É•Í…¸°€Ä¤((Œ1…å½ÕÐ‘¥…¹½ÍÑ¥Ì¸)±…å½ÕÐ€ô€œ€€ÁÉ¥¹Ñ˜ ‰1e=UQ}QMQ}=-qq¸ˆ¤íq¸œ)•áÑÉ„€ô€ (œ€€ÁÉ¥¹Ñ˜ ‰MQÐÐÅ}!=5™…Ù½É¥Ñ•Ìµ™¥ÉÍÐÙ¥ÉÑÕ…°µÍÑ…ÈµÍåÍÑ•´½¹ÑÉ½±±•Èµ™¥ÉÍÑqq¸ˆ¤íq¸œ(œ€€ÁÉ¥¹Ñ˜ ‰MQÐÐÅ}9X±•™ÐµÍÑ¥¬µ‘Á…0ÄµHÄµÍåÍÑ•µÌ0ÈµHÈµ™…Ù½É¥Ñ”dµÍ•…É¡qq¸ˆ¤íq¸œ(œ€€ÁÉ¥¹Ñ˜ ‰MQÐÐÅ}MI …µ•Á…µ½¹ÍÉ••¸µ­•å‰½…ÉÉ•ÍÕ±ÑÌµ©ÕµÀµ¹¼µ…ÕÑ½±…Õ¹¡qq¸ˆ¤íq¸œ(¤)¥˜±…å½ÕÐ¹½Ð¥¸ÍÉŒèÉ…¥Í”MåÍÑ•µá¥Ð ±…å½ÕÐµ…É­•Èµ¥ÍÍ¥¹œœ¤)ÍÉŒ€ôÍÉŒ¹É•Á±…”¡±…å½ÕÐ°•áÑÉ„€¬±…å½ÕÐ°€Ä¤()™½Èµ…É­•È¥¸l(€€€€MÑ…”Ð¸ÐÄ…Ù½É¥Ñ•Ì!½µ”M•…É …¹½¹ÑÉ½±±•È9…Ù¥…Ñ¥½¸…Ñ¥Ù”œ°(€€€€MQÐÐÅ}!=5™…Ù½É¥Ñ•Ìµ™¥ÉÍÐœ°(€€€€Ñ¥½¸èéQ½±•…Ù½É¥Ñ”œ°€Ñ¥½¸èéM•…É œ°€ÍÑ…”ÐÐÅ}Í•…É¡}‘¥…±½œœ°(€€€€e=UHY=I%QLœ°€0È½HÈY=I%Qœ°€dMI œ)tè(€€€¥˜µ…É­•È¹½Ð¥¸ÍÉŒèÉ…¥Í”MåÍÑ•µá¥Ð µ¥ÍÍ¥¹œMÑ…”Ð¸ÐÄµ…É­•Èè€œ€¬µ…É­•È¤()A…Ñ ¡ÍåÌ¹…ÉÙlÉt¤¹ÝÉ¥Ñ•}Ñ•áÐ¡ÍÉŒ°•¹½‘¥¹œôÕÑ˜´àœ¤)ÁÉ¥¹Ð MQÐÐÅ}Y=I%QM}MI!}AQ!}=,œ¤(