#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_final_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura_final.py')
code = original.read_text(encoding='utf-8')

old = "src = src.replace(struct_anchor, struct_anchor + helpers, 1)"
new = r'''system_helper_anchor = 'static void stage42_draw_system_row('
if system_helper_anchor not in src:
    raise SystemExit('system renderer anchor missing for Stage4.21 helpers')
src = src.replace(system_helper_anchor, helpers + '\n\n' + system_helper_anchor, 1)'''
if old not in code:
    raise SystemExit('Stage4.21 helper insertion statement missing')
code = code.replace(old, new, 1)

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
