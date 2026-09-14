#!/usr/bin/env python3
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage43_pixel_system_icons.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

old_marker = 'Stage4.2 stable-order centered carousel active'
new_marker = 'Stage4.3 scalable pixel system icons active'
if old_marker not in src:
    raise SystemExit("expected Stage4.2 stable-order marker not found")
src = src.replace(old_marker, new_marker, 1)

focus_needle = '''enum class FocusZone {\n   Systems,\n   Games\n};\n'''
registry = r'''

enum class IconType {
   GenericPad,
   NesPad,
   MegaDrivePad,
   SnesPad,
   MasterSystemPad,
   GameBoy,
   GameGear,
   PlayStationPad
};

struct SystemVisualSpec {
   const char *system_id;
   const char *short_label;
   IconType icon_type;
   int accent_r;
   int accent_g;
   int accent_b;
   const char *core_hint;
};

/* Visual metadata only. Launch routing remains authoritative in es_systems.cfg. */
static const SystemVisualSpec kStage43VisualSpecs[] = {
   {"nes",          "NES",           IconType::NesPad,          221, 67, 72,  "fceumm_libretro.so"},
   {"famicom",      "FAMICOM",       IconType::NesPad,          221, 67, 72,  "fceumm_libretro.so"},
   {"megadrive",    "SEGA MD",       IconType::MegaDrivePad,    68, 153, 255, "picodrive_libretro.so"},
   {"genesis",      "GENESIS",       IconType::MegaDrivePad,    68, 153, 255, "picodrive_libretro.so"},
   {"mastersystem", "MASTER SYSTEM", IconType::MasterSystemPad, 230, 72, 76,  "picodrive_libretro.so"},
   {"gamegear",     "GAME GEAR",     IconType::GameGear,        72, 166, 240, "picodrive_libretro.so"},
   {"snes",         "SNES",          IconType::SnesPad,         154, 118, 210,""},
   {"sfc",          "SUPER FAMICOM", IconType::SnesPad,         154, 118, 210,""},
   {"gb",           "GAME BOY",      IconType::GameBoy,         130, 154, 112,""},
   {"gbc",          "GAME BOY COLOR",IconType::GameBoy,         104, 158, 212,""},
   {"gba",          "GBA",           IconType::GameBoy,         130, 118, 210,""},
   {"psx",          "PLAYSTATION",   IconType::PlayStationPad,  90, 184, 214,""},
   {"ps1",          "PLAYSTATION",   IconType::PlayStationPad,  90, 184, 214,""}
};

static const SystemVisualSpec kStage43FallbackSpec = {
   "*", "SYSTEM", IconType::GenericPad, 75, 202, 255, ""
};

static const SystemVisualSpec &stage43_visual_spec(const SystemDef &s)
{
   const std::string id = lower(s.name);
   for (const auto &spec : kStage43VisualSpecs)
      if (id == spec.system_id) return spec;
   return kStage43FallbackSpec;
}

static const char *stage43_icon_type_name(IconType t)
{
   switch (t)
   {
      case IconType::NesPad:          return "NES_PAD";
      case IconType::MegaDrivePad:    return "MEGADRIVE_PAD";
      case IconType::SnesPad:         return "SNES_PAD";
      case IconType::MasterSystemPad: return "MASTER_SYSTEM_PAD";
      case IconType::GameBoy:         return "GAMEBOY";
      case IconType::GameGear:        return "GAME_GEAR";
      case IconType::PlayStationPad:  return "PLAYSTATION_PAD";
      case IconType::GenericPad:
      default:                        return "GENERIC_PAD";
   }
}
'''
if focus_needle not in src:
    raise SystemExit("FocusZone declaration not found")
src = src.replace(focus_needle, focus_needle + registry, 1)

# System labels now come from the registry; unknown systems retain a readable fallback.
label_pattern = re.compile(
    r'''static std::string stage42_system_short_name\(const SystemDef &s\)\n\{.*?\n\}\n''',
    re.S,
)
label_replacement = r'''static std::string stage42_system_short_name(const SystemDef &s)
{
   const SystemVisualSpec &spec = stage43_visual_spec(s);
   if (spec.icon_type != IconType::GenericPad)
      return spec.short_label;
   return stage42_ellipsize_px(s.fullname.empty() ? s.name : s.fullname, 2, 150);
}
'''
src, n = label_pattern.subn(label_replacement, src, count=1)
if n != 1:
    raise SystemExit(f"system short-name replacement count={n}, expected 1")

# Add original, dependency-free pixel/raster controller/handheld renderers.
insert_before_generic = 'static void stage42_draw_generic_pad(Fb &fb, int cx, int cy, float scale,\n'
if insert_before_generic not in src:
    raise SystemExit("generic pad renderer anchor not found")
extra_renderers = r'''static void stage43_draw_snes_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int rx = (int)std::lround(60 * scale);
   const int ry = (int)std::lround(27 * scale);
   stage42_fill_ellipse(fb, cx, cy, rx, ry, body);
   const int arm = std::max(14, (int)std::lround(23 * scale));
   const int d = std::max(4, (int)std::lround(7 * scale));
   const int dx = cx - (int)std::lround(31 * scale);
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(28 * scale), cy - (int)std::lround(9 * scale), r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(40 * scale), cy, r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(28 * scale), cy + (int)std::lround(9 * scale), r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(16 * scale), cy, r, accent);
   fill_rect(fb, cx - (int)std::lround(7 * scale), cy + (int)std::lround(13 * scale),
             std::max(5, (int)std::lround(14 * scale)), std::max(2, (int)std::lround(3 * scale)), ink);
}

static void stage43_draw_master_system_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int w = (int)std::lround(112 * scale);
   const int h = (int)std::lround(48 * scale);
   const int x = cx - w / 2, y = cy - h / 2;
   fill_rect(fb, x, y, w, h, body);
   frame_rect(fb, x, y, w, h, std::max(1, (int)std::lround(2 * scale)), ink);
   const int d = std::max(4, (int)std::lround(8 * scale));
   const int arm = std::max(14, (int)std::lround(24 * scale));
   const int dx = x + (int)std::lround(27 * scale);
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(7 * scale));
   stage42_fill_circle(fb, x + w - (int)std::lround(31 * scale), cy, r, accent);
   stage42_fill_circle(fb, x + w - (int)std::lround(14 * scale), cy - (int)std::lround(4 * scale), r, accent);
}

static void stage43_draw_gameboy(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int w = (int)std::lround(70 * scale);
   const int h = (int)std::lround(86 * scale);
   const int x = cx - w / 2, y = cy - h / 2;
   fill_rect(fb, x, y, w, h, body);
   frame_rect(fb, x, y, w, h, std::max(1, (int)std::lround(2 * scale)), ink);
   fill_rect(fb, x + (int)std::lround(9 * scale), y + (int)std::lround(9 * scale),
             (int)std::lround(52 * scale), (int)std::lround(34 * scale), ink);
   fill_rect(fb, x + (int)std::lround(13 * scale), y + (int)std::lround(13 * scale),
             (int)std::lround(44 * scale), (int)std::lround(26 * scale), pack1555(75, 96, 91));
   const int dx = x + (int)std::lround(20 * scale);
   const int dy = y + (int)std::lround(61 * scale);
   const int d = std::max(4, (int)std::lround(6 * scale));
   const int arm = std::max(12, (int)std::lround(19 * scale));
   fill_rect(fb, dx - d / 2, dy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, dy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, x + (int)std::lround(49 * scale), y + (int)std::lround(58 * scale), r, accent);
   stage42_fill_circle(fb, x + (int)std::lround(58 * scale), y + (int)std::lround(67 * scale), r, accent);
}

static void stage43_draw_gamegear(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int rx = (int)std::lround(65 * scale);
   const int ry = (int)std::lround(34 * scale);
   stage42_fill_ellipse(fb, cx, cy, rx, ry, body);
   fill_rect(fb, cx - (int)std::lround(29 * scale), cy - (int)std::lround(21 * scale),
             (int)std::lround(58 * scale), (int)std::lround(42 * scale), ink);
   fill_rect(fb, cx - (int)std::lround(24 * scale), cy - (int)std::lround(16 * scale),
             (int)std::lround(48 * scale), (int)std::lround(32 * scale), pack1555(50, 73, 77));
   const int dx = cx - (int)std::lround(45 * scale);
   const int d = std::max(4, (int)std::lround(6 * scale));
   const int arm = std::max(12, (int)std::lround(19 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(43 * scale), cy - (int)std::lround(6 * scale), r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(52 * scale), cy + (int)std::lround(5 * scale), r, accent);
}

static void stage43_draw_playstation_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int rx = (int)std::lround(55 * scale);
   const int ry = (int)std::lround(25 * scale);
   stage42_fill_ellipse(fb, cx, cy - (int)std::lround(4 * scale), rx, ry, body);
   stage42_fill_ellipse(fb, cx - (int)std::lround(37 * scale), cy + (int)std::lround(20 * scale),
                        (int)std::lround(18 * scale), (int)std::lround(25 * scale), body);
   stage42_fill_ellipse(fb, cx + (int)std::lround(37 * scale), cy + (int)std::lround(20 * scale),
                        (int)std::lround(18 * scale), (int)std::lround(25 * scale), body);
   const int dx = cx - (int)std::lround(30 * scale);
   const int d = std::max(4, (int)std::lround(6 * scale));
   const int arm = std::max(12, (int)std::lround(20 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(29 * scale), cy - (int)std::lround(9 * scale), r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(40 * scale), cy, r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(29 * scale), cy + (int)std::lround(9 * scale), r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(18 * scale), cy, r, accent);
   fill_rect(fb, cx - (int)std::lround(11 * scale), cy + (int)std::lround(5 * scale),
             (int)std::lround(8 * scale), (int)std::lround(3 * scale), ink);
   fill_rect(fb, cx + (int)std::lround(3 * scale), cy + (int)std::lround(5 * scale),
             (int)std::lround(8 * scale), (int)std::lround(3 * scale), ink);
}

'''
src = src.replace(insert_before_generic, extra_renderers + insert_before_generic, 1)

# Insert a single registry-driven dispatcher after the generic renderer and before layout math.
anchor = 'static float stage42_card_center_offset(float rel)\n'
if anchor not in src:
    raise SystemExit("card offset anchor not found")
dispatch = r'''static void stage43_draw_system_icon(Fb &fb, const SystemDef &s, int cx, int cy,
      float scale, uint16_t body, uint16_t ink, uint16_t accent)
{
   switch (stage43_visual_spec(s).icon_type)
   {
      case IconType::NesPad:          stage42_draw_nes_pad(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::MegaDrivePad:    stage42_draw_sega_pad(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::SnesPad:         stage43_draw_snes_pad(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::MasterSystemPad: stage43_draw_master_system_pad(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::GameBoy:         stage43_draw_gameboy(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::GameGear:        stage43_draw_gamegear(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::PlayStationPad:  stage43_draw_playstation_pad(fb, cx, cy, scale, body, ink, accent); break;
      case IconType::GenericPad:
      default:                        stage42_draw_generic_pad(fb, cx, cy, scale, body, ink, accent); break;
   }
}

'''
src = src.replace(anchor, dispatch + anchor, 1)

# Replace the hard-coded NES/Sega selection in the system carousel with the registry dispatcher.
old_draw = '''      const float pad_scale = 0.76f + f * 0.34f;\n      const uint16_t body = selected ? pack1555(196, 204, 214) : pack1555(104, 117, 135);\n      const uint16_t ink = selected ? pack1555(24, 29, 37) : pack1555(42, 50, 62);\n      const uint16_t button = selected ? red : pack1555(118, 74, 82);\n\n      if (stage42_is_nes(s))\n         stage42_draw_nes_pad(fb, cx, y + 35, pad_scale, body, ink, button);\n      else if (stage42_is_sega(s))\n         stage42_draw_sega_pad(fb, cx, y + 35, pad_scale, body, ink, button);\n      else\n         stage42_draw_generic_pad(fb, cx, y + 35, pad_scale, body, ink, button);\n'''
new_draw = '''      const float pad_scale = 0.76f + f * 0.34f;\n      const uint16_t body = selected ? pack1555(196, 204, 214) : pack1555(104, 117, 135);\n      const uint16_t ink = selected ? pack1555(24, 29, 37) : pack1555(42, 50, 62);\n      const SystemVisualSpec &visual = stage43_visual_spec(s);\n      const uint16_t button = selected\n            ? pack1555(visual.accent_r, visual.accent_g, visual.accent_b)\n            : pack1555(visual.accent_r / 2, visual.accent_g / 2, visual.accent_b / 2);\n      stage43_draw_system_icon(fb, s, cx, y + 35, pad_scale, body, ink, button);\n'''
if old_draw not in src:
    raise SystemExit("system icon draw block not found")
src = src.replace(old_draw, new_draw, 1)

# Extend the self-test with registry and future-library checks.
classify_marker = '   printf("SYSTEM_CLASSIFY NES=NES MEGADRIVE=SEGA_MD\\n");\n'
if classify_marker not in src:
    raise SystemExit("classification test marker not found")
registry_test = r'''   if (stage43_visual_spec(nes).icon_type != IconType::NesPad) return 41;
   if (stage43_visual_spec(md).icon_type != IconType::MegaDrivePad) return 42;
   SystemDef snes; snes.name = "snes"; snes.fullname = "Super Nintendo Entertainment System";
   SystemDef ms; ms.name = "mastersystem"; ms.fullname = "Sega Master System";
   SystemDef gb; gb.name = "gb"; gb.fullname = "Nintendo Game Boy";
   SystemDef gg; gg.name = "gamegear"; gg.fullname = "Sega Game Gear";
   SystemDef ps; ps.name = "psx"; ps.fullname = "Sony PlayStation";
   SystemDef unknown; unknown.name = "future-console"; unknown.fullname = "Future Console";
   if (stage43_visual_spec(snes).icon_type != IconType::SnesPad) return 43;
   if (stage43_visual_spec(ms).icon_type != IconType::MasterSystemPad) return 44;
   if (stage43_visual_spec(gb).icon_type != IconType::GameBoy) return 45;
   if (stage43_visual_spec(gg).icon_type != IconType::GameGear) return 46;
   if (stage43_visual_spec(ps).icon_type != IconType::PlayStationPad) return 47;
   if (stage43_visual_spec(unknown).icon_type != IconType::GenericPad) return 48;
   printf("PIXEL_ICON_REGISTRY NES=%s MEGADRIVE=%s SNES=%s MASTER_SYSTEM=%s GAMEBOY=%s GAME_GEAR=%s PLAYSTATION=%s UNKNOWN=%s\n",
          stage43_icon_type_name(stage43_visual_spec(nes).icon_type),
          stage43_icon_type_name(stage43_visual_spec(md).icon_type),
          stage43_icon_type_name(stage43_visual_spec(snes).icon_type),
          stage43_icon_type_name(stage43_visual_spec(ms).icon_type),
          stage43_icon_type_name(stage43_visual_spec(gb).icon_type),
          stage43_icon_type_name(stage43_visual_spec(gg).icon_type),
          stage43_icon_type_name(stage43_visual_spec(ps).icon_type),
          stage43_icon_type_name(stage43_visual_spec(unknown).icon_type));
   printf("PIXEL_ICON_STYLE original-programmatic no-external-theme-assets\n");
   printf("VISUAL_ROUTING_SEPARATION registry=visual-only launcher=es_systems.cfg\n");
'''
src = src.replace(classify_marker, classify_marker + registry_test, 1)

out_path.write_text(src, encoding="utf-8")
print(f"STAGE43_PIXEL_ICONS_PATCH_OK {src_path} -> {out_path}")
