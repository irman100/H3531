#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage442_axis_probe.py INPUT OUTPUT')

src = Path(sys.argv[1]).read_text(encoding='utf-8')

helper = r'''
static void stage442_probe_other_gamepad_axes(Input &in,
      GamepadInput &selected, int step)
{
   /* Diagnostic only: do not bind, group, or rewrite another event node.
    * Twin/dual USB adapters may expose two physical controller ports with
    * identical names, so same-name nodes must not be merged blindly. */
   for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
   {
      GamepadInput &other = in.pads[p];
      if (other.fd < 0 || &other == &selected)
         continue;

      for (;;)
      {
         input_event ev{};
         const ssize_t n = read(other.fd, &ev, sizeof(ev));
         if (n != (ssize_t)sizeof(ev))
            break;

         if (ev.type != EV_ABS || ev.code > ABS_MAX)
            continue;

         const unsigned idx = other.abs_to_axis[ev.code];
         if (idx >= H3531_JS_MAX_AXES)
         {
            fprintf(stderr,
               "[GAMEFRONT] axis-probe-other path=%s device=%s code=%u(%s) mapped=none raw=%d step=%s\n",
               other.path.c_str(), other.name.c_str(), ev.code,
               stage439_abs_name(ev.code), ev.value,
               stage434_steps[step].label);
            continue;
         }

         const int16_t value =
            h3531_front_scale_abs(other.absinfo[idx], ev.value);
         fprintf(stderr,
            "[GAMEFRONT] axis-probe-other path=%s device=%s code=%u(%s) mapped=%u value=%d raw=%d step=%s\n",
            other.path.c_str(), other.name.c_str(), ev.code,
            stage439_abs_name(ev.code), idx, (int)value, ev.value,
            stage434_steps[step].label);
      }
   }
}
'''

anchor = 'static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,'
pos = src.find(anchor)
if pos < 0:
    raise SystemExit('Stage4.42 capture anchor missing')
src = src[:pos] + helper + '\n' + src[pos:]

needle = '''      if (stage434_keyboard_cancel(in))\n         return false;\n\n      if (optional && held_button >= 0 && held_since &&'''
repl = '''      if (stage434_keyboard_cancel(in))\n         return false;\n\n      /* On analog-wizard steps, observe every other gamepad-capable event\n       * node as telemetry only. This proves whether an otherwise invisible\n       * right stick is reported on another node without unsafe auto-grouping. */\n      if (stage434_steps[step].allow_axis)\n         stage442_probe_other_gamepad_axes(in, pad, step);\n\n      if (optional && held_button >= 0 && held_since &&'''
if needle not in src:
    raise SystemExit('Stage4.42 loop insertion anchor missing')
src = src.replace(needle, repl, 1)

src = src.replace(
    'Stage4.41 Favorites Home Search and Controller Navigation active',
    'Stage4.42 Home UX + Cross-node Analog Probe active', 1)

layout = '   printf("LAYOUT_TEST_OK\\n");\n'
marker = '   printf("STAGE442_AXIS_PROBE all-gamepad-event-nodes diagnostic-only no-cross-device-autobind\\n");\n'
if layout not in src:
    raise SystemExit('Stage4.42 layout marker anchor missing')
src = src.replace(layout, marker + layout, 1)

required = [
    'Stage4.42 Home UX + Cross-node Analog Probe active',
    'axis-probe-other path=%s device=%s',
    'diagnostic-only no-cross-device-autobind',
    'stage442_probe_other_gamepad_axes(in, pad, step)',
]
for m in required:
    if m not in src:
        raise SystemExit('missing Stage4.42 marker: ' + m)

Path(sys.argv[2]).write_text(src, encoding='utf-8')
print('STAGE442_AXIS_PROBE_PATCH_OK')
