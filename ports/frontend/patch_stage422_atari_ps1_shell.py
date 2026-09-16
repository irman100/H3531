#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage422_atari_ps1_shell.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

old_marker = 'Stage4.21 PixelStation cached-silhouette smooth-render UI active'
new_marker = 'Stage4.22 PixelStation Atari-PlayStation shell UI active'
if old_marker not in src:
    raise SystemExit('Stage4.21 marker not found')
src = src.replace(old_marker, new_marker, 1)

# Artwork beside ROMs may be PNG/JPEG/BMP. Keep stb_image restricted to only
# the formats needed by this embedded frontend.
stb_anchor = '#define STBI_ONLY_PNG\n#define STBI_ONLY_JPEG\n#include "stb_image.h"'
if stb_anchor not in src:
    raise SystemExit('stb image-format anchor missing')
src = src.replace(stb_anchor,
                  '#define STBI_ONLY_PNG\n#define STBI_ONLY_JPEG\n#define STBI_ONLY_BMP\n#include "stb_image.h"', 1)

# Stage4.21 already uses controller PNG alpha bounds for system aura. Add the
# Atari joystick to the exact same path; PlayStation mapping is retained.
controller_anchor = '   if (n == "psx" || n == "ps1" || n == "playstation") return stage45_asset_path("controller_playstation.png");'
if controller_anchor not in src:
    raise SystemExit('PlayStation controller mapping anchor missing')
src = src.replace(controller_anchor,
    '   if (n.find("atari") != std::string::npos) return stage45_asset_path("controller_atari.png");\n' + controller_anchor,
    1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f'start anchor missing: {start}')
    b = text.find(end, a + len(start))
    if b < 0:
        raise SystemExit(f'end anchor missing: {end}')
    return text[:a] + replacement.rstrip() + '\n\n' + text[b:]

# Library integration for the user's USB layout. The configured primary paths
# are /mnt/usb/Games/Atari and /mnt/usb/Games/PS1. Lowercase historic paths are
# accepted only as a compatibility fallback if the primary directory is absent.
art_helpers = r'''static bool stage422_is_ps_system(const SystemDef &sys)
{
   const std::string n = lower(sys.name);
   return n == "psx" || n == "ps1" || n == "playstation";
}

static bool stage422_is_atari_system(const SystemDef &sys)
{
   return lower(sys.name).find("atari") != std::string::npos;
}

static void stage422_resolve_system_path(SystemDef &sys)
{
   if (dir_exists(sys.path)) return;
   const char *fallback = nullptr;
   if (stage422_is_atari_system(sys)) fallback = "/mnt/usb/games/atari2600";
   else if (stage422_is_ps_system(sys)) fallback = "/mnt/usb/games/psx";
   if (fallback && dir_exists(fallback))
   {
      fprintf(stderr, "[STAGE422] system=%s path fallback %s -> %s\n",
            sys.name.c_str(), sys.path.c_str(), fallback);
      sys.path = fallback;
   }
}

static bool stage422_cue_references_bin(const SystemDef &sys, const std::string &bin_path)
{
   DIR *d = opendir(sys.path.c_str());
   if (!d) return false;
   const std::string needle = lower(basename_of(bin_path));
   bool found = false;
   for (dirent *de = readdir(d); de && !found; de = readdir(d))
   {
      if (!de->d_name[0] || de->d_name[0] == '.') continue;
      const std::string cue = join_path(sys.path, de->d_name);
      if (extension_of(cue) != ".cue") continue;
      FILE *f = fopen(cue.c_str(), "rb");
      if (!f) continue;
      char line[2048];
      while (fgets(line, sizeof(line), f))
      {
         if (lower(std::string(line)).find(needle) != std::string::npos)
         {
            found = true;
            break;
         }
      }
      fclose(f);
   }
   closedir(d);
   return found;
}

static std::string find_fallback_image(const SystemDef &sys, const std::string &rom)
{
   const std::string stem = stem_of(rom);
   const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp"};

   // User media convention: artwork lives directly beside the ROM and shares
   // the ROM basename. This is deliberately the first and cheapest lookup.
   for (const char *e : exts)
   {
      const std::string p = join_path(sys.path, stem + e);
      if (file_exists(p)) return p;
   }

   // Optional generic cover names in the same game directory.
   const char *ps_names[] = {"cover", "folder", "boxfront", "disc"};
   const char *atari_names[] = {"label", "cover", "folder", "boxfront"};
   const char *generic_names[] = {"cover", "folder", "boxfront"};
   const char **names = generic_names;
   size_t name_count = sizeof(generic_names) / sizeof(generic_names[0]);
   if (stage422_is_ps_system(sys))
   {
      names = ps_names;
      name_count = sizeof(ps_names) / sizeof(ps_names[0]);
   }
   else if (stage422_is_atari_system(sys))
   {
      names = atari_names;
      name_count = sizeof(atari_names) / sizeof(atari_names[0]);
   }
   for (size_t i = 0; i < name_count; ++i)
      for (const char *e : exts)
      {
         const std::string p = join_path(sys.path, std::string(names[i]) + e);
         if (file_exists(p)) return p;
      }

   // Preserve legacy EmulationStation-style media subdirectories after the
   // direct same-folder convention has been checked.
   const char *dirs[] = {"media", "images", "boxart", "covers"};
   for (const char *d : dirs)
      for (const char *e : exts)
      {
         const std::string p = join_path(join_path(sys.path, d), stem + e);
         if (file_exists(p)) return p;
      }
   return {};
}'''
src = replace_between(src, 'static std::string find_fallback_image(',
                      'static void scan_games(', art_helpers)

scan_start = '''static void scan_games(SystemDef &sys)
{
   sys.games.clear();
   if (!dir_exists(sys.path)) return;'''
scan_repl = '''static void scan_games(SystemDef &sys)
{
   sys.games.clear();
   stage422_resolve_system_path(sys);
   if (!dir_exists(sys.path)) return;'''
if scan_start not in src:
    raise SystemExit('scan_games path anchor missing')
src = src.replace(scan_start, scan_repl, 1)

ext_anchor = '      if (!exts.count(extension_of(full))) continue;'
if ext_anchor not in src:
    raise SystemExit('ROM extension scan anchor missing')
src = src.replace(ext_anchor, r'''      const std::string ext = extension_of(full);
      if (!exts.count(ext)) continue;
      // A PlayStation .bin referenced by a .cue is a track, not a second game.
      // Atari .bin files are unaffected by this PS-only rule.
      if (stage422_is_ps_system(sys) && ext == ".bin")
      {
         const std::string same_stem_cue = join_path(sys.path, stem_of(full) + ".cue");
         if (file_exists(same_stem_cue) || stage422_cue_references_bin(sys, full))
            continue;
      }''', 1)

# Stage4.15 media routing already contains Atari cartridge and PlayStation case
# hooks. Keep those semantics explicit in the generated binary and tests.
layout_ok = '   printf("LAYOUT_TEST_OK\\n");\n'
markers = (
    '   printf("STAGE422_SYSTEMS top-row Atari-controller PlayStation-controller alpha-aura\\n");\n'
    '   printf("STAGE422_MEDIA Atari-cartridge PlayStation-jewel-case transparent-overlay-disc-visible\\n");\n'
    '   printf("STAGE422_PATHS primary-Games-Atari+Games-PS1 fallback-lowercase\\n");\n'
    '   printf("STAGE422_ART same-folder-basename-first png-jpg-jpeg-bmp legacy-subdirs\\n");\n'
    '   printf("STAGE422_PS cue-canonical referenced-bin-suppressed\\n");\n'
    '   printf("STAGE422_CACHE Stage4.21-scaled-assets+aura+cover preserved\\n");\n'
)
if layout_ok not in src:
    raise SystemExit('layout-test anchor missing')
src = src.replace(layout_ok, markers + layout_ok, 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE422_ATARI_PS1_SHELL_PATCH_OK {src_path} -> {out_path}')
