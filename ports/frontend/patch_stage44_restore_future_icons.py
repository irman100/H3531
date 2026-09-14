#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage44_restore_future_icons.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")
anchor = 'static void stage43_draw_system_icon(Fb &fb, const SystemDef &s, int cx, int cy, float scale,\n'
if anchor not in src:
    raise SystemExit('stage43 dispatcher anchor not found')
if 'static void stage43_draw_snes_pad(' in src:
    out_path.write_text(src, encoding='utf-8')
    print('FUTURE_ICONS_ALREADY_PRESENT')
    raise SystemExit(0)

code = r'''static void stage43_draw_snes_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const uint16_t shell = pack1555(205, 207, 214);
   const uint16_t face = pack1555(177, 179, 190);
   const int rx = (int)std::lround(61 * scale);
   const int ry = (int)std::lround(27 * scale);
   stage42_fill_ellipse(fb, cx, cy, rx, ry, shell);
   stage42_fill_ellipse(fb, cx, cy, std::max(1, rx - 4), std::max(1, ry - 4), face);
   const int dx = cx - (int)std::lround(32 * scale);
   const int d = std::max(4, (int)std::lround(7 * scale));
   const int arm = std::max(15, (int)std::lround(23 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(21 * scale), cy - (int)std::lround(8 * scale), r, pack1555(71, 150, 221));
   stage42_fill_circle(fb, cx + (int)std::lround(34 * scale), cy, r, pack1555(219, 72, 79));
   stage42_fill_circle(fb, cx + (int)std::lround(21 * scale), cy + (int)std::lround(8 * scale), r, pack1555(235, 194, 64));
   stage42_fill_circle(fb, cx + (int)std::lround(8 * scale), cy, r, pack1555(72, 190, 115));
   fill_rect(fb, cx - (int)std::lround(7 * scale), cy + (int)std::lround(13 * scale),
             std::max(5, (int)std::lround(13 * scale)), std::max(2, (int)std::lround(3 * scale)), ink);
}

static void stage43_draw_master_system_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int w = (int)std::lround(116 * scale);
   const int h = (int)std::lround(48 * scale);
   const int x = cx - w / 2, y = cy - h / 2;
   const uint16_t shell = pack1555(45, 49, 58);
   fill_rect(fb, x, y, w, h, shell);
   frame_rect(fb, x, y, w, h, std::max(1, (int)std::lround(2 * scale)), ink);
   fill_rect(fb, x + (int)std::lround(8 * scale), y + (int)std::lround(7 * scale),
             w - (int)std::lround(16 * scale), std::max(2, (int)std::lround(3 * scale)), pack1555(176, 44, 52));
   const int dx = x + (int)std::lround(27 * scale);
   const int d = std::max(4, (int)std::lround(8 * scale));
   const int arm = std::max(15, (int)std::lround(24 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(7 * scale));
   stage42_fill_circle(fb, x + w - (int)std::lround(31 * scale), cy, r, pack1555(219, 57, 64));
   stage42_fill_circle(fb, x + w - (int)std::lround(14 * scale), cy - (int)std::lround(4 * scale), r, pack1555(219, 57, 64));
}

static void stage43_draw_gameboy(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int w = (int)std::lround(68 * scale);
   const int h = (int)std::lround(88 * scale);
   const int x = cx - w / 2, y = cy - h / 2;
   const uint16_t shell = pack1555(194, 195, 187);
   fill_rect(fb, x + 3, y, w - 6, h, shell);
   fill_rect(fb, x, y + 3, w, h - 6, shell);
   frame_rect(fb, x, y, w, h, std::max(1, (int)std::lround(2 * scale)), ink);
   fill_rect(fb, x + (int)std::lround(8 * scale), y + (int)std::lround(9 * scale),
             (int)std::lround(52 * scale), (int)std::lround(36 * scale), pack1555(54, 61, 72));
   fill_rect(fb, x + (int)std::lround(13 * scale), y + (int)std::lround(14 * scale),
             (int)std::lround(42 * scale), (int)std::lround(26 * scale), pack1555(105, 128, 83));
   const int dx = x + (int)std::lround(20 * scale);
   const int dy = y + (int)std::lround(63 * scale);
   const int d = std::max(4, (int)std::lround(6 * scale));
   const int arm = std::max(12, (int)std::lround(18 * scale));
   fill_rect(fb, dx - d / 2, dy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, dy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, x + (int)std::lround(48 * scale), y + (int)std::lround(59 * scale), r, pack1555(190, 55, 113));
   stage42_fill_circle(fb, x + (int)std::lround(57 * scale), y + (int)std::lround(68 * scale), r, pack1555(190, 55, 113));
}

static void stage43_draw_gamegear(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const uint16_t shell = pack1555(37, 41, 49);
   const int rx = (int)std::lround(68 * scale);
   const int ry = (int)std::lround(34 * scale);
   stage42_fill_ellipse(fb, cx, cy, rx, ry, shell);
   fill_rect(fb, cx - (int)std::lround(31 * scale), cy - (int)std::lround(22 * scale),
             (int)std::lround(62 * scale), (int)std::lround(44 * scale), pack1555(15, 18, 24));
   fill_rect(fb, cx - (int)std::lround(25 * scale), cy - (int)std::lround(16 * scale),
             (int)std::lround(50 * scale), (int)std::lround(32 * scale), pack1555(50, 70, 78));
   const int dx = cx - (int)std::lround(47 * scale);
   const int d = std::max(4, (int)std::lround(6 * scale));
   const int arm = std::max(12, (int)std::lround(19 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(45 * scale), cy - (int)std::lround(6 * scale), r, pack1555(57, 139, 216));
   stage42_fill_circle(fb, cx + (int)std::lround(54 * scale), cy + (int)std::lround(6 * scale), r, pack1555(219, 62, 70));
}

static void stage43_draw_playstation_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const uint16_t shell = pack1555(185, 186, 190);
   const int rx = (int)std::lround(55 * scale);
   const int ry = (int)std::lround(24 * scale);
   stage42_fill_ellipse(fb, cx, cy - (int)std::lround(5 * scale), rx, ry, shell);
   stage42_fill_ellipse(fb, cx - (int)std::lround(37 * scale), cy + (int)std::lround(19 * scale),
                        (int)std::lround(18 * scale), (int)std::lround(25 * scale), shell);
   stage42_fill_ellipse(fb, cx + (int)std::lround(37 * scale), cy + (int)std::lround(19 * scale),
                        (int)std::lround(18 * scale), (int)std::lround(25 * scale), shell);
   const int dx = cx - (int)std::lround(31 * scale);
   const int d = std::max(4, (int)std::lround(6 * scale));
   const int arm = std::max(12, (int)std::lround(20 * scale));
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(5 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(29 * scale), cy - (int)std::lround(9 * scale), r, pack1555(64, 173, 119));
   stage42_fill_circle(fb, cx + (int)std::lround(40 * scale), cy, r, pack1555(213, 70, 79));
   stage42_fill_circle(fb, cx + (int)std::lround(29 * scale), cy + (int)std::lround(9 * scale), r, pack1555(59, 131, 210));
   stage42_fill_circle(fb, cx + (int)std::lround(18 * scale), cy, r, pack1555(200, 72, 161));
   stage42_fill_circle(fb, cx - (int)std::lround(12 * scale), cy + (int)std::lround(15 * scale),
                       std::max(4, (int)std::lround(7 * scale)), pack1555(45, 47, 55));
   stage42_fill_circle(fb, cx + (int)std::lround(12 * scale), cy + (int)std::lround(15 * scale),
                       std::max(4, (int)std::lround(7 * scale)), pack1555(45, 47, 55));
}

'''
src = src.replace(anchor, code + anchor, 1)
out_path.write_text(src, encoding='utf-8')
print(f'RESTORE_FUTURE_ICONS_OK {src_path} -> {out_path}')
