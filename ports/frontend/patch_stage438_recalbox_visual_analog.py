#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage438_recalbox_visual_analog.py INPUT OUTPUT")

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

old_steps_start = src.find("static const Stage434BindStep stage434_steps[] = {")
if old_steps_start < 0:
    raise SystemExit("Stage4.38 step array missing")
old_steps_end = src.find("};", old_steps_start)
if old_steps_end < 0:
    raise SystemExit("Stage4.38 step array end missing")
old_steps_end += 2

new_steps = r'''static const Stage434BindStep stage434_steps[] = {
   {"D-PAD UP",          "up",     true},
   {"D-PAD DOWN",        "down",   true},
   {"D-PAD LEFT",        "left",   true},
   {"D-PAD RIGHT",       "right",  true},
   {"LEFT STICK UP",     nullptr,   true},
   {"LEFT STICK LEFT",   nullptr,   true},
   {"RIGHT STICK UP",    nullptr,   true},
   {"RIGHT STICK LEFT",  nullptr,   true},
   {"A  BOTTOM",         "b",      false},
   {"B  RIGHT / BACK",   "a",      false},
   {"X  LEFT",           "y",      false},
   {"Y  TOP",            "x",      false},
   {"START / PAUSE",     "start",  false},
   {"SELECT",            "select", false},
   {"L1  SHOULDER",      "l",      false},
   {"R1  SHOULDER",      "r",      false},
   {"L2  TRIGGER",       "l2",     true},
   {"R2  TRIGGER",       "r2",     true},
   {"L3  LEFT STICK CLICK",  "l3", false},
   {"R3  RIGHT STICK CLICK", "r3", false},
   {"HOTKEY",            nullptr,   false},
};'''

src = src[:old_steps_start] + new_steps + src[old_steps_end:]

# Replace Stage4.37 index constants with the complete Recalbox ordering.
const_start = src.find("static const int STAGE437_DPAD_UP")
const_end = src.find("static std::string stage434_escape_cfg", const_start)
if const_start < 0 or const_end < 0:
    raise SystemExit("Stage4.38 Stage437 index constants missing")

new_consts = r'''static const int STAGE437_DPAD_UP = 0;
static const int STAGE437_DPAD_DOWN = 1;
static const int STAGE437_DPAD_LEFT = 2;
static const int STAGE437_DPAD_RIGHT = 3;
static const int STAGE438_LEFT_STICK_UP = 4;
static const int STAGE438_LEFT_STICK_LEFT = 5;
static const int STAGE438_RIGHT_STICK_UP = 6;
static const int STAGE438_RIGHT_STICK_LEFT = 7;
static const int STAGE437_A = 8;
static const int STAGE437_B = 9;
static const int STAGE437_X = 10;
static const int STAGE437_Y = 11;
static const int STAGE437_START = 12;
static const int STAGE437_SELECT = 13;
static const int STAGE437_L1 = 14;
static const int STAGE437_R1 = 15;
static const int STAGE437_L2 = 16;
static const int STAGE437_R2 = 17;
static const int STAGE438_L3 = 18;
static const int STAGE438_R3 = 19;
static const int STAGE437_HOTKEY = 20;

static bool stage438_axis_only_step(int step)
{
   return step == STAGE438_LEFT_STICK_UP ||
          step == STAGE438_LEFT_STICK_LEFT ||
          step == STAGE438_RIGHT_STICK_UP ||
          step == STAGE438_RIGHT_STICK_LEFT;
}

static bool stage438_optional_step(int step)
{
   return stage438_axis_only_step(step) ||
          step == STAGE437_X || step == STAGE437_Y ||
          step == STAGE437_L1 || step == STAGE437_R1 ||
          step == STAGE437_L2 || step == STAGE437_R2 ||
          step == STAGE438_L3 || step == STAGE438_R3;
}

'''

src = src[:const_start] + new_consts + src[const_end:]

visual_helpers = r'''
static void stage438_draw_stick(Fb &fb, int cx, int cy,
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

   draw_text(fb, cx - 30, cy + 55, name, 1, text);
}

static void stage438_draw_controller_visual(Fb &fb, int step)
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

   /* Shoulder / trigger rows. */
   const bool l1 = step == STAGE437_L1;
   const bool r1 = step == STAGE437_R1;
   const bool l2 = step == STAGE437_L2;
   const bool r2 = step == STAGE437_R2;
   fill_rect(fb, x + 28, y + 10, 92, 20, l2 ? hi : body);
   frame_rect(fb, x + 28, y + 10, 92, 20, 2, l2 ? hi : edge);
   fill_rect(fb, x + w - 120, y + 10, 92, 20, r2 ? hi : body);
   frame_rect(fb, x + w - 120, y + 10, 92, 20, 2, r2 ? hi : edge);
   fill_rect(fb, x + 40, y + 36, 104, 22, l1 ? hi : body);
   frame_rect(fb, x + 40, y + 36, 104, 22, 2, l1 ? hi : edge);
   fill_rect(fb, x + w - 144, y + 36, 104, 22, r1 ? hi : body);
   frame_rect(fb, x + w - 144, y + 36, 104, 22, 2, r1 ? hi : edge);
   draw_text(fb, x + 60, y + 15, "L2", 1, text);
   draw_text(fb, x + w - 93, y + 15, "R2", 1, text);
   draw_text(fb, x + 80, y + 41, "L1", 1, text);
   draw_text(fb, x + w - 104, y + 41, "R1", 1, text);

   /* D-pad. */
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

   /* Face cluster. */
   const int fx = x + w - 88;
   const int fy = y + 120;
   const int bs = 24;
   fill_rect(fb, fx - bs/2, fy + 30 - bs/2, bs, bs,
         step == STAGE437_A ? hi : edge);
   fill_rect(fb, fx + 30 - bs/2, fy - bs/2, bs, bs,
         step == STAGE437_B ? hi : edge);
   fill_rect(fb, fx - 30 - bs/2, fy - bs/2, bs, bs,
         step == STAGE437_X ? hi : edge);
   fill_rect(fb, fx - bs/2, fy - 30 - bs/2, bs, bs,
         step == STAGE437_Y ? hi : edge);
   draw_text(fb, fx - 4, fy + 25, "A", 1, text);
   draw_text(fb, fx + 26, fy - 5, "B", 1, text);
   draw_text(fb, fx - 35, fy - 5, "X", 1, text);
   draw_text(fb, fx - 4, fy - 35, "Y", 1, text);

   /* Analog sticks. */
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
      draw_text(fb, x + 127, y + 197, "L3", 1, text);
   if (step == STAGE438_R3)
      draw_text(fb, x + 232, y + 197, "R3", 1, text);

   /* Start / Select / Hotkey. */
   fill_rect(fb, x + 169, y + 90, 42, 20,
         step == STAGE437_SELECT ? hi : edge);
   fill_rect(fb, x + 220, y + 90, 42, 20,
         step == STAGE437_START ? hi : edge);
   draw_text(fb, x + 177, y + 95, "SEL", 1, text);
   draw_text(fb, x + 228, y + 95, "STA", 1, text);

   if (step == STAGE437_HOTKEY)
   {
      frame_rect(fb, x + 155, y + 145, 86, 34, 3, hi);
      draw_text(fb, x + 178, y + 155, "HK", 2, hi);
   }
}

static void stage438_write_analog_pair(std::ofstream &out,
      const char *base, const Stage434CapturedBind &b)
{
   if (b.axis < 0 || b.axis_dir == 0)
      return;

   const char *captured = b.axis_dir < 0 ? "-" : "+";
   const char *opposite = b.axis_dir < 0 ? "+" : "-";

   out << "input_" << base << "_minus_axis = \"" <<
         captured << b.axis << "\"\n";
   out << "input_" << base << "_plus_axis = \"" <<
         opposite << b.axis << "\"\n";
}
'''

insert_anchor = "static void stage434_draw(Fb &fb, const GamepadInput &pad,"
pos = src.find(insert_anchor)
if pos < 0:
    raise SystemExit("Stage4.38 visual insertion anchor missing")
src = src[:pos] + visual_helpers + "\n" + src[pos:]

draw = r'''static void stage434_draw(Fb &fb, const GamepadInput &pad,
      int step, const std::string &status)
{
   const uint16_t bg   = pack1555(238, 239, 241);
   const uint16_t line = pack1555(75, 155, 205);
   const uint16_t text = pack1555(67, 67, 67);
   const uint16_t dim  = pack1555(120, 120, 120);
   const uint16_t ok   = pack1555(60, 150, 105);

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);
   frame_rect(fb, 90, 55, 1100, 610, 2, pack1555(190, 194, 200));

   draw_text(fb, 135, 92, "CONFIGURING", 3, text);
   draw_text(fb, 135, 137, std::string("GAMEPAD: ") + ascii_safe(pad.name), 1, dim);
   draw_text(fb, 135, 164, "RETROARCH STANDARD PROFILE", 1, dim);

   if (step >= 0 &&
       step < (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])))
   {
      stage438_draw_controller_visual(fb, step);

      draw_text(fb, 135, 235, "PRESS / MOVE", 2, dim);
      draw_text(fb, 135, 282, stage434_steps[step].label, 3, text);

      char progress[64];
      snprintf(progress, sizeof(progress), "STEP %d / %d",
            step + 1,
            (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])));
      draw_text(fb, 135, 345, progress, 2, dim);

      if (stage438_axis_only_step(step))
         draw_text(fb, 135, 405, "MOVE THE REQUESTED ANALOG STICK", 1, dim);
      else if (stage434_steps[step].allow_axis)
         draw_text(fb, 135, 405, "BUTTON OR AXIS DIRECTION", 1, dim);
      else
         draw_text(fb, 135, 405, "PRESS THE REQUESTED BUTTON", 1, dim);

      if (stage438_optional_step(step))
         draw_text(fb, 135, 452,
               "OPTIONAL: HOLD ANY BUTTON 1 SEC TO SKIP", 1, line);
      else
         draw_text(fb, 135, 452, "REQUIRED", 1, dim);

      draw_text(fb, 135, 505, "ESC ON KEYBOARD: CANCEL", 1, dim);
   }
   else
   {
      draw_text(fb, 135, 280,
            status.empty() ? "PROFILE SAVED" : status, 3, ok);
      draw_text(fb, 135, 350,
            "STAYPLAYTION AND RETROARCH USE THE SAME PROFILE", 1, text);
   }

   if (!status.empty() && step >= 0)
      draw_text(fb, 135, 555, status, 1, ok);
}'''
src = replace_function(src, "static void stage434_draw(Fb &fb, const GamepadInput &pad,", draw)

capture = r'''static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,
      Input &in, GamepadInput &pad, int step, Stage434CapturedBind &out)
{
   stage434_draw(back.fb, pad, step, "");
   stage42_present(physical, back);

   stage434_drain_pad(pad);
   if (!stage434_wait_buttons_released(pad, in))
      return false;

   int16_t axis_baseline[H3531_JS_MAX_AXES]{};
   for (int i = 0; i < H3531_JS_MAX_AXES; ++i)
      axis_baseline[i] = pad.axes[i];

   const bool optional = stage438_optional_step(step);
   const bool axis_only = stage438_axis_only_step(step);
   int held_button = -1;
   uint64_t held_since = 0;

   for (;;)
   {
      if (stage434_keyboard_cancel(in))
         return false;

      if (optional && held_button >= 0 && held_since &&
          input_now_ms() - held_since >= 1000ULL)
      {
         out = Stage434CapturedBind{};
         stage434_draw(back.fb, pad, step, "SKIPPED");
         stage42_present(physical, back);
         usleep(250000);
         return true;
      }

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
               if (optional)
               {
                  held_button = (int)idx;
                  held_since = input_now_ms();
               }
               else if (!axis_only)
               {
                  out.button = (int)idx;
                  out.axis = -1;
                  out.axis_dir = 0;
                  return true;
               }
            }
            else if (held_button == (int)idx)
            {
               const uint64_t held =
                  held_since ? input_now_ms() - held_since : 0;
               if (optional && held >= 1000ULL)
               {
                  out = Stage434CapturedBind{};
                  return true;
               }

               if (!axis_only)
               {
                  out.button = (int)idx;
                  out.axis = -1;
                  out.axis_dir = 0;
                  return true;
               }

               held_button = -1;
               held_since = 0;
            }
         }
         else if (ev.type == EV_ABS && ev.code <= ABS_MAX &&
                  stage434_steps[step].allow_axis)
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
src = replace_function(src, "static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,", capture)

# Add the four analog stick axis pairs after normal controls are written.
loop_anchor = '''   for (int i = 0; i < count; ++i)
      if (stage434_steps[i].retro_base && stage434_steps[i].retro_base[0])
         stage434_write_bind(out, stage434_steps[i].retro_base, binds[i]);

   out.close();'''
if loop_anchor not in src:
    raise SystemExit("Stage4.38 profile writer anchor missing")
src = src.replace(loop_anchor,
'''   for (int i = 0; i < count; ++i)
      if (stage434_steps[i].retro_base && stage434_steps[i].retro_base[0])
         stage434_write_bind(out, stage434_steps[i].retro_base, binds[i]);

   stage438_write_analog_pair(out, "l_y", binds[STAGE438_LEFT_STICK_UP]);
   stage438_write_analog_pair(out, "l_x", binds[STAGE438_LEFT_STICK_LEFT]);
   stage438_write_analog_pair(out, "r_y", binds[STAGE438_RIGHT_STICK_UP]);
   stage438_write_analog_pair(out, "r_x", binds[STAGE438_RIGHT_STICK_LEFT]);

   out.close();''', 1)

src = src.replace(
    "Stage4.37 Full controller and Hotkey Actions active",
    "Stage4.38 Recalbox visual full analog controller setup active")

src = src.replace(
    "STAGE437_CONTROLLER L1-R1-L2-R2 hotkey-modifier full-profile",
    "STAGE438_CONTROLLER dpad-two-sticks-L1-R1-L2-R2-L3-R3-hotkey")
src = src.replace(
    "STAGE437_HOTKEYS modifier-plus-action rewind-menu-save-load-fast-exit",
    "STAGE438_VISUAL controller-diagram optional-hold-to-skip standard-retroarch-axis-pairs")

required = [
    "Stage4.38 Recalbox visual full analog controller setup active",
    "LEFT STICK UP",
    "LEFT STICK LEFT",
    "RIGHT STICK UP",
    "RIGHT STICK LEFT",
    "L3  LEFT STICK CLICK",
    "R3  RIGHT STICK CLICK",
    "OPTIONAL: HOLD ANY BUTTON 1 SEC TO SKIP",
    "stage438_draw_controller_visual",
    'stage438_write_analog_pair(out, "l_y"',
    'stage438_write_analog_pair(out, "l_x"',
    'stage438_write_analog_pair(out, "r_y"',
    'stage438_write_analog_pair(out, "r_x"',
    "STAGE438_CONTROLLER dpad-two-sticks-L1-R1-L2-R2-L3-R3-hotkey",
    "STAGE438_VISUAL controller-diagram optional-hold-to-skip standard-retroarch-axis-pairs",
]
for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.38 marker: " + marker)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("STAGE438_RECALBOX_VISUAL_ANALOG_PATCH_OK")
