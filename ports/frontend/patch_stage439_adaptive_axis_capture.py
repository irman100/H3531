#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage439_adaptive_axis_capture.py INPUT OUTPUT")

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

helpers = r'''
static const int STAGE439_AXIS_CAPTURE_THRESHOLD = 5500;
static const int STAGE439_AXIS_STRONG_THRESHOLD = 9000;

static const char *stage439_abs_name(unsigned code)
{
   switch (code)
   {
      case ABS_X: return "ABS_X";
      case ABS_Y: return "ABS_Y";
      case ABS_Z: return "ABS_Z";
      case ABS_RX: return "ABS_RX";
      case ABS_RY: return "ABS_RY";
      case ABS_RZ: return "ABS_RZ";
      case ABS_THROTTLE: return "ABS_THROTTLE";
      case ABS_RUDDER: return "ABS_RUDDER";
      case ABS_WHEEL: return "ABS_WHEEL";
      case ABS_GAS: return "ABS_GAS";
      case ABS_BRAKE: return "ABS_BRAKE";
      case ABS_HAT0X: return "ABS_HAT0X";
      case ABS_HAT0Y: return "ABS_HAT0Y";
      case ABS_HAT1X: return "ABS_HAT1X";
      case ABS_HAT1Y: return "ABS_HAT1Y";
      default: return "ABS_OTHER";
   }
}

static void stage439_log_axis_event(const GamepadInput &pad,
      unsigned code, unsigned mapped, int16_t value,
      int baseline, int delta)
{
   fprintf(stderr,
      "[GAMEFRONT] axis-capture path=%s code=%u(%s) mapped=%u value=%d baseline=%d delta=%d\n",
      pad.path.c_str(), code, stage439_abs_name(code), mapped,
      (int)value, baseline, delta);
}
'''

anchor = "static void stage438_draw_stick(Fb &fb, int cx, int cy,"
pos = src.find(anchor)
if pos < 0:
    raise SystemExit("Stage4.39 helper insertion anchor missing")
src = src[:pos] + helpers + "\n" + src[pos:]

capture = r'''static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,
      Input &in, GamepadInput &pad, int step, Stage434CapturedBind &out)
{
   stage434_draw(back.fb, pad, step, "");
   stage42_present(physical, back);

   stage434_drain_pad(pad);
   if (!stage434_wait_buttons_released(pad, in))
      return false;

   int16_t axis_baseline[H3531_JS_MAX_AXES]{};
   int axis_peak[H3531_JS_MAX_AXES]{};
   int axis_peak_sign[H3531_JS_MAX_AXES]{};
   int axis_seen[H3531_JS_MAX_AXES]{};

   for (int i = 0; i < H3531_JS_MAX_AXES; ++i)
   {
      axis_baseline[i] = pad.axes[i];
      axis_peak[i] = 0;
      axis_peak_sign[i] = 0;
      axis_seen[i] = 0;
   }

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
               const int magnitude = std::abs(delta);

               if (magnitude > axis_peak[idx])
               {
                  axis_peak[idx] = magnitude;
                  axis_peak_sign[idx] = delta < 0 ? -1 : 1;
               }

               if (magnitude >= 1800)
                  ++axis_seen[idx];

               stage439_log_axis_event(pad, ev.code, idx, value,
                     (int)axis_baseline[idx], delta);

               /* Cheap HID pads often advertise a much wider ABS range than
                * they actually emit. A fixed 16000 threshold therefore misses
                * right sticks on some devices. Accept a clearly moving axis
                * at ~17% full scale, or a smaller movement after repeated
                * events confirm it is intentional rather than noise. */
               const bool strong =
                  axis_peak[idx] >= STAGE439_AXIS_STRONG_THRESHOLD;
               const bool confirmed =
                  axis_peak[idx] >= STAGE439_AXIS_CAPTURE_THRESHOLD &&
                  axis_seen[idx] >= 2;

               if (strong || confirmed)
               {
                  out.button = -1;
                  out.axis = (int)idx;

                  /* Use the resulting normalized sign when possible. If the
                   * device never crosses zero, fall back to movement direction. */
                  if (value <= -2500)
                     out.axis_dir = -1;
                  else if (value >= 2500)
                     out.axis_dir = 1;
                  else
                     out.axis_dir = axis_peak_sign[idx] < 0 ? -1 : 1;

                  fprintf(stderr,
                     "[GAMEFRONT] axis-capture ACCEPT mapped=%u peak=%d sign=%d step=%s\n",
                     idx, axis_peak[idx], out.axis_dir,
                     stage434_steps[step].label);
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

src = replace_function(src,
    "static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,",
    capture)

src = src.replace(
    "Stage4.38 Recalbox visual full analog controller setup active",
    "Stage4.39 Adaptive analog axis capture active")

src = src.replace(
    "STAGE438_VISUAL controller-diagram optional-hold-to-skip standard-retroarch-axis-pairs",
    "STAGE439_AXIS adaptive-threshold raw-ABS-telemetry right-stick-compatible")

required = [
    "Stage4.39 Adaptive analog axis capture active",
    "STAGE439_AXIS_CAPTURE_THRESHOLD = 5500",
    "STAGE439_AXIS_STRONG_THRESHOLD = 9000",
    "axis-capture path=%s code=%u(%s)",
    "axis-capture ACCEPT mapped=%u peak=%d sign=%d step=%s",
    "ABS_RX",
    "ABS_RY",
    "ABS_RZ",
    "STAGE439_AXIS adaptive-threshold raw-ABS-telemetry right-stick-compatible",
]
for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.39 marker: " + marker)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("STAGE439_ADAPTIVE_AXIS_CAPTURE_PATCH_OK")
