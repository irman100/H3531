#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage448_control_search_readability.py INPUT OUTPUT")

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

ui = r'''static void stage42_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      FocusZone focus, float game_shift, float system_shift)
{
   const uint16_t text = pack1555(227, 240, 255);
   const uint16_t dim = pack1555(128, 157, 199);
   const uint16_t footer = pack1555(4, 17, 36);
   const uint16_t accent = pack1555(75, 202, 255);

   stage47_draw_background_cached(fb);

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

   fill_rect(fb, 72, 258, (int)fb.w - 144, 1, pack1555(53, 109, 163));
   const std::string era = stage441_is_favorites(sys) ? "YOUR FAVORITES" : stage44_era_name(sys);
   const int era_scale = 2;
   const int ew = text_width(era, era_scale);
   fill_rect(fb, (int)fb.w / 2 - ew / 2 - 14, 247, ew + 28, 23, pack1555(2, 9, 20));
   draw_text(fb, (int)fb.w / 2 - ew / 2, 251, era, era_scale, pack1555(164, 192, 230));

   stage42_draw_games(fb, sys, game_pos, game_shift, focus);

   /* Stage4.48: the help strip is meant to be readable from TV distance.
    * Scale 2 still fits a 1280-wide framebuffer as one compact line. */
   const int footer_y = STAGE413_FOOTER;
   fill_rect(fb, 0, footer_y, (int)fb.w, 46, footer);
   fill_rect(fb, 0, footer_y, (int)fb.w, 1, accent);
   if (focus == FocusZone::Games)
      draw_text(fb, 24, footer_y + 10,
            "DPAD/STICK NAV  L1/R1 SYSTEM  L2/R2 FAVORITE  Y SEARCH  A PLAY  START MENU",
            2, dim);
   else
      draw_text(fb, 24, footer_y + 10,
            "DPAD/STICK NAV  L1/R1 SYSTEM  L2/R2 FAVORITE  Y SEARCH  A SELECT  B BACK",
            2, dim);
}'''
src = replace_function(src, "static void stage42_draw_ui(", ui)

search = r'''static void stage441_draw_search(Fb &fb, const std::vector<SystemDef> &systems,
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
      const int x = 100 + col * 80;
      const int y = 202 + row * 47;
      if (!result_mode && i == key_index)
         fill_rect(fb, x - 10, y - 8, 56, 42, hi);
      char label[2] = {keys[i], 0};
      draw_text(fb, x, y, label, 3, text);
   }

   draw_text(fb, 100, 496, "A ADD   B BACK   START/Y RESULTS", 2, dim);
   draw_text(fb, 100, 526, "DPAD MOVE   F3 SEARCH   KEYBOARD OK", 2, dim);

   const auto hits = stage441_search_hits(systems, query);
   const int rx = 650, ry = 150;
   draw_text(fb, rx, 112, "RESULTS", 2, text);
   if (hits.empty())
      draw_text(fb, rx, ry, query.empty() ? "ENTER A QUERY" : "NO MATCHES", 2, dim);
   else
   {
      const int visible = std::min(9, (int)hits.size());
      int start = 0;
      if (result_index >= visible) start = result_index - visible + 1;
      for (int row = 0; row < visible; ++row)
      {
         const int idx = start + row;
         if (idx >= (int)hits.size()) break;
         const auto &h = hits[(size_t)idx];
         const Game &g = systems[h.system].games[h.game];
         const std::string line_text = stage42_ellipsize_px(
               g.title + "  [" +
               (systems[h.system].fullname.empty() ? systems[h.system].name :
                systems[h.system].fullname) + "]", 2, 520);
         const int yy = ry + row * 43;
         if (result_mode && idx == result_index)
            fill_rect(fb, rx - 8, yy - 6, 535, 34, hi);
         draw_text(fb, rx, yy, line_text, 2,
               result_mode && idx == result_index ? text : dim);
      }
   }
}'''
src = replace_function(src, "static void stage441_draw_search(", search)

stick = r'''static void stage438_draw_stick(Fb &fb, int cx, int cy,
      bool highlighted, bool up, bool left, const char *name)
{
   const uint16_t edge = pack1555(115, 119, 126);
   const uint16_t fill = pack1555(222, 224, 228);
   const uint16_t hi   = pack1555(75, 155, 205);
   const uint16_t text = pack1555(67, 67, 67);

   frame_rect(fb, cx - 46, cy - 46, 92, 92, 3, highlighted ? hi : edge);
   fill_rect(fb, cx - 11, cy - 11, 22, 22, highlighted ? hi : fill);

   if (highlighted && up)
      fill_rect(fb, cx - 5, cy - 38, 10, 22, hi);
   if (highlighted && left)
      fill_rect(fb, cx - 38, cy - 5, 22, 10, hi);

   draw_text(fb, cx - text_width(name, 2) / 2, cy + 53, name, 2, text);
}'''
src = replace_function(src, "static void stage438_draw_stick(", stick)

visual = r'''static void stage438_draw_controller_visual(Fb &fb, int step)
{
   const uint16_t body = pack1555(220, 223, 227);
   const uint16_t edge = pack1555(110, 114, 120);
   const uint16_t hi   = pack1555(75, 155, 205);
   const uint16_t text = pack1555(67, 67, 67);

   const int x = 655;
   const int y = 205;
   const int w = 395;
   const int h = 280;

   fill_rect(fb, x, y + 35, w, h - 35, body);
   frame_rect(fb, x, y + 35, w, h - 35, 3, edge);

   const bool l1 = step == STAGE437_L1;
   const bool r1 = step == STAGE437_R1;
   const bool l2 = step == STAGE437_L2;
   const bool r2 = step == STAGE437_R2;
   fill_rect(fb, x + 28, y + 8, 92, 25, l2 ? hi : body);
   frame_rect(fb, x + 28, y + 8, 92, 25, 2, l2 ? hi : edge);
   fill_rect(fb, x + w - 120, y + 8, 92, 25, r2 ? hi : body);
   frame_rect(fb, x + w - 120, y + 8, 92, 25, 2, r2 ? hi : edge);
   fill_rect(fb, x + 40, y + 36, 104, 27, l1 ? hi : body);
   frame_rect(fb, x + 40, y + 36, 104, 27, 2, l1 ? hi : edge);
   fill_rect(fb, x + w - 144, y + 36, 104, 27, r1 ? hi : body);
   frame_rect(fb, x + w - 144, y + 36, 104, 27, 2, r1 ? hi : edge);
   draw_text(fb, x + 64, y + 13, "L2", 2, text);
   draw_text(fb, x + w - 89, y + 13, "R2", 2, text);
   draw_text(fb, x + 83, y + 42, "L1", 2, text);
   draw_text(fb, x + w - 101, y + 42, "R1", 2, text);

   const int dx = x + 86;
   const int dy = y + 120;
   const bool du = step == STAGE437_DPAD_UP;
   const bool dd = step == STAGE437_DPAD_DOWN;
   const bool dl = step == STAGE437_DPAD_LEFT;
   const bool dr = step == STAGE437_DPAD_RIGHT;
   fill_rect(fb, dx - 12, dy - 38, 24, 28, du ? hi : edge);
   fill_rect(fb, dx - 12, dy + 10, 24, 28, dd ? hi : edge);
   fill_rect(fb, dx - 38, dy - 12, 28, 24, dl ? hi : edge);
   fill_rect(fb, dx + 10, dy - 12, 28, 24, dr ? hi : edge);
   fill_rect(fb, dx - 12, dy - 12, 24, 24, edge);

   const int fx = x + w - 88;
   const int fy = y + 120;
   const int bs = 28;
   fill_rect(fb, fx - bs/2, fy + 32 - bs/2, bs, bs,
         step == STAGE437_A ? hi : edge);
   fill_rect(fb, fx + 32 - bs/2, fy - bs/2, bs, bs,
         step == STAGE437_B ? hi : edge);
   fill_rect(fb, fx - 32 - bs/2, fy - bs/2, bs, bs,
         step == STAGE437_X ? hi : edge);
   fill_rect(fb, fx - bs/2, fy - 32 - bs/2, bs, bs,
         step == STAGE437_Y ? hi : edge);
   draw_text(fb, fx - 5, fy + 27, "A", 2, text);
   draw_text(fb, fx + 27, fy - 5, "B", 2, text);
   draw_text(fb, fx - 37, fy - 5, "X", 2, text);
   draw_text(fb, fx - 5, fy - 37, "Y", 2, text);

   const bool ls = step == STAGE438_LEFT_STICK_UP ||
                   step == STAGE438_LEFT_STICK_LEFT ||
                   step == STAGE438_L3;
   const bool rs = step == STAGE438_RIGHT_STICK_UP ||
                   step == STAGE438_RIGHT_STICK_LEFT ||
                   step == STAGE438_R3;
   stage438_draw_stick(fb, x + 145, y + 205, ls,
         step == STAGE438_LEFT_STICK_UP,
         step == STAGE438_LEFT_STICK_LEFT, "L STICK");
   stage438_draw_stick(fb, x + 250, y + 205, rs,
         step == STAGE438_RIGHT_STICK_UP,
         step == STAGE438_RIGHT_STICK_LEFT, "R STICK");

   if (step == STAGE438_L3)
      draw_text(fb, x + 124, y + 194, "L3", 2, text);
   if (step == STAGE438_R3)
      draw_text(fb, x + 229, y + 194, "R3", 2, text);

   fill_rect(fb, x + 165, y + 88, 48, 25,
         step == STAGE437_SELECT ? hi : edge);
   fill_rect(fb, x + 220, y + 88, 48, 25,
         step == STAGE437_START ? hi : edge);
   draw_text(fb, x + 170, y + 93, "SEL", 2, text);
   draw_text(fb, x + 225, y + 93, "STA", 2, text);

   if (step == STAGE437_HOTKEY)
   {
      frame_rect(fb, x + 155, y + 145, 86, 38, 3, hi);
      draw_text(fb, x + 174, y + 153, "HK", 3, hi);
   }
}'''
src = replace_function(src, "static void stage438_draw_controller_visual(", visual)

wizard = r'''static void stage434_draw(Fb &fb, const GamepadInput &pad,
      int step, const std::string &status)
{
   const uint16_t bg   = pack1555(238, 239, 241);
   const uint16_t line = pack1555(75, 155, 205);
   const uint16_t text = pack1555(67, 67, 67);
   const uint16_t dim  = pack1555(120, 120, 120);
   const uint16_t ok   = pack1555(60, 150, 105);

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);
   frame_rect(fb, 90, 55, 1100, 610, 2, pack1555(190, 194, 200));

   draw_text(fb, 135, 88, "CONFIGURING", 3, text);
   draw_text(fb, 135, 130,
         stage42_ellipsize_px(std::string("GAMEPAD: ") + ascii_safe(pad.name), 2, 470),
         2, dim);
   draw_text(fb, 135, 164, "RETROARCH STANDARD PROFILE", 2, dim);

   if (step >= 0 &&
       step < (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])))
   {
      stage438_draw_controller_visual(fb, step);

      draw_text(fb, 135, 230, "PRESS / MOVE", 2, dim);
      draw_text(fb, 135, 274, stage434_steps[step].label, 3, text);

      char progress[64];
      snprintf(progress, sizeof(progress), "STEP %d / %d",
            step + 1,
            (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])));
      draw_text(fb, 135, 332, progress, 2, dim);

      if (stage438_axis_only_step(step))
         draw_text(fb, 135, 390, "MOVE THE REQUESTED ANALOG STICK", 2, dim);
      else if (stage434_steps[step].allow_axis)
         draw_text(fb, 135, 390, "BUTTON OR AXIS DIRECTION", 2, dim);
      else
         draw_text(fb, 135, 390, "PRESS THE REQUESTED BUTTON", 2, dim);

      if (stage438_optional_step(step))
         draw_text(fb, 135, 435,
               "OPTIONAL: HOLD ANY BUTTON 1 SEC TO SKIP", 2, line);
      else
         draw_text(fb, 135, 435, "REQUIRED", 2, dim);

      draw_text(fb, 135, 480, "ESC ON KEYBOARD: CANCEL", 2, dim);
   }
   else
   {
      draw_text(fb, 135, 280,
            status.empty() ? "PROFILE SAVED" : status, 3, ok);
      draw_text(fb, 135, 350,
            "STAYPLAYTION AND RETROARCH USE THE SAME PROFILE", 2, text);
   }

   if (!status.empty() && step >= 0)
      draw_text(fb, 135, 535, status, 2, ok);
}'''
src = replace_function(src, "static void stage434_draw(Fb &fb, const GamepadInput &pad,", wizard)

hotkey_capture = r'''static bool stage437_capture_bind(Fb &physical, Stage42Backbuffer &back,
      Input &in, GamepadInput &pad, const std::string &label,
      bool allow_axis, Stage434CapturedBind &out)
{
   const uint16_t bg   = pack1555(238, 239, 241);
   const uint16_t line = pack1555(75, 155, 205);
   const uint16_t text = pack1555(67, 67, 67);
   const uint16_t dim  = pack1555(120, 120, 120);

   fill_rect(back.fb, 0, 0, (int)back.fb.w, (int)back.fb.h, bg);
   frame_rect(back.fb, 150, 70, 980, 575, 2, line);
   draw_text(back.fb, 195, 108, "HOTKEY ACTION SETUP", 3, text);
   draw_text(back.fb, 195, 170, "PRESS / MOVE", 2, dim);
   draw_text(back.fb, 195, 228, label, 4, text);
   draw_text(back.fb, 195, 326,
         allow_axis ? "BUTTON OR AXIS DIRECTION" : "BUTTON ONLY",
         2, dim);
   draw_text(back.fb, 195, 376,
         "IN GAME: HOLD HOTKEY + THIS CONTROL",
         2, dim);
   draw_text(back.fb, 195, 430, "ESC ON KEYBOARD: CANCEL", 2, dim);
   stage42_present(physical, back);

   stage434_drain_pad(pad);
   if (!stage434_wait_buttons_released(pad, in))
      return false;

   int16_t axis_baseline[H3531_JS_MAX_AXES]{};
   for (int i = 0; i < H3531_JS_MAX_AXES; ++i)
      axis_baseline[i] = pad.axes[i];

   for (;;)
   {
      if (stage434_keyboard_cancel(in))
         return false;

      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));

      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.code <= KEY_MAX)
         {
            const unsigned idx = pad.key_to_button[ev.code];
            if (idx >= H3531_JS_MAX_BUTTONS)
               continue;

            pad.buttons[idx] = ev.value != 0;
            if (ev.value)
            {
               out.button = (int)idx;
               out.axis = -1;
               out.axis_dir = 0;
               return true;
            }
         }
         else if (allow_axis && ev.type == EV_ABS && ev.code <= ABS_MAX)
         {
            const unsigned idx = pad.abs_to_axis[ev.code];
            if (idx < H3531_JS_MAX_AXES)
            {
               const int16_t value =
                  h3531_front_scale_abs(pad.absinfo[idx], ev.value);
               pad.axes[idx] = value;
               const int delta = (int)value - (int)axis_baseline[idx];
               if (std::abs(delta) > 16000)
               {
                  out.button = -1;
                  out.axis = (int)idx;
                  out.axis_dir = value < 0 ? -1 : 1;
                  return true;
               }
            }
         }
         continue;
      }

      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR)
         return false;

      usleep(5000);
   }
}'''
src = replace_function(src, "static bool stage437_capture_bind(", hotkey_capture)

layout = '   printf("LAYOUT_TEST_OK\\n");\n'
marker = '   printf("STAGE448_READABILITY footer2 search-alpha3 controller-labels2 hotkey-labels2\\n");\n'
if layout not in src:
    raise SystemExit("Stage4.48 layout marker anchor missing")
src = src.replace(layout, marker + layout, 1)

required = [
   "STAGE448_READABILITY footer2 search-alpha3 controller-labels2 hotkey-labels2",
   '"DPAD/STICK NAV  L1/R1 SYSTEM  L2/R2 FAVORITE  Y SEARCH  A PLAY  START MENU",\n            2',
   'draw_text(fb, x, y, label, 3, text);',
   '"A ADD   B BACK   START/Y RESULTS", 2',
   '"DPAD MOVE   F3 SEARCH   KEYBOARD OK", 2',
   '"MOVE THE REQUESTED ANALOG STICK", 2',
   '"OPTIONAL: HOLD ANY BUTTON 1 SEC TO SKIP", 2',
   '"ESC ON KEYBOARD: CANCEL", 2',
   '"IN GAME: HOLD HOTKEY + THIS CONTROL",\n         2',
]
for m in required:
    if m not in src:
        raise SystemExit("missing Stage4.48 marker: " + m)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("STAGE448_CONTROL_SEARCH_READABILITY_PATCH_OK")
