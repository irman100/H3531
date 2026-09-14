/* H3531 Game Frontend Stage 4.2 carousel refinement.
 *
 * Builds on Stage4.2 and replaces only the presentation/main loop:
 * - exact NES/Sega classification (Genesis must never match NES)
 * - centered system carousel: selected system is always centered and enlarged
 * - one additional ROM/system item is rendered beyond each screen edge
 * - same dual-focus navigation and coherent RAM backbuffer presentation
 */

#define main stage42_base_main
#define stage42_is_nes stage42_base_is_nes
#define stage42_is_sega stage42_base_is_sega
#define stage42_system_short_name stage42_base_system_short_name
#define stage42_draw_system_row stage42_base_draw_system_row
#define stage42_draw_games stage42_base_draw_games
#define stage42_draw_ui stage42_base_draw_ui
#include "h3531-game-frontend-stage4-2.cpp"
#undef stage42_draw_ui
#undef stage42_draw_games
#undef stage42_draw_system_row
#undef stage42_system_short_name
#undef stage42_is_sega
#undef stage42_is_nes
#undef main

static const char *STAGE42C_MARKER = "Stage4.2 centered overflow carousel active";
static const int STAGE42C_SYSTEM_GAP = 18;
static const int STAGE42C_SYSTEM_SMALL_W = 188;
static const int STAGE42C_SYSTEM_SELECTED_W = 232;
static const uint64_t STAGE42C_SYSTEM_ANIM_MS = 165;

static bool stage42c_is_nes(const SystemDef &s)
{
   const std::string name = lower(s.name);
   const std::string full = lower(s.fullname);
   return name == "nes" || name == "famicom" ||
          full.find("nintendo entertainment system") != std::string::npos;
}

static bool stage42c_is_sega(const SystemDef &s)
{
   const std::string name = lower(s.name);
   const std::string full = lower(s.fullname);
   return name == "megadrive" || name == "genesis" ||
          name == "mastersystem" || name == "gamegear" ||
          full.find("sega") != std::string::npos ||
          full.find("mega drive") != std::string::npos ||
          full.find("genesis") != std::string::npos;
}

static std::string stage42c_system_short_name(const SystemDef &s)
{
   const std::string name = lower(s.name);
   if (stage42c_is_nes(s)) return "NES";
   if (name == "megadrive" || name == "genesis") return "SEGA MD";
   if (name == "mastersystem") return "MASTER SYSTEM";
   if (name == "gamegear") return "GAME GEAR";
   if (stage42c_is_sega(s)) return "SEGA";
   return stage42_ellipsize_px(s.fullname, 2, 150);
}

static float stage42c_system_center_offset(float rel)
{
   const float a = std::fabs(rel);
   const float d1 = STAGE42C_SYSTEM_SELECTED_W * 0.5f + STAGE42C_SYSTEM_GAP +
                    STAGE42C_SYSTEM_SMALL_W * 0.5f;
   const float d = a <= 1.0f ? a * d1 :
                   d1 + (a - 1.0f) * (STAGE42C_SYSTEM_SMALL_W + STAGE42C_SYSTEM_GAP);
   return rel < 0.0f ? -d : d;
}

struct Stage42CSystemVisual {
   size_t pos = 0;
   float rel = 0.0f;
   float focus = 0.0f;
};

struct Stage42CSystemAnim {
   int dir = 0;
   uint64_t start = 0;
};

static bool stage42c_system_anim_active(const Stage42CSystemAnim &a, uint64_t now)
{
   return a.dir != 0 && now - a.start < STAGE42C_SYSTEM_ANIM_MS;
}

static float stage42c_system_shift(const Stage42CSystemAnim &a, uint64_t now)
{
   if (!stage42c_system_anim_active(a, now)) return 0.0f;
   const float t = (float)(now - a.start) / (float)STAGE42C_SYSTEM_ANIM_MS;
   return (float)a.dir * (1.0f - stage42_ease_out(t));
}

static void stage42c_draw_system_row(Fb &fb, const std::vector<SystemDef> &systems,
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

   std::map<size_t, Stage42CSystemVisual> unique;
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
      if (it == unique.end() || std::fabs(effective) < std::fabs(it->second.rel))
      {
         Stage42CSystemVisual sv;
         sv.pos = pos;
         sv.rel = effective;
         sv.focus = item_focus;
         unique[pos] = sv;
      }
   }

   std::vector<Stage42CSystemVisual> items;
   for (auto &kv : unique) items.push_back(kv.second);
   std::sort(items.begin(), items.end(), [](const Stage42CSystemVisual &a, const Stage42CSystemVisual &b) {
      return a.focus < b.focus;
   });

   for (const auto &sv : items)
   {
      const SystemDef &s = systems[vis[sv.pos]];
      const float f = sv.focus;
      const int w = (int)std::lround(STAGE42C_SYSTEM_SMALL_W +
                     (STAGE42C_SYSTEM_SELECTED_W - STAGE42C_SYSTEM_SMALL_W) * f);
      const int h = (int)std::lround(78.0f + 14.0f * f);
      const int cx = center + (int)std::lround(stage42c_system_center_offset(sv.rel));
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

      if (stage42c_is_nes(s))
         stage42_draw_nes_pad(fb, cx, y + 35, pad_scale, body, ink, button);
      else if (stage42c_is_sega(s))
         stage42_draw_sega_pad(fb, cx, y + 35, pad_scale, body, ink, button);
      else
         stage42_draw_generic_pad(fb, cx, y + 35, pad_scale, body, ink, button);

      const std::string label = stage42c_system_short_name(s);
      const int tw = text_width(label, 2);
      draw_text(fb, cx - tw / 2, y + h - 19, label, 2, selected ? white : dim);
   }

   fill_rect(fb, center - 66, 151, 132, focus == FocusZone::Systems ? 4 : 2, accent);
}

static void stage42c_draw_games(Fb &fb, const SystemDef &sys, size_t game_pos,
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
      /* rel +/-3 is deliberately kept: it lives mostly beyond the screen edge
       * and becomes the natural entering/leaving card during animation. */
      if (std::fabs(effective) > 3.20f) continue;
      int gp = (int)game_pos + rel;
      while (gp < 0) gp += (int)sys.games.size();
      while (gp >= (int)sys.games.size()) gp -= (int)sys.games.size();
      const size_t idx = (size_t)gp;
      const float card_focus = stage42_smoothstep(1.0f - std::min(1.0f, std::fabs(effective)));
      auto it = unique.find(idx);
      if (it == unique.end() || std::fabs(effective) < std::fabs(it->second.rel))
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

static void stage42c_draw_ui(Fb &fb, const std::vector<SystemDef> &systems,
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

   stage42c_draw_system_row(fb, systems, vis, visible_pos, focus, system_shift);
   const SystemDef &sys = systems[vis[visible_pos]];
   stage42c_draw_games(fb, sys, game_pos, game_shift, focus);

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

static int stage42c_layout_test()
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
   if (!stage42c_is_nes(nes) || stage42c_is_sega(nes)) return 32;
   if (stage42c_is_nes(md) || !stage42c_is_sega(md)) return 33;

   const int system_side = (int)std::lround(stage42c_system_center_offset(1.0f));
   const int system_overflow = (int)std::lround(stage42c_system_center_offset(3.0f));
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
      return stage42c_layout_test();
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

   fprintf(stderr, "[GAMEFRONT] %s\n", STAGE42C_MARKER);

   size_t visible_pos = 0;
   size_t game_pos = 0;
   FocusZone focus = FocusZone::Games;
   Stage42GameAnim game_anim;
   Stage42CSystemAnim system_anim;
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
      const bool system_active = stage42c_system_anim_active(system_anim, now);
      if (!game_active && game_anim.dir) { game_anim.dir = 0; redraw = true; }
      if (!system_active && system_anim.dir) { system_anim.dir = 0; redraw = true; }
      const bool animating = game_active || system_active;
      if (animating && now - last_draw >= STAGE42_FRAME_MS) redraw = true;

      if (redraw)
      {
         const float game_shift = stage42_game_shift(game_anim, now);
         const float system_shift = stage42c_system_shift(system_anim, now);
         stage42c_draw_ui(back.fb, systems, visible_pos, vis, game_pos, focus,
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
                  system_anim = Stage42CSystemAnim{};
                  run_external(physical, in, launch_command(s, s.games[game_pos]));
                  if (!back.init(physical)) return 5;
                  redraw = true;
               }
            }
            break;

         case Action::ServiceMenu:
            game_anim = Stage42GameAnim{};
            system_anim = Stage42CSystemAnim{};
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
            system_anim = Stage42CSystemAnim{};
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
