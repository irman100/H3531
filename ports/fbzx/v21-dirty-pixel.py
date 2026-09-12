from pathlib import Path

p = Path("src/llscreen.cpp")
t = p.read_text()

inc = '#include "osd.hh"\n'
if t.count(inc) != 1:
    raise SystemExit("v21: llscreen include marker missing")
t = t.replace(
    inc,
    inc + '\nextern "C" void H3531_FBZX_MarkDirtyAddress(const void *address);\n',
    1,
)

old = '''\tcase 3:\n\t\t*(address++)=*(colour++);\n\tcase 2:\n\t\t*(address++)=*(colour++);\n\t\t*(address++)=*(colour++);\n\tbreak;'''
new = '''\tcase 3:\n\t\t*(address++)=*(colour++);\n\t\t*(address++)=*(colour++);\n\t\t*(address++)=*(colour++);\n\tbreak;\n\tcase 2: {\n\t\tUint16 value16 = *((Uint16 *)colour);\n\t\tUint16 *dst16 = (Uint16 *)address;\n\t\tif (*dst16 != value16) {\n\t\t\t*dst16 = value16;\n\t\t\tH3531_FBZX_MarkDirtyAddress(address);\n\t\t}\n\tbreak;\n\t}'''
if t.count(old) != 1:
    raise SystemExit("v21: little-endian paint_one_pixel marker missing")
t = t.replace(old, new, 1)

p.write_text(t)
print("H3531 v21: changed-pixel dirty-row marking enabled")
