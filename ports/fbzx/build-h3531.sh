#!/usr/bin/env bash
set -euo pipefail

FBZX_REF="${FBZX_REF:-981d48272e1cd04ce258e1060fd9574dd6bb4a60}"
FBZX_URL="${FBZX_URL:-https://github.com/rastersoft/fbzx.git}"
WORK="${WORK:-/tmp/fbzx-h3531}"
SRC="$WORK/src"
OUT="${OUT:-$PWD/out-fbzx}"
SDL_CONFIG="${SDL_CONFIG:-/tmp/sdl-h3531/bin/sdl-config}"
CC="${CC:-arm-linux-musleabi-gcc}"
CXX="${CXX:-arm-linux-musleabi-g++}"
STRIP="${STRIP:-arm-linux-musleabi-strip}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
RUNTIME_SRC="$SCRIPT_DIR/h3531-runtime.c"
RUNTIME_OBJ="$WORK/h3531-runtime.o"
AUDIO_SRC="$SCRIPT_DIR/h3531-audio.cpp"
AUDIO_HDR="$SCRIPT_DIR/h3531-audio.h"
AUDIO_OBJ="$WORK/h3531-audio.o"
FBZX_PATCH="$SCRIPT_DIR/fbzx-3.1.0-screen-bounds.patch"

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"

for f in "$RUNTIME_SRC" "$AUDIO_SRC" "$AUDIO_HDR" "$FBZX_PATCH"; do
  if [ ! -f "$f" ]; then
    echo "ERROR: required H3531 source missing: $f" >&2
    exit 2
  fi
done
if [ ! -x "$SDL_CONFIG" ]; then
  echo "ERROR: SDL_CONFIG not executable: $SDL_CONFIG" >&2
  exit 2
fi

SDL_PREFIX="$(cd "$(dirname "$SDL_CONFIG")/.." && pwd)"
SDL_CFLAGS="$($SDL_CONFIG --cflags) -I$SDL_PREFIX/include"
SDL_STATIC_LIBS="$($SDL_CONFIG --static-libs)"

"$CC" -c -O2 -g -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE \
  "$RUNTIME_SRC" -o "$RUNTIME_OBJ"
"$CXX" -c -O2 -g -std=gnu++14 -fno-pie -march=armv7-a -mfloat-abi=soft \
  -D_GNU_SOURCE -pthread "$AUDIO_SRC" -o "$AUDIO_OBJ"

echo "== Clone pinned FBZX =="
git clone "$FBZX_URL" "$SRC"
cd "$SRC"
git checkout "$FBZX_REF"
UPSTREAM_SHA="$(git rev-parse HEAD)"
printf '%s\n' "$UPSTREAM_SHA" > "$OUT/FBZX-UPSTREAM-SHA.txt"
printf '%s\n' "$FBZX_REF" > "$OUT/FBZX-UPSTREAM-REF.txt"

cp "$AUDIO_HDR" src/h3531-audio.h

echo "== Apply H3531 screen-bounds patch =="
git apply "$FBZX_PATCH"

echo "== Apply H3531 video/audio integration =="
python3 - <<'PY'
from pathlib import Path

# H3531 framebuffer is already physically fullscreen. A second SetVideoMode
# invalidates the logical surface cached by FBZX, so preserve it.
p = Path("src/llscreen.cpp")
t = p.read_text()
marker = "void LLScreen::fullscreen_switch() {\n\n"
if t.count(marker) != 1:
    raise SystemExit("fullscreen marker missing")
insert = '''\tconst char *video_driver = SDL_getenv("SDL_VIDEODRIVER");

\tif ((video_driver != NULL) && (SDL_strcmp(video_driver,"h3531") == 0)) {
\t\tthis->llscreen->flags |= SDL_FULLSCREEN;
\t\tthis->set_mouse();
\t\treturn;
\t}

'''
t = t.replace(marker, marker + insert, 1)

# v15 diagnostic only: measure the complete LLScreen::do_flip() call,
# including surface unlock/lock when SDL requires it. No pacing or rendering
# behaviour is changed by this instrumentation.
include_marker = '#include <string.h>\n'
if t.count(include_marker) != 1:
    raise SystemExit("llscreen include marker missing")
t = t.replace(include_marker,
              '#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n#include <time.h>\n', 1)
old_flip = '''void LLScreen::do_flip() {

\tif (this->mustlock) {
\t\tSDL_UnlockSurface (this->llscreen);
\t\tSDL_Flip(this->llscreen);
\t\tSDL_LockSurface(this->llscreen);
\t} else {
\t\tSDL_Flip(this->llscreen);
\t}
}'''
new_flip = '''void LLScreen::do_flip() {

\tstatic uint64_t diag_flip_sum_ns = 0;
\tstatic uint64_t diag_flip_max_ns = 0;
\tstatic unsigned diag_flip_count = 0;
\tstatic unsigned diag_flip_over_1ms = 0;
\tstatic unsigned diag_flip_over_3ms = 0;
\tstatic unsigned diag_flip_over_5ms = 0;
\tstruct timespec diag_before;
\tstruct timespec diag_after;
\tuint64_t diag_before_ns = 0;
\tint diag_timed = 0;

\tif (clock_gettime(CLOCK_MONOTONIC, &diag_before) == 0) {
\t\tdiag_before_ns = ((uint64_t)diag_before.tv_sec * 1000000000ULL) +
\t\t                 (uint64_t)diag_before.tv_nsec;
\t\tdiag_timed = 1;
\t}

\tif (this->mustlock) {
\t\tSDL_UnlockSurface (this->llscreen);
\t\tSDL_Flip(this->llscreen);
\t\tSDL_LockSurface(this->llscreen);
\t} else {
\t\tSDL_Flip(this->llscreen);
\t}

\tif (diag_timed && (clock_gettime(CLOCK_MONOTONIC, &diag_after) == 0)) {
\t\tuint64_t diag_after_ns = ((uint64_t)diag_after.tv_sec * 1000000000ULL) +
\t\t                         (uint64_t)diag_after.tv_nsec;
\t\tif (diag_after_ns >= diag_before_ns) {
\t\t\tuint64_t elapsed_ns = diag_after_ns - diag_before_ns;
\t\t\tuint32_t elapsed_us = (uint32_t)(elapsed_ns / 1000ULL);
\t\t\tdiag_flip_sum_ns += elapsed_ns;
\t\t\tif (elapsed_ns > diag_flip_max_ns) diag_flip_max_ns = elapsed_ns;
\t\t\tif (elapsed_us > 1000U) ++diag_flip_over_1ms;
\t\t\tif (elapsed_us > 3000U) ++diag_flip_over_3ms;
\t\t\tif (elapsed_us > 5000U) ++diag_flip_over_5ms;
\t\t\t++diag_flip_count;
\t\t\tif (diag_flip_count >= 250U) {
\t\t\t\tfprintf(stderr,
\t\t\t\t        "H3531 video15: flips=%u flip_avg_us=%u flip_max_us=%u >1ms=%u >3ms=%u >5ms=%u mustlock=%d\\n",
\t\t\t\t        diag_flip_count,
\t\t\t\t        (unsigned)((diag_flip_sum_ns / diag_flip_count) / 1000ULL),
\t\t\t\t        (unsigned)(diag_flip_max_ns / 1000ULL),
\t\t\t\t        diag_flip_over_1ms, diag_flip_over_3ms, diag_flip_over_5ms,
\t\t\t\t        this->mustlock ? 1 : 0);
\t\t\t\tdiag_flip_sum_ns = diag_flip_max_ns = 0;
\t\t\t\tdiag_flip_count = diag_flip_over_1ms = diag_flip_over_3ms = diag_flip_over_5ms = 0;
\t\t\t}
\t\t}
\t}
}'''
if t.count(old_flip) != 1:
    raise SystemExit("do_flip marker missing")
t = t.replace(old_flip, new_flip, 1)
p.write_text(t)

# Add a dedicated H3531 sound type. SOUND_NO stays available as a safe fallback.
p = Path("src/llsound.hh")
t = p.read_text()
old = "enum e_soundtype {SOUND_NO, SOUND_OSS, SOUND_ALSA, SOUND_PULSEAUDIO, SOUND_AUTOMATIC};"
new = "enum e_soundtype {SOUND_NO, SOUND_H3531, SOUND_OSS, SOUND_ALSA, SOUND_PULSEAUDIO, SOUND_AUTOMATIC};"
if t.count(old) != 1:
    raise SystemExit("sound enum marker missing")
p.write_text(t.replace(old, new, 1))

p = Path("src/llsound.cpp")
t = p.read_text()
inc = '#include "llsound.hh"\n'
if t.count(inc) != 1:
    raise SystemExit("llsound include marker missing")
t = t.replace(inc, inc + '#include "h3531-audio.h"\n', 1)

old = '''\t\tcase SOUND_NO: // No sound; simulate 8bits mono
\t\t\tthis->sign=0;
\t\t\tthis->format=0;
\t\t\tthis->channels = 1;
\t\t\tthis->freq=48000;
\t\t\tthis->buffer_len=4800; // will wait 1/10 second
\t\t\treturn 0;
\t\tbreak;'''
new = '''\t\tcase SOUND_H3531:
\t\t\tprintf("Trying H3531 AO/HDMI audio\\n");
\t\t\tif (h3531_audio_start() != 0) {
\t\t\t\tprintf("H3531 AO init failed\\n");
\t\t\t\treturn -1;
\t\t\t}
\t\t\tthis->sign=0;
\t\t\tthis->format=0;
\t\t\tthis->channels=1;
\t\t\tthis->freq=48000;
\t\t\tthis->buffer_len=160;
\t\t\treturn 0;
\t\tbreak;
\t\tcase SOUND_NO: // No sound; timing is handled once per video frame on H3531
\t\t\tthis->sign=0;
\t\t\tthis->format=0;
\t\t\tthis->channels = 1;
\t\t\tthis->freq=48000;
\t\t\tthis->buffer_len=4800;
\t\t\treturn 0;
\t\tbreak;'''
if t.count(old) != 1:
    raise SystemExit("init SOUND_NO marker missing")
t = t.replace(old, new, 1)

old = '''\tcase SOUND_NO: // no sound
\t\tusleep(75000); // wait 1/20 second
\t\treturn;
\tbreak;'''
new = '''\tcase SOUND_H3531:
\t\th3531_audio_submit_u8_mono(this->sound, this->buffer_len);
\t\treturn;
\tbreak;
\tcase SOUND_NO: // no sound; H3531 frame clock is in Screen::show_screen
\t\treturn;
\tbreak;'''
if t.count(old) != 1:
    raise SystemExit("play SOUND_NO marker missing")
t = t.replace(old, new, 1)

old = '''\tcase SOUND_NO:
\tbreak;'''
new = '''\tcase SOUND_H3531:
\t\th3531_audio_stop();
\tbreak;
\tcase SOUND_NO:
\tbreak;'''
if t.count(old) != 1:
    raise SystemExit("destructor SOUND_NO marker missing")
t = t.replace(old, new, 1)
p.write_text(t)

# Make H3531 the default sound backend while retaining explicit -nosound.
p = Path("src/emulator.cpp")
t = p.read_text()
needle = "\tCMDLine parse(argc,argv);\n\n\tosd = new OSD();"
replacement = '''\tCMDLine parse(argc,argv);

\tif (getenv("SDL_VIDEODRIVER") == NULL)
\t\tsetenv("SDL_VIDEODRIVER","h3531",0);
\tif (getenv("SDL_FBDEV") == NULL)
\t\tsetenv("SDL_FBDEV","/dev/fb0",0);

\tosd = new OSD();'''
if t.count(needle) != 1:
    raise SystemExit("emulator main marker missing")
t = t.replace(needle, replacement, 1)

needle = "\tenum e_soundtype sound_type = SOUND_AUTOMATIC;\n\tif (parse.nosound) {"
replacement = '''\tenum e_soundtype sound_type = SOUND_AUTOMATIC;
\tconst char *h3531_video_driver = getenv("SDL_VIDEODRIVER");
\tif ((h3531_video_driver != NULL) && !strcmp(h3531_video_driver,"h3531"))
\t\tsound_type = SOUND_H3531;
\tif (parse.nosound) {'''
if t.count(needle) != 1:
    raise SystemExit("sound default marker missing")
t = t.replace(needle, replacement, 1)

needle = "\tif (parse.ds) {\n\t\tordenador->dblscan = true;\n\t}\n\tif (parse.ss) {"
replacement = '''\tif ((h3531_video_driver != NULL) && !strcmp(h3531_video_driver,"h3531"))
\t\tordenador->dblscan = true;
\tif (parse.ds) {
\t\tordenador->dblscan = true;
\t}
\tif (parse.ss) {'''
if t.count(needle) != 1:
    raise SystemExit("doublescan marker missing")
t = t.replace(needle, replacement, 1)
p.write_text(t)

# Exact 48 kHz sample clock: accumulate tstates*48000 against 3.5 MHz.
# This is equivalent to the required 72/73-tstate sequence but has no drift.
p = Path("src/spk_ay.cpp")
t = p.read_text()
old = '''\tthis->tstados_counter_sound += tstados;

\twhile (this->tstados_counter_sound >= llsound->tst_sample)\t{

\t\tthis->tstados_counter_sound -= llsound->tst_sample;'''
new = '''\tconst bool h3531_exact_clock = (llsound->sound_type == SOUND_H3531) && !ordenador->turbo;
\tif (h3531_exact_clock)
\t\tthis->tstados_counter_sound += tstados * 48000;
\telse
\t\tthis->tstados_counter_sound += tstados;

\tconst int h3531_sample_threshold = h3531_exact_clock ? 3500000 : (int)llsound->tst_sample;
\twhile (this->tstados_counter_sound >= h3531_sample_threshold)\t{

\t\tthis->tstados_counter_sound -= h3531_sample_threshold;'''
if t.count(old) != 1:
    raise SystemExit("sample clock marker missing")
p.write_text(t.replace(old, new, 1))

# Pace exactly once after each completed video flip. AO SendFrame is on its own
# thread, so video timing cannot inherit AO kernel jitter anymore.
p = Path("src/screen.cpp")
t = p.read_text()
inc = '#include "keyboard.hh"\n'
if t.count(inc) != 1:
    raise SystemExit("screen include marker missing")
t = t.replace(inc, inc + '#include "h3531-audio.h"\n', 1)
needle = "\t\t\tllscreen->do_flip();\n\n\t\t\tcurr_frames=0;"
replacement = "\t\t\tllscreen->do_flip();\n\t\t\th3531_audio_pace_frame(ordenador->turbo ? 1 : 0);\n\n\t\t\tcurr_frames=0;"
if t.count(needle) != 1:
    raise SystemExit("video flip marker missing")
p.write_text(t.replace(needle, replacement, 1))

# PC arrows -> real ZX CAPS SHIFT+5/6/7/8 chords.
p = Path("src/keyboard.cpp")
t = p.read_text()
repls = {
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_7;
\t\tbreak;''': '''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_7;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_6;
\t\tbreak;''': '''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_6;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_8;
\t\tbreak;''': '''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_8;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_5;
\t\tbreak;''': '''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_5;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
}
for old, new in repls.items():
    if t.count(old) != 1:
        raise SystemExit("cursor marker missing")
    t = t.replace(old, new, 1)
p.write_text(t)
PY

MAKEFILE=src/Makefile
sed -i \
  -e "s#^CC=.*#CC=$CXX -c -O2 -g -std=gnu++14 -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE -pthread#" \
  -e "s#^CPP=.*#CPP=$CXX -c -O2 -g -std=gnu++14 -fno-pie -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE -pthread#" \
  -e "s#^LN=.*#LN=$CXX -O2 -static -no-pie -march=armv7-a -mfloat-abi=soft -pthread#" \
  -e "s#^CFLAGS +=.*#CFLAGS += $SDL_CFLAGS#" \
  -e "s#^CPPFLAGS +=.*#CPPFLAGS += $SDL_CFLAGS#" \
  -e "s#^LDFLAGS +=.*#LDFLAGS += $RUNTIME_OBJ $AUDIO_OBJ $SDL_STATIC_LIBS -pthread#" \
  "$MAKEFILE"

if grep -E 'pkg-config.*(pulse|alsa)|D_SOUND_(PULSE|ALSA|OSS)' "$MAKEFILE"; then
  echo "ERROR: desktop audio dependency leaked into build" >&2
  exit 4
fi

grep -F 'SOUND_H3531' src/llsound.cpp >/dev/null
grep -F 'h3531_audio_pace_frame' src/screen.cpp >/dev/null
grep -F 'tstados * 48000' src/spk_ay.cpp >/dev/null
grep -F 'H3531 video15:' src/llscreen.cpp >/dev/null

export PATH="$(dirname "$SDL_CONFIG"):$PATH"
echo "== Build static FBZX H3531 buffered-audio validation binary =="
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
  echo "ERROR: no FBZX executable found" >&2
  exit 5
fi

cp "$BIN" "$OUT/fbzx-buffered-unstripped.APP"
cp "$BIN" "$OUT/fbzx-buffered.APP"
"$STRIP" "$OUT/fbzx-buffered.APP" || true
cp COPYING "$OUT/FBZX-COPYING.txt" 2>/dev/null || true
cp AMSTRAD "$OUT/FBZX-AMSTRAD.txt" 2>/dev/null || true
cp data/keymap.bmp "$OUT/keymap.bmp" 2>/dev/null || true

cat > "$OUT/RUN-H3531.txt" <<'EOF'
H3531 FBZX buffered-audio diagnostic v15
========================================

This is v14 audio/pacing plus diagnostic timing of LLScreen::do_flip().
No flash, SPI NOR, U-Boot environment or saveenv operation is performed.

Expected runtime diagnostics include:
  H3531 AO buffered worker ready: batch-v14 960U8 -> 6x160 AO, queue=16 blocks, prefill=6, frame-layout-v3, len=320B
  H3531 perf14: ...
  H3531 video15: flips=250 flip_avg_us=... flip_max_us=... >1ms=... >3ms=... >5ms=... mustlock=...
EOF

file "$OUT/fbzx-buffered.APP" | tee "$OUT/FILE.txt"
file "$OUT/fbzx-buffered-unstripped.APP" | tee "$OUT/FILE-UNSTRIPPED.txt"
if command -v arm-linux-musleabi-readelf >/dev/null 2>&1; then
  arm-linux-musleabi-readelf -h "$OUT/fbzx-buffered.APP" > "$OUT/READELF.txt"
  arm-linux-musleabi-readelf -A "$OUT/fbzx-buffered.APP" >> "$OUT/READELF.txt" || true
  arm-linux-musleabi-readelf -l "$OUT/fbzx-buffered.APP" > "$OUT/READELF-PROGRAM.txt"
  arm-linux-musleabi-readelf -s "$OUT/fbzx-buffered-unstripped.APP" | \
    grep -E 'h3531_audio_(start|submit_u8_mono|pace_frame|stop)' > "$OUT/H3531-AUDIO-SYMBOLS.txt" || true
fi
sha256sum "$OUT"/* | tee "$OUT/SHA256SUMS.txt"

echo "FBZX H3531 diagnostic v15 build complete: $OUT/fbzx-buffered.APP"
