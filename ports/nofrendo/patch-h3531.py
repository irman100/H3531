from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
port_dir = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parent)

# Replace the RV1103 RGB565 fb backend with the Hi3531 A1R5G5B5 backend.
(root / "platform" / "fb.c").write_text((port_dir / "fb-h3531.c").read_text())

# Hi3531 HIFB is A1R5G5B5, not RGB565.
p = root / "platform" / "osd_linux.c"
s = p.read_text()
old = "c=(pal[i].b>>3)+((pal[i].g>>2)<<5)+((pal[i].r>>3)<<11);"
new = "c=(uint16)(0x8000U | ((pal[i].r>>3)<<10) | ((pal[i].g>>3)<<5) | (pal[i].b>>3));"
if s.count(old) != 1:
    raise SystemExit(f"palette marker count={s.count(old)}")
s = s.replace(old, new, 1)
s = s.replace("#define SCREEN_WIDTH 320", "#define SCREEN_WIDTH 1280", 1)
s = s.replace("#define SCREEN_HEIGHT 240", "#define SCREEN_HEIGHT 720", 1)
p.write_text(s)

# Allow a deterministic evdev device on the DVR board. If unset, retain the
# upstream scan of /dev/input/event*.
p = root / "platform" / "input.c"
s = p.read_text()
needle = "static int find_keyboard_device(void) {\n    DIR *dir;"
insert = '''static int find_keyboard_device(void) {\n    const char *forced = getenv("NOFRENDO_INPUT");\n    if (forced && forced[0]) {\n        int fd = open(forced, O_RDONLY | O_NONBLOCK);\n        if (fd >= 0) {\n            printf("H3531 NES input: forced evdev %s\\n", forced);\n            return fd;\n        }\n        fprintf(stderr, "H3531 NES input: cannot open %s\\n", forced);\n    }\n\n    DIR *dir;'''
if s.count(needle) != 1:
    raise SystemExit(f"input marker count={s.count(needle)}")
s = s.replace(needle, insert, 1)
p.write_text(s)

# Give the test build an unmistakable identity.
p = root / "main.c"
s = p.read_text()
needle = "int main(int argc, char *argv[])\n{\n"
insert = 'int main(int argc, char *argv[])\n{\n    printf("H3531 Nofrendo v1: framebuffer A1R5G5B5 integer scaler + evdev\\n");\n'
if s.count(needle) != 1:
    raise SystemExit(f"main marker count={s.count(needle)}")
s = s.replace(needle, insert, 1)
p.write_text(s)

print("H3531 Nofrendo patch applied")
