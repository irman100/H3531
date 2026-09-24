#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage435_event_wizard.py INPUT OUTPUT")

src = Path(sys.argv[1]).read_text(encoding="utf-8")

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        raise SystemExit("function not found: " + signature)
    brace = text.find("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + replacement.rstrip() + text[i+1:]
    raise SystemExit("unterminated function: " + signature)

src = replace_function(src, "static void stage434_drain_pad(GamepadInput &pad)", r'''static void stage434_drain_pad(GamepadInput &pad)
{
   if (pad.fd < 0) return;

   for (;;)
   {
      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n != (ssize_t)sizeof(ev)) break;

      if (ev.type == EV_KEY && ev.code <= KEY_MAX)
      {
         const unsigned idx = pad.key_to_button[ev.code];
         if (idx < H3531_JS_MAX_BUTTONS)
            pad.buttons[idx] = ev.value != 0;
      }
      else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
      {
         const unsigned idx = pad.abs_to_axis[ev.code];
         if (idx < H3531_JS_MAX_AXES)
            pad.axes[idx] =
               h3531_front_scale_abs(pad.absinfo[idx], ev.value);
      }
   }
}''')

src = replace_function(src, "static bool stage434_wait_buttons_released(GamepadInput &pad, Input &in)", r'''static bool stage434_wait_buttons_released(GamepadInput &pad, Input &in)
{
   const uint64_t deadline = input_now_ms() + 2500ULL;

   while (input_now_ms() < deadline)
   {
      if (stage434_keyboard_cancel(in)) return false;

      bool released = true;
      for (int i = 0; i < H3531_JS_MAX_BUTTONS; ++i)
         if (pad.buttons[i]) released = false;

      if (released) return true;

      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.code <= KEY_MAX)
         {
            const unsigned idx = pad.key_to_button[ev.code];
            if (idx < H3531_JS_MAX_BUTTONS)
               pad.buttons[idx] = ev.value != 0;
         }
         else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
         {
            const unsigned idx = pad.abs_to_axis[ev.code];
            if (idx < H3531_JS_MAX_AXES)
               pad.axes[idx] =
                  h3531_front_scale_abs(pad.absinfo[idx], ev.value);
         }
      }
      else
         usleep(5000);
   }
   return true;
}''')

src = replace_function(src, "static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,", r'''static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,
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
         else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
         {
            const unsigned idx = pad.abs_to_axis[ev.code];
            if (idx < H3531_JS_MAX_AXES)
            {
               const int16_t value =
                  h3531_front_scale_abs(pad.absinfo[idx], ev.value);
               pad.axes[idx] = value;
               const int delta = (int)value - (int)axis_baseline[idx];

               if (stage434_steps[step].direction &&
                   std::abs(delta) > 16000)
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
}''')

src = src.replace("NO /DEV/INPUT/JS CONTROLLER FOUND",
                  "NO GAMEPAD EVENT DEVICE FOUND")
src = src.replace("STANDARD RETROARCH LINUXRAW PROFILE",
                  "RETROARCH LINUXRAW PROFILE / EVENT FALLBACK")
src = src.replace("Stage4.34 RetroArch controller profile wizard active",
                  "Stage4.35 RetroArch event-fallback controller wizard active")

for marker in [
    "Stage4.35 RetroArch event-fallback controller wizard active",
    "NO GAMEPAD EVENT DEVICE FOUND",
    "RETROARCH LINUXRAW PROFILE / EVENT FALLBACK",
    "standard RetroArch controller profile saved",
]:
    if marker not in src:
        raise SystemExit("missing Stage4.35 wizard marker: " + marker)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("STAGE435_EVENT_WIZARD_PATCH_OK")
