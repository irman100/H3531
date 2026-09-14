/* H3531 Game Frontend Stage 4.2 - centered overflow carousel.
 *
 * Standalone presentation layer over the proven Stage4.0 backend:
 * - exact NES/Sega classification (Genesis can never match NES)
 * - two focus zones: Systems / Games
 * - selected system always centered and slightly enlarged
 * - system carousel and ROM carousel may extend beyond both screen edges
 * - one additional ROM/system item is prepared beyond each edge
 * - equal 14 px outer card gaps
 * - RAM backbuffer + one coherent framebuffer blit per animation frame
 * - no SDL/OpenGL; framebuffer + evdev remain unchanged
 */

#include <cmath>
#include <sys/time.h>

#define draw_ui stage40_draw_ui
#define main stage40_main
#include "h3531-game-frontend.cpp"
#undef main
#undef draw_ui

static const int STAGE42_CARD_GAP = 14;
static const int STAGE42_CARD_PAD = 5;
static const int STAGE42_SMALL_W = 172;
static const int STAGE42_SMALL_H = 260;
static const int STAGE42_SELECTED_W = 256;
static const int STAGE42_SELECTED_H = 364;
static const int STAGE42_SYSTEM_GAP = 18;
static const int STAGE42_SYSTEM_SMALL_W = 188;
static const int STAGE42_SYSTEM_SELECTED_W = 232;
static const uint64_t STAGE42_GAME_ANIM_MS = 165;
static const uint64_t STAGE42_SYSTEM_ANIM_MS = 165;
static const uint64_t STAGE42_FRAME_MS = 20;
static const char *STAGE42_MARKER = "Stage4.2 centered overflow carousel active";

enum class FocusZone {
   Systems,
   Games
};

struct Stage42Art {
   std::string path;
   int w = 0;
   int h = 0;
   std::vector<unsigned char> rgba;
   uint64_t stamp = 0;
};

static std::vector<Stage42Art> stage42_art_cache;
static uint64_t stage42_art_stamp = 1;
static const size_t STAGE42_ART_CACHE_LIMIT = 11;

static uint64_t stage42_now_ms()
{
   timeval tv{};
   gettimeofday(&tv, nullptr);
   return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static float stage42_clamp01(float v)
{
   if (v < 0.0f) return 0.0f;
   if (v > 1.0f) return 1.0f;
   return v;
}

static float stage42_ease_out(float t)
{
   t = stage42_clamp01(t);
   const float u = 1.0f - t;
   return 1.0f - u * u * u;
}

static float stage42_smoothstep(float t)
{
   t = stage42_clamp01(t);
   return t * t * (3.0f - 2.0f * t);
}

static std::string stage42_ellipsize_px(const std::string &raw, int scale, int max_px)
{
   std::string s = ascii_safe(raw);
   if (text_width(s, scale) <= max_px) return s;
   while (s.size() > 3)
   {
      s.resize(s.size() - 1);
      const std::string out = s + "...";
      if (text_width(out, scale) <= max_px) return out;
   }
   return "...";
}

static void stage42_fill_circle(Fb &fb, int cx, int cy, int r, uint16_t c)
{
   if (r <= 0) return;
   for (int y = -r; y <= r; ++y)
   {
      const int span = (int)std::sqrt((double)std::max(0, r * r - y * y));
      fill_rect(fb, cx - span, cy + y, span * 2 + 1, 1, c);
   }
}

static void stage42_fill_ellipse(Fb &fb, int cx, int cy, int rx, int ry, uint16_t c)
{
   if (rx <= 0 || ry <= 0) return;
   for (int y = -ry; y <= ry; ++y)
   {
      const double f = 1.0 - ((double)y * y) / ((double)ry * ry);
      const int span = (int)std::lround(rx * std::sqrt(std::max(0.0, f)));
      fill_rect(fb, cx - span, cy + y, span * 2 + 1, 1, c);
   }
}

static Stage42Art *stage42_get_art(const std::string &path)
{
   if (path.empty() || !file_exists(path)) return nullptr;
   for (auto &e : stage42_art_cache)
   {
      if (e.path == path)
      {
         e.stamp = stage42_art_stamp++;
         return &e;
      }
   }

   int iw = 0, ih = 0, comp = 0;
   unsigned char *raw = stbi_load(path.c_str(), &iw, &ih, &comp, 4);
   if (!raw || iw <= 0 || ih <= 0)
   {
      if (raw) stbi_image_free(raw);
      return nullptr;
   }

   const int max_w = 420;
   const int max_h = 520;
   const double scale = std::min(1.0, std::min((double)max_w / iw, (double)max_h / ih));
   const int ow = std::max(1, (int)std::lround(iw * scale));
   const int oh = std::max(1, (int)std::lround(ih * scale));

   Stage42Art fresh;
   fresh.path = path;
   fresh.w = ow;
   fresh.h = oh;
   fresh.stamp = stage42_art_stamp++;
   fresh.rgba.resize((size_t)ow * oh * 4U);

   for (int y = 0; y < oh; ++y)
   {
      const int sy = (int)((int64_t)y * ih / oh);
      for (int x = 0; x < ow; ++x)
      {
         const int sx = (int)((int64_t)x * iw / ow);
         const unsigned char *src = raw + ((size_t)sy * iw + sx) * 4U;
         unsigned char *dst = fresh.rgba.data() + ((size_t)y * ow + x) * 4U;
         dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
      }
   }
   stbi_image_free(raw);

   if (stage42_art_cache.size() >= STAGE42_ART_CACHE_LIMIT)
   {
      auto it = std::min_element(stage42_art_cache.begin(), stage42_art_cache.end(),
            [](const Stage42Art &a, const Stage42Art &b) { return a.stamp < b.stamp; });
      if (it != stage42_art_cache.end()) stage42_art_cache.erase(it);
   }
   stage42_art_cache.push_back(std::move(fresh));
   return &stage42_art_cache.back();
}

static void stage42_clear_art_cache()
{
   stage42_art_cache.clear();
   stage42_art_stamp = 1;
}

static bool stage42_draw_art(Fb &fb, const std::string &path, int x, int y, int w, int h)
{
   Stage42Art *img = stage42_get_art(path);
   if (!img || img->w <= 0 || img->h <= 0) return false;

   const double sx = (double)w / img->w;
   const double sy = (double)h / img->h;
   const double s = std::min(sx, sy);
   const int dw = std::max(1, (int)std::lround(img->w * s));
   const int dh = std::max(1, (int)std::lround(img->h * s));
   const int dx = x + (w - dw) / 2;
   const int dy = y + (h - dh) / 2;

   for (int yy = 0; yy < dh; ++yy)
   {
      const int src_y = (int)((int64_t)yy * img->h / dh);
      const int dst_y = dy + yy;
      if (dst_y < 0 || dst_y >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dst_y);
      for (int xx = 0; xx < dw; ++xx)
      {
         const int dst_x = dx + xx;
         if (dst_x < 0 || dst_x >= (int)fb.w) continue;
         const int src_x = (int)((int64_t)xx * img->w / dw);
         const unsigned char *p = img->rgba.data() + ((size_t)src_y * img->w + src_x) * 4U;
         if (p[3] >= 96) row[dst_x] = pack1555(p[0], p[1], p[2]);
      }
   }
   return true;
}

struct Stage42Backbuffer {
   std::vector<uint16_t> pixels;
   Fb fb;

   bool init(const Fb &physical)
   {
      if (!physical.w || !physical.h) return false;
      pixels.assign((size_t)physical.w * physical.h, pack1555(0, 0, 0));
      fb = physical;
      fb.fd = -1;
      fb.mem = reinterpret_cast<uint8_t*>(pixels.data());
      fb.stride = physical.w * 2U;
      fb.len = pixels.size() * sizeof(uint16_t);
      return true;
   }
};

static void stage42_present(Fb &physical, const Stage42Backbuffer &back)
{
   if (!physical.mem || !back.fb.mem) return;
   const size_t row_bytes = (size_t)physical.w * 2U;
   for (unsigned y = 0; y < physical.h; ++y)
      memcpy(physical.mem + (size_t)y * physical.stride,
             back.fb.mem + (size_t)y * back.fb.stride,
             row_bytes);
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
}

static bool stage42_is_nes(const SystemDef &s)
{
   const std::string name = lower(s.name);
   const std::string full = lower(s.fullname);
   return name == "nes" || name == "famicom" ||
          full.find("nintendo entertainment system") != std::string::npos;
}

static bool stage42_is_sega(const SystemDef &s)
{
   const std::string name = lower(s.name);
   const std::string full = lower(s.fullname);
   return name == "megadrive" || name == "genesis" ||
          name == "mastersystem" || name == "gamegear" ||
          full.find("sega") != std::string::npos ||
          full.find("mega drive") != std::string::npos ||
          full.find("genesis") != std::string::npos;
}

static std::string stage42_system_short_name(const SystemDef &s)
{
   const std::string name = lower(s.name);
   if (stage42_is_nes(s)) return "NES";
   if (name == "megadrive" || name == "genesis") return "SEGA MD";
   if (name == "mastersystem") return "MASTER SYSTEM";
   if (name == "gamegear") return "GAME GEAR";
   if (stage42_is_sega(s)) return "SEGA";
   return stage42_ellipsize_px(s.fullname, 2, 150);
}

static void stage42_draw_nes_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int w = (int)std::lround(112 * scale);
   const int h = (int)std::lround(52 * scale);
   const int x = cx - w / 2, y = cy - h / 2;
   fill_rect(fb, x, y, w, h, body);
   frame_rect(fb, x, y, w, h, std::max(1, (int)std::lround(2 * scale)), ink);
   const int d = std::max(4, (int)std::lround(9 * scale));
   const int arm = std::max(14, (int)std::lround(27 * scale));
   const int dx = x + (int)std::lround(28 * scale);
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(7 * scale));
   stage42_fill_circle(fb, x + w - (int)std::lround(34 * scale), cy + (int)std::lround(5 * scale), r, accent);
   stage42_fill_circle(fb, x + w - (int)std::lround(17 * scale), cy - (int)std::lround(3 * scale), r, accent);
   const int sw = std::max(5, (int)std::lround(13 * scale));
   const int sh = std::max(2, (int)std::lround(4 * scale));
   fill_rect(fb, cx - sw - 2, cy + (int)std::lround(13 * scale), sw, sh, ink);
   fill_rect(fb, cx + 2, cy + (int)std::lround(13 * scale), sw, sh, ink);
}

static void stage42_draw_sega_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int rx = (int)std::lround(64 * scale);
   const int ry = (int)std::lround(30 * scale);
   stage42_fill_ellipse(fb, cx, cy, rx, ry, body);
   stage42_fill_ellipse(fb, cx, cy,
         std::max(1, rx - (int)std::lround(4 * scale)),
         std::max(1, ry - (int)std::lround(4 * scale)), pack1555(24, 29, 42));
   const int d = std::max(4, (int)std::lround(8 * scale));
   const int arm = std::max(12, (int)std::lround(24 * scale));
   const int dx = cx - (int)std::lround(31 * scale);
   fill_rect(fb, dx - d / 2, cy - arm / 2, d, arm, ink);
   fill_rect(fb, dx - arm / 2, cy - d / 2, arm, d, ink);
   const int r = std::max(3, (int)std::lround(6 * scale));
   stage42_fill_circle(fb, cx + (int)std::lround(17 * scale), cy + (int)std::lround(7 * scale), r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(34 * scale), cy, r, accent);
   stage42_fill_circle(fb, cx + (int)std::lround(50 * scale), cy - (int)std::lround(8 * scale), r, accent);
   fill_rect(fb, cx - (int)std::lround(7 * scale), cy + (int)std::lround(16 * scale),
             std::max(5, (int)std::lround(14 * scale)), std::max(2, (int)std::lround(3 * scale)), ink);
}

static void stage42_draw_generic_pad(Fb &fb, int cx, int cy, float scale,
      uint16_t body, uint16_t ink, uint16_t accent)
{
   const int rx = (int)std::lround(58 * scale), ry = (int)std::lround(27 * scale);
   stage42_fill_ellipse(fb, cx, cy, rx, ry, body);
   fill_rect(fb, cx - (int)std::lround(37 * scale), cy - (int)std::lround(4 * scale),
             (int)std::lround(22 * scale), (int)std::lround(8 * scale), ink);
   fill_rect(fb, cx - (int)std::lround(30 * scale), cy - (int)std::lround(11 * scale),
             (int)std::lround(8 * scale), (int)std::lround(22 * scale), ink);
   stage42_fill_circle(fb, cx + (int)std::lround(26 * scale), cy,
                       std::max(3, (int)std::lround(6 * scale)), accent);
}

static float stage42_card_center_offset(float rel)
{
   const float outer_small = STAGE42_SMALL_W + STAGE42_CARD_PAD * 2.0f;
   const float outer_selected = STAGE42_SELECTED_W + STAGE42_CARD_PAD * 2.0f;
   const float a = std::fabs(rel);
   const float d1 = outer_selected * 0.5f + STAGE42_CARD_GAP + outer_small * 0.5f;
   const float d = a <= 1.0f ? a * d1 :
                   d1 + (a - 1.0f) * (outer_small + STAGE42_CARD_GAP);
   return rel < 0.0f ? -d : d;
}

static float stage42_system_center_offset(float rel)
{
   const float a = std::fabs(rel);
   const float d1 = STAGE42_SYSTEM_SELECTED_W * 0.5f + STAGE42_SYSTEM_GAP +
                    STAGE42_SYSTEM_SMALL_W * 0.5f;
   const float d = a <= 1.0f ? a * d1 :
                   d1 + (a - 1.0f) * (STAGE42_SYSTEM_SMALL_W + STAGE42_SYSTEM_GAP);
   return rel < 0.0f ? -d : d;
}

struct Stage42GameAnim {
   int dir = 0;
   uint64_t start = 0;
};

struct Stage42SystemAnim {
   int dir = 0;
   uint64_t start = 0;
};

static bool stage42_game_anim_active(const Stage42GameAnim &a, uint64_t now)
{
   return a.dir != 0 && now - a.start < STAGE42_GAME_ANIM_MS;
}

static bool stage42_system_anim_active(const Stage42SystemAnim &a, uint64_t now)
{
   return a.dir != 0 && now - a.start < STAGE42_SYSTEM_ANIM_MS;
}

static float stage42_game_shift(const Stage42GameAnim &a, uint64_t now)
{
   if (!stage42_game_anim_active(a, now)) return 0.0f;
   const float t = (float)(now - a.start) / (float)STAGE42_GAME_ANIM_MS;
   return (float)a.dir * (1.0f - stage42_ease_out(t));
}

static float stage42_system_shift(const Stage42SystemAnim &a, uint64_t now)
{
   if (!stage42_system_anim_active(a, now)) return 0.0f;
   const float t = (float)(now - a.start) / (float)STAGE42_SYSTEM_ANIM_MS;
   return (float)a.dir * (1.0f - stage42_ease_out(t));
}

struct Stage42CardVisual {
   size_t index = 0;
   float rel = 0.0f;
   float focus = 0.0f;
};

struct Stage42SystemVisual {
   size_t pos = 0;
   float rel = 0.0f;
   float focus = 0.0f;
};

static bool stage42_should_replace_duplicate(float current_rel, float candidate_rel, float shift)
{
   const float ca = std::fabs(current_rel);
   const float na = std::fabs(candidate_rel);
   if (na + 0.0001f < ca) return true;
   if (std::fabs(na - ca) > 0.0001f) return false;
   if (shift > 0.001f) return candidate_rel > current_rel;
   if (shift < -0.001f) return candidate_rel < current_rel;
   return candidate_rel > current_rel;
}

static void stage42_draw_system_row(Fb &fb, const std::vector<SystemDef> &systems,
      const std::vector<size_t> &vis, size_t visible_pos, FocusZone focus, float system_shift)
{
   if (vis.empty()) return;

   const uint16_t row_bg = pack1555(13, 25, 42);
   const uint16_t slot_bg = pack1555(20, 34, 53);
   const uint16_t slot_dim = pack1555(57, 72, 92);
   const uint16_t white = pack1555(235, 242, 250);
   const uint16_t dim = pack1555(126, 143, 166);
   const uint16_t accent = pack1555(75, 202, 255);
   const uint16_t red = pack1555(220, 64, 72);
   const int center = (int)fb.w / 2;

   fill_rect(fb, 0, 45, (int)fb.w, 116, row_bg);

   std::map<size_t, Stage42SystemVisual> unique;
   for (int rel = -4; rel <= 4; ++rel)
   {
      const float effective = (float)rel + system_shift;
      if (std::fabs(effective) > 3.20f) continue;
      int p = (int)visible_pos + rel;
      while (p < 0) p += (int)vis.size();
      while (p >= (int)vis.size()) p -= (int)vis.size();
      const size_t pos = (size_t)p;
      const float item_focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));
      auto it = unique.find(pos);
      if (it == unique.end() || stage42_should_replace_duplicate(it->second.rel, effective, system_shift))
      {
         Stage42SystemVisual sv;
         sv.pos = pos;
         sv.rel = effective;
         sv.focus = item_focus;
         unique[pos] = sv;
      }
   }

   std::vector<Stage42SystemVisual> items;
   for (auto &kv : unique) items.push_back(kv.second);
   std::sort(items.begin(), items.end(), [](const Stage42SystemVisual &a, const Stage42SystemVisual &b) {
      return a.focus < b.focus;
   });

   for (const auto &sv : items)
   {
      const SystemDef &s = systems[vis[sv.pos]];
      const float f = sv.focus;
      const int w = (int)std::lround(STAGE42_SYSTEM_SMALL_W +
                     (STAGE42_SYSTEM_SELECTED_W - STAGE42_SYSTEM_SMALL_W) * f);
      const int h = (int)std::lround(78.0f + 14.0f * f);
      const int cx = center + (int)std::lround(stage42_system_center_offset(sv.rel));
      const int x = cx - w / 2;
      const int y = 55 + (int)std::lround((1.0f - f) * 7.0f);
      const bool selected = std::fabs(sv.rel) < 0.50f;

      fill_rect(fb, x, y, w, h, slot_bg);
      frame_rect(fb, x, y, w, h,
                 focus == FocusZone::Systems && selected ? 4 : 2,
                 focus == FocusZone::Systems && selected ? accent : slot_dim);

      const float pad_scale = 0.76f + f * 0.34f;
      const uint16_t body = selected ? pack1555(196, 204, 214) : pack1555(104, 117, 135);
      const uint16_t ink = selected ? pack1555(24, 29, 37) : pack1555(42, 50, 62);
      const uint16_t button = selected ? red : pack1555(118, 74, 82);

      if (stage42_is_nes(s))
         stage42_draw_nes_pad(fb, cx, y + 35, pad_scale, body, ink, button);
      else if (stage42_is_sega(s))
         stage42_draw_sega_pad(fb, cx, y + 35, pad_scale, body, ink, button);
      else
         stage42_draw_generic_pad(fb, cx, y + 35, pad_scale, body, ink, button);

      const std::string label = stage42_system_short_name(s);
      const int tw = text_width(label, 2);
      draw_text(fb, cx - tw / 2, y + h - 19, label, 2, selected ? white : dim);
   }

   fill_rect(fb, center - 66, 151, 132, focus == FocusZone::Systems ? 4 : 2, accent);
}

static void stage42_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
      float game_shift, FocusZone focus)
{
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;

   const uint16_t panel = pack1555(20, 29, 43);
   const uint16_t panel2 = pack1555(28, 39, 57);
   const uint16_t border = pack1555(66, 80, 101);
   const uint16_t accent = pack1555(75, 202, 255);
   const uint16_t text = pack1555(238, 243, 249);
   const uint16_t dim = pack1555(136, 151, 173);
   const int center = (int)fb.w / 2;
   const int card_center_y = 365;

   std::map<size_t, Stage42CardVisual> unique;
   for (int rel = -4; rel <= 4; ++rel)
   {
      const float effective = (float)rel + game_shift;
      /* rel +/-3 intentionally lives mostly outside the visible 1280px area. */
      if (std::fabs(effective) > 3.20f) continue;
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      const size_t idx = (size_t)gp;
      const float card_focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));
      auto it = unique.find(idx);
      if (it == unique.end() || stage42_should_replace_duplicate(it->second.rel, effective, game_shift))
      {
         Stage42CardVisual cv;
         cv.index = idx;
         cv.rel = effective;
         cv.focus = card_focus;
         unique[idx] = cv;
      }
   }

   std::vector<Stage42CardVisual> cards;
   for (auto &kv : unique) cards.push_back(kv.second);
   std::sort(cards.begin(), cards.end(), [](const Stage42CardVisual &a, const Stage42CardVisual &b) {
      return a.focus < b.focus;
   });

   for (const auto &cv : cards)
   {
      const Game &g = sys.games[cv.index];
      const float f = cv.focus;
      const int w = (int)std::lround(STAGE42_SMALL_W + (STAGE42_SELECTED_W - STAGE42_SMALL_W) * f);
      const int h = (int)std::lround(STAGE42_SMALL_H + (STAGE42_SELECTED_H - STAGE42_SMALL_H) * f);
      const int x = center + (int)std::lround(stage42_card_center_offset(cv.rel)) - w / 2;
      const int y = card_center_y - h / 2;
      const int pad = STAGE42_CARD_PAD;
      const uint16_t frame_color = (f > 0.5f && focus == FocusZone::Games) ? accent : border;
      const int frame = (f > 0.5f && focus == FocusZone::Games) ? 4 : 2;

      fill_rect(fb, x - pad, y - pad, w + pad * 2, h + pad * 2, panel);
      fill_rect(fb, x, y, w, h, panel2);
      if (!stage42_draw_art(fb, g.image_path, x, y, w, h))
         draw_fallback_card(fb, sys, x, y, w, h);
      frame_rect(fb, x - pad, y - pad, w + pad * 2, h + pad * 2, frame, frame_color);

      if (f < 0.24f && x + w > 0 && x < (int)fb.w)
      {
         const std::string label = stage42_ellipsize_px(g.title, 1, w + pad * 2);
         draw_text(fb, x - pad, y + h + 11, label, 1, dim);
      }
   }

   const Game &selected = sys.games[game_pos];
   const std::string title = stage42_ellipsize_px(selected.title, 3, 760);
   const int tw = text_width(title, 3);
   draw_text(fb, center - tw / 2, 567, title, 3, focus == FocusZone::Games ? text : dim);
   const std::string counter = std::to_string(game_pos + 1) + " / " + std::to_string(sys.games.size());
   const int cw = text_width(counter, 2);
   draw_text(fb, center - cw / 2, 608, counter, 2, dim);
}

static void stage42_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      FocusZone focus, float game_shift, float system_shift)
{
   const uint16_t bg = pack1555(8, 14, 24);
   const uint16_t top = pack1555(11, 22, 38);
   const uint16_t footer = pack1555(13, 24, 40);
   const uint16_t text = pack1555(238, 243, 249);
   const uint16_t dim = pack1555(132, 148, 170);
   const uint16_t accent = pack1555(75, 202, 255);

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);
   fill_rect(fb, 0, 0, (int)fb.w, 45, top);
   draw_text(fb, 28, 15, "H3531 GAME LIBRARY", 2, text);
   const std::string runtime = "RetroArch / libretro";
   const int rw = text_width(runtime, 2);
   draw_text(fb, (int)fb.w - rw - 28, 15, runtime, 2, dim);

   if (vis.empty())
   {
      draw_text(fb, 80, 250, "NO GAMES FOUND", 5, text);
      draw_text(fb, 82, 330, "Put ROMs in /mnt/usb/games/nes or /mnt/usb/games/md", 2, dim);
      return;
   }

   stage42_draw_system_row(fb, systems, vis, visible_pos, focus, system_shift);
   const SystemDef &sys = systems[vis[visible_pos]];
   stage42_draw_games(fb, sys, game_pos, game_shift, focus);

   const int footer_y = (int)fb.h - 56;
   fill_rect(fb, 0, footer_y, (int)fb.w, 56, footer);
   fill_rect(fb, 0, footer_y, (int)fb.w, 2, accent);
   if (focus == FocusZone::Games)
      draw_text(fb, 28, footer_y + 19,
            "LEFT/RIGHT GAME   UP SYSTEMS   ENTER PLAY   F5 RESCAN   F1 RETROARCH   ESC EXIT", 2, dim);
   else
      draw_text(fb, 28, footer_y + 19,
            "LEFT/RIGHT SYSTEM   DOWN GAMES   ENTER GAMES   F5 RESCAN   F1 RETROARCH   ESC EXIT", 2, dim);
}

static int stage42_layout_test()
{
   const int widths[5] = {STAGE42_SMALL_W, STAGE42_SMALL_W, STAGE42_SELECTED_W,
                          STAGE42_SMALL_W, STAGE42_SMALL_W};
   const float rels[5] = {-2.f, -1.f, 0.f, 1.f, 2.f};
   int centers[5];
   for (int i = 0; i < 5; ++i) centers[i] = (int)std::lround(stage42_card_center_offset(rels[i]));
   for (int i = 1; i < 5; ++i)
   {
      const int prev_right = centers[i - 1] + widths[i - 1] / 2 + STAGE42_CARD_PAD;
      const int next_left = centers[i] - widths[i] / 2 - STAGE42_CARD_PAD;
      const int gap = next_left - prev_right;
      printf("OUTER_CARD_GAP %d->%d = %d\n", i - 1, i, gap);
      if (gap != STAGE42_CARD_GAP) return 31;
   }

   SystemDef nes;
   nes.name = "nes";
   nes.fullname = "Nintendo Entertainment System";
   SystemDef md;
   md.name = "megadrive";
   md.fullname = "Sega Mega Drive / Genesis";
   if (!stage42_is_nes(nes) || stage42_is_sega(nes)) return 32;
   if (stage42_is_nes(md) || !stage42_is_sega(md)) return 33;

   const int system_side = (int)std::lround(stage42_system_center_offset(1.0f));
   const int system_overflow = (int)std::lround(stage42_system_center_offset(3.0f));
   const int game_overflow = (int)std::lround(stage42_card_center_offset(3.0f));
   printf("SYSTEM_CENTER selected=0 side=%d overflow=%d\n", system_side, system_overflow);
   printf("GAME_OVERFLOW rel3_center_offset=%d\n", game_overflow);
   printf("SYSTEM_CLASSIFY NES=NES MEGADRIVE=SEGA_MD\n");
   printf("FOCUS_FLOW Games --UP--> Systems --DOWN--> Games\n");
   printf("BACKBUFFER coherent-full-frame-blit\n");
   printf("CAROUSEL selected-centered overflow-items=1-per-side\n");
   printf("LAYOUT_TEST_OK\n");
   return 0;
}

int main(int argc, char **argv)
{
   if (argc >= 2 && std::string(argv[1]) == "--layout-test")
      return stage42_layout_test();
   if (argc >= 2 && std::string(argv[1]) == "--self-test")
      return stage40_main(argc, argv);

   std::string cfg = kDefaultConfig;
   if (argc >= 3 && std::string(argv[1]) == "--config") cfg = argv[2];

   std::vector<SystemDef> systems;
   if (!load_systems(cfg, systems)) return 2;
   scan_all(systems);

   Fb physical;
   Input in;
   if (!fb_open(physical)) return 3;
   input_open(in);

   Stage42Backbuffer back;
   if (!back.init(physical))
   {
      input_close(in);
      fb_close(physical);
      return 4;
   }

   fprintf(stderr, "[GAMEFRONT] %s\n", STAGE42_MARKER);

   size_t visible_pos = 0;
   size_t game_pos = 0;
   FocusZone focus = FocusZone::Games;
   Stage42GameAnim game_anim;
   Stage42SystemAnim system_anim;
   bool redraw = true;
   bool running = true;
   uint64_t last_draw = 0;

   while (running)
   {
      auto vis = visible_systems(systems);
      if (!vis.empty() && visible_pos >= vis.size()) visible_pos = 0;
      if (!vis.empty())
      {
         const auto &games = systems[vis[visible_pos]].games;
         if (!games.empty() && game_pos >= games.size()) game_pos = 0;
      }

      const uint64_t now = stage42_now_ms();
      const bool game_active = stage42_game_anim_active(game_anim, now);
      const bool system_active = stage42_system_anim_active(system_anim, now);
      if (!game_active && game_anim.dir) { game_anim.dir = 0; redraw = true; }
      if (!system_active && system_anim.dir) { system_anim.dir = 0; redraw = true; }
      const bool animating = game_active || system_active;
      if (animating && now - last_draw >= STAGE42_FRAME_MS) redraw = true;

      if (redraw)
      {
         const float game_shift = stage42_game_shift(game_anim, now);
         const float system_shift = stage42_system_shift(system_anim, now);
         stage42_draw_ui(back.fb, systems, visible_pos, vis, game_pos, focus,
                         game_shift, system_shift);
         stage42_present(physical, back);
         redraw = false;
         last_draw = now;
      }

      const Action a = input_poll(in);
      switch (a)
      {
         case Action::PrevGame:
            if (focus == FocusZone::Games)
            {
               if (!vis.empty() && !game_active)
               {
                  auto &games = systems[vis[visible_pos]].games;
                  if (!games.empty())
                  {
                     game_pos = game_pos ? game_pos - 1 : games.size() - 1;
                     game_anim.dir = -1;
                     game_anim.start = stage42_now_ms();
                     redraw = true;
                  }
               }
            }
            else if (!vis.empty() && !system_active)
            {
               visible_pos = visible_pos ? visible_pos - 1 : vis.size() - 1;
               game_pos = 0;
               system_anim.dir = -1;
               system_anim.start = stage42_now_ms();
               redraw = true;
            }
            break;

         case Action::NextGame:
            if (focus == FocusZone::Games)
            {
               if (!vis.empty() && !game_active)
               {
                  auto &games = systems[vis[visible_pos]].games;
                  if (!games.empty())
                  {
                     game_pos = (game_pos + 1) % games.size();
                     game_anim.dir = 1;
                     game_anim.start = stage42_now_ms();
                     redraw = true;
                  }
               }
            }
            else if (!vis.empty() && !system_active)
            {
               visible_pos = (visible_pos + 1) % vis.size();
               game_pos = 0;
               system_anim.dir = 1;
               system_anim.start = stage42_now_ms();
               redraw = true;
            }
            break;

         case Action::PrevSystem:
            if (focus == FocusZone::Games) { focus = FocusZone::Systems; redraw = true; }
            break;

         case Action::NextSystem:
            if (focus == FocusZone::Systems) { focus = FocusZone::Games; redraw = true; }
            break;

         case Action::Launch:
            if (focus == FocusZone::Systems)
            {
               focus = FocusZone::Games;
               redraw = true;
            }
            else if (!vis.empty())
            {
               SystemDef &s = systems[vis[visible_pos]];
               if (!s.games.empty())
               {
                  game_anim = Stage42GameAnim{};
                  system_anim = Stage42SystemAnim{};
                  run_external(physical, in, launch_command(s, s.games[game_pos]));
                  if (!back.init(physical)) return 5;
                  redraw = true;
               }
            }
            break;

         case Action::ServiceMenu:
            game_anim = Stage42GameAnim{};
            system_anim = Stage42SystemAnim{};
            run_external(physical, in, std::string(kRetroArchMenu));
            if (!back.init(physical)) return 5;
            redraw = true;
            break;

         case Action::Rescan:
            scan_all(systems);
            stage42_clear_art_cache();
            visible_pos = 0;
            game_pos = 0;
            focus = FocusZone::Games;
            game_anim = Stage42GameAnim{};
            system_anim = Stage42SystemAnim{};
            redraw = true;
            break;

         case Action::Exit:
            running = false;
            break;

         case Action::None:
         default:
            break;
      }

      usleep(animating ? 4000 : 10000);
   }

   fill_rect(physical, 0, 0, (int)physical.w, (int)physical.h, pack1555(0, 0, 0));
   input_close(in);
   fb_close(physical);
   fprintf(stderr, "[GAMEFRONT] Stage4.2 carousel clean exit\n");
   return 0;
}
