from pathlib import Path

p = Path("src/cargador.cpp")
s = p.read_text()

old = '''\tcase 1: // 128k\n\t\tprintf("Mode 128K\\n");\n\t\tordenador->mode128k = 2; // +2 mode\n\t\tordenador->issue = 3;\n\t\tResetComputer();'''
new = '''\tcase 1: // 128k\n\t\tprintf("Mode 128K\\n");\n\t\tprintf("H3531 model: classic 128K ROM set\\n");\n\t\tordenador->mode128k = 1; // classic 128K mode\n\t\tordenador->issue = 3;\n\t\tResetComputer();'''

if s.count(old) != 1:
    raise SystemExit(f"v22: 128K mode marker count={s.count(old)}")

p.write_text(s.replace(old, new, 1))
print("H3531 v22: 128K snapshots mapped to classic Spectrum 128K mode")
