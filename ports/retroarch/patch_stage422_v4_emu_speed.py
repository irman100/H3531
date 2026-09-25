#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage422_v4_emu_speed.py INPUT OUTPUT')

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding='utf-8')

old = '''   if (current_core->retro_run)\n   {\n      current_core->retro_run();\n      audio_driver_frame_end();\n   }\n'''
if old not in src:
    raise SystemExit('retro_run anchor missing')

new = '''   if (current_core->retro_run)\n   {\n      static uint64_t h3531_emu_runs = 0;\n      static uint64_t h3531_emu_core_us = 0;\n      static retro_time_t h3531_emu_window_start = 0;\n      retro_time_t h3531_run_start = cpu_features_get_time_usec();\n      retro_time_t h3531_run_end;\n\n      current_core->retro_run();\n      h3531_run_end = cpu_features_get_time_usec();\n      audio_driver_frame_end();\n\n      if (!h3531_emu_window_start)\n         h3531_emu_window_start = h3531_run_start;\n\n      h3531_emu_core_us += (uint64_t)(h3531_run_end - h3531_run_start);\n      ++h3531_emu_runs;\n\n      if (h3531_emu_runs >= 300U)\n      {\n         retro_time_t h3531_now = cpu_features_get_time_usec();\n         retro_time_t h3531_elapsed = h3531_now - h3531_emu_window_start;\n         double h3531_target_hz = video_state_get_ptr()->av_info.timing.fps;\n         double h3531_core_hz = h3531_elapsed > 0\n               ? ((double)h3531_emu_runs * 1000000.0 / (double)h3531_elapsed)\n               : 0.0;\n         double h3531_speed_pct = h3531_target_hz > 0.0\n               ? (h3531_core_hz * 100.0 / h3531_target_hz)\n               : 0.0;\n         double h3531_core_avg_ms = h3531_emu_runs > 0\n               ? ((double)h3531_emu_core_us / (double)h3531_emu_runs / 1000.0)\n               : 0.0;\n\n         RARCH_LOG("[H3531] EMU runs=%llu core_hz=%.2f target_hz=%.2f speed=%.1f%% core_avg=%.3fms\\n",\n               (unsigned long long)h3531_emu_runs,\n               h3531_core_hz, h3531_target_hz, h3531_speed_pct,\n               h3531_core_avg_ms);\n\n         h3531_emu_runs = 0;\n         h3531_emu_core_us = 0;\n         h3531_emu_window_start = h3531_now;\n      }\n   }\n'''

src = src.replace(old, new, 1)
out_path.write_text(src, encoding='utf-8')
print(f'STAGE422_V4_EMU_SPEED_PATCH_OK {src_path} -> {out_path}')
