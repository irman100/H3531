#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura.py')
code = original.read_text(encoding='utf-8')

# Normalize multiline C++ anchors from the first generator.
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

# Stage4.8 already installed the eight-step focus quantizer. Stage4.21 may be
# applied to either an older source or the current Stage4.20 chain, so accept
# the desired implementation when it is already present.
old_logic = """if old_quant not in src:\n    raise SystemExit('Stage4.20 focus quantizer not found')\nsrc = src.replace(old_quant, new_quant, 1)"""
new_logic = """if old_quant in src:\n    src = src.replace(old_quant, new_quant, 1)\nelif new_quant not in src:\n    raise SystemExit('focus quantizer missing or unexpected')"""
if old_logic not in code:
    raise SystemExit('cannot patch quantizer compatibility logic')
code = code.replace(old_logic, new_logic, 1)

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
