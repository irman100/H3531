#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage44_normalize_dispatcher.py INPUT OUTPUT')
s = Path(sys.argv[1]).read_text(encoding='utf-8')
old = 'static void stage43_draw_system_icon(Fb &fb, const SystemDef &s, int cx, int cy,\n      float scale, uint16_t body, uint16_t ink, uint16_t accent)'
new = 'static void stage43_draw_system_icon(Fb &fb, const SystemDef &s, int cx, int cy, float scale,\n      uint16_t body, uint16_t ink, uint16_t accent)'
if old not in s:
    raise SystemExit('dispatcher signature not found')
s = s.replace(old, new, 1)
Path(sys.argv[2]).write_text(s, encoding='utf-8')
print('STAGE44_DISPATCHER_NORMALIZED')
