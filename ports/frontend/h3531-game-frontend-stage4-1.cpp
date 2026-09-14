/* H3531 Game Frontend Stage 4.1 UI polish layer.
 *
 * Reuses the proven Stage4.0 backend and replaces only presentation/main loop:
 * - unique fixed-width system slots (no duplicate/overlapping labels)
 * - equal edge-to-edge card gaps around the enlarged selected card
 * - lightweight 190 ms ease-out carousel animation
 * - subtle vertical system transition
 * - small decoded-art cache so animation does not re-read PNG/JPEG each frame
 * - no SDL/OpenGL; framebuffer + evdev path remains unchanged
 */

#include <cmath>
#include <sys/time.h>

#define draw_ui stage40_draw_ui
#define main stage40_main
#include "h3531-game-frontend.cpp"
#undef main
#undef draw_ui

static const int STAGE41_CARD_GAP = 18;
static const int STAGE41_SMALL_W = 176;
static const int STAGE41_SMALL_H = 268;
static const int STAGE41_SELECTED_W = 260;
static const int STAGE41_SELECTED_H = 372;
static const uint64_t STAGE41_ANIMATION_MS = 190;
static const uint64_t STAGE41_FRAME_MS = 24;

struct ArtCacheEntry {
   std::string path;
   int w = 0;
   int h = 0;
   std::vector<unsigned char> rgba;
   uint64_t stamp = 0;
};

static std::vector<ArtCacheEntry> stage41_art_cache;
static uint64_t stage41_art_stamp = 1;
static const size_t STAGE41_ART_CACHE_LIMIT = 7;

static uint64_t stage41_now_ms()
{
   timeval tv{};
   gettimeofday(&tv, nullptr);
   return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static float stage41_clamp01(float v)
{
   if (v < 0.0f) return 0.0f;
   if (v > 1.0f) return 1.0f;
   return v;
}

static float stage41_ease_out(float t)
{
   t = stage41_clamp01(t);
   float u = 1.0f - t;
   return 1.0f - u * u * u;
}

static float stage41_smoothstep(float t)
{
   t = stage41_clamp01(t);
   return t * t * (3.0f - 2.0f * t);
}

static std::string stage41_ellipsize_px(const std::string &raw, int scale, int max_px)
{
   std::string s = ascii_safe(raw);
   if (text_width(s, scale) <= max_px) return s;
   while (s.size() > 3)
   {
      s.resize(s.size() - 1);
      std::string out = s + "...";
      if (text_width(out, scale) <= max_px) return out;
   }
   return "...";
}

static ArtCacheEntry *stage41_get_art(const std::string &path)
{
   if (path.empty() || !file_exists(path)) return nullptr;
   for (auto &e : stage41_art_cache)
   {
      if (e.path == path)
      {
         e.stamp = stage41_art_stamp++;
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
   double scale = std::min(1.0, std::min((double)max_w / iw, (double)max_h / ih));
   int ow = std::max(1, (int)std::lround(iw * scale));
   int oh = std::max(1, (int)std::lround(ih * scale));

   ArtCacheEntry fresh;
   fresh.path = path;
   fresh.w = ow;
   fresh.h = oh;
   fresh.stamp = stage41_art_stamp++;
   fresh.rgba.resize((size_t)ow * oh * 4U);

   for (int y = 0; y < oh; ++y)
   {
      int sy = (int)((int64_t)y * ih / oh);
      for (int x = 0; x < ow; ++x)
      {
         int sx = (int)((int64_t)x * iw / ow);
         const unsigned char *src = raw + ((size_t)sy * iw + sx) * 4U;
         unsigned char *dst = fresh.rgba.data() + ((size_t)y * ow + x) * 4U;
         dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
      }
   }
   stbi_image_free(raw);

   if (stage41_art_cache.size() >= STAGE41_ART_CACHE_LIMIT)
   {
      auto it = std::min_element(stage41_art_cache.begin(), stage41_art_cache.end(),
            [](const ArtCacheEntry &a, const ArtCacheEntry &b) { return a.stamp < b.stamp; });
      if (it != stage41_art_cache.end()) stage41_art_cache.erase(it);
   }
   stage41_art_cache.push_back(std::move(fresh));
   return &stage41_art_cache.back();
}

static void stage41_clear_art_cache()
{
   stage41_art_cache.clear();
   stage41_art_stamp = 1;
}

static bool stage41_draw_art(Fb &fb, const std::string &path, int x, int y, int w, int h)
{
   ArtCacheEntry *img = stage41_get_art(path);
   if (!img || img->w <= 0 || img->h <= 0) return false;

   double sx = (double)w / img->w;
   double sy = (double)h / img->h;
   double s = std::min(sx, sy);
   int dw = std::max(1, (int)std::lround(img->w * s));
   int dh = std::max(1, (int)std::lround(img->h * s));
   int dx = x + (w - dw) / 2;
   int dy = y + (h - dh) / 2;

   for (int yy = 0; yy < dh; ++yy)
   {
      int src_y = (int)((int64_t)yy * img->h / dh);
      int dst_y = dy + yy;
      if (dst_y < 0 || dst_y >= (int)fb.h) continue;
      uint16_t *row = fb_row(fb, dst_y);
      for (int xx = 0; xx < dw; ++xx)
      {
         int dst_x = dx + xx;
         if (dst_x < 0 || dst_x >= (int)fb.w) continue;
         int src_x = (int)((int64_t)xx * img->w / dw);
         const unsigned char *p = img->rgba.data() + ((size_t)src_y * img->w + src_x) * 4U;
         if (p[3] >= 96) row[dst_x] = pack1555(p[0], p[1], p[2]);
      }
   }
   return true;
}

static float stage41_card_center_offset(float rel)
{
   float a = std::fabs(rel);
   float d1 = STAGE41_SELECTED_W * 0.5f + STAGE41_CARD_GAP + STAGE41_SMALL_W * 0.5f;
   float d = a <= 1.0f ? a * d1 : d1 + (a - 1.0f) * (STAGE41_SMALL_W + STAGE41_CARD_GAP);
   return rel < 0.0f ? -d : d;
}

struct Stage41CardVisual {
   size_t index = 0;
   float rel = 0.0f;
   float focus = 0.0f;
};

static void stage41_draw_system_strip(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis)
{
   const uint16_t accent = pack1555(84, 196, 255);
   const uint16_t text = pack1555(236, 241, 248);
   const uint16_t dim = pack1555(132, 148, 171);
   int center = (int)fb.w / 2;
   std::set<size_t> used;

   for (int rel = -1; rel <= 1; ++rel)
   {
      if (vis.empty()) break;
      int p = (int)visible_pos + rel;
      while (p < 0) p += (int)vis.size();
      while (p >= (int)vis.size()) p -= (int)vis.size();
      size_t idx = vis[(size_t)p];
      if (used.count(idx)) continue;
      used.insert(idx);

      const SystemDef &s = systems[idx];
      int slot_center = center + rel * 360;
      int scale = rel == 0 ? 3 : 2;
      int max_w = rel == 0 ? 420 : 250;
      std::string name = stage41_ellipsize_px(s.fullname, scale, max_w);
      int tw = text_width(name, scale);
      int y = rel == 0 ? 84 : 91;
      draw_text(fb, slot_center - tw / 2, y, name, scale, rel == 0 ? text : dim);
      if (rel == 0)
      {
         int line_w = std::min(280, std::max(130, tw + 28));
         fill_rect(fb, center - line_w / 2, 124, line_w, 3, accent);
      }
   }
}

static void stage41_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
      size_t visible_pos, const std::vector<size_t> &vis, size_t game_pos,
      float game_shift, float system_shift)
{
   const uint16_t bg = pack1555(9, 15, 25);
   const uint16_t header = pack1555(12, 27, 47);
   const uint16_t panel = pack1555(22, 31, 46);
   const uint16_t panel2 = pack1555(29, 40, 58);
   const uint16_t shadow = pack1555(4, 7, 12);
   const uint16_t accent = pack1555(84, 196, 255);
   const uint16_t text = pack1555(238, 243, 249);
   const uint16_t dim = pack1555(139, 153, 174);
   const uint16_t border = pack1555(68, 82, 104);

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, bg);
   fill_rect(fb, 0, 0, (int)fb.w, 142, header);
   fill_rect(fb, 0, 140, (int)fb.w, 2, pack1555(20, 44, 70));

   draw_text(fb, 38, 24, "H3531 GAME LIBRARY", 3, text);
   std::string runtime = "RetroArch / libretro";
   int rw = text_width(runtime, 2);
   draw_text(fb, (int)fb.w - rw - 38, 30, runtime, 2, dim);

   if (vis.empty())
   {
      draw_text(fb, 80, 250, "NO GAMES FOUND", 5, text);
      draw_text(fb, 82, 330, "Put ROMs in /mnt/usb/games/nes or /mnt/usb/games/md", 2, dim);
      draw_text(fb, 82, 370, "F5: rescan   F1: RetroArch service menu   ESC: exit", 2, dim);
      return;
   }

   stage41_draw_system_strip(fb, systems, visible_pos, vis);

   const SystemDef &sys = systems[vis[visible_pos]];
   if (sys.games.empty()) return;
   if (game_pos >= sys.games.size()) game_pos = 0;
   const Game &selected = sys.games[game_pos];

   int center = (int)fb.w / 2;
   int card_center_y = 349 + (int)std::lround(system_shift * 24.0f);
   std::map<size_t, Stage41CardVisual> unique;

   for (int rel = -3; rel <= 3; ++rel)
   {
      float effective = (float)rel + game_shift;
      if (std::fabs(effective) > 2.08f) continue;
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      size_t idx = (size_t)gp;
      float focus = stage41_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));
      auto it = unique.find(idx);
      if (it == unique.end() || std::fabs(effective) < std::fabs(it->second.rel))
         unique[idx] = Stage41CardVisual{idx, effective, focus};
   }

   std::vector<Stage41CardVisual> cards;
   for (auto &kv : unique) cards.push_back(kv.second);
   std::sort(cards.begin(), cards.end(), [](const Stage41CardVisual &a, const Stage41CardVisual &b) {
      return a.focus < b.focus;
   });

   for (const auto &cv : cards)
   {
      const Game &g = sys.games[cv.index];
      float f = cv.focus;
      int w = (int)std::lround(STAGE41_SMALL_W + (STAGE41_SELECTED_W - STAGE41_SMALL_W) * f);
      int h = (int)std::lround(STAGE41_SMALL_H + (STAGE41_SELECTED_H - STAGE41_SMALL_H) * f);
      int x = center + (int)std::lround(stage41_card_center_offset(cv.rel)) - w / 2;
      int y = card_center_y - h / 2;
      int frame = 2 + (int)std::lround(3.0f * f);

      if (f > 0.18f) fill_rect(fb, x + 8, y + 10, w + 14, h + 14, shadow);
      fill_rect(fb, x - 7, y - 7, w + 14, h + 14, panel);
      fill_rect(fb, x, y, w, h, panel2);
      if (!stage41_draw_art(fb, g.image_path, x, y, w, h))
         draw_fallback_card(fb, sys, x, y, w, h);
      frame_rect(fb, x - 7, y - 7, w + 14, h + 14, frame, f > 0.5f ? accent : border);

      if (f < 0.25f)
      {
         std::string label = stage41_ellipsize_px(g.title, 1, w);
         draw_text(fb, x, y + h + 12, label, 1, dim);
      }
   }

   std::string title = stage41_ellipsize_px(selected.title, 3, 760);
   int tw = text_width(title, 3);
   draw_text(fb, center - tw / 2, 558, title, 3, text);
   std::string counter = std::to_string(game_pos + 1) + " / " + std::to_string(sys.games.size());
   int cw = text_width(counter, 2);
   draw_text(fb, center - cw / 2, 602, counter, 2, dim);

   int footer_y = (int)fb.h - 58;
   fill_rect(fb, 0, footer_y, (int)fb.w, 58, header);
   fill_rect(fb, 0, footer_y, (int)fb.w, 2, accent);
   draw_text(fb, 26, footer_y + 20,
         "LEFT/RIGHT GAME   UP/DOWN SYSTEM   ENTER PLAY   F5 RESCAN   F1 RETROARCH   ESC EXIT",
         2, dim);

#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
}

struct Stage41Anim {
   int game_dir = 0;
   int system_dir = 0;
   uint64_t game_start = 0;
   uint64_t system_start = 0;
};

static bool stage41_anim_active(const Stage41Anim &a)
{
   return a.game_dir != 0 || a.system_dir != 0;
}

static void stage41_finish_animations(Stage41Anim &a, uint64_t now, bool &redraw)
{
   if (a.game_dir && now - a.game_start >= STAGE41_ANIMATION_MS)
   {
      a.game_dir = 0;
      redraw = true;
   }
   if (a.system_dir && now - a.system_start >= STAGE41_ANIMATION_MS)
   {
      a.system_dir = 0;
      redraw = true;
   }
}

static float stage41_shift(int dir, uint64_t start, uint64_t now)
{
   if (!dir) return 0.0f;
   float t = (float)(now - start) / (float)STAGE41_ANIMATION_MS;
   return (float)dir * (1.0f - stage41_ease_out(t));
}

static int stage41_layout_test()
{
   const int widths[5] = {STAGE41_SMALL_W, STAGE41_SMALL_W, STAGE41_SELECTED_W,
                          STAGE41_SMALL_W, STAGE41_SMALL_W};
   const float rels[5] = {-2.f, -1.f, 0.f, 1.f, 2.f};
   int centers[5];
   for (int i = 0; i < 5; ++i) centers[i] = (int)std::lround(stage41_card_center_offset(rels[i]));
   for (int i = 1; i < 5; ++i)
   {
      int prev_right = centers[i - 1] + widths[i - 1] / 2;
      int next_left = centers[i] - widths[i] / 2;
      int gap = next_left - prev_right;
      printf("CARD_GAP %d->%d = %d\n", i - 1, i, gap);
      if (gap != STAGE41_CARD_GAP) return 11;
   }
   int sys_gap = 360 - (250 / 2 + 420 / 2);
   printf("SYSTEM_SLOT_GAP=%d CARD_GAP=%d ANIMATION_MS=%llu\n",
         sys_gap, STAGE41_CARD_GAP, (unsigned long long)STAGE41_ANIMATION_MS);
   if (sys_gap <= 0) return 12;
   printf("LAYOUT_TEST_OK\n");
   return 0;
}

int main(int argc, char **argv)
{
   if (argc >= 2 && std::string(argv[1]) == "--layout-test")
      return stage41_layout_test();
   if (argc >= 2 && std::string(argv[1]) == "--self-test")
      return stage40_main(argc, argv);

   std::string cfg = kDefaultConfig;
   if (argc >= 3 && std::string(argv[1]) == "--config") cfg = argv[2];

   std::vector<SystemDef> systems;
   if (!load_systems(cfg, systems)) return 2;
   scan_all(systems);

   Fb fb;
   Input in;
   if (!fb_open(fb)) return 3;
   input_open(in);

   fprintf(stderr, "[GAMEFRONT] Stage4.1 polish active: unique system slots, equal-gap carousel, %llums animation\n",
         (unsigned long long)STAGE41_ANIMATION_MS);

   size_t visible_pos = 0;
   size_t game_pos = 0;
   bool redraw = true;
   bool running = true;
   uint64_t last_draw = 0;
   Stage41Anim anim;

   while (running)
   {
      auto vis = visible_systems(systems);
      if (!vis.empty() && visible_pos >= vis.size()) visible_pos = 0;
      if (!vis.empty())
      {
         const auto &games = systems[vis[visible_pos]].games;
         if (!games.empty() && game_pos >= games.size()) game_pos = 0;
      }

      uint64_t now = stage41_now_ms();
      stage41_finish_animations(anim, now, redraw);
      bool animating = stage41_anim_active(anim);
      if (animating && now - last_draw >= STAGE41_FRAME_MS) redraw = true;

      if (redraw)
      {
         float game_shift = stage41_shift(anim.game_dir, anim.game_start, now);
         float system_shift = stage41_shift(anim.system_dir, anim.system_start, now);
         stage41_draw_ui(fb, systems, visible_pos, vis, game_pos, game_shift, system_shift);
         redraw = false;
         last_draw = now;
      }

      Action a = input_poll(in);
      switch (a)
      {
         case Action::PrevGame:
            if (!vis.empty() && !anim.game_dir)
            {
               auto &g = systems[vis[visible_pos]].games;
               if (!g.empty())
               {
                  game_pos = game_pos ? game_pos - 1 : g.size() - 1;
                  anim.game_dir = -1;
                  anim.game_start = stage41_now_ms();
                  redraw = true;
               }
            }
            break;
         case Action::NextGame:
            if (!vis.empty() && !anim.game_dir)
            {
               auto &g = systems[vis[visible_pos]].games;
               if (!g.empty())
               {
                  game_pos = (game_pos + 1) % g.size();
                  anim.game_dir = 1;
                  anim.game_start = stage41_now_ms();
                  redraw = true;
               }
            }
            break;
         case Action::PrevSystem:
            if (!vis.empty() && !anim.system_dir)
            {
               visible_pos = visible_pos ? visible_pos - 1 : vis.size() - 1;
               game_pos = 0;
               anim.system_dir = -1;
               anim.system_start = stage41_now_ms();
               redraw = true;
            }
            break;
         case Action::NextSystem:
            if (!vis.empty() && !anim.system_dir)
            {
               visible_pos = (visible_pos + 1) % vis.size();
               game_pos = 0;
               anim.system_dir = 1;
               anim.system_start = stage41_now_ms();
               redraw = true;
            }
            break;
         case Action::Launch:
            if (!vis.empty())
            {
               SystemDef &s = systems[vis[visible_pos]];
               if (!s.games.empty())
               {
                  anim = Stage41Anim{};
                  run_external(fb, in, launch_command(s, s.games[game_pos]));
                  redraw = true;
               }
            }
            break;
         case Action::ServiceMenu:
            anim = Stage41Anim{};
            run_external(fb, in, std::string(kRetroArchMenu));
            redraw = true;
            break;
         case Action::Rescan:
            scan_all(systems);
            stage41_clear_art_cache();
            visible_pos = 0;
            game_pos = 0;
            anim = Stage41Anim{};
            redraw = true;
            break;
         case Action::Exit:
            running = false;
            break;
         case Action::None:
         default:
            break;
      }
      usleep(animating ? 5000 : 10000);
   }

   fill_rect(fb, 0, 0, (int)fb.w, (int)fb.h, pack1555(0, 0, 0));
   input_close(in);
   fb_close(fb);
   fprintf(stderr, "[GAMEFRONT] Stage4.1 clean exit\n");
   return 0;
}
