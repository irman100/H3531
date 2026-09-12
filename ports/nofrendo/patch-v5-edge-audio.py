from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Give the hardware test binary an unmistakable v5 identity.  The actual
# right-edge crop lives in fb-h3531.c and the AO5 recovery path in h3531-ao.c,
# both copied by patch-h3531.py before this patch runs.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v4: persistent video bitmap + exact 60Hz + deferred AO/resampler + A1R5G5B5"
new = "H3531 Nofrendo v5: right-edge crop + AO5 recovery + persistent video + exact 60Hz + A1R5G5B5"
if s.count(old) != 1:
    raise SystemExit(f"v4 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

print("H3531 Nofrendo v5 edge/audio patch applied")
