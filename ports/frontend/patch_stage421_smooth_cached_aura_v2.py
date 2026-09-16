#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura.py')
code = original.read_text(encoding='utf-8')

# Stage4.8 already owns frame pacing and the 8-step focus quantizer. Remove the
# redundant quantizer replacement from the first Stage4.21 generator entirely.
code, count = re.subn(
    r"old_quant\s*=\s*'''.*?# Late cartridge-media patches had regressed to the uncached cover scaler\.",
    '# Late cartridge-media patches had regressed to the uncached cover scaler.',
    code,
    count=1,
    flags=re.S,
)
if count != 1:
    raise SystemExit('cannot remove redundant quantizer block')

# Normalize multiline C++ anchors that remain in the first generator.
names = ['struct_anchor', 'old_system_aura', 'new_system_aura', 'new_present']
for name in names:
    pattern = re.compile(rf"({name}\s*=\s*''')(.*?)(''')", re.S)
    def repl(m):
        body = m.group(2)
        body = body.replace('\\\\n', '\n')
        body = body.replace('\\n', '\n')
        return m.group(1) + body + m.group(3)
    code, n = pattern.subn(repl, code, count=1)
    if n != 1:
        raise SystemExit(f'cannot normalize assignment: {name}')

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
