#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage445_ps1_runtime_policy.py INPUT OUTPUT')

src = Path(sys.argv[1]).read_text(encoding='utf-8')

old_sig = 'static void stage413_write_ra_cfg()'
new_sig = 'static void stage413_write_ra_cfg(const SystemDef &sys)'
if old_sig not in src:
    raise SystemExit('stage413_write_ra_cfg signature anchor missing')
src = src.replace(old_sig, new_sig, 1)

old_gran = '   out << "rewind_granularity = \\"1\\"\\n";'
new_gran = '''   const bool stage445_heavy_rewind = lower(sys.name) == "ps1";
   const int stage445_granularity = stage445_heavy_rewind ? 60 : 1;
   out << "rewind_granularity = \\"" << stage445_granularity << "\\"\\n";
   fprintf(stderr,
         "[GAMEFRONT] RUNTIME-POLICY system=%s rewind=%s granularity=%d\\n",
         sys.name.c_str(), stage413_settings.rewind ? "on" : "off",
         stage445_granularity);'''
if old_gran not in src:
    raise SystemExit('rewind granularity anchor missing')
src = src.replace(old_gran, new_gran, 1)

old_call = '   stage413_write_ra_cfg();'
new_call = '   stage413_write_ra_cfg(sys);'
if old_call not in src:
    raise SystemExit('stage413_write_ra_cfg call anchor missing')
src = src.replace(old_call, new_call, 1)

layout = '   printf("LAYOUT_TEST_OK\\n");\n'
marker = '   printf("STAGE445_PS1_RUNTIME_POLICY rewind-granularity60 versioned-savestate-compatible\\n");\n'
if layout not in src:
    raise SystemExit('layout marker anchor missing')
src = src.replace(layout, marker + layout, 1)

required = [
    'stage445_heavy_rewind',
    'stage445_granularity = stage445_heavy_rewind ? 60 : 1',
    'RUNTIME-POLICY system=%s rewind=%s granularity=%d',
    'stage413_write_ra_cfg(sys)',
    'STAGE445_PS1_RUNTIME_POLICY rewind-granularity60 versioned-savestate-compatible',
]
for marker in required:
    if marker not in src:
        raise SystemExit('missing Stage4.45 marker: ' + marker)

Path(sys.argv[2]).write_text(src, encoding='utf-8')
print('STAGE445_PS1_RUNTIME_POLICY_PATCH_OK')
