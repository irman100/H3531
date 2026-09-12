from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Unmistakable hardware-test identity.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v5: right-edge crop + AO5 recovery + persistent video + exact 60Hz + A1R5G5B5"
new = "H3531 Nofrendo v6: proven AO frame + shadow/vsync video + right-edge crop + exact 60Hz"
if s.count(old) != 1:
    raise SystemExit(f"v5 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

# The Linux port's emulation loop busy-spins between timer ticks.  On this
# dual-core Linux 3.0 target that needlessly competes with the timer/audio
# threads.  Yield only when there is genuinely no NES frame work pending, and
# report a few multi-tick bursts so real-hardware tests can distinguish timer
# jitter / frameskip pressure from framebuffer tearing.
p = root / "core" / "nes" / "nes.c"
s = p.read_text()

include_marker = "#include <stdlib.h>\n"
if s.count(include_marker) != 1:
    raise SystemExit(f"stdlib include marker count={s.count(include_marker)}")
s = s.replace(include_marker, include_marker + "#include <sched.h>\n", 1)

start_marker = '''void nes_emulate(void)\n{\n   int last_ticks, frames_to_render;\n'''
start_replacement = '''void nes_emulate(void)\n{\n   int last_ticks, frames_to_render;\n   static unsigned h3531_tick_burst_logs = 0;\n'''
if s.count(start_marker) != 1:
    raise SystemExit(f"nes_emulate start marker count={s.count(start_marker)}")
s = s.replace(start_marker, start_replacement, 1)

old_tick = '''      if (nofrendo_ticks != last_ticks)\n      {\n         int tick_diff = nofrendo_ticks - last_ticks;\n\n         frames_to_render += tick_diff;\n         gui_tick(tick_diff);\n         last_ticks = nofrendo_ticks;\n      }\n\n      if (true == nes.pause)\n'''
new_tick = '''      if (nofrendo_ticks != last_ticks)\n      {\n         int tick_diff = nofrendo_ticks - last_ticks;\n\n         if (tick_diff > 1 && h3531_tick_burst_logs < 12)\n         {\n            fprintf(stderr,\n                    "H3531 NES timing v6: tick burst=%d (frameskip pressure)\\n",\n                    tick_diff);\n            h3531_tick_burst_logs++;\n         }\n\n         frames_to_render += tick_diff;\n         gui_tick(tick_diff);\n         last_ticks = nofrendo_ticks;\n      }\n\n      if (nofrendo_ticks == last_ticks && frames_to_render == 0)\n      {\n         sched_yield();\n         continue;\n      }\n\n      if (true == nes.pause)\n'''
if s.count(old_tick) != 1:
    raise SystemExit(f"NES tick-loop marker count={s.count(old_tick)}")
s = s.replace(old_tick, new_tick, 1)
p.write_text(s)

print("H3531 Nofrendo v6 audio/sync patch applied")
