#!/usr/bin/env python3
from pathlib import Path
import re
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_fceumm_h3531_free_shoulders.py INPUT OUTPUT")

src = Path(sys.argv[1]).read_text(encoding="utf-8")

old = "   palette_switch_enabled = libretro_supports_set_variable;"
new = """   /* H3531: keep fceumm_palette fully available through Core Options,
    * but do not reserve any physical shoulder-button shortcut for it. */
   palette_switch_enabled = false;"""
if old not in src:
    raise SystemExit("palette shortcut enable anchor missing")
src = src.replace(old, new, 1)

old_fds = """      bool curL = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
      bool curR = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
      static bool prevL = false, prevR = false;

      if (curL && !prevL)
         FCEU_FDSSelect();          /* Swap FDisk side */
      prevL = curL;

      if (curR && !prevR)
         FCEU_FDSInsert(-1);        /* Insert or eject the disk */
      prevR = curR;"""
new_fds = """      /* H3531: L1 is intentionally left free for frontend/user hotkeys.
       * FDS disk-side control must be performed from RetroArch's disk/core UI.
       * R1 remains the legacy insert/eject shortcut. */
      bool curR = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
      static bool prevR = false;

      if (curR && !prevR)
         FCEU_FDSInsert(-1);        /* Insert or eject the disk */
      prevR = curR;"""
if old_fds not in src:
    raise SystemExit("FDS L1 anchor missing")
src = src.replace(old_fds, new_fds, 1)

# Remove visible shoulder descriptors so the core no longer advertises
# actions that are intentionally reserved for frontend/user hotkeys.
src = re.sub(
    r'^\s*\{\s*0,\s*RETRO_DEVICE_JOYPAD,\s*0,\s*RETRO_DEVICE_ID_JOYPAD_L2,\s*"Switch Palette \(\+ Left/Right\)"\s*\},\s*\n',
    '',
    src,
    flags=re.MULTILINE)
src = re.sub(
    r'^\s*\{\s*0,\s*RETRO_DEVICE_JOYPAD,\s*0,\s*RETRO_DEVICE_ID_JOYPAD_L,\s*"\(FDS\) Disk Side Change"\s*\},\s*\n',
    '',
    src,
    flags=re.MULTILINE)

required = [
    "palette_switch_enabled = false;",
    "L1 is intentionally left free for frontend/user hotkeys",
    "FDS disk-side control must be performed from RetroArch's disk/core UI",
]
for marker in required:
    if marker not in src:
        raise SystemExit("missing H3531 FCEUmm marker: " + marker)

Path(sys.argv[2]).write_text(src, encoding="utf-8")
print("FCEUMM_H3531_FREE_SHOULDERS_PATCH_OK")
