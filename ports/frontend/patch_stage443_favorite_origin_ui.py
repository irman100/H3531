#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage443_favorite_origin_ui.py INPUT OUTPUT')

src = Path(sys.argv[1]).read_text(encoding='utf-8')

old = '''   for (const auto &s : systems)
      for (const auto &g : s.games)
         if (stage413_favorite(g))
            fav.games.push_back(g);'''
new = '''   for (const auto &s : systems)
      for (const auto &g : s.games)
         if (stage413_favorite(g))
         {
            Game fg = g;
            fg.origin_system = s.name;
            fg.origin_fullname = s.fullname;
            fg.origin_theme = s.theme;
            fav.games.push_back(fg);
         }'''
if old not in src:
    raise SystemExit('Stage4.43 favorites copy anchor missing')
src = src.replace(old, new, 1)

anchor = '''static size_t stage441_visible_pos_for_system(const std::vector<SystemDef> &systems,
      size_t sys_index)'''
helper = r'''static SystemDef stage443_visual_system(const SystemDef &container, const Game &g)
{
   if (!stage441_is_favorites(container) || g.origin_system.empty())
      return container;

   SystemDef out;
   out.name = g.origin_system;
   out.fullname = g.origin_fullname;
   out.theme = g.origin_theme;
   return out;
}

'''
if anchor not in src:
    raise SystemExit('Stage4.43 visual-system insertion anchor missing')
src = src.replace(anchor, helper + anchor, 1)

old_star = '   const int s = selected ? 6 : 4;'
if old_star not in src:
    raise SystemExit('Stage4.43 star size anchor missing')
src = src.replace(old_star, '   const int s = selected ? 4 : 3;', 1)

start = src.find('static void stage42_draw_games(')
end = src.find('static void stage42_draw_ui(', start)
if start < 0 or end < 0:
    raise SystemExit('Stage4.43 draw-games function anchors missing')
fn = src[start:end]
needle = '      const Game &g = sys.games[cv.index];\n'
if needle not in fn:
    raise SystemExit('Stage4.43 per-card game anchor missing')
fn = fn.replace(needle, needle + '      const SystemDef visual_sys = stage443_visual_system(sys, g);\n', 1)
fn = fn.replace('const std::string stage427_name = lower(sys.name);',
                'const std::string stage427_name = lower(visual_sys.name);', 1)
fn = fn.replace('const std::string media = stage415_media_asset(sys);',
                'const std::string media = stage415_media_asset(visual_sys);', 1)
fn = fn.replace('const Stage415LabelRect lr = stage416_label_rect(sys, mr);',
                'const Stage415LabelRect lr = stage416_label_rect(visual_sys, mr);', 1)
fn = fn.replace('const std::string stage417_name = lower(sys.name);',
                'const std::string stage417_name = lower(visual_sys.name);', 1)
fn = fn.replace('stage415_draw_rom_label(fb, sys, g, lr, selected);',
                'stage415_draw_rom_label(fb, visual_sys, g, lr, selected);', 1)
fn = fn.replace('stage441_draw_big_star(fb, mr.x + mr.w / 2, mr.y - 20, true);',
                'stage441_draw_big_star(fb, mr.x + mr.w / 2, mr.y - 26, true);', 1)

bottom_old = '''   const Game &g = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(g.title, 3, 700);'''
bottom_new = '''   const Game &g = sys.games[game_pos];
   const SystemDef visual_sys = stage443_visual_system(sys, g);
   const std::string title = stage42_ellipsize_px(g.title, 3, 700);'''
if bottom_old not in fn:
    raise SystemExit('Stage4.43 selected game anchor missing')
fn = fn.replace(bottom_old, bottom_new, 1)
fn = fn.replace('const std::string meta = stage415_system_label(sys) + "  *  " +',
                'const std::string meta = stage415_system_label(visual_sys) + "  *  " +', 1)
src = src[:start] + fn + src[end:]

layout = '   printf("LAYOUT_TEST_OK\\n");\n'
marker = ('   printf("STAGE443_FAVORITES origin-system-media console-correct-labels '
          'selected-star44-no-overlap\\n");\n')
if layout not in src:
    raise SystemExit('Stage4.43 layout marker anchor missing')
src = src.replace(layout, marker + layout, 1)

required = [
    'fg.origin_system = s.name',
    'stage443_visual_system',
    'stage415_media_asset(visual_sys)',
    'stage416_label_rect(visual_sys, mr)',
    'stage415_draw_rom_label(fb, visual_sys, g, lr, selected)',
    'stage415_system_label(visual_sys)',
    'selected ? 4 : 3',
    'mr.y - 26',
    'STAGE443_FAVORITES origin-system-media console-correct-labels selected-star44-no-overlap',
]
for marker in required:
    if marker not in src:
        raise SystemExit('missing Stage4.43 marker: ' + marker)

Path(sys.argv[2]).write_text(src, encoding='utf-8')
print('STAGE443_FAVORITE_ORIGIN_UI_PATCH_OK')
