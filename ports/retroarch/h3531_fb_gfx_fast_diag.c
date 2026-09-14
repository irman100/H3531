/* H3531 RetroArch Stage 3.2 framebuffer fast path + diagnostics.
 *
 * This is deliberately a single video-driver translation unit.  It keeps the
 * Stage 3.1 RGB565 fast path and adds lightweight timing around the actual
 * presentation call.  We do not include the Stage 3.1 fast driver and then
 * define a second video_fpga object: doing so creates two driver definitions.
 *
 * Every 300 game frames UART/stderr receives:
 *   [H3531] PERF frames=300 fps=... present_avg=...ms
 */

#include <time.h>

#define h3531_present h3531_present_reference
#define h3531_frame h3531_frame_reference
#define h3531_free h3531_free_reference
#define video_fpga video_fpga_reference
#include "h3531_fb_gfx_base.inc"
#undef h3531_present
#undef h3531_frame
#undef h3531_free
#undef video_fpga

static uint16_t *h3531_fast_shadow = NULL;
static size_t h3531_fast_shadow_pixels = 0;
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
   /* Exact equivalent of the reference RGB565 conversion, but only shifts
    * and masks: R5 stays R5, G6 drops its LSB to G5, B5 stays B5. */
   return (uint16_t)(0x8000U | ((p >> 1) & 0x7fe0U) | (p & 0x001fU));
}

static bool h3531_fast_shadow_reserve(size_t pixels)
{
   uint16_t *tmp;

   if (pixels <= h3531_fast_shadow_pixels)
      return true;

   tmp = (uint16_t*)realloc(h3531_fast_shadow, pixels * sizeof(uint16_t));
   if (!tmp)
      return false;

   h3531_fast_shadow = tmp;
   h3531_fast_shadow_pixels = pixels;
   return true;
}

static void h3531_fast_copy_shadow(h3531_fb_t *h,
      unsigned dx, unsigned dy, unsigned dw, unsigned dh)
{
   unsigned y;

   for (y = 0; y < dh; ++y)
   {
      uint16_t *dst = (uint16_t*)(h->mem + (size_t)(dy + y) * h->stride) + dx;
      const uint16_t *src = h3531_fast_shadow + (size_t)y * dw;
      memcpy(dst, src, (size_t)dw * sizeof(uint16_t));
   }

#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
}

static bool h3531_fast_integer_scale_rgb565(const uint8_t *src,
      unsigned src_w, unsigned src_h, unsigned src_pitch,
      unsigned dw, unsigned dh)
{
   unsigned scale;
   unsigned sy;

   if (!src_w || !src_h || !dw || !dh)
      return false;
   if (dw % src_w || dh % src_h)
      return false;

   scale = dw / src_w;
   if (!scale || scale != dh / src_h || scale > 4U)
      return false;

   for (sy = 0; sy < src_h; ++sy)
   {
      const uint16_t *s = (const uint16_t*)(src + (size_t)sy * src_pitch);
      uint16_t *first = h3531_fast_shadow + (size_t)(sy * scale) * dw;
      unsigned sx;

      if (scale == 3U)
      {
         for (sx = 0; sx < src_w; ++sx)
         {
            uint16_t c = h3531_fast_rgb565_to_a1r5g5b5(s[sx]);
            unsigned d = sx * 3U;
            first[d] = c;
            first[d + 1U] = c;
            first[d + 2U] = c;
         }
      }
      else if (scale == 2U)
      {
         for (sx = 0; sx < src_w; ++sx)
         {
            uint16_t c = h3531_fast_rgb565_to_a1r5g5b5(s[sx]);
            unsigned d = sx * 2U;
            first[d] = c;
            first[d + 1U] = c;
         }
      }
      else if (scale == 4U)
      {
         for (sx = 0; sx < src_w; ++sx)
         {
            uint16_t c = h3531_fast_rgb565_to_a1r5g5b5(s[sx]);
            unsigned d = sx * 4U;
            first[d] = c;
            first[d + 1U] = c;
            first[d + 2U] = c;
            first[d + 3U] = c;
         }
      }
      else
      {
         for (sx = 0; sx < src_w; ++sx)
            first[sx] = h3531_fast_rgb565_to_a1r5g5b5(s[sx]);
      }

      if (scale > 1U)
      {
         unsigned vy;
         for (vy = 1; vy < scale; ++vy)
            memcpy(first + (size_t)vy * dw,
                   first, (size_t)dw * sizeof(uint16_t));
      }
   }

   return true;
}

static void h3531_fast_generic_scale_rgb565(const uint8_t *src,
      unsigned src_w, unsigned src_h, unsigned src_pitch,
      unsigned dw, unsigned dh)
{
   uint32_t y_fp = 0;
   uint32_t x_step;
   uint32_t y_step;
   unsigned y;

   x_step = (uint32_t)(((uint64_t)src_w << 16) / dw);
   y_step = (uint32_t)(((uint64_t)src_h << 16) / dh);

   for (y = 0; y < dh; ++y)
   {
      unsigned sy = y_fp >> 16;
      const uint16_t *s;
      uint16_t *d = h3531_fast_shadow + (size_t)y * dw;
      uint32_t x_fp = 0;
      unsigned x;

      if (sy >= src_h)
         sy = src_h - 1U;
      s = (const uint16_t*)(src + (size_t)sy * src_pitch);

      for (x = 0; x < dw; ++x)
      {
         unsigned sx = x_fp >> 16;
         if (sx >= src_w)
            sx = src_w - 1U;
         d[x] = h3531_fast_rgb565_to_a1r5g5b5(s[sx]);
         x_fp += x_step;
      }

      y_fp += y_step;
   }
}

static void h3531_present_fast(h3531_fb_t *h, const void *src,
      unsigned src_w, unsigned src_h, unsigned src_pitch,
      unsigned bits, bool menu_texture)
{
   unsigned dx, dy, dw, dh;
   size_t pixels;
   bool integer_scaled;

   if (!h || !h->mem || !src || !src_w || !src_h || !src_pitch)
      return;

   /* RGUI and any 32-bit core keep the proven Stage3 renderer. */
   if (menu_texture || bits != 16U)
   {
      h3531_present_reference(h, src, src_w, src_h, src_pitch,
            bits, menu_texture);
      return;
   }

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

   pixels = (size_t)dw * dh;
   if (!h3531_fast_shadow_reserve(pixels))
   {
      h3531_present_reference(h, src, src_w, src_h, src_pitch,
            bits, menu_texture);
      return;
   }

   integer_scaled = h3531_fast_integer_scale_rgb565(
         (const uint8_t*)src, src_w, src_h, src_pitch, dw, dh);
   if (!integer_scaled)
      h3531_fast_generic_scale_rgb565((const uint8_t*)src,
            src_w, src_h, src_pitch, dw, dh);

   h3531_fast_copy_shadow(h, dx, dy, dw, dh);

   if (!h3531_fastpath_logged)
   {
      RARCH_LOG("[H3531] RGB565 fastpath active: %ux%u -> %ux%u (%s)\n",
            src_w, src_h, dw, dh,
            integer_scaled ? "integer" : "fixed-point");
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
   free(h3531_fast_shadow);
   h3531_fast_shadow = NULL;
   h3531_fast_shadow_pixels = 0;
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
