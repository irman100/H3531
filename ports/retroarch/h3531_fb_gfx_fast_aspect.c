/* H3531 RetroArch Stage 3.6 framebuffer fast aspect scaler.
 *
 * Viewport/aspect policy remains entirely upstream RetroArch via
 * video_driver_update_viewport().  This file only accelerates the final
 * nearest-neighbour presentation into the Hi3531 A1R5G5B5 framebuffer.
 *
 * Stage 3.5 rendered every destination pixel into a full shadow frame and
 * then copied that full frame again to /dev/fb0.  At the proven NES viewport
 * 256x224 -> 879x672 this cost ~17.3 ms/present on real hardware.
 *
 * Stage 3.6 uses the standard software-scaler optimisation for nearest
 * neighbour output:
 *   - X/Y source maps are precomputed only when geometry changes;
 *   - one converted/scaled scanline is kept in normal RAM;
 *   - repeated destination rows reuse that scanline;
 *   - scanlines are copied directly to the mmap'd framebuffer;
 *   - there is no full-frame shadow buffer and no per-pixel division.
 */

#include <time.h>
#include "../video_driver.h"

#define h3531_present h3531_present_reference
#define h3531_frame h3531_frame_reference
#define h3531_free h3531_free_reference
#define video_fpga video_fpga_reference
#include "h3531_fb_gfx_base.inc"
#undef h3531_present
#undef h3531_frame
#undef h3531_free
#undef video_fpga

static uint16_t *h3531_row = NULL;
static size_t h3531_row_cap = 0;
static uint32_t *h3531_xmap = NULL;
static size_t h3531_xmap_cap = 0;
static uint32_t *h3531_ymap = NULL;
static size_t h3531_ymap_cap = 0;
static unsigned h3531_map_src_w = 0;
static unsigned h3531_map_src_h = 0;
static unsigned h3531_map_dw = 0;
static unsigned h3531_map_dh = 0;
static bool h3531_fastpath_logged = false;

static uint64_t h3531_perf_frames = 0;
static uint64_t h3531_perf_present_ns = 0;
static struct timespec h3531_perf_window_start;
static bool h3531_perf_started = false;

static uint64_t h3531_timespec_ns(const struct timespec *ts)
{
   return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static void h3531_perf_record(const struct timespec *t0,
      const struct timespec *t1)
{
   uint64_t t0_ns = h3531_timespec_ns(t0);
   uint64_t t1_ns = h3531_timespec_ns(t1);

   if (!h3531_perf_started)
   {
      h3531_perf_window_start = *t0;
      h3531_perf_started = true;
   }

   h3531_perf_present_ns += t1_ns - t0_ns;
   ++h3531_perf_frames;

   if (h3531_perf_frames >= 300U)
   {
      uint64_t start_ns = h3531_timespec_ns(&h3531_perf_window_start);
      uint64_t elapsed_ns = t1_ns - start_ns;
      double fps = elapsed_ns ?
         ((double)h3531_perf_frames * 1000000000.0 / (double)elapsed_ns) : 0.0;
      double present_ms = h3531_perf_frames ?
         ((double)h3531_perf_present_ns / (double)h3531_perf_frames /
          1000000.0) : 0.0;

      RARCH_LOG("[H3531] PERF frames=%llu fps=%.2f present_avg=%.3fms\n",
            (unsigned long long)h3531_perf_frames, fps, present_ms);

      h3531_perf_frames = 0;
      h3531_perf_present_ns = 0;
      h3531_perf_window_start = *t1;
   }
}

static uint16_t h3531_fast_rgb565_to_a1r5g5b5(uint16_t p)
{
   return (uint16_t)(0x8000U | ((p >> 1) & 0x7fe0U) | (p & 0x001fU));
}

static bool h3531_fast_retroarch_rect(const h3531_fb_t *h,
      unsigned *dx, unsigned *dy, unsigned *dw, unsigned *dh)
{
   struct video_viewport vp;
   settings_t *settings;
   bool keep_aspect;
   unsigned x;
   unsigned y;
   unsigned w;
   unsigned hh;

   if (!h || !h->screen_w || !h->screen_h)
      return false;

   memset(&vp, 0, sizeof(vp));
   vp.full_width = h->screen_w;
   vp.full_height = h->screen_h;

   settings = config_get_ptr();
   keep_aspect = settings ? settings->bools.video_force_aspect : true;
   video_driver_update_viewport(&vp, false, keep_aspect, true);

   if (!vp.width || !vp.height)
      return false;

   x = vp.x < 0 ? 0U : (unsigned)vp.x;
   y = vp.y < 0 ? 0U : (unsigned)vp.y;
   if (x >= h->screen_w || y >= h->screen_h)
      return false;

   w = vp.width;
   hh = vp.height;
   if (w > h->screen_w - x)
      w = h->screen_w - x;
   if (hh > h->screen_h - y)
      hh = h->screen_h - y;
   if (!w || !hh)
      return false;

   *dx = x;
   *dy = y;
   *dw = w;
   *dh = hh;
   return true;
}

static bool h3531_reserve_maps(unsigned src_w, unsigned src_h,
      unsigned dw, unsigned dh)
{
   uint16_t *new_row;
   uint32_t *new_xmap;
   uint32_t *new_ymap;
   unsigned x;
   unsigned y;

   if (dw > h3531_row_cap)
   {
      new_row = (uint16_t*)realloc(h3531_row, (size_t)dw * sizeof(*h3531_row));
      if (!new_row)
         return false;
      h3531_row = new_row;
      h3531_row_cap = dw;
   }

   if (dw > h3531_xmap_cap)
   {
      new_xmap = (uint32_t*)realloc(h3531_xmap,
            (size_t)dw * sizeof(*h3531_xmap));
      if (!new_xmap)
         return false;
      h3531_xmap = new_xmap;
      h3531_xmap_cap = dw;
   }

   if (dh > h3531_ymap_cap)
   {
      new_ymap = (uint32_t*)realloc(h3531_ymap,
            (size_t)dh * sizeof(*h3531_ymap));
      if (!new_ymap)
         return false;
      h3531_ymap = new_ymap;
      h3531_ymap_cap = dh;
   }

   if (src_w == h3531_map_src_w && src_h == h3531_map_src_h &&
       dw == h3531_map_dw && dh == h3531_map_dh)
      return true;

   for (x = 0; x < dw; ++x)
   {
      uint32_t sx = (uint32_t)(((uint64_t)x * src_w) / dw);
      if (sx >= src_w)
         sx = src_w - 1U;
      h3531_xmap[x] = sx;
   }

   for (y = 0; y < dh; ++y)
   {
      uint32_t sy = (uint32_t)(((uint64_t)y * src_h) / dh);
      if (sy >= src_h)
         sy = src_h - 1U;
      h3531_ymap[y] = sy;
   }

   h3531_map_src_w = src_w;
   h3531_map_src_h = src_h;
   h3531_map_dw = dw;
   h3531_map_dh = dh;

   RARCH_LOG("[H3531] scaler maps rebuilt: %ux%u -> %ux%u\n",
         src_w, src_h, dw, dh);
   return true;
}

static void h3531_build_scaled_row_rgb565(const uint16_t *src,
      unsigned src_w, unsigned dw)
{
   uint32_t last_sx = UINT32_MAX;
   uint16_t color = 0;
   unsigned x;

   (void)src_w;
   for (x = 0; x < dw; ++x)
   {
      uint32_t sx = h3531_xmap[x];
      if (sx != last_sx)
      {
         color = h3531_fast_rgb565_to_a1r5g5b5(src[sx]);
         last_sx = sx;
      }
      h3531_row[x] = color;
   }
}

static bool h3531_present_rowreuse_rgb565(h3531_fb_t *h,
      const uint8_t *src, unsigned src_w, unsigned src_h,
      unsigned src_pitch, unsigned dx, unsigned dy,
      unsigned dw, unsigned dh)
{
   uint32_t cached_sy = UINT32_MAX;
   unsigned y;

   if (!h3531_reserve_maps(src_w, src_h, dw, dh))
      return false;

   for (y = 0; y < dh; ++y)
   {
      uint32_t sy = h3531_ymap[y];
      uint16_t *dst;

      if (sy != cached_sy)
      {
         const uint16_t *srow = (const uint16_t*)(src + (size_t)sy * src_pitch);
         h3531_build_scaled_row_rgb565(srow, src_w, dw);
         cached_sy = sy;
      }

      dst = (uint16_t*)(h->mem + (size_t)(dy + y) * h->stride) + dx;
      memcpy(dst, h3531_row, (size_t)dw * sizeof(*h3531_row));
   }

#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   return true;
}

static void h3531_present_fast(h3531_fb_t *h, const void *src,
      unsigned src_w, unsigned src_h, unsigned src_pitch,
      unsigned bits, bool menu_texture)
{
   unsigned dx, dy, dw, dh;
   bool retroarch_rect;

   if (!h || !h->mem || !src || !src_w || !src_h || !src_pitch)
      return;

   if (menu_texture || bits != 16U)
   {
      h3531_present_reference(h, src, src_w, src_h, src_pitch,
            bits, menu_texture);
      return;
   }

   retroarch_rect = h3531_fast_retroarch_rect(h, &dx, &dy, &dw, &dh);
   if (!retroarch_rect)
      h3531_calc_rect(h, src_w, src_h, &dx, &dy, &dw, &dh);

   if (dx != h->last_x || dy != h->last_y ||
       dw != h->last_w || dh != h->last_h)
   {
      h3531_clear(h);
      h->last_x = dx;
      h->last_y = dy;
      h->last_w = dw;
      h->last_h = dh;
   }

   if (!h3531_present_rowreuse_rgb565(h, (const uint8_t*)src,
            src_w, src_h, src_pitch, dx, dy, dw, dh))
   {
      h3531_present_reference(h, src, src_w, src_h, src_pitch,
            bits, menu_texture);
      return;
   }

   if (!h3531_fastpath_logged)
   {
      RARCH_LOG("[H3531] RetroArch viewport active: %ux%u+%u+%u\n",
            dw, dh, dx, dy);
      RARCH_LOG("[H3531] RGB565 row-reuse fastpath active: %ux%u -> %ux%u (retroarch-viewport)\n",
            src_w, src_h, dw, dh);
      h3531_fastpath_logged = true;
   }
}

static bool h3531_frame_fast(void *data, const void *frame,
      unsigned frame_width, unsigned frame_height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   h3531_fb_t *h = (h3531_fb_t*)data;
   const void *src = frame;
   unsigned width = frame_width;
   unsigned height = frame_height;
   unsigned src_pitch = pitch;
   unsigned bits = h ? h->frame_bits : 16U;
   bool menu_texture = false;
#ifdef HAVE_MENU
   bool menu_is_alive = video_info &&
      (video_info->menu_st_flags & MENU_ST_FLAG_ALIVE);
#else
   bool menu_is_alive = false;
#endif

   (void)frame_count;
   (void)msg;

   if (!h)
      return false;

#ifdef HAVE_MENU
   menu_driver_frame(menu_is_alive, video_info);

   if (h->menu_frame && menu_is_alive)
   {
      src          = h->menu_frame;
      width        = h->menu_width;
      height       = h->menu_height;
      src_pitch    = h->menu_pitch;
      bits         = h->menu_bits;
      menu_texture = true;
   }
   else
#endif
   {
      if (frame_width > 4 && frame_height > 4)
      {
         h->frame_width  = frame_width;
         h->frame_height = frame_height;
         h->frame_pitch  = pitch;
      }

      width     = h->frame_width;
      height    = h->frame_height;
      src_pitch = h->frame_pitch;

      if (menu_is_alive && frame_width <= 4 && frame_height <= 4)
         return true;
   }

   if (src && width && height && src_pitch)
   {
      bool measure_game = !menu_texture && frame_width > 4U && frame_height > 4U;
      struct timespec t0, t1;

      if (measure_game)
         clock_gettime(CLOCK_MONOTONIC, &t0);

      h3531_present_fast(h, src, width, height, src_pitch, bits, menu_texture);

      if (measure_game)
      {
         clock_gettime(CLOCK_MONOTONIC, &t1);
         h3531_perf_record(&t0, &t1);
      }
   }

   return true;
}

static void h3531_free_fast(void *data)
{
   free(h3531_row);
   free(h3531_xmap);
   free(h3531_ymap);
   h3531_row = NULL;
   h3531_xmap = NULL;
   h3531_ymap = NULL;
   h3531_row_cap = 0;
   h3531_xmap_cap = 0;
   h3531_ymap_cap = 0;
   h3531_map_src_w = 0;
   h3531_map_src_h = 0;
   h3531_map_dw = 0;
   h3531_map_dh = 0;
   h3531_fastpath_logged = false;
   h3531_perf_frames = 0;
   h3531_perf_present_ns = 0;
   h3531_perf_started = false;
   h3531_free_reference(data);
}

video_driver_t video_fpga = {
   h3531_init,
   h3531_frame_fast,
   h3531_set_nonblock_state,
   h3531_alive,
   h3531_focus,
   h3531_suppress_screensaver,
   h3531_has_windowed,
   h3531_set_shader,
   h3531_free_fast,
   "h3531",
   h3531_set_viewport,
   h3531_set_rotation,
   NULL,
   NULL,
   NULL,
#ifdef HAVE_OVERLAY
   NULL,
#endif
   h3531_get_poke_interface,
   NULL,
   NULL,
   NULL,
#ifdef HAVE_GFX_WIDGETS
   NULL
#endif
};
