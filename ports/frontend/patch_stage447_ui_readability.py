#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage447_ui_readability.py INPUT OUTPUT")

src = Path(sys.argv[1]).read_text(encoding="utf-8")

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        raise SystemExit("function not found: " + signature)
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("opening brace not found: " + signature)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + replacement.rstrip() + text[i+1:]
    raise SystemExit("unterminated function: " + signature)

star = r'''static void stage441_draw_big_star(Fb &fb, int cx, int cy, bool selected)
{
   /* Stage4.47: favorite marker fits cleanly between divider and cartridge. */
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
   const int s = selected ? 3 : 2;
   const uint16_t c = selected ? pack1555(255, 220, 70) : pack1555(205, 182, 82);
   const int w = 11 * s;
   const int h = 11 * s;
   for (int yy = 0; yy < 11; ++yy)
      for (int xx = 0; xx < 11; ++xx)
         if (p[yy][xx] == '#')
            fill_rect(fb, cx - w/2 + xx*s, cy - h/2 + yy*s, s, s, c);
}'''
src = replace_function(src, "static void stage441_draw_big_star(", star)

ui = r'''static void stage42_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      FocusZone focus, float game_shift, float system_shift)
{
   const uint16_t text = pack1555(227, 240, 255);
   const uint16_t dim = pack1555(128, 157, 199);
   const uint16_t footer = pack1555(4, 17, 36);
   const uint16_t accent = pack1555(75, 202, 255);

   stage47_draw_background_cached(fb);

   /* Stage4.47 readability pass: larger identity and strapline. */
   draw_text(fb, 36, 20, "STAYPLAYTION", 3, text);
   draw_text(fb, 37, 55, "GAMES PAST ALWAYS PLAY", 2, dim);

   std::time_t tt = std::time(nullptr);
   std::tm *tmv = std::localtime(&tt);
   char timebuf[16] = "--:--";
   char datebuf[32] = "--- --- --";
   if (tmv)
   {
      std::strftime(timebuf, sizeof(timebuf), "%H:%M", tmv);
      std::strftime(datebuf, sizeof(datebuf), "%a %b %d", tmv);
      for (char *p = datebuf; *p; ++p)
         *p = (char)std::toupper((unsigned char)*p);
   }

   /* Right aligned clock; date is centered directly underneath it. */
   const int right_margin = 36;
   const int time_scale = 3;
   const int date_scale = 2;
   const int time_w = text_width(timebuf, time_scale);
   const int date_w = text_width(datebuf, date_scale);
   const int time_x = (int)fb.w - right_margin - time_w;
   const int time_cx = time_x + time_w / 2;
   draw_text(fb, time_x, 20, timebuf, time_scale, text);
   draw_text(fb, time_cx - date_w / 2, 55, datebuf, date_scale, dim);

   if (vis.empty())
   {
      draw_text(fb, 480, 330, "NO GAMES FOUND", 3, text);
      return;
   }

   stage42_draw_system_row(fb, systems, vis, visible_pos, focus, system_shift);
   const SystemDef &sys = systems[vis[visible_pos]];

   /* Divider label is now large enough to read, while keeping the line visible. */
   fill_rect(fb, 72, 258, (int)fb.w - 144, 1, pack1555(53, 109, 163));
   const std::string era = stage441_is_favorites(sys) ? "YOUR FAVORITES" : stage44_era_name(sys);
   const int era_scale = 2;
   const int ew = text_width(era, era_scale);
   fill_rect(fb, (int)fb.w / 2 - ew / 2 - 14, 247, ew + 28, 23, pack1555(2, 9, 20));
   draw_text(fb, (int)fb.w / 2 - ew / 2, 251, era, era_scale, pack1555(164, 192, 230));

   stage42_draw_games(fb, sys, game_pos, game_shift, focus);

   const int footer_y = STAGE413_FOOTER;
   fill_rect(fb, 0, footer_y, (int)fb.w, 46, footer);
   fill_rect(fb, 0, footer_y, (int)fb.w, 1, accent);
   if (focus == FocusZone::Games)
      draw_text(fb, 28, footer_y + 12, "STICK/DPAD NAV  L1/R1 SYSTEM  L2/R2 FAVORITE  Y SEARCH  A PLAY  START MENU", 1, dim);
   else
      draw_text(fb, 28, footer_y + 12, "STICK/DPAD NAV  L1/R1 SYSTEM  L2/R2 FAVORITE  Y SEARCH  A SELECT  B BACK", 1, dim);
}'''
src = replace_function(src, "static void stage42_draw_ui(", ui)

menu = r'''static void stage436_draw_panel(Fb &fb, Stage436MenuPage page,
      int item, const Game *game, const Input &in)
{
   /* Stage4.47: wider panel and substantially larger menu typography. */
   const int w = 840;
   const int h = 600;
   const int x = ((int)fb.w - w) / 2;
   const int y = ((int)fb.h - h) / 2;

   const uint16_t shadow = pack1555(8, 12, 18);
   const uint16_t panel  = pack1555(238, 239, 241);
   const uint16_t title  = pack1555(67, 67, 67);
   const uint16_t normal = pack1555(92, 92, 92);
   const uint16_t dim    = pack1555(145, 145, 145);
   const uint16_t select = pack1555(205, 210, 216);
   const uint16_t accent = pack1555(75, 155, 205);

   fill_rect(fb, x + 10, y + 10, w, h, shadow);
   fill_rect(fb, x, y, w, h, panel);
   frame_rect(fb, x, y, w, h, 2, pack1555(190, 194, 200));

   const std::string heading = stage436_page_title(page);
   const int tw = text_width(heading, 4);
   draw_text(fb, x + (w - tw) / 2, y + 22, heading, 4, title);
   fill_rect(fb, x + 28, y + 82, w - 56, 2, pack1555(205, 208, 212));

   const std::vector<std::string> rows = stage436_rows(page, game, in);
   const int row_h = 52;
   const int row_y = y + 112;

   for (size_t i = 0; i < rows.size(); ++i)
   {
      const int yy = row_y + (int)i * row_h;
      if ((int)i == item)
      {
         fill_rect(fb, x + 24, yy - 9, w - 48, 43, select);
         fill_rect(fb, x + 24, yy - 9, 6, 43, accent);
      }

      const bool informational =
         page == Stage436MenuPage::Controllers && i == 5;
      const int row_scale = informational ? 2 : 3;

      draw_text(fb, x + 44, yy,
            rows[i], row_scale,
            informational ? dim : ((int)i == item ? title : normal));
   }

   draw_text(fb, x + 34, y + h - 60,
         "A  SELECT      B  BACK      START  CLOSE MENU",
         2, dim);

   if (page == Stage436MenuPage::Controllers)
      draw_text(fb, x + 34, y + h - 32,
            "ONE CONTROLLER PROFILE IS SHARED WITH RETROARCH",
            2, dim);
   else if (page == Stage436MenuPage::Hotkeys)
      draw_text(fb, x + 34, y + h - 32,
            "REWIND REQUIRES GAME SETTINGS > REWIND = ON",
            2, dim);
   else if (page == Stage436MenuPage::Games)
      draw_text(fb, x + 34, y + h - 32,
            "LEFT / RIGHT CHANGES THE SELECTED VALUE",
            2, dim);
   else
      draw_text(fb, x + 34, y + h - 32,
            "RECALBOX-STYLE COMPACT MENU",
            2, dim);
}'''
src = replace_function(src, "static void stage436_draw_panel(", menu)

layout = '   printf("LAYOUT_TEST_OK\\n");\n'
marker = '   printf("STAGE447_READABILITY star33 header3 subtitle2 divider2 menu3 clock-right date-centered\\n");\n'
if layout not in src:
    raise SystemExit("Stage4.47 layout marker anchor missing")
src = src.replace(layout, marker + layout, 1)

required = [
   "STAGE447_READABILITY star33 header3 subtitle2 divider2 menu3 clock-right date-centered",
   'const int s = selected ? 3 : 2;',
   'draw_text(fb, 36, 20, "STAYPLAYTION", 3, text);',
   'draw_text(fb, 37, 55, "GAMES PAST ALWAYS PLAY", 2, dim);',
   'const int right_margin = 36;',
   'const int time_scale = 3;',
   'const int date_scale = 2;',
   'const int era_scale = 2;',
   'const int w = 840;',
   'const int row_scale = informational ? 2 : 3;',
]
for m in required:
    if m not in src:
        raise SystemExit("missing Stage4.47 marker: " + m)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("STAGE447_UI_READABILITY_PATCH_OK")
