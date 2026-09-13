from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# H3531 Home Computer lifecycle integration:
# Escape must leave the emulator cleanly so the session supervisor can
# relaunch Monitor.  Upstream input.c only maps joypad keys and otherwise
# ignores KEY_ESC.
p = root / "platform" / "input.c"
s = p.read_text()

inc = '#include <linux/input.h>\n'
if s.count(inc) != 1:
    raise SystemExit(f"linux/input include marker count={s.count(inc)}")
s = s.replace(inc, inc + '#include <nes.h>\n', 1)

marker = '''            int set_bit = (ev.value == 1); // 如果按下为 true, 弹起为 false
            
            switch (ev.code) {'''
replacement = '''            int set_bit = (ev.value == 1); // 如果按下为 true, 弹起为 false

            /* H3531 system lifecycle: Escape exits the emulator cleanly.
               nes_poweroff() asks the normal emulation loop to finish, so
               Nofrendo can run its ordinary shutdown/cleanup path and the
               external session supervisor can restart Monitor. */
            if (ev.code == KEY_ESC && ev.value == 1) {
                printf("H3531 NES input v11.1: ESC -> clean exit to Monitor\\n");
                joypad_state = 0;
                nes_poweroff();
                continue;
            }
            
            switch (ev.code) {'''
if s.count(marker) != 1:
    raise SystemExit(f"input switch marker count={s.count(marker)}")
s = s.replace(marker, replacement, 1)
p.write_text(s)

# Make the test binary visibly distinguishable without touching v11 timing,
# audio, video, or framebuffer code.
p = root / "main.c"
s = p.read_text()
old = 'H3531 Nofrendo v11: AO 48k hardware master clock + exact 800-sample frames + v7 video'
new = 'H3531 Nofrendo v11.1: v11 timing/audio/video + ESC clean exit'
if s.count(old) != 1:
    raise SystemExit(f"v11 identity marker count={s.count(old)}")
s = s.replace(old, new, 1)
p.write_text(s)

print("H3531 Nofrendo v11.1 Escape-exit patch applied")
