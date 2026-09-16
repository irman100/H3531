#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage421_smooth_cached_aura_v2.py INPUT OUTPUT')

original = Path(__file__).with_name('patch_stage421_smooth_cached_aura.py')
code = original.read_text(encoding='utf-8')

# Stage4.13 owns the final focus function, so Stage4.21 leaves it untouched.
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

# The final Stage4.20 chain is based on the Stage4.8 system-row geometry plus
# Stage4.13 vertical offsets. Retarget the optimization anchors to those actual
# values rather than the older Stage4.7 values used in the first draft.
code = code.replace('const int by = selected ? y + 10 : y + 4;',
                    'const int by = selected ? y + 9 : y + 4;')
code = code.replace('const int bh = selected ? 91 : 82;',
                    'const int bh = selected ? 88 : 80;')
code = code.replace("'const int content_h = selected ? 142 : 116;'",
                    "'const int content_h = selected ? 138 : 114;'")
code = code.replace("'const int y = selected ? 99 : 111;'",
                    "'const int y = selected ? 66 : 82;'")

sys.argv = [str(original), sys.argv[1], sys.argv[2]]
ns = {'__name__': '__main__', '__file__': str(original)}
exec(compile(code, str(original), 'exec'), ns, ns)
