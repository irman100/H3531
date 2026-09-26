#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_fceumm_h3531_turbo_default.py INPUT OUTPUT")

src = Path(sys.argv[1]).read_text(encoding="utf-8")

needle = '''      "fceumm_turbo_enable",
      "Turbo Enable",
      NULL,
      "Enables or disables turbo buttons.",
      NULL,
      "input",
      {
         { "None",     NULL },
         { "Player 1", NULL },
         { "Player 2", NULL },
         { "Both",     NULL },
         { NULL, NULL },
      },
      "None",
   },'''

replacement = '''      "fceumm_turbo_enable",
      "Turbo Enable",
      NULL,
      "Enables or disables turbo buttons.",
      NULL,
      "input",
      {
         { "None",     NULL },
         { "Player 1", NULL },
         { "Player 2", NULL },
         { "Both",     NULL },
         { NULL, NULL },
      },
      "Player 1",
   },'''

if needle not in src:
    raise SystemExit("FCEUmm turbo default anchor missing")

src = src.replace(needle, replacement, 1)

if '"fceumm_turbo_enable"' not in src or '"Player 1",\n   },' not in src:
    raise SystemExit("turbo default replacement missing")

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("FCEUMM_H3531_TURBO_DEFAULT_PLAYER1_PATCH_OK")
