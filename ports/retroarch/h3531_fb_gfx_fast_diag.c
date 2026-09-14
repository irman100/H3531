/* H3531 RetroArch Stage 3.2 framebuffer wrapper.
 *
 * Builds on the physically proven Stage 3.1 fast RGB565 path and adds only a
 * lightweight timing diagnostic.  Every 300 presented game frames it reports
 * effective video-driver FPS and average presentation time to UART/stderr via
 * RetroArch logging.  This lets the board test tell us whether the remaining
 * slowdown is in emulation/runloop or framebuffer presentation.
 */

#include <time.h>

#define h3531_present_fast h3531_present_fast_stage31
#define h3531_frame_fast h3531_frame_fast_stage31
#define h3531_free_fast h3531_free_fast_stage31
#define video_fpga video_fpga_stage31
#include "h3531_fb_gfx_fast_base.inc"
#undef h3531_present_fast
#undef h3531_frame_fast
#undef h3531_free_fast
#undef video_fpga

static uint64_t h3531_perf_frames = 0;
static uint64_t h3531_perf_present_ns = 0;
static struct timespec h3531_perf_window_start;
static bool h3531_perf_started = false;

static uint64_t h3531_timespec_ns(const struct timespec *ts)
{
   return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static bool h3531_frame_stage32(void *data, const void *frame,
      unsigned frame_width, unsigned frame_height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   struct timespec t0, t1;
   bool ok;

   clock_gettime(CLOCK_MONOTONIC, &t0);
   ok = h3531_frame_fast_stage31(data, frame, frame_width, frame_height,
         frame_count, pitch, msg, video_info);
   clock_gettime(CLOCK_MONOTONIC, &t1);

   /* Count only normal game-sized frames, not the tiny/menu sentinel frames. */
   if (frame_width > 4U && frame_height > 4U)
   {
      uint64_t t0_ns = h3531_timespec_ns(&t0);
      uint64_t t1_ns = h3531_timespec_ns(&t1);

      if (!h3531_perf_started)
      {
         h3531_perf_window_start = t0;
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
            ((double)h3531_perf_present_ns / (double)h3531_perf_frames / 1000000.0) : 0.0;

         RARCH_LOG("[H3531] PERF frames=%llu fps=%.2f present_avg=%.3fms\n",
               (unsigned long long)h3531_perf_frames, fps, present_ms);

         h3531_perf_frames = 0;
         h3531_perf_present_ns = 0;
         h3531_perf_window_start = t1;
      }
   }

   return ok;
}

static void h3531_free_stage32(void *data)
{
   h3531_perf_frames = 0;
   h3531_perf_present_ns = 0;
   h3531_perf_started = false;
   h3531_free_fast_stage31(data);
}

video_driver_t video_fpga = {
   h3531_init,
   h3531_frame_stage32,
   h3531_set_nonblock_state,
   h3531_alive,
   h3531_focus,
   h3531_suppress_screensaver,
   h3531_has_windowed,
   h3531_set_shader,
   h3531_free_stage32,
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
