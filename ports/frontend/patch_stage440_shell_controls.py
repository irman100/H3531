#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage440_shell_controls.py INPUT OUTPUT')

src = Path(sys.argv[1]).read_text(encoding='utf-8')

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0: raise SystemExit('function not found: ' + signature)
    brace = text.find('{', start)
    if brace < 0: raise SystemExit('opening brace not found: ' + signature)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == '{': depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[:start] + replacement.rstrip() + text[i+1:]
    raise SystemExit('unterminated function: ' + signature)

def replace_struct(text, name, replacement):
    sig = 'struct ' + name
    start = text.find(sig)
    if start < 0: raise SystemExit('struct not found: ' + name)
    brace = text.find('{', start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == '{': depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                semi = text.find(';', i)
                if semi < 0: raise SystemExit('struct semicolon missing: ' + name)
                return text[:start] + replacement.rstrip() + text[semi+1:]
    raise SystemExit('unterminated struct: ' + name)

src = replace_struct(src, 'GamepadProfile', r'''struct GamepadProfile {
   bool loaded = false;
   JoyBinding up, down, left, right;
   JoyBinding confirm, cancel;
   JoyBinding x, y;
   JoyBinding start, select;
   JoyBinding l, r, l2, r2;
   JoyBinding lx_minus, lx_plus, ly_minus, ly_plus;
   JoyBinding menu;
   std::string path;
};''')

old = '''   pad.profile.l       = h3531_profile_bind(kv, "l");\n   pad.profile.r       = h3531_profile_bind(kv, "r");\n   pad.profile.menu    = h3531_profile_bind(kv, "menu_toggle");'''
new = '''   pad.profile.l       = h3531_profile_bind(kv, "l");\n   pad.profile.r       = h3531_profile_bind(kv, "r");\n   pad.profile.l2      = h3531_profile_bind(kv, "l2");\n   pad.profile.r2      = h3531_profile_bind(kv, "r2");\n   pad.profile.lx_minus = h3531_profile_bind(kv, "l_x_minus");\n   pad.profile.lx_plus  = h3531_profile_bind(kv, "l_x_plus");\n   pad.profile.ly_minus = h3531_profile_bind(kv, "l_y_minus");\n   pad.profile.ly_plus  = h3531_profile_bind(kv, "l_y_plus");\n   pad.profile.menu    = h3531_profile_bind(kv, "menu_toggle");'''
if old not in src: raise SystemExit('profile load anchor missing')
src = src.replace(old, new, 1)

old_enum = '''enum class Action {\n   None,\n   PrevGame,\n   NextGame,\n   PrevSystem,\n   NextSystem,\n   Launch,\n   ServiceMenu,\n   Rescan,\n   Exit\n};'''
new_enum = '''enum class Action {\n   None,\n   PrevGame,\n   NextGame,\n   PrevSystem,\n   NextSystem,\n   PrevSystemDirect,\n   NextSystemDirect,\n   ToggleFavorite,\n   Search,\n   Launch,\n   ServiceMenu,\n   Rescan,\n   Exit\n};'''
if old_enum not in src: raise SystemExit('Action enum anchor missing')
src = src.replace(old_enum, new_enum, 1)

src = replace_function(src, 'static Action keyboard_action(unsigned code)', r'''static Action keyboard_action(unsigned code)
{
   switch (code)
   {
      case KEY_LEFT: return Action::PrevGame;
      case KEY_RIGHT: return Action::NextGame;
      case KEY_UP: return Action::PrevSystem;
      case KEY_DOWN: return Action::NextSystem;
      case KEY_ENTER: case KEY_SPACE: return Action::Launch;
      case KEY_F1: return Action::ServiceMenu;
      case KEY_F3: return Action::Search;
      case KEY_F: return Action::ToggleFavorite;
      case KEY_F5: case KEY_R: return Action::Rescan;
      case KEY_ESC: case KEY_BACKSPACE: return Action::Exit;
      default: return Action::None;
   }
}''')

src = replace_function(src, 'static Action h3531_profile_button_action(const GamepadInput &pad, unsigned button)', r'''static Action h3531_profile_button_action(const GamepadInput &pad, unsigned button)
{
   const GamepadProfile &p = pad.profile;

   if (p.loaded)
   {
      if (h3531_button_match(p.left, button)) return Action::PrevGame;
      if (h3531_button_match(p.right, button)) return Action::NextGame;
      if (h3531_button_match(p.up, button)) return Action::PrevSystem;
      if (h3531_button_match(p.down, button)) return Action::NextSystem;

      if (h3531_button_match(p.confirm, button)) return Action::Launch;
      if (h3531_button_match(p.cancel, button)) return Action::Exit;
      if (h3531_button_match(p.menu, button) ||
          (!h3531_binding_valid(p.menu) && h3531_button_match(p.start, button)))
         return Action::ServiceMenu;

      /* Stayplaytion home controls: shoulders page systems directly. */
      if (h3531_button_match(p.l, button)) return Action::PrevSystemDirect;
      if (h3531_button_match(p.r, button)) return Action::NextSystemDirect;

      /* Either trigger toggles the highlighted ROM in Favorites. */
      if (h3531_button_match(p.l2, button) ||
          h3531_button_match(p.r2, button))
         return Action::ToggleFavorite;

      /* Physical top/Y opens shell search. */
      if (h3531_button_match(p.y, button)) return Action::Search;
      return Action::None;
   }

   if (button == 0) return Action::Launch;
   if (button == 1) return Action::Exit;
   if (button == 3) return Action::Search;
   if (button == 4) return Action::PrevSystemDirect;
   if (button == 5) return Action::NextSystemDirect;
   if (button == 7 || button == 9) return Action::ServiceMenu;
   return Action::None;
}''')

src = replace_function(src, 'static Action h3531_profile_axis_action(GamepadInput &pad,', r'''static Action h3531_profile_axis_action(GamepadInput &pad,
      unsigned axis, int16_t value)
{
   if (axis >= H3531_JS_MAX_AXES) return Action::None;

   const int zone = h3531_axis_zone(value);
   const int old = pad.axis_zone[axis];
   pad.axis_zone[axis] = zone;
   if (zone == 0 || zone == old) return Action::None;

   const GamepadProfile &p = pad.profile;
   if (p.loaded)
   {
      /* D-pad profile remains authoritative. */
      if (h3531_axis_match(p.left, axis, zone)) return Action::PrevGame;
      if (h3531_axis_match(p.right, axis, zone)) return Action::NextGame;
      if (h3531_axis_match(p.up, axis, zone)) return Action::PrevSystem;
      if (h3531_axis_match(p.down, axis, zone)) return Action::NextSystem;

      /* Left analog stick mirrors the four shell navigation arrows. */
      if (h3531_axis_match(p.lx_minus, axis, zone)) return Action::PrevGame;
      if (h3531_axis_match(p.lx_plus, axis, zone)) return Action::NextGame;
      if (h3531_axis_match(p.ly_minus, axis, zone)) return Action::PrevSystem;
      if (h3531_axis_match(p.ly_plus, axis, zone)) return Action::NextSystem;

      if (h3531_axis_match(p.confirm, axis, zone)) return Action::Launch;
      if (h3531_axis_match(p.cancel, axis, zone)) return Action::Exit;
      if (h3531_axis_match(p.menu, axis, zone)) return Action::ServiceMenu;
      if (h3531_axis_match(p.l2, axis, zone) || h3531_axis_match(p.r2, axis, zone))
         return Action::ToggleFavorite;
      return Action::None;
   }

   if (axis == 0 || axis == 6)
      return zone < 0 ? Action::PrevGame : Action::NextGame;
   if (axis == 1 || axis == 7)
      return zone < 0 ? Action::PrevSystem : Action::NextSystem;
   return Action::None;
}''')


src = replace_function(src, 'static std::vector<size_t> visible_systems(const std::vector<SystemDef> &systems)', r'''static std::vector<size_t> visible_systems(const std::vector<SystemDef> &systems)
{
   std::vector<size_t> out;
   for (size_t i = 0; i < systems.size(); ++i)
      if (!systems[i].games.empty() || lower(systems[i].name) == "favorites")
         out.push_back(i);
   return out;
}''')

required = [
    'PrevSystemDirect', 'NextSystemDirect', 'ToggleFavorite', 'Action::Search',
    'l_x_minus', 'l_y_minus', 'Physical top/Y opens shell search'
]
for marker in required:
    if marker not in src: raise SystemExit('missing Stage4.40 marker: ' + marker)

Path(sys.argv[2]).write_text(src, encoding='utf-8')
print('STAGE440_SHELL_CONTROLS_PATCH_OK')
