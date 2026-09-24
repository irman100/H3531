/* H3531 Game Frontend Stage 4.0
 *
 * Lightweight EmulationStation-compatible TV frontend for Hi3531.
 * - direct /dev/fb0 A1R5G5B5 rendering (no SDL/OpenGL)
 * - direct evdev keyboard navigation
 * - es_systems.cfg system -> ROM path/extensions/launch command routing
 * - gamelist.xml metadata + PNG/JPEG artwork via stb_image
 * - automatic RetroArch core selection is encoded in each system command
 *
 * Third-party libraries are fetched at pinned revisions by CI:
 *   tinyxml2  (zlib license)
 *   stb_image / stb_easy_font (MIT/public-domain dual style)
 */

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "tinyxml2.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define STB_EASY_FONT_IMPLEMENTATION
#include "stb_easy_font.h"

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

static const char *kDefaultConfig = "/mnt/usb/H3531/APPS/gamefront/es_systems.cfg";
static const char *kRetroArchMenu = "/mnt/usb/H3531/APPS/retroarch/RETROARCH.APP";
static const char *kFramebuffer = "/dev/fb0";

struct Game {
   std::string rom_path;
   std::string title;
   std::string image_path;
   std::string description;
};

struct SystemDef {
   std::string name;
   std::string fullname;
   std::string path;
   std::vector<std::string> extensions;
   std::string command;
   std::string theme;
   std::vector<Game> games;
};

struct Fb {
   int fd = -1;
   uint8_t *mem = nullptr;
   size_t len = 0;
   fb_fix_screeninfo fix{};
   fb_var_screeninfo var{};
   unsigned w = 0;
   unsigned h = 0;
   unsigned stride = 0;
};

static const int H3531_MAX_GAMEPADS = 4;
static const int H3531_JS_MAX_AXES = 32;
static const int H3531_JS_MAX_BUTTONS = 32;
static const char *H3531_RA_AUTOCONFIG =
      "/mnt/usb/H3531/USER/retroarch/autoconfig";

struct JoyBinding {
   int button = -1;
   int axis = -1;
   int axis_dir = 0;
};

struct GamepadProfile {
   bool loaded = false;
   JoyBinding up, down, left, right;
   JoyBinding confirm, cancel;
   JoyBinding x, y;
   JoyBinding start, select;
   JoyBinding l, r;
   JoyBinding menu;
   std::string path;
};

struct GamepadInput {
   int fd = -1;
   std::string path;
   std::string name;
   bool buttons[H3531_JS_MAX_BUTTONS]{};
   int16_t axes[H3531_JS_MAX_AXES]{};
   int axis_zone[H3531_JS_MAX_AXES]{};
   GamepadProfile profile;
};

struct Input {
   int fd = -1;                 // keyboard evdev
   std::string path;
   GamepadInput pads[H3531_MAX_GAMEPADS];
   uint64_t next_scan_ms = 0;
};

static std::string h3531_profile_trim(const std::string &v)
{
   size_t a = 0, b = v.size();
   while (a < b && std::isspace((unsigned char)v[a])) ++a;
   while (b > a && std::isspace((unsigned char)v[b - 1])) --b;
   return v.substr(a, b - a);
}

static std::string h3531_profile_unquote(std::string v)
{
   v = h3531_profile_trim(v);
   if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
      return v.substr(1, v.size() - 2);
   return v;
}

static std::string h3531_profile_filename(const std::string &device)
{
   static const char *bad = "~#%&*{}\\:[]?/|'\"";
   std::string out = device;
   for (char &c : out)
      if (std::strchr(bad, c)) c = '_';
   return out + ".cfg";
}

static bool h3531_read_cfg(const std::string &path,
      std::map<std::string, std::string> &kv)
{
   std::ifstream in(path);
   std::string line;
   if (!in) return false;

   while (std::getline(in, line))
   {
      bool quoted = false;
      for (size_t i = 0; i < line.size(); ++i)
      {
         if (line[i] == '"') quoted = !quoted;
         else if (line[i] == '#' && !quoted)
         {
            line.resize(i);
            break;
         }
      }

      const size_t p = line.find('=');
      if (p == std::string::npos) continue;

      const std::string key = h3531_profile_trim(line.substr(0, p));
      const std::string val = h3531_profile_unquote(line.substr(p + 1));
      if (!key.empty()) kv[key] = val;
   }
   return true;
}

static JoyBinding h3531_profile_bind(
      const std::map<std::string, std::string> &kv,
      const std::string &base)
{
   JoyBinding b;
   auto ib = kv.find("input_" + base + "_btn");
   if (ib != kv.end() && !ib->second.empty() && ib->second != "nul")
   {
      char *end = nullptr;
      long n = std::strtol(ib->second.c_str(), &end, 10);
      if (end && *end == '\0' && n >= 0 && n < H3531_JS_MAX_BUTTONS)
         b.button = (int)n;
   }

   auto ia = kv.find("input_" + base + "_axis");
   if (ia != kv.end() && ia->second.size() >= 2 &&
       (ia->second[0] == '+' || ia->second[0] == '-'))
   {
      char *end = nullptr;
      long n = std::strtol(ia->second.c_str() + 1, &end, 10);
      if (end && *end == '\0' && n >= 0 && n < H3531_JS_MAX_AXES)
      {
         b.axis = (int)n;
         b.axis_dir = ia->second[0] == '-' ? -1 : 1;
      }
   }
   return b;
}

static bool h3531_binding_valid(const JoyBinding &b)
{
   return b.button >= 0 || (b.axis >= 0 && b.axis_dir != 0);
}

static void h3531_profile_load(GamepadInput &pad)
{
   std::map<std::string, std::string> kv;
   const std::string file = std::string(H3531_RA_AUTOCONFIG) + "/" +
         h3531_profile_filename(pad.name);

   pad.profile = GamepadProfile{};
   pad.profile.path = file;

   if (!h3531_read_cfg(file, kv))
   {
      fprintf(stderr, "[GAMEFRONT] no RetroArch profile for %s (%s); bootstrap mapping active\n",
            pad.name.c_str(), file.c_str());
      return;
   }

   auto dev = kv.find("input_device");
   if (dev != kv.end() && !dev->second.empty() && dev->second != pad.name)
   {
      fprintf(stderr, "[GAMEFRONT] profile device mismatch: %s != %s\n",
            dev->second.c_str(), pad.name.c_str());
      return;
   }

   auto drv = kv.find("input_driver");
   if (drv != kv.end() && !drv->second.empty() && drv->second != "linuxraw")
   {
      fprintf(stderr, "[GAMEFRONT] profile driver mismatch: %s\n",
            drv->second.c_str());
      return;
   }

   pad.profile.up      = h3531_profile_bind(kv, "up");
   pad.profile.down    = h3531_profile_bind(kv, "down");
   pad.profile.left    = h3531_profile_bind(kv, "left");
   pad.profile.right   = h3531_profile_bind(kv, "right");

   /* RetroPad convention: physical South/bottom maps to B,
    * East/right maps to A, West/left maps to Y, North/top maps to X. */
   pad.profile.confirm = h3531_profile_bind(kv, "b");
   pad.profile.cancel  = h3531_profile_bind(kv, "a");
   pad.profile.x       = h3531_profile_bind(kv, "y");
   pad.profile.y       = h3531_profile_bind(kv, "x");

   pad.profile.start   = h3531_profile_bind(kv, "start");
   pad.profile.select  = h3531_profile_bind(kv, "select");
   pad.profile.l       = h3531_profile_bind(kv, "l");
   pad.profile.r       = h3531_profile_bind(kv, "r");
   pad.profile.menu    = h3531_profile_bind(kv, "menu_toggle");

   pad.profile.loaded =
      h3531_binding_valid(pad.profile.up) &&
      h3531_binding_valid(pad.profile.down) &&
      h3531_binding_valid(pad.profile.left) &&
      h3531_binding_valid(pad.profile.right) &&
      h3531_binding_valid(pad.profile.confirm);

   fprintf(stderr, "[GAMEFRONT] RetroArch linuxraw profile %s: %s\n",
         pad.profile.loaded ? "loaded" : "incomplete", file.c_str());
}

static uint64_t input_now_ms()
{
   timeval tv{};
   gettimeofday(&tv, nullptr);
   return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static bool contains_ci(const char *s, const char *needle)
{
   if (!s || !needle || !*needle) return false;
   std::string a = lower(s), b = lower(needle);
   return a.find(b) != std::string::npos;
}

#define BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define NBITS(x) (((x) + BITS_PER_LONG - 1U) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) (((arr)[(unsigned)(bit) / BITS_PER_LONG] >> ((unsigned)(bit) % BITS_PER_LONG)) & 1UL)

static bool keyboard_capable(int fd, const char *name)
{
   unsigned long evbits[NBITS(EV_MAX + 1)]{};
   unsigned long keybits[NBITS(KEY_MAX + 1)]{};
   unsigned score = 0;
   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
       !TEST_BIT(EV_KEY, evbits)) return false;
   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;

   const unsigned keys[] = {
      KEY_ENTER, KEY_ESC, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT
   };
   for (unsigned k : keys) if (TEST_BIT(k, keybits)) ++score;

   if (contains_ci(name, "keyboard") || contains_ci(name, "kbd"))
      return score >= 4;
   return score >= 6;
}

static bool input_keyboard_open(Input &in)
{
   const char *forced = getenv("GAMEFRONT_INPUT");
   if (forced && *forced)
   {
      int fd = open(forced, O_RDONLY | O_NONBLOCK);
      if (fd >= 0)
      {
         char name[128]{};
         if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
            snprintf(name, sizeof(name), "forced");
         if (keyboard_capable(fd, name))
         {
            in.fd = fd;
            in.path = forced;
            fprintf(stderr, "[GAMEFRONT] keyboard forced %s\n", forced);
            return true;
         }
         close(fd);
      }
   }

   for (int i = 0; i < 64; ++i)
   {
      char path[64], name[128]{};
      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      int fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0) continue;
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "event%d", i);
      if (keyboard_capable(fd, name))
      {
         in.fd = fd;
         in.path = path;
         fprintf(stderr, "[GAMEFRONT] keyboard ready %s (%s)\n", path, name);
         return true;
      }
      close(fd);
   }
   return false;
}

static bool input_js_path_in_use(const Input &in, const char *path)
{
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0 && in.pads[i].path == path) return true;
   return false;
}

static void input_scan_gamepads(Input &in)
{
   for (int js = 0; js < H3531_MAX_GAMEPADS; ++js)
   {
      int slot = -1;
      for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
         if (in.pads[p].fd < 0) { slot = p; break; }
      if (slot < 0) return;

      char path[64];
      snprintf(path, sizeof(path), "/dev/input/js%d", js);
      if (input_js_path_in_use(in, path)) continue;

      int fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0) continue;

      char name[128]{};
      if (ioctl(fd, JSIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "js%d", js);

      GamepadInput &pad = in.pads[slot];
      pad = GamepadInput{};
      pad.fd = fd;
      pad.path = path;
      pad.name = name;
      h3531_profile_load(pad);

      /* Consume JS_EVENT_INIT state immediately. */
      for (;;)
      {
         js_event ev{};
         const ssize_t n = read(fd, &ev, sizeof(ev));
         if (n != (ssize_t)sizeof(ev)) break;
         const unsigned type = ev.type & ~JS_EVENT_INIT;
         if (type == JS_EVENT_BUTTON && ev.number < H3531_JS_MAX_BUTTONS)
            pad.buttons[ev.number] = ev.value != 0;
         else if (type == JS_EVENT_AXIS && ev.number < H3531_JS_MAX_AXES)
            pad.axes[ev.number] = ev.value;
      }

      fprintf(stderr, "[GAMEFRONT] linuxraw-compatible gamepad%d ready %s (%s) profile=%s\n",
            slot + 1, path, name, pad.profile.loaded ? "yes" : "bootstrap");
   }
}

static void input_rescan(Input &in)
{
   const uint64_t now = input_now_ms();
   if (now < in.next_scan_ms) return;
   in.next_scan_ms = now + 1000ULL;

   if (in.fd < 0)
      input_keyboard_open(in);

   input_scan_gamepads(in);
}

static bool input_open(Input &in)
{
   in.next_scan_ms = 0;
   input_rescan(in);

   if (in.fd < 0)
      fprintf(stderr, "[GAMEFRONT] keyboard absent; hotplug scan active\n");

   bool have_pad = false;
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0) have_pad = true;

   if (!have_pad)
      fprintf(stderr, "[GAMEFRONT] /dev/input/js* gamepad absent; hotplug scan active\n");

   return in.fd >= 0 || have_pad;
}

static void input_close(Input &in)
{
   if (in.fd >= 0) close(in.fd);
   in.fd = -1;
   in.path.clear();

   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
   {
      if (in.pads[i].fd >= 0) close(in.pads[i].fd);
      in.pads[i] = GamepadInput{};
   }
   in.next_scan_ms = 0;
}

static Action keyboard_action(unsigned code)
{
   switch (code)
   {
      case KEY_LEFT: return Action::PrevGame;
      case KEY_RIGHT: return Action::NextGame;
      case KEY_UP: return Action::PrevSystem;
      case KEY_DOWN: return Action::NextSystem;
      case KEY_ENTER: case KEY_SPACE: return Action::Launch;
      case KEY_F1: return Action::ServiceMenu;
      case KEY_F5: case KEY_R: return Action::Rescan;
      case KEY_ESC: case KEY_BACKSPACE: return Action::Exit;
      default: return Action::None;
   }
}

static bool h3531_button_match(const JoyBinding &b, unsigned button)
{
   return b.button >= 0 && (unsigned)b.button == button;
}

static bool h3531_axis_match(const JoyBinding &b, unsigned axis, int zone)
{
   return b.axis >= 0 && b.axis_dir != 0 &&
          (unsigned)b.axis == axis && b.axis_dir == zone;
}

static Action h3531_profile_button_action(const GamepadInput &pad, unsigned button)
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

      if (h3531_button_match(p.l, button)) return Action::PrevSystem;
      if (h3531_button_match(p.r, button)) return Action::NextSystem;
      return Action::None;
   }

   /* Bootstrap mapping before a standard profile exists.
    * Linux joystick API normally exposes the Xbox/GameStick face cluster as
    * A=0, B=1, X=2, Y=3 and Start as 7/9. */
   if (button == 0) return Action::Launch;
   if (button == 1) return Action::Exit;
   if (button == 7 || button == 9) return Action::ServiceMenu;
   if (button == 4) return Action::PrevSystem;
   if (button == 5) return Action::NextSystem;
   return Action::None;
}

static int h3531_axis_zone(int16_t value)
{
   if (value < -16000) return -1;
   if (value > 16000) return 1;
   return 0;
}

static Action h3531_profile_axis_action(GamepadInput &pad,
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
      if (h3531_axis_match(p.left, axis, zone)) return Action::PrevGame;
      if (h3531_axis_match(p.right, axis, zone)) return Action::NextGame;
      if (h3531_axis_match(p.up, axis, zone)) return Action::PrevSystem;
      if (h3531_axis_match(p.down, axis, zone)) return Action::NextSystem;
      if (h3531_axis_match(p.confirm, axis, zone)) return Action::Launch;
      if (h3531_axis_match(p.cancel, axis, zone)) return Action::Exit;
      if (h3531_axis_match(p.menu, axis, zone)) return Action::ServiceMenu;
      return Action::None;
   }

   /* Common Linux joystick layouts:
    * axes 0/1 = primary stick or D-pad;
    * axes 6/7 = hat/D-pad on many USB GameStick/XInput-like pads. */
   if (axis == 0 || axis == 6)
      return zone < 0 ? Action::PrevGame : Action::NextGame;
   if (axis == 1 || axis == 7)
      return zone < 0 ? Action::PrevSystem : Action::NextSystem;

   return Action::None;
}

static void gamepad_disconnect(GamepadInput &pad)
{
   if (pad.fd >= 0) close(pad.fd);
   fprintf(stderr, "[GAMEFRONT] joystick disconnected %s; waiting for hotplug\n",
         pad.path.empty() ? "<unknown>" : pad.path.c_str());
   pad = GamepadInput{};
}

static Action input_poll(Input &in)
{
   input_rescan(in);

   if (in.fd >= 0)
   {
      input_event ev{};
      for (;;)
      {
         ssize_t n = read(in.fd, &ev, sizeof(ev));
         if (n == (ssize_t)sizeof(ev))
         {
            if (ev.type != EV_KEY || (ev.value != 1 && ev.value != 2))
               continue;
            Action a = keyboard_action(ev.code);
            if (a != Action::None) return a;
            continue;
         }
         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR))
            break;
         if (n <= 0)
         {
            fprintf(stderr, "[GAMEFRONT] keyboard disconnected %s; waiting for hotplug\n",
                  in.path.empty() ? "<unknown>" : in.path.c_str());
            close(in.fd);
            in.fd = -1;
            in.path.clear();
            in.next_scan_ms = 0;
         }
         break;
      }
   }

   for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
   {
      GamepadInput &pad = in.pads[p];
      if (pad.fd < 0) continue;

      for (;;)
      {
         js_event ev{};
         const ssize_t n = read(pad.fd, &ev, sizeof(ev));

         if (n == (ssize_t)sizeof(ev))
         {
            const unsigned type = ev.type & ~JS_EVENT_INIT;

            if (type == JS_EVENT_BUTTON &&
                ev.number < H3531_JS_MAX_BUTTONS)
            {
               pad.buttons[ev.number] = ev.value != 0;
               if (ev.value)
               {
                  Action a = h3531_profile_button_action(pad, ev.number);
                  if (a != Action::None) return a;
               }
            }
            else if (type == JS_EVENT_AXIS &&
                     ev.number < H3531_JS_MAX_AXES)
            {
               pad.axes[ev.number] = ev.value;
               Action a = h3531_profile_axis_action(
                     pad, ev.number, ev.value);
               if (a != Action::None) return a;
            }
            continue;
         }

         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR))
            break;

         if (n <= 0)
         {
            gamepad_disconnect(pad);
            in.next_scan_ms = 0;
         }
         break;
      }
   }

   return Action::None;
}

static std::vector<size_t> visible_systems(const std::vector<SystemDef> &systems)
{
   std::vector<size_t> out;
   for (size_t i = 0; i < systems.size(); ++i)
      if (!systems[i].games.empty()) out.push_back(i);
   return out;
}

static void draw_fallback_card(Fb &fb, const SystemDef &sys, int x, int y, int w, int h)
{
   uint16_t c = pack1555(45, 58, 78);
   uint16_t c2 = pack1555(66, 86, 116);
   fill_rect(fb, x, y, w, h, c);
   fill_rect(fb, x + 8, y + 8, w - 16, h - 16, c2);
   std::string label = sys.name.empty() ? "GAME" : sys.name;
   std::transform(label.begin(), label.end(), label.begin(), [](unsigned char ch){ return (char)std::toupper(ch); });
   int scale = 4;
   int tw = text_width(label, scale);
   draw_text(fb, x + (w - tw) / 2, y + h / 2 - 16, label, scale, pack1555(235, 240, 248));
}

static void draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos)
{
   const uint16_t bg = pack1555(14, 18, 26);
   const uint16_t panel = pack1555(24, 30, 42);
   const uint16_t accent = pack1555(78, 170, 255);
   const uint16_t text = pack1555(236, 240, 246);
   const uint16_t dim = pack1555(145, 154, 170);
   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);

   draw_text(fb, 38, 26, "H3531 GAME LIBRARY", 3, text);
   draw_text(fb, (int)fb.w - 320, 32, "RetroArch / libretro", 2, dim);

   if (vis.empty())
   {
      draw_text(fb, 80, 250, "NO GAMES FOUND", 5, text);
      draw_text(fb, 82, 330, "Put ROMs in /mnt/usb/games/nes or /mnt/usb/games/md", 2, dim);
      draw_text(fb, 82, 370, "F5: rescan   F1: RetroArch service menu   ESC: exit", 2, dim);
      return;
   }

   const SystemDef &sys = systems[vis[visible_pos]];
   int sys_y = 82;
   int center = (int)fb.w / 2;
   for (int rel = -2; rel <= 2; ++rel)
   {
      int p = (int)visible_pos + rel;
      while (p < 0) p += (int)vis.size();
      while (p >= (int)vis.size()) p -= (int)vis.size();
      const SystemDef &s = systems[vis[(size_t)p]];
      int scale = rel == 0 ? 3 : 2;
      std::string name = ellipsize(s.fullname, rel == 0 ? 32 : 20);
      int tw = text_width(name, scale);
      int x = center + rel * 245 - tw / 2;
      draw_text(fb, x, sys_y + (rel == 0 ? 0 : 7), name, scale, rel == 0 ? accent : dim);
   }
   fill_rect(fb, center - 110, 120, 220, 3, accent);

   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;
   const Game &selected = sys.games[game_pos];

   int card_y = 160;
   int selected_w = 270, selected_h = 380;
   int small_w = 185, small_h = 280;
   for (int rel = -2; rel <= 2; ++rel)
   {
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      const Game &g = sys.games[(size_t)gp];
      bool sel = rel == 0;
      int w = sel ? selected_w : small_w;
      int h = sel ? selected_h : small_h;
      int x = center + rel * 250 - w / 2;
      int y = card_y + (sel ? 0 : 52);
      fill_rect(fb, x - 8, y - 8, w + 16, h + 16, panel);
      if (!draw_image(fb, g.image_path, x, y, w, h)) draw_fallback_card(fb, sys, x, y, w, h);
      frame_rect(fb, x - 8, y - 8, w + 16, h + 16, sel ? 5 : 2, sel ? accent : pack1555(60, 70, 86));
      if (!sel) draw_text(fb, x, y + h + 14, ellipsize(g.title, 20), 1, dim);
   }

   std::string title = ellipsize(selected.title, 48);
   int tw = text_width(title, 3);
   draw_text(fb, center - tw / 2, 570, title, 3, text);
   std::string counter = std::to_string(game_pos + 1) + " / " + std::to_string(sys.games.size());
   int cw = text_width(counter, 2);
   draw_text(fb, center - cw / 2, 612, counter, 2, dim);

   fill_rect(fb, 0, (int)fb.h - 56, (int)fb.w, 56, panel);
   draw_text(fb, 28, (int)fb.h - 38,
         "LEFT/RIGHT game   UP/DOWN system   ENTER play   F5 rescan   F1 RetroArch   ESC exit",
         2, dim);

#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
}

static int run_external(Fb &fb, Input &in, const std::string &cmd)
{
   fprintf(stderr, "[GAMEFRONT] launch: %s\n", cmd.c_str());
   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, pack1555(0,0,0));
   fb_close(fb);
   input_close(in);
   sync();
   int rc = system(cmd.c_str());
   fprintf(stderr, "[GAMEFRONT] child returned rc=%d\n", rc);
   if (!fb_open(fb)) return -1;
   input_open(in);
   return rc;
}

static int self_test(const std::string &cfg)
{
   std::vector<SystemDef> systems;
   if (!load_systems(cfg, systems)) return 2;
   scan_all(systems);
   size_t total = 0;
   for (const auto &s : systems)
   {
      printf("SYSTEM name=%s fullname=%s games=%zu path=%s\n",
            s.name.c_str(), s.fullname.c_str(), s.games.size(), s.path.c_str());
      total += s.games.size();
      for (const auto &g : s.games)
         printf("ROUTE system=%s rom=%s command=%s\n",
               s.name.c_str(), g.rom_path.c_str(), launch_command(s, g).c_str());
   }
   printf("SELF_TEST_OK systems=%zu games=%zu\n", systems.size(), total);
   return systems.empty() ? 3 : 0;
}

int main(int argc, char **argv)
{
   std::string cfg = kDefaultConfig;
   if (argc >= 2 && std::string(argv[1]) == "--self-test")
   {
      if (argc >= 3) cfg = argv[2];
      return self_test(cfg);
   }
   if (argc >= 3 && std::string(argv[1]) == "--config") cfg = argv[2];

   std::vector<SystemDef> systems;
   if (!load_systems(cfg, systems)) return 2;
   scan_all(systems);

   Fb fb;
   Input in;
   if (!fb_open(fb)) return 3;
   input_open(in);

   size_t visible_pos = 0;
   size_t game_pos = 0;
   bool redraw = true;
   bool running = true;

   while (running)
   {
      auto vis = visible_systems(systems);
      if (!vis.empty() && visible_pos >= vis.size()) visible_pos = 0;
      if (!vis.empty())
      {
         const auto &games = systems[vis[visible_pos]].games;
         if (!games.empty() && game_pos >= games.size()) game_pos = 0;
      }
      if (redraw)
      {
         draw_ui(fb, systems, visible_pos, vis, game_pos);
         redraw = false;
      }

      Action a = input_poll(in);
      switch (a)
      {
         case Action::PrevGame:
            if (!vis.empty())
            {
               auto &g = systems[vis[visible_pos]].games;
               if (!g.empty()) game_pos = game_pos ? game_pos - 1 : g.size() - 1;
               redraw = true;
            }
            break;
         case Action::NextGame:
            if (!vis.empty())
            {
               auto &g = systems[vis[visible_pos]].games;
               if (!g.empty()) game_pos = (game_pos + 1) % g.size();
               redraw = true;
            }
            break;
         case Action::PrevSystem:
            if (!vis.empty()) { visible_pos = visible_pos ? visible_pos - 1 : vis.size() - 1; game_pos = 0; redraw = true; }
            break;
         case Action::NextSystem:
            if (!vis.empty()) { visible_pos = (visible_pos + 1) % vis.size(); game_pos = 0; redraw = true; }
            break;
         case Action::Launch:
            if (!vis.empty())
            {
               SystemDef &s = systems[vis[visible_pos]];
               if (!s.games.empty())
               {
                  run_external(fb, in, launch_command(s, s.games[game_pos]));
                  redraw = true;
               }
            }
            break;
         case Action::ServiceMenu:
            run_external(fb, in, std::string(kRetroArchMenu)); redraw = true; break;
         case Action::Rescan:
            scan_all(systems); visible_pos = 0; game_pos = 0; redraw = true; break;
         case Action::Exit: running = false; break;
         case Action::None: default: break;
      }
      usleep(10000);
   }

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, pack1555(0,0,0));
   input_close(in);
   fb_close(fb);
   fprintf(stderr, "[GAMEFRONT] clean exit\n");
   return 0;
}
