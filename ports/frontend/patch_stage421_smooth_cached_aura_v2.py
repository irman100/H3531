#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura.py')
code = original.read_text(encoding='utf-8')

# The first Stage4.21 generator accidentally encoded a handful of multiline
# C++ anchor/replacement strings with literal "\\n" sequences. Convert only
# those named triple-quoted assignments to real newlines before executing it.
names = [
    'old_quant', 'new_quant', 'struct_anchor',
    'old_system_aura', 'new_system_aura', 'new_present'
]
for name in names:
    pattern = re.compile(rf"({name}\s*=\s*''')(.*?)(''')", re.S)
    def repl(m):
        return m.group(1) + m.group(2).replace('\\n', '\n') + m.group(3)
    code, count = pattern.subn(repl, code, count=1)
    if count != 1:
        raise SystemExit(f'cannot normalize assignment: {name}')

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
