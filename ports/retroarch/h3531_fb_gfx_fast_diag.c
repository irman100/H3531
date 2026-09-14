/* H3531 RetroArch Stage 3.4 framebuffer fast path + diagnostics.
 *
 * IMPORTANT: this file is the translation unit copied to RetroArch's
 * gfx/drivers/fpga_gfx.c by the Stage3.4 workflow.  The previous revision
 * accidentally carried a private copy of the older Stage3.2 renderer, so the
 * current h3531_fb_gfx_fast.c (including the 2x integer-scale cap) was never
 * part of the final frontend even though CI validated its source file.
 *
 * Keep one canonical fast renderer: the workflow copies h3531_fb_gfx_fast.c
 * next to this file as h3531_fb_gfx_fast_base.inc.  We include that exact
 * implementation here, rename only its exported driver object/frame callback,
 * and wrap the live frame callback with lightweight timing.  Therefore the
 * binary marker [H3531] integer scale cap=2 proves that the same renderer used
 * on hardware contains the Stage3.4 cap.
 *
 * Every 300 game frames UART/stderr receives:
 *   [H3531] PERF frames=300 fps=... present_avg=...ms
 */

#include <time.h>

/* Pull in the canonical Stage3.4 renderer while reserving the public
 * video_fpga symbol for the diagnostic wrapper below. */
#define h3531_frame_fast h3531_frame_fast_stage34
#define video_fpga video_fpga_stage34_base
#include "h3531_fb_gfx_fast_base.inc"
#undef video_fpga
#undef h3531_frame_fast

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

static bool h3531_frame_fast_diag(void *data, const void *frame,
      unsigned frame_width, unsigned frame_height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   bool measure_game = frame_width > 4U && frame_height > 4U;
   bool result;
   struct timespec t0;
   struct timespec t1;

#ifdef HAVE_MENU
   if (video_info && (video_info->menu_st_flags & MENU_ST_FLAG_ALIVE))
      measure_game = false;
#endif

   if (measure_game)
      clock_gettime(CLOCK_MONOTONIC, &t0);

   result = h3531_frame_fast_stage34(data, frame, frame_width, frame_height,
         frame_count, pitch, msg, video_info);

   if (measure_game)
   {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      h3531_perf_record(&t0, &t1);
   }

   return result;
}

static void h3531_free_fast_diag(void *data)
{
   h3531_perf_frames = 0;
   h3531_perf_present_ns = 0;
   h3531_perf_started = false;
   h3531_free_fast(data);
}

/* Public driver object used by RetroArch's HAVE_FPGA registration slot. */
video_driver_t video_fpga = {
   h3531_init,
   h3531_frame_fast_diag,
   h3531_set_nonblock_state,
   h3531_alive,
   h3531_focus,
   h3531_suppress_screensaver,
   h3531_has_windowed,
   h3531_set_shader,
   h3531_free_fast_diag,
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
