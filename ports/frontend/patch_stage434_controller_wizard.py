#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage434_controller_wizard.py INPUT OUTPUT")

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

wizard = r'''
struct Stage434CapturedBind {
   int button = -1;
   int axis = -1;
   int axis_dir = 0;
};

struct Stage434BindStep {
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
};

static std::string stage434_escape_cfg(const std::string &v)
{
   std::string out;
   out.reserve(v.size());
   for (char c : v)
   {
      if (c == '"' || c == '\\') out.push_back('\\');
      out.push_back(c);
   }
   return out;
}

static void stage434_draw(Fb &fb, const GamepadInput &pad,
      int step, const std::string &status)
{
   const uint16_t bg   = pack1555(4, 13, 28);
   const uint16_t line = pack1555(69, 221, 255);
   const uint16_t text = pack1555(234, 245, 255);
   const uint16_t dim  = pack1555(134, 158, 183);
   const uint16_t ok   = pack1555(105, 235, 160);

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);
   frame_rect(fb, 160, 74, 960, 565, 2, line);

   draw_text(fb, 205, 112, "CONTROLLER SETUP", 3, text);
   draw_text(fb, 205, 166, std::string("DEVICE: ") + ascii_safe(pad.name), 1, dim);
   draw_text(fb, 205, 198, "STANDARD RETROARCH LINUXRAW PROFILE", 1, dim);

   if (step >= 0 && step < (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])))
   {
      draw_text(fb, 205, 278, "PRESS / MOVE", 2, dim);
      draw_text(fb, 205, 324, stage434_steps[step].label, 4, text);

      char progress[64];
      snprintf(progress, sizeof(progress), "STEP %d / %d",
            step + 1,
            (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])));
      draw_text(fb, 205, 402, progress, 2, dim);

      if (stage434_steps[step].direction)
         draw_text(fb, 205, 456, "D-PAD BUTTON OR AXIS DIRECTION", 1, dim);
      else
         draw_text(fb, 205, 456, "PRESS THE REQUESTED BUTTON", 1, dim);

      draw_text(fb, 205, 505, "ESC ON KEYBOARD: CANCEL", 1, dim);
   }
   else
   {
      draw_text(fb, 205, 294, status.empty() ? "PROFILE SAVED" : status, 3, ok);
      draw_text(fb, 205, 372,
            "RETROARCH AND STAYPLAYTION NOW USE THE SAME PROFILE", 1, text);
   }

   if (!status.empty() && step >= 0)
      draw_text(fb, 205, 552, status, 1, ok);
}

static bool stage434_keyboard_cancel(Input &in)
{
   if (in.fd < 0) return false;

   for (;;)
   {
      input_event ev{};
      const ssize_t n = read(in.fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.value == 1 && ev.code == KEY_ESC)
            return true;
         continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         break;
      break;
   }
   return false;
}

static void stage434_drain_pad(GamepadInput &pad)
{
   if (pad.fd < 0) return;
   for (;;)
   {
      js_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n != (ssize_t)sizeof(ev)) break;
      const unsigned type = ev.type & ~JS_EVENT_INIT;
      if (type == JS_EVENT_BUTTON && ev.number < H3531_JS_MAX_BUTTONS)
         pad.buttons[ev.number] = ev.value != 0;
      else if (type == JS_EVENT_AXIS && ev.number < H3531_JS_MAX_AXES)
         pad.axes[ev.number] = ev.value;
   }
}

static bool stage434_wait_neutral(GamepadInput &pad, Input &in)
{
   const uint64_t deadline = input_now_ms() + 4000ULL;

   while (input_now_ms() < deadline)
   {
      if (stage434_keyboard_cancel(in)) return false;

      bool neutral = true;
      for (int i = 0; i < H3531_JS_MAX_BUTTONS; ++i)
         if (pad.buttons[i]) neutral = false;
      for (int i = 0; i < H3531_JS_MAX_AXES; ++i)
         if (std::abs((int)pad.axes[i]) > 12000) neutral = false;

      if (neutral) return true;

      js_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         const unsigned type = ev.type & ~JS_EVENT_INIT;
         if (type == JS_EVENT_BUTTON && ev.number < H3531_JS_MAX_BUTTONS)
            pad.buttons[ev.number] = ev.value != 0;
         else if (type == JS_EVENT_AXIS && ev.number < H3531_JS_MAX_AXES)
            pad.axes[ev.number] = ev.value;
      }
      else
         usleep(5000);
   }

   return true;
}

static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,
      Input &in, GamepadInput &pad, int step, Stage434CapturedBind &out)
{
   stage434_draw(back.fb, pad, step, "");
   stage42_present(physical, back);

   stage434_drain_pad(pad);
   if (!stage434_wait_neutral(pad, in))
      return false;

   for (;;)
   {
      if (stage434_keyboard_cancel(in))
         return false;

      js_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));

      if (n == (ssize_t)sizeof(ev))
      {
         const unsigned type = ev.type & ~JS_EVENT_INIT;

         if (type == JS_EVENT_BUTTON && ev.number < H3531_JS_MAX_BUTTONS)
         {
            pad.buttons[ev.number] = ev.value != 0;

            if (ev.value)
            {
               out.button = ev.number;
               out.axis = -1;
               out.axis_dir = 0;
               return true;
            }
         }
         else if (type == JS_EVENT_AXIS && ev.number < H3531_JS_MAX_AXES)
         {
            pad.axes[ev.number] = ev.value;

            if (stage434_steps[step].direction && std::abs((int)ev.value) > 20000)
            {
               out.button = -1;
               out.axis = ev.number;
               out.axis_dir = ev.value < 0 ? -1 : 1;
               return true;
            }
         }
         continue;
      }

      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
         return false;

      usleep(5000);
   }
}

static void stage434_write_bind(std::ofstream &out,
      const char *base, const Stage434CapturedBind &b)
{
   if (b.button >= 0)
      out << "input_" << base << "_btn = \"" << b.button << "\"\n";
   else if (b.axis >= 0 && b.axis_dir)
      out << "input_" << base << "_axis = \"" <<
            (b.axis_dir < 0 ? "-" : "+") << b.axis << "\"\n";
}

static bool stage434_mkdir(const char *path)
{
   if (mkdir(path, 0777) == 0 || errno == EEXIST)
      return true;
   return false;
}

static bool stage434_set_config_value(
      std::vector<std::string> &lines,
      const std::string &key,
      const std::string &value)
{
   const std::string replacement = key + " = \"" + value + "\"";
   for (std::string &line : lines)
   {
      const std::string t = h3531_profile_trim(line);
      if (t.compare(0, key.size(), key) != 0)
         continue;

      size_t p = key.size();
      while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
      if (p < t.size() && t[p] == '=')
      {
         line = replacement;
         return true;
      }
   }

   lines.push_back(replacement);
   return true;
}

static bool stage434_save_menu_hotkey(const Stage434CapturedBind &bind)
{
   const char *path = "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg";
   const std::string tmp = std::string(path) + ".tmp";
   std::ifstream in(path);
   std::vector<std::string> lines;
   std::string line;

   if (!in) return false;
   while (std::getline(in, line)) lines.push_back(line);
   in.close();

   if (bind.button >= 0)
   {
      stage434_set_config_value(lines, "input_menu_toggle_btn",
            std::to_string(bind.button));
      stage434_set_config_value(lines, "input_menu_toggle_axis", "nul");
   }
   else if (bind.axis >= 0 && bind.axis_dir)
   {
      stage434_set_config_value(lines, "input_menu_toggle_btn", "nul");
      stage434_set_config_value(lines, "input_menu_toggle_axis",
            std::string(bind.axis_dir < 0 ? "-" : "+") +
            std::to_string(bind.axis));
   }
   else
      return false;

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
         "[STAYPLAYTION] RetroArch standard menu hotkey saved in %s\\n",
         path);
   return true;
}

static bool stage434_save_profile(GamepadInput &pad,
      const Stage434CapturedBind *binds)
{
   if (!stage434_mkdir("/mnt/usb/H3531/USER")) return false;
   if (!stage434_mkdir("/mnt/usb/H3531/USER/retroarch")) return false;
   if (!stage434_mkdir(H3531_RA_AUTOCONFIG)) return false;

   const std::string path = std::string(H3531_RA_AUTOCONFIG) + "/" +
         h3531_profile_filename(pad.name);
   const std::string tmp = path + ".tmp";

   std::ofstream out(tmp, std::ios::trunc);
   if (!out) return false;

   out << "# Stayplaytion controller setup - standard RetroArch autoconfig\n";
   out << "# Compatible with RetroArch linuxraw joypad driver\n";
   out << "input_driver = \"linuxraw\"\n";
   out << "input_device = \"" << stage434_escape_cfg(pad.name) << "\"\n";

   const int count = (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0]));
   for (int i = 0; i < count; ++i)
      stage434_write_bind(out, stage434_steps[i].retro_base, binds[i]);

   out.close();
   if (!out) return false;

   if (rename(tmp.c_str(), path.c_str()) != 0)
   {
      unlink(tmp.c_str());
      return false;
   }

   fprintf(stderr, "[STAYPLAYTION] standard RetroArch controller profile saved: %s\n",
         path.c_str());

   const int menu_index =
      (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0])) - 1;
   if (!stage434_save_menu_hotkey(binds[menu_index]))
      fprintf(stderr,
            "[STAYPLAYTION] WARNING: could not save RetroArch menu hotkey\n");

   h3531_profile_load(pad);
   return pad.profile.loaded;
}

static void stage434_controller_setup(Fb &physical, Input &in,
      Stage42Backbuffer &back)
{
   input_rescan(in);

   GamepadInput *pad = nullptr;
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0)
      {
         pad = &in.pads[i];
         break;
      }

   if (!pad)
   {
      const uint16_t bg = pack1555(4, 13, 28);
      const uint16_t text = pack1555(234, 245, 255);
      const uint16_t dim = pack1555(134, 158, 183);
      fill_rect(back.fb, 0, 0, (int)back.fb.w, (int)back.fb.h, bg);
      draw_text(back.fb, 250, 280, "NO /DEV/INPUT/JS CONTROLLER FOUND", 2, text);
      draw_text(back.fb, 250, 330, "CONNECT A USB GAMEPAD AND TRY AGAIN", 1, dim);
      stage42_present(physical, back);
      usleep(1800000);
      return;
   }

   Stage434CapturedBind captured[
      sizeof(stage434_steps) / sizeof(stage434_steps[0])];

   const int count = (int)(sizeof(stage434_steps) / sizeof(stage434_steps[0]));
   for (int i = 0; i < count; ++i)
   {
      if (!stage434_capture(physical, back, in, *pad, i, captured[i]))
      {
         stage434_draw(back.fb, *pad, -1, "SETUP CANCELLED");
         stage42_present(physical, back);
         usleep(900000);
         return;
      }

      char status[96];
      if (captured[i].button >= 0)
         snprintf(status, sizeof(status), "CAPTURED BUTTON %d", captured[i].button);
      else
         snprintf(status, sizeof(status), "CAPTURED AXIS %c%d",
               captured[i].axis_dir < 0 ? '-' : '+', captured[i].axis);

      stage434_draw(back.fb, *pad, i, status);
      stage42_present(physical, back);
      usleep(180000);
   }

   const bool saved = stage434_save_profile(*pad, captured);
   stage434_draw(back.fb, *pad, -1,
         saved ? "PROFILE SAVED" : "PROFILE SAVE FAILED");
   stage42_present(physical, back);
   usleep(saved ? 1300000 : 2200000);
}
'''

anchor = "static void stage413_quick_menu(Fb &physical, Input &in, Stage42Backbuffer &back,"
pos = src.find(anchor)
if pos < 0:
    raise SystemExit("quick menu anchor missing")
src = src[:pos] + wizard + "\n" + src[pos:]

old = r'''               else if (item == 5 || item == 6)
               {
                  /* Both entries intentionally open stock RetroArch RGUI.
                   * Controller Settings points the user to:
                   * Settings > Input > RetroPad Binds > Port 1 Controls.
                   * RetroArch Settings exposes the complete standard menu. */
                  stage430_persist();
                  run_external(physical, in, std::string(kRetroArchMenu));
                  if (!back.init(physical)) return;
               }'''

new = r'''               else if (item == 5)
               {
                  stage430_persist();
                  stage434_controller_setup(physical, in, back);
               }
               else if (item == 6)
               {
                  stage430_persist();
                  run_external(physical, in, std::string(kRetroArchMenu));
                  if (!back.init(physical)) return;
               }'''

if old not in src:
    raise SystemExit("Stage4.33 controller/settings handler anchor missing")
src = src.replace(old, new, 1)

src = src.replace("Stage4.33 Standard RetroArch input settings active",
                  "Stage4.34 RetroArch controller profile wizard active")

required = [
    "Stage4.34 RetroArch controller profile wizard active",
    "standard RetroArch controller profile saved",
    r'input_driver = \"linuxraw\"',
    r'input_device = \"',
    "D-PAD UP",
    "A  BOTTOM",
    "Y  TOP",
    "MENU / PAUSE",
    "input_menu_toggle_btn",
]
for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.34 marker: " + marker)

out_path.write_text(src, encoding="utf-8")
print("STAGE434_CONTROLLER_WIZARD_PATCH_OK")
