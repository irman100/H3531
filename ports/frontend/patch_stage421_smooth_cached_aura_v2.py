#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura.py')
code = original.read_text(encoding='utf-8')

# The first Stage4.21 generator encoded some multiline C++ strings as escaped
# newlines. Normalize both possible spellings before executing the generator.
names = [
    'old_quant', 'new_quant', 'struct_anchor',
    'old_system_aura', 'new_system_aura', 'new_present'
]
for name in names:
    pattern = re.compile(rf"({name}\s*=\s*''')(.*?)(''')", re.S)
    def repl(m):
        body = m.group(2)
        body = body.replace('\\\\n', '\n')
        body = body.replace('\\n', '\n')
        return m.group(1) + body + m.group(3)
    code, count = pattern.subn(repl, code, count=1)
    if count != 1:
        raise SystemExit(f'cannot normalize assignment: {name}')

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
