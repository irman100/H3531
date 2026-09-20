#!/bin/bash
set -euo pipefail

FBZX_REF="${FBZX_REF:-981d48272e1cd04ce258e1060fd9574dd6bb4a60}"
FBZX_URL="${FBZX_URL:-https://github.com/rastersoft/fbzx.git}"
FBZX_SRC="${FBZX_SRC:-}"
WORK="${WORK:-/build/fbzx-x11}"
OUT="${OUT:-/build/out-fbzx-x11}"
H3531_SRC="${H3531_SRC:-/build/h3531-fbzx}"
SDL_CONFIG="${SDL_CONFIG:-/opt/sdl-x11/bin/sdl-config}"

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"

if [ -n "$FBZX_SRC" ]; then
    cp -a "$FBZX_SRC" "$WORK/src"
else
    git clone "$FBZX_URL" "$WORK/src"
fi
cd "$WORK/src"
git checkout "$FBZX_REF"

cp "$H3531_SRC/h3531-audio.cpp" src/h3531-audio.cpp
cp "$H3531_SRC/h3531-audio.h" src/h3531-audio.h
git apply "$H3531_SRC/fbzx-3.1.0-screen-bounds.patch"

python3 - <<'PY'
from pathlib import Path

p=Path("src/llsound.hh")
s=p.read_text()
old="enum e_soundtype {SOUND_NO, SOUND_OSS, SOUND_ALSA, SOUND_PULSEAUDIO, SOUND_AUTOMATIC};"
new="enum e_soundtype {SOUND_NO, SOUND_H3531, SOUND_OSS, SOUND_ALSA, SOUND_PULSEAUDIO, SOUND_AUTOMATIC};"
assert s.count(old)==1
p.write_text(s.replace(old,new,1))

p=Path("src/llsound.cpp")
s=p.read_text()
inc='#include "llsound.hh"\n'
assert s.count(inc)==1
s=s.replace(inc,inc+'#include "h3531-audio.h"\n',1)
old='''\t\tcase SOUND_NO: // No sound; simulate 8bits mono
\t\t\tthis->sign=0;
\t\t\tthis->format=0;
\t\t\tthis->channels = 1;
\t\t\tthis->freq=48000;
\t\t\tthis->buffer_len=4800; // will wait 1/10 second
\t\t\treturn 0;
\t\tbreak;'''
new='''\t\tcase SOUND_H3531:
\t\t\tprintf("Trying H3531 AO/HDMI audio\\n");
\t\t\tif (h3531_audio_start() != 0) return -1;
\t\t\tthis->sign=0;
\t\t\tthis->format=0;
\t\t\tthis->channels=1;
\t\t\tthis->freq=48000;
\t\t\tthis->buffer_len=160;
\t\t\treturn 0;
\t\tbreak;
\t\tcase SOUND_NO:
\t\t\tthis->sign=0;
\t\t\tthis->format=0;
\t\t\tthis->channels=1;
\t\t\tthis->freq=48000;
\t\t\tthis->buffer_len=4800;
\t\t\treturn 0;
\t\tbreak;'''
assert s.count(old)==1
s=s.replace(old,new,1)
old='''\tcase SOUND_NO: // no sound
\t\tusleep(75000); // wait 1/20 second
\t\treturn;
\tbreak;'''
new='''\tcase SOUND_H3531:
\t\th3531_audio_submit_u8_mono(this->sound, this->buffer_len);
\t\treturn;
\tbreak;
\tcase SOUND_NO:
\t\treturn;
\tbreak;'''
assert s.count(old)==1
s=s.replace(old,new,1)
old='''\tcase SOUND_NO:
\tbreak;'''
new='''\tcase SOUND_H3531:
\t\th3531_audio_stop();
\tbreak;
\tcase SOUND_NO:
\tbreak;'''
assert s.count(old)==1
p.write_text(s.replace(old,new,1))

p=Path("src/emulator.cpp")
s=p.read_text()
needle="\tenum e_soundtype sound_type = SOUND_AUTOMATIC;\n\tif (parse.nosound) {"
replacement='''\tenum e_soundtype sound_type = SOUND_AUTOMATIC;
\tconst char *h3531_audio = getenv("H3531_AUDIO");
\tif (h3531_audio && !strcmp(h3531_audio,"1"))
\t\tsound_type = SOUND_H3531;
\tif (parse.nosound) {'''
assert s.count(needle)==1
p.write_text(s.replace(needle,replacement,1))

p=Path("src/spk_ay.cpp")
s=p.read_text()
old='''\tthis->tstados_counter_sound += tstados;

\twhile (this->tstados_counter_sound >= llsound->tst_sample)\t{

\t\tthis->tstados_counter_sound -= llsound->tst_sample;'''
new='''\tconst bool h3531_exact_clock = (llsound->sound_type == SOUND_H3531) && !ordenador->turbo;
\tif (h3531_exact_clock)
\t\tthis->tstados_counter_sound += tstados * 48000;
\telse
\t\tthis->tstados_counter_sound += tstados;

\tconst int h3531_sample_threshold = h3531_exact_clock ? 3500000 : (int)llsound->tst_sample;
\twhile (this->tstados_counter_sound >= h3531_sample_threshold)\t{

\t\tthis->tstados_counter_sound -= h3531_sample_threshold;'''
assert s.count(old)==1
p.write_text(s.replace(old,new,1))

p=Path("src/screen.cpp")
s=p.read_text()
inc='#include "keyboard.hh"\n'
assert s.count(inc)==1
s=s.replace(inc,inc+'#include "h3531-audio.h"\n',1)
needle="\t\t\tllscreen->do_flip();\n\n\t\t\tcurr_frames=0;"
replacement="\t\t\tllscreen->do_flip();\n\t\t\th3531_audio_pace_frame(ordenador->turbo ? 1 : 0);\n\n\t\t\tcurr_frames=0;"
assert s.count(needle)==1
p.write_text(s.replace(needle,replacement,1))

p=Path("src/keyboard.cpp")
s=p.read_text()
repls={
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_7;
\t\tbreak;''':'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_7;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_6;
\t\tbreak;''':'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_6;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_8;
\t\tbreak;''':'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_8;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_5;
\t\tbreak;''':'''\t\tcase 0:\t// cursor
\t\t\ttemporal_io = SDLK_5;
\t\t\tthis->k8 = 1;
\t\tbreak;''',
}
for old,new in repls.items():
    assert s.count(old)==1
    s=s.replace(old,new,1)
p.write_text(s)
PY

SDL_CFLAGS="$("$SDL_CONFIG" --cflags)"
SDL_LIBS="$("$SDL_CONFIG" --libs)"

g++ -c -O2 -g -std=gnu++0x -march=armv7-a -mfloat-abi=soft     -D_GNU_SOURCE -pthread src/h3531-audio.cpp -o "$WORK/h3531-audio.o"

MAKEFILE=src/Makefile
sed -i   -e "s#^CC=.*#CC=g++ -c -O2 -g -std=gnu++0x -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE -pthread#"   -e "s#^CPP=.*#CPP=g++ -c -O2 -g -std=gnu++0x -march=armv7-a -mfloat-abi=soft -D_GNU_SOURCE -pthread#"   -e "s#^LN=.*#LN=g++ -O2 -march=armv7-a -mfloat-abi=soft -pthread#"   -e "s#^CFLAGS +=.*#CFLAGS += $SDL_CFLAGS#"   -e "s#^CPPFLAGS +=.*#CPPFLAGS += $SDL_CFLAGS#"   -e "s#^LDFLAGS +=.*#LDFLAGS += $WORK/h3531-audio.o $SDL_LIBS -pthread#"   "$MAKEFILE"

make clean >/dev/null 2>&1 || true
make -j2

BIN=
for candidate in src/fbzx fbzx src/FBZX FBZX; do
    if [ -x "$candidate" ]; then BIN="$candidate"; break; fi
done
[ -n "$BIN" ] || { echo "ERROR: FBZX binary not found"; exit 5; }

cp "$BIN" "$OUT/FBZX-X11.BIN"
strip "$OUT/FBZX-X11.BIN" || true
cp data/keymap.bmp "$OUT/keymap.bmp" 2>/dev/null || true
printf '%s\n' "$FBZX_REF" >"$OUT/FBZX-UPSTREAM-REF.txt"
file "$OUT/FBZX-X11.BIN"
readelf -h "$OUT/FBZX-X11.BIN"
readelf -d "$OUT/FBZX-X11.BIN"
