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

echo "== Apply H3531 compatibility backport =="
git apply "$FBZX_PATCH"

echo "== Upstream revision =="
echo "$UPSTREAM_SHA"
echo "== H3531 SDL flags =="
echo "CFLAGS: $SDL_CFLAGS"
echo "LIBS:   $SDL_STATIC_LIBS"

# FBZX 3.1.0 hard-codes native g++ and host pkg-config for SDL, PulseAudio and
# ALSA. Keep the emulator sources untouched except for the explicit H3531
# compatibility patch above; patch only the build description to use our
# ARMv7 soft-float compiler and proven static SDL 1.2 H3531 backend.
# GCC 11 defaults to GNU++17, where std::byte conflicts with Z80Free's legacy
# global typedef named byte. FBZX 3.1.0 predates C++17, so pin GNU++14 here.
# No D_SOUND_* macro is enabled in this first port; board launch uses -nosound.
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

Launch target:

  export SDL_VIDEODRIVER=h3531
  export SDL_FBDEV=/dev/fb0
  ./fbzx.APP -nosound -fs

This build keeps the fatal-signal UART diagnostic enabled during physical
validation and backports the screen end-of-buffer guard used by newer FBZX.
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
