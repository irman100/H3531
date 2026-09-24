#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage433_standard_input_settings.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

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

write_cfg = r'''static void stage413_write_ra_cfg()
{
   stage413_dirs();
   std::ofstream out(STAGE413_RA_CFG, std::ios::trunc);
   if (!out) return;

   /* Stage4.33: only shell-level runtime policy belongs in appendconfig.
    * Controller, keyboard and hotkey binds belong to RetroArch's standard
    * configuration/autoconfig files and must never be overwritten here. */
   out << "savestate_auto_save = \"" << (stage413_settings.auto_save ? "true" : "false") << "\"\n";
   out << "savestate_auto_load = \"" << (stage413_settings.auto_load ? "true" : "false") << "\"\n";
   out << "savestate_thumbnail_enable = \"true\"\n";
   out << "state_slot = \"" << stage413_settings.state_slot << "\"\n";
   out << "rewind_enable = \"" << (stage413_settings.rewind ? "true" : "false") << "\"\n";
   out << "rewind_buffer_size = \"" << stage413_settings.rewind_buffer_mb << "\"\n";
   out << "rewind_granularity = \"" << stage413_settings.rewind_granularity << "\"\n";
}'''

page_count = r'''static int stage430_page_count(Stage430Page page)
{
   switch (page)
   {
      case Stage430Page::Root: return 8;
      case Stage430Page::States: return 2;
      case Stage430Page::AutoSaveLoad: return 3;
      case Stage430Page::Rewind: return 4;
      case Stage430Page::Hotkeys: return 2;
   }
   return 1;
}'''

rows = r'''static std::vector<std::string> stage430_rows(Stage430Page page, const Game *game)
{
   std::vector<std::string> rows;

   if (page == Stage430Page::Root)
   {
      rows.push_back("BACK TO LIBRARY");
      rows.push_back(std::string("FAVORITE                 ") +
            (game && stage413_favorite(*game) ? "YES" : "NO"));
      rows.push_back("SAVE STATES              >");
      rows.push_back("AUTO SAVE / LOAD         >");
      rows.push_back("REWIND                   >");
      rows.push_back("CONTROLLER SETTINGS");
      rows.push_back("RETROARCH SETTINGS");
      rows.push_back("SAVE SETTINGS & BACK");
   }
   else if (page == Stage430Page::States)
   {
      rows.push_back("< BACK");
      rows.push_back(std::string("STATE SLOT               ") +
            std::to_string(stage413_settings.state_slot));
   }
   else if (page == Stage430Page::AutoSaveLoad)
   {
      rows.push_back("< BACK");
      rows.push_back(std::string("AUTO SAVE ON EXIT        ") +
            (stage413_settings.auto_save ? "ON" : "OFF"));
      rows.push_back(std::string("AUTO LOAD ON START       ") +
            (stage413_settings.auto_load ? "ON" : "OFF"));
   }
   else if (page == Stage430Page::Rewind)
   {
      rows.push_back("< BACK");
      rows.push_back(std::string("REWIND ENABLED           ") +
            (stage413_settings.rewind ? "ON" : "OFF"));
      rows.push_back(std::string("BUFFER                   ") +
            std::to_string(stage413_settings.rewind_buffer_mb) + " MB");
      rows.push_back(std::string("GRANULARITY              ") +
            std::to_string(stage413_settings.rewind_granularity));
   }
   else
   {
      rows.push_back("< BACK");
      rows.push_back("INPUT / HOTKEYS ARE MANAGED BY RETROARCH");
   }

   return rows;
}'''

menu_draw = r'''static void stage430_menu_draw(Fb &fb, Stage430Page page, int item, const Game *game)
{
   const int x = 300, y = 82, w = 680, h = 548;
   const uint16_t bg = pack1555(5, 15, 31);
   const uint16_t hi = pack1555(17, 67, 104);
   const uint16_t line = pack1555(69, 221, 255);
   const uint16_t text = pack1555(234, 245, 255);
   const uint16_t dim = pack1555(134, 158, 183);

   fill_rect(fb, x, y, w, h, bg);
   frame_rect(fb, x, y, w, h, 2, line);
   draw_text(fb, x + 24, y + 18, stage430_page_title(page), 2, text);

   const std::vector<std::string> rows = stage430_rows(page, game);
   const int row_h = 43;
   for (size_t i = 0; i < rows.size(); ++i)
   {
      const int yy = y + 70 + (int)i * row_h;
      if ((int)i == item)
      {
         fill_rect(fb, x + 14, yy - 6, w - 28, 32, hi);
         fill_rect(fb, x + 14, yy - 6, 4, 32, line);
      }
      draw_text(fb, x + 30, yy + 2, rows[i], 2,
            (int)i == item ? text : dim);
   }

   if (page == Stage430Page::Root)
   {
      draw_text(fb, x + 24, y + h - 56,
            "CONTROLLER / KEYBOARD / HOTKEYS USE STANDARD RETROARCH SETTINGS",
            1, dim);
      draw_text(fb, x + 24, y + h - 31,
            "RETROARCH: SETTINGS > INPUT > RETROPAD BINDS / HOTKEYS",
            1, dim);
   }
   else
   {
      draw_text(fb, x + 24, y + h - 56,
            "ENTER APPLY   LEFT/RIGHT CHANGE   ESC/F1 BACK",
            1, dim);
      draw_text(fb, x + 24, y + h - 31,
            "INPUT BINDS ARE NEVER WRITTEN BY STAYPLAYTION",
            1, dim);
   }
}'''

quick_menu = r'''static void stage413_quick_menu(Fb &physical, Input &in, Stage42Backbuffer &back,
      std::vector<SystemDef> &systems, size_t visible_pos, size_t &game_pos, FocusZone focus)
{
   Stage430Page page = Stage430Page::Root;
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

      stage42_draw_ui(back.fb, systems, visible_pos, vis, game_pos,
            focus, 0.0f, 0.0f);
      stage430_menu_draw(back.fb, page, item, g);
      stage42_present(physical, back);

      const Action a = input_poll(in);
      const int count = stage430_page_count(page);

      switch (a)
      {
         case Action::PrevSystem:
            item = item ? item - 1 : count - 1;
            break;

         case Action::NextSystem:
            item = (item + 1) % count;
            break;

         case Action::PrevGame:
         case Action::NextGame:
         {
            const int dir = a == Action::PrevGame ? -1 : 1;

            if (page == Stage430Page::Root)
            {
               if (item == 1 && !sys.games.empty())
                  game_pos = stage413_toggle_favorite(sys, game_pos);
            }
            else if (page == Stage430Page::States)
            {
               if (item == 1)
                  stage413_settings.state_slot =
                     (stage413_settings.state_slot + dir + 10) % 10;
            }
            else if (page == Stage430Page::AutoSaveLoad)
            {
               if (item == 1)
                  stage413_settings.auto_save = !stage413_settings.auto_save;
               else if (item == 2)
                  stage413_settings.auto_load = !stage413_settings.auto_load;
            }
            else if (page == Stage430Page::Rewind)
            {
               if (item == 1)
                  stage413_settings.rewind = !stage413_settings.rewind;
               else if (item == 2)
               {
                  static const int values[] = {4, 8, 16, 32, 64};
                  int p = 1;
                  for (int i = 0; i < 5; ++i)
                     if (stage413_settings.rewind_buffer_mb == values[i]) p = i;
                  p = (p + dir + 5) % 5;
                  stage413_settings.rewind_buffer_mb = values[p];
               }
               else if (item == 3)
               {
                  stage413_settings.rewind_granularity =
                     ((stage413_settings.rewind_granularity - 1 + dir + 4) % 4) + 1;
               }
            }

            stage430_persist();
            break;
         }

         case Action::Launch:
         {
            if (page == Stage430Page::Root)
            {
               if (item == 0)
                  open = false;
               else if (item == 1 && !sys.games.empty())
                  game_pos = stage413_toggle_favorite(sys, game_pos);
               else if (item == 2)
               {
                  page = Stage430Page::States;
                  item = 0;
               }
               else if (item == 3)
               {
                  page = Stage430Page::AutoSaveLoad;
                  item = 0;
               }
               else if (item == 4)
               {
                  page = Stage430Page::Rewind;
                  item = 0;
               }
               else if (item == 5 || item == 6)
               {
                  /* Both entries intentionally open stock RetroArch RGUI.
                   * Controller Settings points the user to:
                   * Settings > Input > RetroPad Binds > Port 1 Controls.
                   * RetroArch Settings exposes the complete standard menu. */
                  stage430_persist();
                  run_external(physical, in, std::string(kRetroArchMenu));
                  if (!back.init(physical)) return;
               }
               else if (item == 7)
               {
                  stage430_persist();
                  open = false;
               }
            }
            else if (page == Stage430Page::States)
            {
               if (item == 0)
               {
                  page = Stage430Page::Root;
                  item = 2;
               }
               else if (item == 1)
                  stage413_settings.state_slot =
                     (stage413_settings.state_slot + 1) % 10;
               stage430_persist();
            }
            else if (page == Stage430Page::AutoSaveLoad)
            {
               if (item == 0)
               {
                  page = Stage430Page::Root;
                  item = 3;
               }
               else if (item == 1)
                  stage413_settings.auto_save = !stage413_settings.auto_save;
               else if (item == 2)
                  stage413_settings.auto_load = !stage413_settings.auto_load;
               stage430_persist();
            }
            else if (page == Stage430Page::Rewind)
            {
               if (item == 0)
               {
                  page = Stage430Page::Root;
                  item = 4;
               }
               else if (item == 1)
                  stage413_settings.rewind = !stage413_settings.rewind;
               else if (item == 2)
               {
                  static const int values[] = {4, 8, 16, 32, 64};
                  int p = 1;
                  for (int i = 0; i < 5; ++i)
                     if (stage413_settings.rewind_buffer_mb == values[i]) p = i;
                  stage413_settings.rewind_buffer_mb = values[(p + 1) % 5];
               }
               else if (item == 3)
                  stage413_settings.rewind_granularity =
                     stage413_settings.rewind_granularity % 4 + 1;
               stage430_persist();
            }
            else
            {
               page = Stage430Page::Root;
               item = 5;
            }
            break;
         }

         case Action::Exit:
         case Action::ServiceMenu:
            if (page == Stage430Page::Root)
               open = false;
            else
            {
               page = Stage430Page::Root;
               item = 0;
            }
            break;

         case Action::Rescan:
         case Action::None:
         default:
            break;
      }

      usleep(10000);
   }
}'''

legacy_popup = r'''static void stage430_bind_popup(Fb &fb, const std::string &action, const std::string &current)
{
   (void)fb;
   (void)action;
   (void)current;
}'''

legacy_capture = r'''static std::string stage430_capture_key(Fb &physical, Input &in, Stage42Backbuffer &back,
      const std::string &action, const std::string &current)
{
   (void)physical;
   (void)in;
   (void)back;
   (void)action;
   return current;
}'''

src = replace_function(src, "static void stage413_write_ra_cfg()", write_cfg)
src = replace_function(src, "static void stage430_bind_popup(Fb &fb, const std::string &action, const std::string &current)", legacy_popup)
src = replace_function(src, "static std::string stage430_capture_key(Fb &physical, Input &in, Stage42Backbuffer &back,", legacy_capture)
src = replace_function(src, "static int stage430_page_count(Stage430Page page)", page_count)
src = replace_function(src, "static std::vector<std::string> stage430_rows(Stage430Page page, const Game *game)", rows)
src = replace_function(src, "static void stage430_menu_draw(Fb &fb, Stage430Page page, int item, const Game *game)", menu_draw)
src = replace_function(src, "static void stage413_quick_menu(Fb &physical, Input &in, Stage42Backbuffer &back,", quick_menu)

src = src.replace("Stage4.31 Stayplaytion shell lifecycle active",
                  "Stage4.33 Standard RetroArch input settings active")

src = src.replace(
    "STAGE430_MENU root states autosave-load rewind hotkeys retroarch-menu nested",
    "STAGE433_INPUT standard-retroarch-settings no-shell-hotkey-overrides")
src = src.replace(
    "STAGE430_BINDS capture-key plus-left-right-cycle save-load-rewind-slot-menu persistent",
    "STAGE433_CONTROLLER controller-settings retroarch-settings standard-autoconfig")
src = src.replace(
    "STAGE430_RA_OVERRIDE auto-save auto-load state-slot rewind-buffer rewind-granularity custom-keys",
    "STAGE433_RA_OVERRIDE autosave-autoload-slot-rewind-only no-input-binds")

required = [
    "CONTROLLER SETTINGS",
    "RETROARCH SETTINGS",
    "INPUT BINDS ARE NEVER WRITTEN BY STAYPLAYTION",
    "Stage4.33 Standard RetroArch input settings active",
]
if "ASSIGN HOTKEY" in src or "PRESS F1-F12 / A-Z / 0-9 / SPACE" in src:
    raise SystemExit("legacy hotkey capture UI survived")

for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.33 marker: " + marker)

for forbidden in [
    'out << "input_save_state',
    'out << "input_load_state',
    'out << "input_rewind',
    'out << "input_menu_toggle',
    'out << "config_save_on_exit = \\"false\\"',
]:
    if forbidden in src:
        raise SystemExit("legacy input override survived: " + forbidden)

out_path.write_text(src, encoding="utf-8")
print("STAGE433_STANDARD_RETROARCH_INPUT_SETTINGS_PATCH_OK")
