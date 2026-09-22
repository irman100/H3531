#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage431_steam_flash_lifecycle.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

# User-facing branding. RetroArch/libretro remain internal engine/component names.
src = src.replace("Stage4.30 PixelStation full-menu hotkey-binding UI active",
                  "Stage4.31 Stayplaytion shell lifecycle active")
src = src.replace("PixelStation", "Stayplaytion")
src = src.replace("PIXELSTATION", "STAYPLAYTION")
src = src.replace('"H3531 RETRO"', '"STAYPLAYTION"')
src = src.replace('"H3531 GAME LIBRARY"', '"STAYPLAYTION"')
src = src.replace('"GAMES  PAST  ALWAYS  PLAY"', '"STAYPLAYTION GAME SHELL"')
src = src.replace("F1 RETROARCH", "F1 ENGINE MENU")
src = src.replace("F1 RetroArch", "F1 Engine Menu")

# A ROM opened from the desktop is passed as argv[1] by h3531-app-run.
# Keep Stayplaytion as the parent process: launch the engine once, then return
# to the normal shell loop after RetroArch exits via F12.
cfg_anchor = "   std::string cfg = kDefaultConfig;\n"
if cfg_anchor not in src:
    raise SystemExit("main cfg anchor not found")
src = src.replace(
    cfg_anchor,
    cfg_anchor +
    '   std::string startup_rom;\n'
    '   if (argc >= 2 && argv[1] && argv[1][0] != \'-\') startup_rom = argv[1];\n',
    1,
)

loop_anchor = "   while (running)\n   {\n"
if loop_anchor not in src:
    raise SystemExit("main loop anchor not found")

startup_code = r'''   if (!startup_rom.empty())
   {
      const std::string ext = extension_of(startup_rom);
      int fallback_system = -1;
      int selected_system = -1;

      for (size_t i = 0; i < systems.size(); ++i)
      {
         bool ext_match = false;
         for (const auto &e : systems[i].extensions)
            if (lower(e) == ext) { ext_match = true; break; }

         if (!ext_match) continue;
         if (fallback_system < 0) fallback_system = (int)i;

         const std::string prefix = systems[i].path.empty()
               ? std::string()
               : (systems[i].path.back() == '/' ? systems[i].path : systems[i].path + "/");
         if (!prefix.empty() && startup_rom.compare(0, prefix.size(), prefix) == 0)
         {
            selected_system = (int)i;
            break;
         }
      }

      if (selected_system < 0) selected_system = fallback_system;

      if (selected_system >= 0)
      {
         Game direct;
         direct.rom_path = startup_rom;
         direct.title = stem_of(startup_rom);
         fprintf(stderr, "[STAYPLAYTION] startup ROM -> engine: %s\n", startup_rom.c_str());
         run_external(physical, in, launch_command(systems[(size_t)selected_system], direct));
         if (!back.init(physical)) return 5;
         redraw = true;
      }
      else
      {
         fprintf(stderr, "[STAYPLAYTION] unsupported startup ROM: %s\n", startup_rom.c_str());
      }
   }

'''
src = src.replace(loop_anchor, startup_code + loop_anchor, 1)

# Lifecycle markers used by CI and target logs.
marker_anchor = 'fprintf(stderr, "[GAMEFRONT] %s\\n", STAGE42_MARKER);'
if marker_anchor in src:
    src = src.replace(
        marker_anchor,
        marker_anchor + '\n   fprintf(stderr, "[STAYPLAYTION] shell active; F12 returns from engine, ESC exits shell\\n");',
        1,
    )

out_path.write_text(src, encoding="utf-8")
print(f"STAGE431_STEAM_FLASH_LIFECYCLE_PATCH_OK {src_path} -> {out_path}")
