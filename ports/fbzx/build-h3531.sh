#!/usr/bin/env bash
set -euo pipefail

FBZX_REF="${FBZX_REF:-3.1.0}"
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

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"

echo "== Clone FBZX =="
git clone "$FBZX_URL" "$SRC"
cd "$SRC"
git checkout "$FBZX_REF"
UPSTREAM_SHA="$(git rev-parse HEAD)"
printf '%s\n' "$UPSTREAM_SHA" > "$OUT/FBZX-UPSTREAM-SHA.txt"
printf '%s\n' "$FBZX_REF" > "$OUT/FBZX-UPSTREAM-REF.txt"

echo "== Upstream revision =="
echo "$UPSTREAM_SHA"

echo "== Source layout =="
find . -maxdepth 2 -type f | sort | sed -n '1,240p'

echo "== Top-level Makefile =="
if [ -f Makefile ]; then sed -n '1,260p' Makefile; fi

echo "== src/Makefile =="
if [ -f src/Makefile ]; then sed -n '1,320p' src/Makefile; fi

if [ ! -x "$SDL_CONFIG" ]; then
  echo "ERROR: SDL_CONFIG not executable: $SDL_CONFIG" >&2
  exit 2
fi

# FBZX 3.1.x is the SDL 1.2 generation.  Force every literal sdl-config
# reference to the cross-built H3531 SDL so the host SDL can never leak in.
for f in Makefile src/Makefile; do
  if [ -f "$f" ]; then
    sed -i "s#\bsdl-config\b#$SDL_CONFIG#g" "$f"
  fi
done

export CC CXX AR RANLIB
export PATH="$(dirname "$SDL_CONFIG"):$PATH"
export CFLAGS="-O2 -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE ${CFLAGS:-}"
export CXXFLAGS="-O2 -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE ${CXXFLAGS:-}"
export LDFLAGS="-static -no-pie ${LDFLAGS:-}"
export SDL_CONFIG

# First pass deliberately uses upstream's own build graph.  We keep sound code
# compiled, but the first board launch will use -nosound so H3531 does not need
# ALSA/OSS/PulseAudio working yet.
echo "== Build FBZX with H3531 SDL 1.2 =="
set +e
make clean >/dev/null 2>&1
set -e

make -j2 \
  CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" \
  CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
  SDL_CONFIG="$SDL_CONFIG"

# Upstream versions have used both top-level and src/ output locations.
BIN=""
for candidate in fbzx src/fbzx FBZX src/FBZX; do
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
  find . -maxdepth 3 -type f | sort | tail -200
  exit 3
fi

cp "$BIN" "$OUT/fbzx.APP"
"$STRIP" "$OUT/fbzx.APP" || true
cp COPYING "$OUT/FBZX-COPYING.txt" 2>/dev/null || true
cp AMSTRAD "$OUT/FBZX-AMSTRAD.txt" 2>/dev/null || true
cat > "$OUT/RUN-H3531.txt" <<'EOF'
First H3531 board test:

  fbzx.APP -nosound -fs

Goals for v0:
- start through the H3531 SDL 1.2 framebuffer backend;
- show the Spectrum screen;
- keyboard works;
- ESC exits cleanly and the session supervisor restores Monitor.

Sound is intentionally deferred until video/input are proven.
EOF

file "$OUT/fbzx.APP" | tee "$OUT/FILE.txt"
if command -v arm-linux-musleabi-readelf >/dev/null 2>&1; then
  arm-linux-musleabi-readelf -h "$OUT/fbzx.APP" > "$OUT/READELF.txt"
  arm-linux-musleabi-readelf -A "$OUT/fbzx.APP" >> "$OUT/READELF.txt" || true
fi
sha256sum "$OUT"/* | tee "$OUT/SHA256SUMS.txt"

echo "FBZX H3531 build complete: $OUT/fbzx.APP"
