#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage436_recalbox_menu.py INPUT OUTPUT")

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

helpers = r'''
enum class Stage436MenuPage
{
   Root,
   Games,
   Controllers,
   System,
   Quit
};

static bool stage436_request_exit = false;

static const char *stage436_page_title(Stage436MenuPage page)
{
   switch (page)
   {
      case Stage436MenuPage::Root: return "MAIN MENU";
      case Stage436MenuPage::Games: return "GAME SETTINGS";
      case Stage436MenuPage::Controllers: return "CONTROLLERS SETTINGS";
      case Stage436MenuPage::System: return "SYSTEM SETTINGS";
      case Stage436MenuPage::Quit: return "QUIT";
   }
   return "MAIN MENU";
}

static std::vector<std::string> stage436_rows(Stage436MenuPage page,
      const Game *game)
{
   std::vector<std::string> rows;

   switch (page)
   {
      case Stage436MenuPage::Root:
         rows.push_back("GAME SETTINGS                         >");
         rows.push_back("CONTROLLERS SETTINGS                  >");
         rows.push_back("RETROARCH SETTINGS");
         rows.push_back("SYSTEM SETTINGS                       >");
         rows.push_back("QUIT                                  >");
         break;

      case Stage436MenuPage::Games:
         rows.push_back("< BACK");
         rows.push_back(std::string("FAVORITE                    ") +
               (game && stage413_favorite(*game) ? "YES" : "NO"));
         rows.push_back(std::string("AUTO SAVE / LOAD            ") +
               (stage413_settings.auto_save ? "ON" : "OFF"));
         rows.push_back(std::string("REWIND                      ") +
               (stage413_settings.rewind ? "ON" : "OFF"));
         rows.push_back(std::string("STATE SLOT                  ") +
               std::to_string(stage413_settings.state_slot));
         break;

      case Stage436MenuPage::Controllers:
      {
         rows.push_back("< BACK");
         rows.push_back("CONFIGURE A CONTROLLER");
         rows.push_back("RETROARCH INPUT / HOTKEYS");
         rows.push_back("RESET CURRENT CONTROLLER PROFILE");

         bool configured = false;
         std::string name = "NO CONTROLLER";
         for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
            if (stage436_input_pad_fd_PLACEHOLDER)
            {
               name = stage436_input_pad_name_PLACEHOLDER;
               configured = stage436_input_pad_loaded_PLACEHOLDER;
               break;
            }

         rows.push_back(std::string("CURRENT: ") +
               (configured ? "CONFIGURED  " : "UNCONFIGURED  ") + name);
         break;
      }

      case Stage436MenuPage::System:
         rows.push_back("< BACK");
         rows.push_back("RESCAN GAME LIBRARY");
         rows.push_back("SYSTEM INFORMATION");
         rows.push_back("RETURN TO DESKTOP");
         break;

      case Stage436MenuPage::Quit:
         rows.push_back("< CANCEL");
         rows.push_back("RETURN TO DESKTOP");
         break;
   }

   return rows;
}

static int stage436_page_count(Stage436MenuPage page)
{
   switch (page)
   {
      case Stage436MenuPage::Root: return 5;
      case Stage436MenuPage::Games: return 5;
      case Stage436MenuPage::Controllers: return 5;
      case Stage436MenuPage::System: return 4;
      case Stage436MenuPage::Quit: return 2;
   }
   return 1;
}

static void stage436_draw_panel(Fb &fb, Stage436MenuPage page,
      int item, const Game *game)
{
   const int w = 720;
   const int h = 560;
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
   const int tw = text_width(heading, 3);
   draw_text(fb, x + (w - tw) / 2, y + 30, heading, 3, title);
   fill_rect(fb, x + 28, y + 76, w - 56, 2, pack1555(205, 208, 212));

   const std::vector<std::string> rows = stage436_rows(page, game);
   const int row_h = 48;
   const int row_y = y + 105;

   for (size_t i = 0; i < rows.size(); ++i)
   {
      const int yy = row_y + (int)i * row_h;
      if ((int)i == item)
      {
         fill_rect(fb, x + 24, yy - 8, w - 48, 38, select);
         fill_rect(fb, x + 24, yy - 8, 5, 38, accent);
      }

      const bool informational =
         page == Stage436MenuPage::Controllers && i == 4;

      draw_text(fb, x + 44, yy,
            rows[i], 2,
            informational ? dim : ((int)i == item ? title : normal));
   }

   draw_text(fb, x + 34, y + h - 60,
         "A  SELECT        B  BACK        START  CLOSE MENU",
         1, dim);

   if (page == Stage436MenuPage::Controllers)
      draw_text(fb, x + 34, y + h - 34,
            "ONE CONTROLLER PROFILE IS SHARED WITH RETROARCH",
            1, dim);
   else if (page == Stage436MenuPage::Games)
      draw_text(fb, x + 34, y + h - 34,
            "LEFT / RIGHT CHANGES THE SELECTED VALUE",
            1, dim);
   else
      draw_text(fb, x + 34, y + h - 34,
            "RECALBOX-STYLE COMPACT MENU",
            1, dim);
}

static int stage436_connected_gamepads(const Input &in)
{
   int count = 0;
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (stage436_input_pad_fd_PLACEHOLDER)
         ++count;
   return count;
}

static bool stage436_has_configured_gamepad(const Input &in)
{
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (stage436_input_pad_fd_PLACEHOLDER &&
          stage436_input_pad_loaded_PLACEHOLDER)
         return true;
   return false;
}

static bool stage436_raw_pad_activity(GamepadInput &pad)
{
   if (pad.fd < 0) return false;

   for (;;)
   {
      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.value == 1)
            return true;

         if (ev.type == EV_ABS && ev.code <= ABS_MAX)
         {
            const unsigned idx = pad.abs_to_axis[ev.code];
            if (idx < H3531_JS_MAX_AXES)
            {
               const int16_t old_value = pad.axes[idx];
               const int16_t value =
                  h3531_front_scale_abs(pad.absinfo[idx], ev.value);
               pad.axes[idx] = value;
               if (std::abs((int)value - (int)old_value) > 16000)
                  return true;
            }
         }
         continue;
      }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR))
         return false;

      return false;
   }
}

static bool stage436_keyboard_continue(Input &in)
{
   if (in.fd < 0) return false;

   for (;;)
   {
      input_event ev{};
      const ssize_t n = read(in.fd, &ev, sizeof(ev));

      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.value == 1 &&
             (ev.code == KEY_ENTER || ev.code == KEY_ESC))
            return true;
         continue;
      }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR))
         return false;

      return false;
   }
}

static void stage436_draw_welcome(Fb &physical, Stage42Backbuffer &back,
      int count)
{
   const uint16_t bg = pack1555(238, 239, 241);
   const uint16_t title = pack1555(67, 67, 67);
   const uint16_t normal = pack1555(100, 100, 100);
   const uint16_t accent = pack1555(75, 155, 205);

   fill_rect(back.fb, 0, 0, (int)back.fb.w, (int)back.fb.h, bg);

   const std::string welcome = "WELCOME";
   draw_text(back.fb,
         ((int)back.fb.w - text_width(welcome, 4)) / 2,
         145, welcome, 4, title);

   char detected[96];
   snprintf(detected, sizeof(detected),
         count == 1 ? "1 GAMEPAD DETECTED" : "%d GAMEPADS DETECTED", count);
   draw_text(back.fb,
         ((int)back.fb.w - text_width(detected, 2)) / 2,
         250, detected, 2, normal);

   fill_rect(back.fb, 350, 315, 580, 58, pack1555(205, 210, 216));
   fill_rect(back.fb, 350, 315, 6, 58, accent);
   const std::string prompt = "PRESS ANY BUTTON TO CONFIGURE";
   draw_text(back.fb,
         ((int)back.fb.w - text_width(prompt, 2)) / 2,
         333, prompt, 2, title);

   const std::string keyboard = "ENTER / ESC  CONTINUE WITH KEYBOARD";
   draw_text(back.fb,
         ((int)back.fb.w - text_width(keyboard, 1)) / 2,
         430, keyboard, 1, normal);

   const std::string note = "CONTROLLER MAPPING WILL ALSO BE USED BY RETROARCH";
   draw_text(back.fb,
         ((int)back.fb.w - text_width(note, 1)) / 2,
         485, note, 1, normal);

   stage42_present(physical, back);
}

static void stage436_first_run_controller(Fb &physical, Input &in,
      Stage42Backbuffer &back)
{
   input_rescan(in);

   const int count = stage436_connected_gamepads(in);
   if (count <= 0 || stage436_has_configured_gamepad(in))
      return;

   stage436_draw_welcome(physical, back, count);

   for (;;)
   {
      input_watch_poll(in);
      input_rescan(in);

      for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      {
         if (stage436_input_pad_fd_PLACEHOLDER &&
             !stage436_input_pad_loaded_PLACEHOLDER &&
             stage436_raw_pad_activity(in.pads[i]))
         {
            stage434_controller_setup(physical, in, back);
            return;
         }
      }

      if (stage436_keyboard_continue(in))
         return;

      usleep(5000);
   }
}

static void stage436_show_system_info(Fb &physical,
      Stage42Backbuffer &back)
{
   const uint16_t bg = pack1555(238, 239, 241);
   const uint16_t title = pack1555(67, 67, 67);
   const uint16_t normal = pack1555(100, 100, 100);

   fill_rect(back.fb, 0, 0, (int)back.fb.w, (int)back.fb.h, bg);
   draw_text(back.fb, 395, 180, "STAYPLAYTION", 4, title);
   draw_text(back.fb, 430, 280, "STAGE 6.8 INPUT / FRONTEND LINE", 2, normal);
   draw_text(back.fb, 430, 330, "CONTROLLER: EVDEV + RETROARCH PROFILE", 1, normal);
   draw_text(back.fb, 430, 365, "DESKTOP: LIVE HID HOTPLUG", 1, normal);
   draw_text(back.fb, 430, 400, "A / B  RETURN", 1, normal);
   stage42_present(physical, back);
   usleep(900000);
}

static void stage436_delete_current_profile(Input &in)
{
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
   {
      GamepadInput &pad = in.pads[i];
      if (pad.fd < 0) continue;

      const std::string path = h3531_profile_path(pad.name);
      if (!path.empty())
         unlink(path.c_str());

      pad.profile = GamepadProfile{};
      fprintf(stderr,
            "[STAYPLAYTION] controller profile reset: %s\n",
            path.c_str());
      return;
   }
}
'''

# Replace placeholders with real field references, while preserving loop variable i.
helpers = helpers.replace("stage436_input_pad_fd_PLACEHOLDER", "in.pads[i].fd >= 0")
helpers = helpers.replace("stage436_input_pad_name_PLACEHOLDER", "in.pads[i].name")
helpers = helpers.replace("stage436_input_pad_loaded_PLACEHOLDER", "in.pads[i].profile.loaded")

anchor = "static void stage413_quick_menu(Fb &physical, Input &in, Stage42Backbuffer &back,"
pos = src.find(anchor)
if pos < 0:
    raise SystemExit("Stage4.36 quick menu insertion anchor missing")
src = src[:pos] + helpers + "\n" + src[pos:]

menu = r'''static void stage413_quick_menu(Fb &physical, Input &in,
      Stage42Backbuffer &back, std::vector<SystemDef> &systems,
      size_t visible_pos, size_t &game_pos, FocusZone focus)
{
   Stage436MenuPage page = Stage436MenuPage::Root;
   int item = 0;
   bool open = true;

   while (open)
   {
      auto vis = visible_systems(systems);
      if (!vis.empty() && visible_pos >= vis.size()) visible_pos = 0;

      SystemDef *sys = nullptr;
      Game *game = nullptr;
      if (!vis.empty())
      {
         sys = &systems[vis[visible_pos]];
         if (!sys->games.empty())
         {
            if (game_pos >= sys->games.size()) game_pos = 0;
            game = &sys->games[game_pos];
         }
      }

      stage42_draw_ui(back.fb, systems, visible_pos, vis, game_pos,
            focus, 0.0f, 0.0f);
      stage436_draw_panel(back.fb, page, item, game);
      stage42_present(physical, back);

      const Action a = input_poll(in);
      const int count = stage436_page_count(page);

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

            if (page == Stage436MenuPage::Games)
            {
               if (item == 1 && sys && game)
                  game_pos = stage413_toggle_favorite(*sys, game_pos);
               else if (item == 2)
                  stage413_settings.auto_save = !stage413_settings.auto_save;
               else if (item == 3)
                  stage413_settings.rewind = !stage413_settings.rewind;
               else if (item == 4)
                  stage413_settings.state_slot =
                     (stage413_settings.state_slot + dir + 10) % 10;

               stage430_persist();
            }
            break;
         }

         case Action::Launch:
         {
            if (page == Stage436MenuPage::Root)
            {
               if (item == 0)
               {
                  page = Stage436MenuPage::Games;
                  item = 0;
               }
               else if (item == 1)
               {
                  page = Stage436MenuPage::Controllers;
                  item = 0;
               }
               else if (item == 2)
               {
                  stage430_persist();
                  run_external(physical, in, std::string(kRetroArchMenu));
                  if (!back.init(physical)) return;
               }
               else if (item == 3)
               {
                  page = Stage436MenuPage::System;
                  item = 0;
               }
               else if (item == 4)
               {
                  page = Stage436MenuPage::Quit;
                  item = 0;
               }
            }
            else if (page == Stage436MenuPage::Games)
            {
               if (item == 0)
               {
                  page = Stage436MenuPage::Root;
                  item = 0;
               }
               else if (item == 1 && sys && game)
                  game_pos = stage413_toggle_favorite(*sys, game_pos);
               else if (item == 2)
                  stage413_settings.auto_save = !stage413_settings.auto_save;
               else if (item == 3)
                  stage413_settings.rewind = !stage413_settings.rewind;
               else if (item == 4)
                  stage413_settings.state_slot =
                     (stage413_settings.state_slot + 1) % 10;

               stage430_persist();
            }
            else if (page == Stage436MenuPage::Controllers)
            {
               if (item == 0)
               {
                  page = Stage436MenuPage::Root;
                  item = 1;
               }
               else if (item == 1)
               {
                  stage434_controller_setup(physical, in, back);
               }
               else if (item == 2)
               {
                  stage430_persist();
                  run_external(physical, in, std::string(kRetroArchMenu));
                  if (!back.init(physical)) return;
               }
               else if (item == 3)
               {
                  stage436_delete_current_profile(in);
                  stage434_controller_setup(physical, in, back);
               }
            }
            else if (page == Stage436MenuPage::System)
            {
               if (item == 0)
               {
                  page = Stage436MenuPage::Root;
                  item = 3;
               }
               else if (item == 1)
               {
                  scan_all(systems);
                  stage42_clear_art_cache();
               }
               else if (item == 2)
                  stage436_show_system_info(physical, back);
               else if (item == 3)
               {
                  stage436_request_exit = true;
                  open = false;
               }
            }
            else if (page == Stage436MenuPage::Quit)
            {
               if (item == 0)
               {
                  page = Stage436MenuPage::Root;
                  item = 4;
               }
               else if (item == 1)
               {
                  stage436_request_exit = true;
                  open = false;
               }
            }
            break;
         }

         case Action::Exit:
            if (page == Stage436MenuPage::Root)
               open = false;
            else
            {
               page = Stage436MenuPage::Root;
               item = 0;
            }
            break;

         case Action::ServiceMenu:
            open = false;
            break;

         case Action::Rescan:
         case Action::None:
         default:
            break;
      }

      usleep(10000);
   }
}'''

src = replace_function(src,
    "static void stage413_quick_menu(Fb &physical, Input &in,",
    menu)

# Automatically offer controller setup when a connected pad has no saved profile.
main_anchor = '''   fprintf(stderr, "[GAMEFRONT] %s\\n", STAGE42_MARKER);

   size_t visible_pos = 0;'''
if main_anchor not in src:
    raise SystemExit("Stage4.36 main startup anchor missing")
src = src.replace(main_anchor,
'''   fprintf(stderr, "[GAMEFRONT] %s\\n", STAGE42_MARKER);

   stage436_first_run_controller(physical, in, back);

   size_t visible_pos = 0;''', 1)

# Recalbox-style: B/back never exits the shell from the library.
exit_anchor = '''         case Action::Exit:
            running = false;
            break;'''
if exit_anchor not in src:
    raise SystemExit("Stage4.36 main Exit anchor missing")
src = src.replace(exit_anchor,
'''         case Action::Exit:
            /* Recalbox-style: B is Back, never an accidental shell exit. */
            redraw = true;
            break;''', 1)

# QUIT from the menu is the only normal return-to-desktop path.
service_anchor = '''            stage413_quick_menu(physical, in, back, systems, visible_pos, game_pos, focus);
            redraw = true;
            break;'''
if service_anchor not in src:
    raise SystemExit("Stage4.36 ServiceMenu anchor missing")
src = src.replace(service_anchor,
'''            stage436_request_exit = false;
            stage413_quick_menu(physical, in, back, systems, visible_pos, game_pos, focus);
            if (stage436_request_exit)
               running = false;
            redraw = true;
            break;''', 1)

src = src.replace(
    "Stage4.35 RetroArch event-fallback controller wizard active",
    "Stage4.36 Recalbox-style first-run and main menu active")

# Update layout-test markers when present.
src = src.replace(
    "STAGE433_INPUT standard-retroarch-settings no-shell-hotkey-overrides",
    "STAGE436_MENU recalbox-style nested start-menu B-back quit-explicit")
src = src.replace(
    "STAGE433_CONTROLLER controller-settings retroarch-settings standard-autoconfig",
    "STAGE436_FIRST_RUN welcome press-any configure-standard-profile")
src = src.replace(
    "STAGE433_RA_OVERRIDE autosave-autoload-slot-rewind-only no-input-binds",
    "STAGE436_INPUT one-profile stayplaytion-retroarch no-shell-bind-overrides")

required = [
    "Stage4.36 Recalbox-style first-run and main menu active",
    "PRESS ANY BUTTON TO CONFIGURE",
    "MAIN MENU",
    "GAME SETTINGS",
    "CONTROLLERS SETTINGS",
    "CONFIGURE A CONTROLLER",
    "RETROARCH INPUT / HOTKEYS",
    "SYSTEM SETTINGS",
    "RETURN TO DESKTOP",
    "Recalbox-style: B is Back",
    "STAGE436_MENU recalbox-style nested start-menu B-back quit-explicit",
]

for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.36 marker: " + marker)

out_path.write_text(src, encoding="utf-8")
print("STAGE436_RECALBOX_MENU_PATCH_OK")
