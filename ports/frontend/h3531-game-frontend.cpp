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
#include <map>
#include <set>
#include <sstream>
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

struct GamepadInput {
   int fd = -1;
   std::string path;
   std::string name;
   input_absinfo absinfo[ABS_MAX + 1]{};
   bool abs_valid[ABS_MAX + 1]{};
   int axis_zone[ABS_MAX + 1]{};
};

struct Input {
   int fd = -1;                 // keyboard evdev; kept for Stage4.30 key capture
   std::string path;
   GamepadInput pads[H3531_MAX_GAMEPADS];
   uint64_t next_scan_ms = 0;
};

enum class Action {
   None,
   PrevGame,
   NextGame,
   PrevSystem,
   NextSystem,
   Launch,
   ServiceMenu,
   Rescan,
   Exit
};

static std::string trim(const std::string &s)
{
   size_t a = 0;
   size_t b = s.size();
   while (a < b && std::isspace((unsigned char)s[a])) ++a;
   while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
   return s.substr(a, b - a);
}

static std::string lower(std::string s)
{
   for (char &c : s)
      c = (char)std::tolower((unsigned char)c);
   return s;
}

static bool file_exists(const std::string &p)
{
   struct stat st{};
   return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const std::string &p)
{
   struct stat st{};
   return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string dirname_of(const std::string &p)
{
   size_t n = p.find_last_of('/');
   if (n == std::string::npos) return ".";
   if (n == 0) return "/";
   return p.substr(0, n);
}

static std::string basename_of(const std::string &p)
{
   size_t n = p.find_last_of('/');
   return n == std::string::npos ? p : p.substr(n + 1);
}

static std::string stem_of(const std::string &p)
{
   std::string b = basename_of(p);
   size_t n = b.find_last_of('.');
   return n == std::string::npos ? b : b.substr(0, n);
}

static std::string extension_of(const std::string &p)
{
   std::string b = basename_of(p);
   size_t n = b.find_last_of('.');
   return n == std::string::npos ? std::string() : lower(b.substr(n));
}

static std::string join_path(const std::string &a, const std::string &b)
{
   if (b.empty()) return a;
   if (b[0] == '/') return b;
   if (a.empty() || a == ".") return b;
   if (a.back() == '/') return a + b;
   return a + "/" + b;
}

static std::string resolve_media_path(const std::string &system_path,
      const std::string &raw)
{
   std::string p = trim(raw);
   if (p.empty()) return p;
   if (p[0] == '/') return p;
   if (p.size() >= 2 && p[0] == '.' && p[1] == '/')
      return join_path(system_path, p.substr(2));
   if (p.size() >= 2 && p[0] == '~' && p[1] == '/')
   {
      const char *home = getenv("HOME");
      if (home && *home) return join_path(home, p.substr(2));
   }
   return join_path(system_path, p);
}

static std::vector<std::string> split_words(const std::string &s)
{
   std::istringstream in(s);
   std::vector<std::string> out;
   std::string t;
   while (in >> t) out.push_back(lower(t));
   return out;
}

static std::string xml_text(XMLElement *e, const char *name)
{
   if (!e) return {};
   XMLElement *c = e->FirstChildElement(name);
   return c && c->GetText() ? trim(c->GetText()) : std::string();
}

static bool load_systems(const std::string &cfg, std::vector<SystemDef> &systems)
{
   XMLDocument doc;
   if (doc.LoadFile(cfg.c_str()) != tinyxml2::XML_SUCCESS)
   {
      fprintf(stderr, "[GAMEFRONT] cannot load %s: %s\n", cfg.c_str(), doc.ErrorStr());
      return false;
   }
   XMLElement *root = doc.FirstChildElement("systemList");
   if (!root)
   {
      fprintf(stderr, "[GAMEFRONT] %s has no <systemList>\n", cfg.c_str());
      return false;
   }

   systems.clear();
   for (XMLElement *e = root->FirstChildElement("system"); e;
        e = e->NextSiblingElement("system"))
   {
      SystemDef s;
      s.name = xml_text(e, "name");
      s.fullname = xml_text(e, "fullname");
      s.path = xml_text(e, "path");
      s.extensions = split_words(xml_text(e, "extension"));
      s.command = xml_text(e, "command");
      s.theme = xml_text(e, "theme");
      if (s.fullname.empty()) s.fullname = s.name;
      if (s.name.empty() || s.path.empty() || s.command.empty() || s.extensions.empty())
      {
         fprintf(stderr, "[GAMEFRONT] ignoring incomplete system entry '%s'\n", s.name.c_str());
         continue;
      }
      systems.push_back(std::move(s));
   }
   fprintf(stderr, "[GAMEFRONT] loaded %zu systems from %s\n", systems.size(), cfg.c_str());
   return !systems.empty();
}

struct MetaEntry {
   std::string title;
   std::string image;
   std::string desc;
};

static std::map<std::string, MetaEntry> load_gamelist(const SystemDef &sys)
{
   std::map<std::string, MetaEntry> map;
   std::string path = join_path(sys.path, "gamelist.xml");
   if (!file_exists(path)) return map;

   XMLDocument doc;
   if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
   {
      fprintf(stderr, "[GAMEFRONT] gamelist parse failed: %s\n", path.c_str());
      return map;
   }
   XMLElement *root = doc.FirstChildElement("gameList");
   if (!root) return map;

   for (XMLElement *e = root->FirstChildElement("game"); e;
        e = e->NextSiblingElement("game"))
   {
      std::string raw_path = xml_text(e, "path");
      if (raw_path.empty()) continue;
      std::string key = basename_of(raw_path);
      MetaEntry m;
      m.title = xml_text(e, "name");
      m.image = resolve_media_path(sys.path, xml_text(e, "image"));
      m.desc = xml_text(e, "desc");
      map[key] = std::move(m);
   }
   fprintf(stderr, "[GAMEFRONT] %s metadata entries=%zu\n", sys.name.c_str(), map.size());
   return map;
}

static std::string find_fallback_image(const SystemDef &sys, const std::string &rom)
{
   const std::string stem = stem_of(rom);
   const char *dirs[] = {"media", "images", "boxart", "covers", ""};
   const char *exts[] = {".png", ".jpg", ".jpeg"};
   for (const char *d : dirs)
      for (const char *e : exts)
      {
         std::string base = d[0] ? join_path(sys.path, d) : sys.path;
         std::string p = join_path(base, stem + e);
         if (file_exists(p)) return p;
      }
   return {};
}

static void scan_games(SystemDef &sys)
{
   sys.games.clear();
   if (!dir_exists(sys.path)) return;
   std::set<std::string> exts(sys.extensions.begin(), sys.extensions.end());
   auto metadata = load_gamelist(sys);

   DIR *d = opendir(sys.path.c_str());
   if (!d) return;
   for (dirent *de = readdir(d); de; de = readdir(d))
   {
      if (!de->d_name[0] || de->d_name[0] == '.') continue;
      std::string full = join_path(sys.path, de->d_name);
      struct stat st{};
      if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      if (!exts.count(extension_of(full))) continue;

      Game g;
      g.rom_path = full;
      g.title = stem_of(full);
      auto it = metadata.find(basename_of(full));
      if (it != metadata.end())
      {
         if (!it->second.title.empty()) g.title = it->second.title;
         if (!it->second.image.empty() && file_exists(it->second.image)) g.image_path = it->second.image;
         g.description = it->second.desc;
      }
      if (g.image_path.empty()) g.image_path = find_fallback_image(sys, full);
      sys.games.push_back(std::move(g));
   }
   closedir(d);
   std::sort(sys.games.begin(), sys.games.end(), [](const Game &a, const Game &b) {
      return lower(a.title) < lower(b.title);
   });
   fprintf(stderr, "[GAMEFRONT] system=%s games=%zu path=%s\n",
         sys.name.c_str(), sys.games.size(), sys.path.c_str());
}

static void scan_all(std::vector<SystemDef> &systems)
{
   for (auto &s : systems) scan_games(s);
}

static std::string shell_quote(const std::string &s)
{
   std::string out = "'";
   for (char c : s)
   {
      if (c == '\'') out += "'\\''";
      else out += c;
   }
   out += "'";
   return out;
}

static void replace_all(std::string &s, const std::string &from, const std::string &to)
{
   size_t pos = 0;
   while ((pos = s.find(from, pos)) != std::string::npos)
   {
      s.replace(pos, from.size(), to);
      pos += to.size();
   }
}

static std::string launch_command(const SystemDef &sys, const Game &game)
{
   std::string cmd = sys.command;
   const std::string q = shell_quote(game.rom_path);
   replace_all(cmd, "%ROM_RAW%", q);
   replace_all(cmd, "%ROM%", q);
   replace_all(cmd, "%BASENAME%", shell_quote(stem_of(game.rom_path)));
   return cmd;
}

static uint16_t pack1555(unsigned r, unsigned g, unsigned b)
{
   return (uint16_t)(0x8000U | (((r >> 3) & 31U) << 10) |
         (((g >> 3) & 31U) << 5) | ((b >> 3) & 31U));
}

static bool fb_open(Fb &fb)
{
   fb.fd = open(kFramebuffer, O_RDWR);
   if (fb.fd < 0)
   {
      fprintf(stderr, "[GAMEFRONT] open %s failed: %s\n", kFramebuffer, strerror(errno));
      return false;
   }
   if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.fix) < 0 ||
       ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var) < 0)
   {
      fprintf(stderr, "[GAMEFRONT] framebuffer ioctl failed: %s\n", strerror(errno));
      close(fb.fd); fb.fd = -1; return false;
   }
   fb.w = fb.var.xres;
   fb.h = fb.var.yres;
   fb.stride = fb.fix.line_length;
   size_t fallback = (size_t)fb.stride * (fb.var.yres_virtual ? fb.var.yres_virtual : fb.var.yres);
   fb.len = fb.fix.smem_len ? fb.fix.smem_len : fallback;
   if (fb.var.bits_per_pixel != 16 || !fb.w || !fb.h || fb.stride < fb.w * 2U)
   {
      fprintf(stderr, "[GAMEFRONT] unsupported framebuffer %ux%u %ubpp stride=%u\n",
            fb.w, fb.h, fb.var.bits_per_pixel, fb.stride);
      close(fb.fd); fb.fd = -1; return false;
   }
   fb.mem = (uint8_t*)mmap(nullptr, fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
   if (fb.mem == MAP_FAILED)
   {
      fb.mem = nullptr;
      fprintf(stderr, "[GAMEFRONT] framebuffer mmap failed: %s\n", strerror(errno));
      close(fb.fd); fb.fd = -1; return false;
   }
   fprintf(stderr, "[GAMEFRONT] framebuffer ready %ux%u %ubpp stride=%u\n",
         fb.w, fb.h, fb.var.bits_per_pixel, fb.stride);
   return true;
}

static void fb_close(Fb &fb)
{
   if (fb.mem) { munmap(fb.mem, fb.len); fb.mem = nullptr; }
   if (fb.fd >= 0) { close(fb.fd); fb.fd = -1; }
}

static inline uint16_t *fb_row(Fb &fb, int y)
{
   return (uint16_t*)(fb.mem + (size_t)y * fb.stride);
}

static void fill_rect(Fb &fb, int x, int y, int w, int h, uint16_t color)
{
   if (!fb.mem || w <= 0 || h <= 0) return;
   int x0 = std::max(0, x), y0 = std::max(0, y);
   int x1 = std::min((int)fb.w, x + w), y1 = std::min((int)fb.h, y + h);
   for (int yy = y0; yy < y1; ++yy)
   {
      uint16_t *row = fb_row(fb, yy);
      for (int xx = x0; xx < x1; ++xx) row[xx] = color;
   }
}

static void frame_rect(Fb &fb, int x, int y, int w, int h, int t, uint16_t c)
{
   fill_rect(fb, x, y, w, t, c);
   fill_rect(fb, x, y + h - t, w, t, c);
   fill_rect(fb, x, y, t, h, c);
   fill_rect(fb, x + w - t, y, t, h, c);
}

struct EasyVertex { float x, y, z; unsigned char c[4]; };

static std::string ascii_safe(const std::string &s)
{
   std::string out;
   for (unsigned char c : s)
   {
      if (c >= 32 && c <= 126) out.push_back((char)c);
      else if ((c & 0xC0) != 0x80) out.push_back('?');
   }
   return out;
}

static std::string ellipsize(const std::string &s, size_t n)
{
   std::string a = ascii_safe(s);
   if (a.size() <= n) return a;
   if (n <= 3) return a.substr(0, n);
   return a.substr(0, n - 3) + "...";
}

static void draw_text(Fb &fb, int x, int y, const std::string &raw,
      int scale, uint16_t color)
{
   std::string text = ascii_safe(raw);
   if (text.empty()) return;
   std::vector<unsigned char> buf(text.size() * 320 + 1024);
   unsigned char rgba[4] = {255, 255, 255, 255};
   int quads = stb_easy_font_print(0.0f, 0.0f, (char*)text.c_str(), rgba,
         buf.data(), (int)buf.size());
   EasyVertex *v = (EasyVertex*)buf.data();
   for (int q = 0; q < quads; ++q)
   {
      EasyVertex *p = v + q * 4;
      float minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
      for (int i = 1; i < 4; ++i)
      {
         minx = std::min(minx, p[i].x); maxx = std::max(maxx, p[i].x);
         miny = std::min(miny, p[i].y); maxy = std::max(maxy, p[i].y);
      }
      int rx = x + (int)(minx * scale);
      int ry = y + (int)(miny * scale);
      int rw = std::max(1, (int)((maxx - minx) * scale + 0.5f));
      int rh = std::max(1, (int)((maxy - miny) * scale + 0.5f));
      fill_rect(fb, rx, ry, rw, rh, color);
   }
}

static int text_width(const std::string &raw, int scale)
{
   std::string s = ascii_safe(raw);
   return (int)(stb_easy_font_width((char*)s.c_str()) * scale);
}

static bool draw_image(Fb &fb, const std::string &path, int x, int y, int w, int h)
{
   if (path.empty() || !file_exists(path)) return false;
   int iw = 0, ih = 0, comp = 0;
   unsigned char *img = stbi_load(path.c_str(), &iw, &ih, &comp, 4);
   if (!img || iw <= 0 || ih <= 0)
   {
      if (img) stbi_image_free(img);
      return false;
   }

   double sx = (double)w / (double)iw;
   double sy = (double)h / (double)ih;
   double s = std::min(sx, sy);
   int dw = std::max(1, (int)(iw * s));
   int dh = std::max(1, (int)(ih * s));
   int dx = x + (w - dw) / 2;
   int dy = y + (h - dh) / 2;

   for (int yy = 0; yy < dh; ++yy)
   {
      int src_y = (int)((int64_t)yy * ih / dh);
      int dst_y = dy + yy;
      if (dst_y < 0 || dst_y >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dst_y);
      for (int xx = 0; xx < dw; ++xx)
      {
         int dst_x = dx + xx;
         if (dst_x < 0 || dst_x >= (int)fb.w) continue;
         int src_x = (int)((int64_t)xx * iw / dw);
         const unsigned char *p = img + ((size_t)src_y * iw + src_x) * 4U;
         if (p[3] >= 96) row[dst_x] = pack1555(p[0], p[1], p[2]);
      }
   }
   stbi_image_free(img);
   return true;
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

static uint64_t input_now_ms()
{
   timeval tv{};
   gettimeofday(&tv, nullptr);
   return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static bool keyboard_capable(int fd, const char *name)
{
   unsigned long evbits[NBITS(EV_MAX + 1)]{};
   unsigned long keybits[NBITS(KEY_MAX + 1)]{};
   unsigned score = 0;
   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 || !TEST_BIT(EV_KEY, evbits)) return false;
   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;
   const unsigned keys[] = {KEY_ENTER, KEY_ESC, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
   for (unsigned k : keys) if (TEST_BIT(k, keybits)) ++score;
   if (contains_ci(name, "keyboard") || contains_ci(name, "kbd")) return score >= 4;
   return score >= 6;
}

static bool gamepad_capable(int fd, const char *name)
{
   unsigned long evbits[NBITS(EV_MAX + 1)]{};
   unsigned long keybits[NBITS(KEY_MAX + 1)]{};
   unsigned long absbits[NBITS(ABS_MAX + 1)]{};
   unsigned buttons = 0;
   bool axes = false;
   bool dpad = false;

   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;

   if (TEST_BIT(EV_KEY, evbits))
   {
      if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;
      const unsigned game_keys[] = {
         BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
         BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
         BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_TOP2
      };
      for (unsigned k : game_keys) if (TEST_BIT(k, keybits)) ++buttons;
      dpad = TEST_BIT(BTN_DPAD_LEFT, keybits) || TEST_BIT(BTN_DPAD_RIGHT, keybits) ||
             TEST_BIT(BTN_DPAD_UP, keybits) || TEST_BIT(BTN_DPAD_DOWN, keybits);
   }

   if (TEST_BIT(EV_ABS, evbits))
   {
      if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) return false;
      axes = (TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits)) ||
             (TEST_BIT(ABS_HAT0X, absbits) && TEST_BIT(ABS_HAT0Y, absbits));
   }

   const bool name_hint = contains_ci(name, "gamepad") ||
                          contains_ci(name, "joystick") ||
                          contains_ci(name, "controller") ||
                          contains_ci(name, "game stick");

   return (buttons >= 2 && (axes || dpad)) ||
          (name_hint && buttons >= 1 && (axes || dpad));
}

static bool input_keyboard_open(Input &in)
{
   const char *forced = getenv("GAMEFRONT_INPUT");
   if (forced && *forced)
   {
      int fd = open(forced, O_RDONLY | O_NONBLOCK);
      if (fd >= 0)
      {
         in.fd = fd;
         in.path = forced;
         fprintf(stderr, "[GAMEFRONT] keyboard forced %s\n", forced);
         return true;
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

static bool input_path_in_use(const Input &in, const char *path)
{
   if (in.fd >= 0 && in.path == path) return true;
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0 && in.pads[i].path == path) return true;
   return false;
}

static void gamepad_prepare(GamepadInput &pad)
{
   const unsigned axes[] = {
      ABS_X, ABS_Y, ABS_RX, ABS_RY,
      ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y
   };
   for (unsigned code : axes)
   {
      input_absinfo ai{};
      if (ioctl(pad.fd, EVIOCGABS(code), &ai) == 0)
      {
         pad.absinfo[code] = ai;
         pad.abs_valid[code] = true;
         pad.axis_zone[code] = 0;
      }
   }
}

static bool input_scan_gamepads(Input &in)
{
   bool added = false;

   for (int event_no = 0; event_no < 64; ++event_no)
   {
      int slot = -1;
      for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
         if (in.pads[p].fd < 0) { slot = p; break; }
      if (slot < 0) break;

      char path[64], name[128]{};
      snprintf(path, sizeof(path), "/dev/input/event%d", event_no);
      if (input_path_in_use(in, path)) continue;

      int fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0) continue;
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "event%d", event_no);

      if (!gamepad_capable(fd, name))
      {
         close(fd);
         continue;
      }

      GamepadInput &pad = in.pads[slot];
      pad.fd = fd;
      pad.path = path;
      pad.name = name;
      memset(pad.abs_valid, 0, sizeof(pad.abs_valid));
      memset(pad.axis_zone, 0, sizeof(pad.axis_zone));
      gamepad_prepare(pad);
      fprintf(stderr, "[GAMEFRONT] gamepad%d ready %s (%s)\n",
            slot + 1, path, name);
      added = true;
   }

   return added;
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
      fprintf(stderr, "[GAMEFRONT] gamepad absent; hotplug scan active\n");

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

static Action gamepad_button_action(unsigned code)
{
   switch (code)
   {
      case BTN_DPAD_LEFT: return Action::PrevGame;
      case BTN_DPAD_RIGHT: return Action::NextGame;
      case BTN_DPAD_UP: return Action::PrevSystem;
      case BTN_DPAD_DOWN: return Action::NextSystem;

      case BTN_SOUTH:
      case BTN_TRIGGER:
         return Action::Launch;

      case BTN_EAST:
      case BTN_THUMB:
         return Action::Exit;

      case BTN_START:
      case BTN_NORTH:
      case BTN_TOP:
         return Action::ServiceMenu;

      case BTN_TL:
         return Action::PrevSystem;
      case BTN_TR:
         return Action::NextSystem;

      default:
         return Action::None;
   }
}

static int gamepad_axis_zone(GamepadInput &pad, unsigned code, int value)
{
   if (code > ABS_MAX) return 0;

   int minimum = -32768;
   int maximum = 32767;
   if (pad.abs_valid[code])
   {
      minimum = pad.absinfo[code].minimum;
      maximum = pad.absinfo[code].maximum;
   }

   if (maximum <= minimum) return 0;

   const int center = minimum + (maximum - minimum) / 2;
   int threshold = (maximum - minimum) / 4;
   if (threshold < 1) threshold = 1;

   if (value < center - threshold) return -1;
   if (value > center + threshold) return 1;
   return 0;
}

static Action gamepad_axis_action(GamepadInput &pad, unsigned code, int value)
{
   if (code > ABS_MAX) return Action::None;

   const int zone = gamepad_axis_zone(pad, code, value);
   const int old = pad.axis_zone[code];
   pad.axis_zone[code] = zone;

   if (zone == 0 || zone == old) return Action::None;

   switch (code)
   {
      case ABS_X:
      case ABS_HAT0X:
         return zone < 0 ? Action::PrevGame : Action::NextGame;
      case ABS_Y:
      case ABS_HAT0Y:
         return zone < 0 ? Action::PrevSystem : Action::NextSystem;
      default:
         return Action::None;
   }
}

static void gamepad_disconnect(GamepadInput &pad)
{
   if (pad.fd >= 0) close(pad.fd);
   fprintf(stderr, "[GAMEFRONT] gamepad disconnected %s; waiting for hotplug\n",
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
            if (ev.type != EV_KEY || (ev.value != 1 && ev.value != 2)) continue;
            Action a = keyboard_action(ev.code);
            if (a != Action::None) return a;
            continue;
         }
         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
         if (n <= 0)
         {
            fprintf(stderr, "[GAMEFRONT] keyboard disconnected %s; waiting for hotplug\n",
                  in.path.empty() ? "<unknown>" : in.path.c_str());
            close(in.fd);
            in.fd = -1;
            in.path.clear();
            in.next_scan_ms = 0;
            break;
         }
         break;
      }
   }

   for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
   {
      GamepadInput &pad = in.pads[p];
      if (pad.fd < 0) continue;

      input_event ev{};
      for (;;)
      {
         ssize_t n = read(pad.fd, &ev, sizeof(ev));
         if (n == (ssize_t)sizeof(ev))
         {
            Action a = Action::None;
            if (ev.type == EV_KEY && ev.value == 1)
               a = gamepad_button_action(ev.code);
            else if (ev.type == EV_ABS)
               a = gamepad_axis_action(pad, ev.code, ev.value);

            if (a != Action::None) return a;
            continue;
         }

         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
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
