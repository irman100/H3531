#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage422_library.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

stb_anchor = '#define STBI_ONLY_PNG\n#define STBI_ONLY_JPEG\n#include "stb_image.h"'
if stb_anchor not in src:
    raise SystemExit('stb image-format anchor missing')
src = src.replace(stb_anchor,
                  '#define STBI_ONLY_PNG\n#define STBI_ONLY_JPEG\n#define STBI_ONLY_BMP\n#include "stb_image.h"', 1)


def replace_between(text, start, end, replacement):
    a = text.find(start)
    if a < 0:
        raise SystemExit(f'start anchor missing: {start}')
    b = text.find(end, a + len(start))
    if b < 0:
        raise SystemExit(f'end anchor missing: {end}')
    return text[:a] + replacement.rstrip() + '\n\n' + text[b:]

helpers = r'''static bool stage422_is_ps_system(const SystemDef &sys)
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

   // Primary user convention: artwork is directly beside the ROM and shares
   // its basename. Check it before legacy media subdirectories.
   for (const char *e : exts)
   {
      const std::string p = join_path(sys.path, stem + e);
      if (file_exists(p)) return p;
   }

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
                      'static void scan_games(', helpers)

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
      // In PlayStation folders, a .bin referenced by a .cue is a CD track,
      // not a second game. Atari .bin files remain normal ROMs.
      if (stage422_is_ps_system(sys) && ext == ".bin")
      {
         const std::string same_stem_cue = join_path(sys.path, stem_of(full) + ".cue");
         if (file_exists(same_stem_cue) || stage422_cue_references_bin(sys, full))
            continue;
      }''', 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE422_LIBRARY_PATCH_OK {src_path} -> {out_path}')
