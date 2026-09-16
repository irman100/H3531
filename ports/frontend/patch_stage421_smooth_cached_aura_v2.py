#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura.py')
code = original.read_text(encoding='utf-8')

# Stage4.13 owns the final continuous focus function; leave it untouched.
code, count = re.subn(
    r"old_quant\s*=\s*'''.*?# Late cartridge-media patches had regressed to the uncached cover scaler\.",
    '# Late cartridge-media patches had regressed to the uncached cover scaler.',
    code, count=1, flags=re.S)
if count != 1:
    raise SystemExit('cannot remove redundant quantizer block')

# Normalize multiline C++ anchors in the first generator.
names = ['struct_anchor', 'old_system_aura', 'new_system_aura', 'new_present']
for name in names:
    pattern = re.compile(rf"({name}\s*=\s*''')(.*?)(''')", re.S)
    def repl(m):
        body = m.group(2).replace('\\\\n', '\n').replace('\\n', '\n')
        return m.group(1) + body + m.group(3)
    code, n = pattern.subn(repl, code, count=1)
    if n != 1:
        raise SystemExit(f'cannot normalize assignment: {name}')

# Retarget final Stage4.20 geometry where it is still referenced elsewhere.
code = code.replace("'const int content_h = selected ? 142 : 116;'",
                    "'const int content_h = selected ? 138 : 114;'")
code = code.replace("'const int y = selected ? 99 : 111;'",
                    "'const int y = selected ? 66 : 82;'")
code = code.replace('focus-steps=8', 'continuous-focus')

# Replace the fragile exact old-system block check inside the generator with a
# structural replacement. Preserve new_system_aura itself, but identify the
# target block by its stable Stage4.20 aura start and fallback boundary.
pattern = re.compile(
    r"old_system_aura\s*=\s*'''.*?'''\s*\n"
    r"(new_system_aura\s*=\s*'''.*?''')\s*\n"
    r"if old_system_aura not in src:\s*\n"
    r"\s*raise SystemExit\('Stage4\.20 system aura/draw block missing'\)\s*\n"
    r"src = src\.replace\(old_system_aura, new_system_aura, 1\)",
    re.S)
replacement = r'''\1
system_a = src.find('      if (selected && focus == FocusZone::Systems)')
system_b = src.find('      if (!drawn)', system_a + 1)
if system_a < 0 or system_b < 0:
    raise SystemExit('Stage4.20 system aura structural anchors missing')
src = src[:system_a] + new_system_aura.rstrip() + '\n' + src[system_b:]'''
code, n = pattern.subn(replacement, code, count=1)
if n != 1:
    raise SystemExit('cannot install structural system-aura replacement')

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
