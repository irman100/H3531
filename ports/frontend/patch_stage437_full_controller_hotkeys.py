#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage437_full_controller_hotkeys.py INPUT OUTPUT")

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

old_steps = r'''struct Stage434BindStep {
   const char *label;
   const char *retro_base;
   bool direction;
};

static const Stage434BindStep stage434_steps[] = {
   {"D-PAD UP",    "up",     true},
   {"D-PAD DOWN",  "down",   true},
   {"D-PAD LEFT",  "left",   true},
   {"D-PAD RIGHT", "right",  true},
   {"A  BOTTOM",   "b",      false},
   {"B  RIGHT",    "a",      false},
   {"X  LEFT",     "y",      false},
   {"Y  TOP",      "x",      false},
   {"START / PAUSE","start",  false},
   {"SELECT",      "select", false},
   {"L SHOULDER",  "l",      false},
   {"R SHOULDER",  "r",      false},
   {"MENU / PAUSE", "menu_toggle", false},
};'''

new_steps = r'''struct Stage434BindStep {
   const char *label;
   const char *retro_base;
   bool allow_axis;
};

static const Stage434BindStep stage434_steps[] = {
   {"D-PAD UP",       "up",     true},
   {"D-PAD DOWN",     "down",   true},
   {"D-PAD LEFT",     "left",   true},
   {"D-PAD RIGHT",    "right",  true},
   {"A  BOTTOM",      "b",      false},
   {"B  RIGHT / BACK","a",      false},
   {"X  LEFT",        "y",      false},
   {"Y  TOP",         "x",      false},
   {"START / PAUSE",  "start",  false},
   {"SELECT",         "select", false},
   {"L1  SHOULDER",   "l",      false},
   {"R1  SHOULDER",   "r",      false},
   {"L2  TRIGGER",    "l2",     true},
   {"R2  TRIGGER",    "r2",     true},
   {"HOTKEY",         nullptr,   false},
};

static const int STAGE437_DPAD_UP = 0;
static const int STAGE437_DPAD_DOWN = 1;
static const int STAGE437_DPAD_LEFT = 2;
static const int STAGE437_DPAD_RIGHT = 3;
static const int STAGE437_A = 4;
static const int STAGE437_B = 5;
static const int STAGE437_X = 6;
static const int STAGE437_Y = 7;
static const int STAGE437_START = 8;
static const int STAGE437_SELECT = 9;
static const int STAGE437_L1 = 10;
static const int STAGE437_R1 = 11;
static const int STAGE437_L2 = 12;
static const int STAGE437_R2 = 13;
static const int STAGE437_HOTKEY = 14;'''

if old_steps not in src:
    raise SystemExit("Stage4.37 step block anchor missing")
src = src.replace(old_steps, new_steps, 1)

src = src.replace("stage434_steps[step].direction",
                  "stage434_steps[step].allow_axis")

src = src.replace(
'''      if (stage434_steps[step].allow_axis)
         draw_text(fb, 205, 456, "D-PAD BUTTON OR AXIS DIRECTION", 1, dim);
      else
         draw_text(fb, 205, 456, "PRESS THE REQUESTED BUTTON", 1, dim);''',
'''      if (stage434_steps[step].allow_axis)
         draw_text(fb, 205, 456, "PRESS BUTTON OR MOVE THE REQUESTED AXIS", 1, dim);
      else
         draw_text(fb, 205, 456, "PRESS THE REQUESTED BUTTON", 1, dim);''',
1)

# Do not write the HOTKEY pseudo-step into the device autoconfig as a RetroPad control.
old_loop = '''   const int count = (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0]));
   for (int i = 0; i < count; ++i)
      stage434_write_bind(out, stage434_steps[i].retro_base, binds[i]);'''
new_loop = '''   const int count = (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0]));
   for (int i = 0; i < count; ++i)
      if (stage434_steps[i].retro_base && stage434_steps[i].retro_base[0])
         stage434_write_bind(out, stage434_steps[i].retro_base, binds[i]);'''
if old_loop not in src:
    raise SystemExit("Stage4.37 profile loop anchor missing")
src = src.replace(old_loop, new_loop, 1)

helpers = r'''
static bool stage437_bind_equal(
      const Stage434CapturedBind &a,
      const Stage434CapturedBind &b)
{
   if (a.button >= 0 || b.button >= 0)
      return a.button >= 0 && b.button >= 0 && a.button == b.button;

   return a.axis >= 0 && b.axis >= 0 &&
          a.axis == b.axis && a.axis_dir == b.axis_dir;
}

static Stage434CapturedBind stage437_cfg_bind(const char *base)
{
   Stage434CapturedBind out;
   std::map<std::string, std::string> kv;
   if (!h3531_read_cfg("/mnt/usb/H3531/APPS/retroarch/retroarch.cfg", kv))
      return out;

   const std::string stem = std::string("input_") + base;
   auto ib = kv.find(stem + "_btn");
   if (ib != kv.end() && !ib->second.empty() && ib->second != "nul")
   {
      char *end = nullptr;
      long n = std::strtol(ib->second.c_str(), &end, 10);
      if (end && *end == '\0' && n >= 0 && n < H3531_JS_MAX_BUTTONS)
         out.button = (int)n;
   }

   auto ia = kv.find(stem + "_axis");
   if (ia != kv.end() && ia->second.size() >= 2 &&
       (ia->second[0] == '+' || ia->second[0] == '-'))
   {
      char *end = nullptr;
      long n = std::strtol(ia->second.c_str() + 1, &end, 10);
      if (end && *end == '\0' && n >= 0 && n < H3531_JS_MAX_AXES)
      {
         out.axis = (int)n;
         out.axis_dir = ia->second[0] == '-' ? -1 : 1;
      }
   }

   return out;
}

static bool stage437_write_global_binding(
      const char *base,
      const Stage434CapturedBind &bind)
{
   const char *path = "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg";
   const std::string tmp = std::string(path) + ".tmp";
   std::ifstream in(path);
   std::vector<std::string> lines;
   std::string line;

   if (!in) return false;
   while (std::getline(in, line)) lines.push_back(line);
   in.close();

   const std::string stem = std::string("input_") + base;

   if (bind.button >= 0)
   {
      stage434_set_config_value(lines, stem + "_btn",
            std::to_string(bind.button));
      stage434_set_config_value(lines, stem + "_axis", "nul");
   }
   else if (bind.axis >= 0 && bind.axis_dir)
   {
      stage434_set_config_value(lines, stem + "_btn", "nul");
      stage434_set_config_value(lines, stem + "_axis",
            std::string(bind.axis_dir < 0 ? "-" : "+") +
            std::to_string(bind.axis));
   }
   else
   {
      stage434_set_config_value(lines, stem + "_btn", "nul");
      stage434_set_config_value(lines, stem + "_axis", "nul");
   }

   std::ofstream out(tmp, std::ios::trunc);
   if (!out) return false;
   for (const std::string &l : lines) out << l << "\n";
   out.close();
   if (!out) return false;

   if (rename(tmp.c_str(), path) != 0)
   {
      unlink(tmp.c_str());
      return false;
   }

   fprintf(stderr,
         "[STAYPLAYTION] RetroArch hotkey binding saved: %s\n",
         base);
   return true;
}

static const char *stage437_hotkey_actions[] = {
   "rewind",
   "menu_toggle",
   "save_state",
   "load_state",
   "hold_fast_forward",
   "exit_emulator",
};

static const char *stage437_hotkey_action_names[] = {
   "REWIND",
   "RETROARCH MENU",
   "SAVE STATE",
   "LOAD STATE",
   "FAST FORWARD (HOLD)",
   "EXIT GAME",
};

static void stage437_clear_duplicate_action(
      const char *except_base,
      const Stage434CapturedBind &bind)
{
   const int count = (int)(sizeof(stage437_hotkey_actions) /
         sizeof(stage437_hotkey_actions[0]));

   for (int i = 0; i < count; ++i)
   {
      if (!strcmp(stage437_hotkey_actions[i], except_base))
         continue;

      const Stage434CapturedBind other =
         stage437_cfg_bind(stage437_hotkey_actions[i]);
      if (stage437_bind_equal(other, bind))
         stage437_write_global_binding(stage437_hotkey_actions[i],
               Stage434CapturedBind{});
   }
}

static std::string stage437_bind_raw_label(
      const Stage434CapturedBind &bind)
{
   if (bind.button >= 0)
      return std::string("BUTTON ") + std::to_string(bind.button);

   if (bind.axis >= 0 && bind.axis_dir)
      return std::string("AXIS ") +
         (bind.axis_dir < 0 ? "-" : "+") +
         std::to_string(bind.axis);

   return "NOT SET";
}

static bool stage437_capture_bind(Fb &physical, Stage42Backbuffer &back,
      Input &in, GamepadInput &pad, const std::string &label,
      bool allow_axis, Stage434CapturedBind &out)
{
   const uint16_t bg   = pack1555(238, 239, 241);
   const uint16_t line = pack1555(75, 155, 205);
   const uint16_t text = pack1555(67, 67, 67);
   const uint16_t dim  = pack1555(120, 120, 120);

   fill_rect(back.fb, 0, 0, (int)back.fb.w, (int)back.fb.h, bg);
   frame_rect(back.fb, 160, 74, 960, 565, 2, line);
   draw_text(back.fb, 205, 112, "HOTKEY ACTION SETUP", 3, text);
   draw_text(back.fb, 205, 180, "PRESS / MOVE", 2, dim);
   draw_text(back.fb, 205, 244, label, 4, text);
   draw_text(back.fb, 205, 338,
         allow_axis ? "BUTTON OR AXIS DIRECTION" : "BUTTON ONLY",
         1, dim);
   draw_text(back.fb, 205, 390,
         "IN GAME: HOLD HOTKEY + THIS CONTROL",
         1, dim);
   draw_text(back.fb, 205, 448, "ESC ON KEYBOARD: CANCEL", 1, dim);
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
            if (idx < H3531_JS_MAX_BUTTONS)
            {
               pad.buttons[idx] = ev.value != 0;
               if (ev.value)
               {
                  out.button = (int)idx;
                  out.axis = -1;
                  out.axis_dir = 0;
                  return true;
               }
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
}

static GamepadInput *stage437_first_pad(Input &in)
{
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0)
         return &in.pads[i];
   return nullptr;
}

static void stage437_assign_hotkey_action(Fb &physical,
      Input &in, Stage42Backbuffer &back, int row)
{
   GamepadInput *pad = stage437_first_pad(in);
   if (!pad) return;

   Stage434CapturedBind bind;

   if (row == 1)
   {
      if (!stage437_capture_bind(physical, back, in, *pad,
            "HOTKEY MODIFIER", false, bind))
         return;

      stage437_write_global_binding("enable_hotkey", bind);
      return;
   }

   const int action = row - 2;
   const int count = (int)(sizeof(stage437_hotkey_actions) /
         sizeof(stage437_hotkey_actions[0]));
   if (action < 0 || action >= count)
      return;

   if (!stage437_capture_bind(physical, back, in, *pad,
         stage437_hotkey_action_names[action], true, bind))
      return;

   const Stage434CapturedBind modifier =
      stage437_cfg_bind("enable_hotkey");
   if (stage437_bind_equal(modifier, bind))
   {
      fprintf(stderr,
            "[STAYPLAYTION] refusing hotkey action identical to modifier\n");
      return;
   }

   stage437_clear_duplicate_action(stage437_hotkey_actions[action], bind);
   stage437_write_global_binding(stage437_hotkey_actions[action], bind);
}

static std::vector<std::string> stage437_hotkey_rows()
{
   std::vector<std::string> rows;
   rows.push_back("< BACK");
   rows.push_back(std::string("HOTKEY MODIFIER          ") +
         stage437_bind_raw_label(stage437_cfg_bind("enable_hotkey")));

   const int count = (int)(sizeof(stage437_hotkey_actions) /
         sizeof(stage437_hotkey_actions[0]));
   for (int i = 0; i < count; ++i)
      rows.push_back(std::string(stage437_hotkey_action_names[i]) +
            "          " +
            stage437_bind_raw_label(
               stage437_cfg_bind(stage437_hotkey_actions[i])));

   return rows;
}

static void stage437_apply_recalbox_defaults(
      const Stage434CapturedBind *binds)
{
   /* Recalbox-style defaults. Every action still remains editable in
    * Controllers Settings -> Hotkey Actions. */
   stage437_write_global_binding("enable_hotkey",
         binds[STAGE437_HOTKEY]);
   stage437_write_global_binding("menu_toggle",
         binds[STAGE437_B]);
   stage437_write_global_binding("save_state",
         binds[STAGE437_Y]);
   stage437_write_global_binding("load_state",
         binds[STAGE437_X]);
   stage437_write_global_binding("rewind",
         binds[STAGE437_DPAD_LEFT]);
   stage437_write_global_binding("hold_fast_forward",
         binds[STAGE437_DPAD_RIGHT]);
   stage437_write_global_binding("exit_emulator",
         binds[STAGE437_START]);
}
'''

anchor = "static bool stage434_save_profile(GamepadInput &pad,"
pos = src.find(anchor)
if pos < 0:
    raise SystemExit("Stage4.37 save-profile insertion anchor missing")
src = src[:pos] + helpers + "\n" + src[pos:]

# Replace the old single menu-hotkey tail with modifier + Recalbox-style defaults.
tail_start = src.find("   const int menu_index =")
tail_end_marker = "   h3531_profile_load(pad);"
tail_end = src.find(tail_end_marker, tail_start)
if tail_start < 0 or tail_end < 0:
    raise SystemExit("Stage4.37 legacy menu-hotkey tail anchors missing")
src = (src[:tail_start] +
'''   stage437_apply_recalbox_defaults(binds);

''' +
       src[tail_end:])

# Extend the Stage4.36 menu with a dedicated Hotkey Actions page.
src = src.replace(
'''enum class Stage436MenuPage
{
   Root,
   Games,
   Controllers,
   System,
   Quit
};''',
'''enum class Stage436MenuPage
{
   Root,
   Games,
   Controllers,
   Hotkeys,
   System,
   Quit
};''',
1)

src = src.replace(
'''      case Stage436MenuPage::Controllers: return "CONTROLLERS SETTINGS";
      case Stage436MenuPage::System: return "SYSTEM SETTINGS";''',
'''      case Stage436MenuPage::Controllers: return "CONTROLLERS SETTINGS";
      case Stage436MenuPage::Hotkeys: return "HOTKEY ACTIONS";
      case Stage436MenuPage::System: return "SYSTEM SETTINGS";''',
1)

src = src.replace(
'''         rows.push_back("< BACK");
         rows.push_back("CONFIGURE A CONTROLLER");
         rows.push_back("RETROARCH INPUT / HOTKEYS");
         rows.push_back("RESET CURRENT CONTROLLER PROFILE");''',
'''         rows.push_back("< BACK");
         rows.push_back("CONFIGURE A CONTROLLER");
         rows.push_back("HOTKEY ACTIONS                           >");
         rows.push_back("RETROARCH INPUT / HOTKEYS");
         rows.push_back("RESET CURRENT CONTROLLER PROFILE");''',
1)

# Insert Hotkeys rows before System case.
system_case = '''      case Stage436MenuPage::System:
         rows.push_back("< BACK");'''
if system_case not in src:
    raise SystemExit("Stage4.37 System rows anchor missing")
src = src.replace(system_case,
'''      case Stage436MenuPage::Hotkeys:
         return stage437_hotkey_rows();

      case Stage436MenuPage::System:
         rows.push_back("< BACK");''', 1)

src = src.replace(
'''      case Stage436MenuPage::Controllers: return 4;
      case Stage436MenuPage::System: return 4;''',
'''      case Stage436MenuPage::Controllers: return 5;
      case Stage436MenuPage::Hotkeys: return 8;
      case Stage436MenuPage::System: return 4;''',
1)

src = src.replace(
'''      const bool informational =
         page == Stage436MenuPage::Controllers && i == 4;''',
'''      const bool informational =
         page == Stage436MenuPage::Controllers && i == 5;''',
1)

src = src.replace(
'''   if (page == Stage436MenuPage::Controllers)
      draw_text(fb, x + 34, y + h - 34,
            "ONE CONTROLLER PROFILE IS SHARED WITH RETROARCH",
            1, dim);
   else if (page == Stage436MenuPage::Games)''',
'''   if (page == Stage436MenuPage::Controllers)
      draw_text(fb, x + 34, y + h - 34,
            "ONE CONTROLLER PROFILE IS SHARED WITH RETROARCH",
            1, dim);
   else if (page == Stage436MenuPage::Hotkeys)
      draw_text(fb, x + 34, y + h - 34,
            "REWIND REQUIRES GAME SETTINGS > REWIND = ON",
            1, dim);
   else if (page == Stage436MenuPage::Games)''',
1)

old_controller_handler = r'''            else if (page == Stage436MenuPage::Controllers)
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
            }'''

new_controller_handler = r'''            else if (page == Stage436MenuPage::Controllers)
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
                  page = Stage436MenuPage::Hotkeys;
                  item = 0;
               }
               else if (item == 3)
               {
                  stage430_persist();
                  run_external(physical, in, std::string(kRetroArchMenu));
                  if (!back.init(physical)) return;
               }
               else if (item == 4)
               {
                  stage436_delete_current_profile(in);
                  stage434_controller_setup(physical, in, back);
               }
            }
            else if (page == Stage436MenuPage::Hotkeys)
            {
               if (item == 0)
               {
                  page = Stage436MenuPage::Controllers;
                  item = 2;
               }
               else
                  stage437_assign_hotkey_action(physical, in, back, item);
            }'''

if old_controller_handler not in src:
    raise SystemExit("Stage4.37 controller handler anchor missing")
src = src.replace(old_controller_handler, new_controller_handler, 1)

old_back = r'''         case Action::Exit:
            if (page == Stage436MenuPage::Root)
               open = false;
            else
            {
               page = Stage436MenuPage::Root;
               item = 0;
            }
            break;'''

new_back = r'''         case Action::Exit:
            if (page == Stage436MenuPage::Root)
               open = false;
            else if (page == Stage436MenuPage::Hotkeys)
            {
               page = Stage436MenuPage::Controllers;
               item = 2;
            }
            else
            {
               page = Stage436MenuPage::Root;
               item = 0;
            }
            break;'''

if old_back not in src:
    raise SystemExit("Stage4.37 Back handler anchor missing")
src = src.replace(old_back, new_back, 1)

src = src.replace(
    "Stage4.36 Recalbox-style first-run and main menu active",
    "Stage4.37 Full controller and Hotkey Actions active")

src = src.replace(
    "STAGE436_FIRST_RUN welcome press-any configure-standard-profile",
    "STAGE437_CONTROLLER L1-R1-L2-R2 hotkey-modifier full-profile")
src = src.replace(
    "STAGE436_INPUT one-profile stayplaytion-retroarch no-shell-bind-overrides",
    "STAGE437_HOTKEYS modifier-plus-action rewind-menu-save-load-fast-exit")

required = [
    "Stage4.37 Full controller and Hotkey Actions active",
    "L1  SHOULDER",
    "R1  SHOULDER",
    "L2  TRIGGER",
    "R2  TRIGGER",
    '{"HOTKEY",',
    "HOTKEY ACTIONS",
    "HOTKEY MODIFIER",
    "REWIND",
    "RETROARCH MENU",
    "SAVE STATE",
    "LOAD STATE",
    "FAST FORWARD (HOLD)",
    "EXIT GAME",
    '"enable_hotkey"',
    '"rewind"',
    '"hold_fast_forward"',
    "REWIND REQUIRES GAME SETTINGS > REWIND = ON",
    "STAGE437_CONTROLLER L1-R1-L2-R2 hotkey-modifier full-profile",
    "STAGE437_HOTKEYS modifier-plus-action rewind-menu-save-load-fast-exit",
]

for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.37 marker: " + marker)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("STAGE437_FULL_CONTROLLER_HOTKEYS_PATCH_OK")
