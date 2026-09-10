#!/usr/bin/env bash
set -euo pipefail

# FBZX 3.1.0 tag resolves to this exact upstream revision. Pin the commit so
# our H3531 build cannot silently change if the upstream tag ever moves.
FBZX_REF="${FBZX_REF:-981d48272e1cd04ce258e1060fd9574dd6bb4a60}"
FBZX_URL="${FBZX_URL:-https://gitlab.com/rastersoft/fbzx.git}"
WORK="${WORK:-/tmp/fbzx-h3531}"
SRC="$WORK/src"
OUT="${OUT:-$PWD/out-fbzx}"
SDL_CONFIG="${SDL_CONFIG:-/tmp/sdl-h3531/bin/sdl-config}"
CC="${CC:-arm-linux-musleabi-gcc}"
CXX="${CXX:-arm-linux-musleabi-g++}"
AR="${AR:-arm-linux-musleabi-ar}"
RANLIB="${RANLIB:-arm-linux-musleabi-ranlib}"
STRIP="${STRIP:-arm-linux-musleabi-strip}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DIAG_SRC="$SCRIPT_DIR/h3531-sigill-diag.c"
DIAG_OBJ="$WORK/h3531-sigill-diag.o"
FBZX_PATCH="$SCRIPT_DIR/fbzx-3.1.0-screen-bounds.patch"

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"

if [ ! -x "$SDL_CONFIG" ]; then
  echo "ERROR: SDL_CONFIG not executable: $SDL_CONFIG" >&2
  exit 2
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "ERROR: C++ cross compiler not found: $CXX" >&2
  exit 2
fi
if [ ! -f "$DIAG_SRC" ]; then
  echo "ERROR: fatal-signal diagnostic source missing: $DIAG_SRC" >&2
  exit 2
fi
if [ ! -f "$FBZX_PATCH" ]; then
  echo "ERROR: FBZX screen bounds patch missing: $FBZX_PATCH" >&2
  exit 2
fi

SDL_PREFIX="$(cd "$(dirname "$SDL_CONFIG")/.." && pwd)"
# sdl-config from SDL 1.2 emits -I$prefix/include/SDL, which is correct for
# #include <SDL.h>. FBZX 3.1.0 uses the older #include <SDL/SDL.h> spelling,
# so also expose the parent include directory.
SDL_CFLAGS="$($SDL_CONFIG --cflags) -I$SDL_PREFIX/include"
SDL_STATIC_LIBS="$($SDL_CONFIG --static-libs)"

# The factory Linux 3.0.8 image does not print useful user-space fatal-signal
# register state to dmesg. Link a tiny constructor-based handler so physical
# board tests report PC/registers/stack directly to UART while this port is
# being validated.
"$CC" -c -O2 -g -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE \
  "$DIAG_SRC" -o "$DIAG_OBJ"

echo "== Clone FBZX =="
git clone "$FBZX_URL" "$SRC"
cd "$SRC"
git checkout "$FBZX_REF"
UPSTREAM_SHA="$(git rev-parse HEAD)"
printf '%s\n' "$UPSTREAM_SHA" > "$OUT/FBZX-UPSTREAM-SHA.txt"
printf '%s\n' "$FBZX_REF" > "$OUT/FBZX-UPSTREAM-REF.txt"

echo "== Apply H3531 screen-bounds backport =="
git apply "$FBZX_PATCH"

# FBZX 3.1.0 constructs Screen before processing -fs. Screen caches the
# SDL_Surface pixel pointer. The H3531 SDL backend owns a software logical
# framebuffer and its SetVideoMode replaces that buffer, so FBZX's normal
# fullscreen toggle would leave Screen with a dangling pixel pointer.
# H3531 fbdev is physically fullscreen already: keep the existing surface and
# only mark it fullscreen instead of entering SDL_SetVideoMode a second time.
python3 - <<'PY'
from pathlib import Path

path = Path("src/llscreen.cpp")
text = path.read_text()
marker = "void LLScreen::fullscreen_switch() {\n\n"
if text.count(marker) != 1:
    raise SystemExit("ERROR: expected unique LLScreen::fullscreen_switch marker not found")
insertion = '''\tconst char *video_driver = SDL_getenv("SDL_VIDEODRIVER");

\tif ((video_driver != NULL) && (SDL_strcmp(video_driver,"h3531") == 0)) {
\t\tthis->llscreen->flags |= SDL_FULLSCREEN;
\t\tthis->set_mouse();
\t\treturn;
\t}

'''
path.write_text(text.replace(marker, marker + insertion, 1))
PY

if ! grep -A12 -F 'void LLScreen::fullscreen_switch()' src/llscreen.cpp | grep -F 'SDL_strcmp(video_driver,"h3531")' >/dev/null; then
  echo "ERROR: H3531 fullscreen surface-preservation patch was not applied" >&2
  exit 3
fi

# The file manager execs .APP files without per-application arguments. Make the
# H3531 FBZX binary self-contained: select the H3531 SDL backend if the launch
# environment did not already do so, default to no sound (audio is not ported
# yet), and default to doublescan. An explicit -ss still overrides doublescan.
# Also translate PC cursor keys to the real ZX cursor chords CAPS SHIFT+5/6/7/8.
echo "== Apply H3531 launch defaults and cursor-key backport =="
python3 - <<'PY'
from pathlib import Path

emu = Path("src/emulator.cpp")
text = emu.read_text()
needle = "\tCMDLine parse(argc,argv);\n\n\tosd = new OSD();"
replacement = '''\tCMDLine parse(argc,argv);\n\n\tif (getenv("SDL_VIDEODRIVER") == NULL)\n\t\tsetenv("SDL_VIDEODRIVER","h3531",0);\n\tif (getenv("SDL_FBDEV") == NULL)\n\t\tsetenv("SDL_FBDEV","/dev/fb0",0);\n\n\tosd = new OSD();'''
if text.count(needle) != 1:
    raise SystemExit("ERROR: emulator main marker not found")
text = text.replace(needle, replacement, 1)

needle = "\tenum e_soundtype sound_type = SOUND_AUTOMATIC;\n\tif (parse.nosound) {"
replacement = '''\tenum e_soundtype sound_type = SOUND_AUTOMATIC;\n\tconst char *h3531_video_driver = getenv("SDL_VIDEODRIVER");\n\tif ((h3531_video_driver != NULL) && !strcmp(h3531_video_driver,"h3531"))\n\t\tsound_type = SOUND_NO;\n\tif (parse.nosound) {'''
if text.count(needle) != 1:
    raise SystemExit("ERROR: sound default marker not found")
text = text.replace(needle, replacement, 1)

needle = "\tif (parse.ds) {\n\t\tordenador->dblscan = true;\n\t}\n\tif (parse.ss) {"
replacement = '''\tif ((h3531_video_driver != NULL) && !strcmp(h3531_video_driver,"h3531"))\n\t\tordenador->dblscan = true;\n\tif (parse.ds) {\n\t\tordenador->dblscan = true;\n\t}\n\tif (parse.ss) {'''
if text.count(needle) != 1:
    raise SystemExit("ERROR: doublescan marker not found")
text = text.replace(needle, replacement, 1)
emu.write_text(text)

kbd = Path("src/keyboard.cpp")
text = kbd.read_text()
repls = {
'''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_7;\n\t\tbreak;''': '''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_7;\n\t\t\tthis->k8 = 1; // CAPS SHIFT + 7 = cursor up on ZX Spectrum\n\t\tbreak;''',
'''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_6;\n\t\tbreak;''': '''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_6;\n\t\t\tthis->k8 = 1; // CAPS SHIFT + 6 = cursor down\n\t\tbreak;''',
'''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_8;\n\t\tbreak;''': '''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_8;\n\t\t\tthis->k8 = 1; // CAPS SHIFT + 8 = cursor right\n\t\tbreak;''',
'''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_5;\n\t\tbreak;''': '''\t\tcase 0:\t// cursor\n\t\t\ttemporal_io = SDLK_5;\n\t\t\tthis->k8 = 1; // CAPS SHIFT + 5 = cursor left\n\t\tbreak;''',
}
for old, new in repls.items():
    if text.count(old) != 1:
        raise SystemExit("ERROR: expected unique cursor mapping not found")
    text = text.replace(old, new, 1)
kbd.write_text(text)
PY

if ! grep -F 'setenv("SDL_VIDEODRIVER","h3531",0)' src/emulator.cpp >/dev/null; then
  echo "ERROR: H3531 SDL default was not applied" >&2
  exit 3
fi
if ! grep -F 'ordenador->dblscan = true;' src/emulator.cpp >/dev/null; then
  echo "ERROR: H3531 doublescan default was not applied" >&2
  exit 3
fi
if [ "$(grep -c 'CAPS SHIFT +' src/keyboard.cpp)" -lt 4 ]; then
  echo "ERROR: ZX cursor-key backport was not applied" >&2
  exit 3
fi

echo "== Upstream revision =="
echo "$UPSTREAM_SHA"
echo "== H3531 SDL flags =="
echo "CFLAGS: $SDL_CFLAGS"
echo "LIBS:   $SDL_STATIC_LIBS"

# FBZX 3.1.0 hard-codes native g++ and host pkg-config for SDL, PulseAudio and
# ALSA. Keep the emulator sources untouched except for the explicit H3531
# compatibility adjustments above; patch only the build description to use our
# ARMv7 soft-float compiler and proven static SDL 1.2 H3531 backend.
# GCC 11 defaults to GNU++17, where std::byte conflicts with Z80Free's legacy
# global typedef named byte. FBZX 3.1.0 predates C++17, so pin GNU++14 here.
# No D_SOUND_* macro is enabled in this first port; H3531 defaults to no sound.
MAKEFILE=src/Makefile
if [ ! -f "$MAKEFILE" ]; then
  echo "ERROR: expected $MAKEFILE was not found" >&2
  exit 3
fi

sed -i \
  -e "s#^CC=.*#CC=$CXX -c -O2 -g -std=gnu++14 -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE#" \
  -e "s#^CPP=.*#CPP=$CXX -c -O2 -g -std=gnu++14 -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE#" \
  -e "s#^LN=.*#LN=$CXX -O2 -static -no-pie -march=armv7-a -mfloat-abi=soft#" \
  -e "s#^CFLAGS +=.*#CFLAGS += $SDL_CFLAGS#" \
  -e "s#^CPPFLAGS +=.*#CPPFLAGS += $SDL_CFLAGS#" \
  -e "s#^LDFLAGS +=.*#LDFLAGS += $DIAG_OBJ $SDL_STATIC_LIBS#" \
  "$MAKEFILE"

echo "== Patched FBZX src/Makefile =="
sed -n '1,90p' "$MAKEFILE"

if grep -E 'pkg-config.*(pulse|alsa)|D_SOUND_(PULSE|ALSA|OSS)' "$MAKEFILE"; then
  echo "ERROR: desktop sound dependency leaked into H3531 build" >&2
  exit 4
fi
if grep -E '^(CC|CPP|LN)=g\+\+' "$MAKEFILE"; then
  echo "ERROR: native host g++ leaked into H3531 build" >&2
  exit 4
fi

export PATH="$(dirname "$SDL_CONFIG"):$PATH"

echo "== Build FBZX with static H3531 SDL 1.2 =="
make clean >/dev/null 2>&1 || true
make -j2

BIN=""
for candidate in src/fbzx fbzx src/FBZX FBZX; do
  if [ -f "$candidate" ] && [ -x "$candidate" ]; then
    BIN="$candidate"
    break
  fi
done
if [ -z "$BIN" ]; then
  BIN="$(find . -maxdepth 3 -type f -perm -111 -name 'fbzx*' | head -n 1 || true)"
fi
if [ -z "$BIN" ]; then
  echo "ERROR: build completed but no FBZX executable was found" >&2
  exit 5
fi

# Keep one symbol-rich copy for resolving physical-board fault addresses plus
# the normal stripped .APP used for the test.
cp "$BIN" "$OUT/fbzx-unstripped.APP"
cp "$BIN" "$OUT/fbzx.APP"
"$STRIP" "$OUT/fbzx.APP" || true
cp COPYING "$OUT/FBZX-COPYING.txt" 2>/dev/null || true
cp AMSTRAD "$OUT/FBZX-AMSTRAD.txt" 2>/dev/null || true
cp data/keymap.bmp "$OUT/keymap.bmp" 2>/dev/null || true
cp "$FBZX_PATCH" "$OUT/FBZX-H3531-SCREEN-BOUNDS.patch"

cat > "$OUT/RUN-H3531.txt" <<'EOF'
H3531 FBZX physical validation build
====================================

Normal launch from H3531 Monitor / File Manager requires no arguments:

  ./fbzx.APP

H3531 defaults built into this port:

  SDL_VIDEODRIVER=h3531 (when not already set)
  SDL_FBDEV=/dev/fb0    (when not already set)
  sound disabled
  doublescan enabled

An explicit -ss still disables doublescan. The normal PC arrow keys are mapped
to ZX Spectrum CAPS SHIFT+5/6/7/8 cursor chords.

This build keeps the fatal-signal UART diagnostic enabled during physical
validation, guards every FBZX pixel write against the cached logical surface,
and preserves that logical surface across the H3531 fullscreen toggle.
EOF

file "$OUT/fbzx.APP" | tee "$OUT/FILE.txt"
file "$OUT/fbzx-unstripped.APP" | tee "$OUT/FILE-UNSTRIPPED.txt"
if command -v arm-linux-musleabi-readelf >/dev/null 2>&1; then
  arm-linux-musleabi-readelf -h "$OUT/fbzx.APP" > "$OUT/READELF.txt"
  arm-linux-musleabi-readelf -A "$OUT/fbzx.APP" >> "$OUT/READELF.txt" || true
  arm-linux-musleabi-readelf -l "$OUT/fbzx.APP" > "$OUT/READELF-PROGRAM.txt"
fi
sha256sum "$OUT"/* | tee "$OUT/SHA256SUMS.txt"

echo "FBZX H3531 validation build complete: $OUT/fbzx.APP"
