#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage42_stable_system_order.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.2 centered overflow carousel active'
new_marker = 'Stage4.2 stable-order centered carousel active'
if old_marker not in src:
    raise SystemExit("expected Stage4.2 marker not found")
src = src.replace(old_marker, new_marker, 1)

# Systems are deliberately NOT circular. Their source order is canonical:
# indices below the selected item stay on the left, indices above stay on
# the right. During a transition only positions interpolate toward/from
# centre; identities never wrap through the opposite screen edge.
pattern = re.compile(
    r'''   std::map<size_t, Stage42SystemVisual> unique;\n'''
    r'''   for \(int rel = -4; rel <= 4; \+\+rel\)\n'''
    r'''   \{\n'''
    r'''.*?'''
    r'''   std::vector<Stage42SystemVisual> items;\n'''
    r'''   for \(auto &kv : unique\) items\.push_back\(kv\.second\);\n''',
    re.S,
)
replacement = '''   std::vector<Stage42SystemVisual> items;\n   for (size_t pos = 0; pos < vis.size(); ++pos)\n   {\n      const float effective = (float)((int)pos - (int)visible_pos) + system_shift;\n      if (std::fabs(effective) > 3.20f) continue;\n      Stage42SystemVisual sv;\n      sv.pos = pos;\n      sv.rel = effective;\n      sv.focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));\n      items.push_back(sv);\n   }\n'''
src, n = pattern.subn(replacement, src, count=1)
if n != 1:
    raise SystemExit(f"system carousel block replacement count={n}, expected 1")

old_prev = '''            else if (!vis.empty() && !system_active)\n            {\n               visible_pos = visible_pos ? visible_pos - 1 : vis.size() - 1;\n               game_pos = 0;\n               system_anim.dir = -1;\n               system_anim.start = stage42_now_ms();\n               redraw = true;\n            }'''
new_prev = '''            else if (!vis.empty() && !system_active && visible_pos > 0)\n            {\n               --visible_pos;\n               game_pos = 0;\n               system_anim.dir = -1;\n               system_anim.start = stage42_now_ms();\n               redraw = true;\n            }'''
if old_prev not in src:
    raise SystemExit("previous-system wrap block not found")
src = src.replace(old_prev, new_prev, 1)

old_next = '''            else if (!vis.empty() && !system_active)\n            {\n               visible_pos = (visible_pos + 1) % vis.size();\n               game_pos = 0;\n               system_anim.dir = 1;\n               system_anim.start = stage42_now_ms();\n               redraw = true;\n            }'''
new_next = '''            else if (!vis.empty() && !system_active && visible_pos + 1 < vis.size())\n            {\n               ++visible_pos;\n               game_pos = 0;\n               system_anim.dir = 1;\n               system_anim.start = stage42_now_ms();\n               redraw = true;\n            }'''
if old_next not in src:
    raise SystemExit("next-system wrap block not found")
src = src.replace(old_next, new_next, 1)

old_test = '   printf("CAROUSEL selected-centered overflow-items=1-per-side\\n");\n'
new_test = (
    '   printf("CAROUSEL selected-centered overflow-items=1-per-side\\n");\n'
    '   printf("SYSTEM_ORDER stable-linear no-wrap\\n");\n'
    '   printf("SYSTEM_TWO NES-left SEGA-right identities-never-cross\\n");\n'
)
if old_test not in src:
    raise SystemExit("layout-test carousel marker not found")
src = src.replace(old_test, new_test, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STABLE_ORDER_PATCH_OK {src_path} -> {out_path}")
