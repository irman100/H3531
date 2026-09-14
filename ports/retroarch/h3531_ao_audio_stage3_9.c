/* H3531 RetroArch Stage 3.9 audio integration.
 *
 * Goal: preserve the Stage3.7 threaded startup behaviour, but eliminate its
 * long-term timing drift by exposing the real Hi3531 AO queue as the device
 * clock to RetroArch.
 *
 * The Stage3.8 transport is used as the base because it contains the corrected
 * bounded write_avail() accounting. Stage3.9 adds back wait_writable() to use
 * RetroArch's standard threaded audio pipeline and adds frames_consumed()
 * using the normal queue-driver rule:
 *
 *   submitted frames - frames still queued in hardware
 *
 * No emulator/core timing constants are invented here. RetroArch owns pacing
 * and rate control; the H3531 adapter only reports actual hardware state.
 */

#define audio_oss audio_oss_stage38_transport
#include "h3531_ao_audio_base.inc"
#undef audio_oss

static void *h3531_clock_ctx;
static size_t h3531_clock_floor;
static unsigned h3531_clock_reads;

static size_t h3531_audio_wait_writable_stage39(void *data, size_t len)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   int rc;
   size_t avail;

   if (!ctx)
      return 0;

   rc = h3531_wait_free(ctx);
   if (rc != 0)
      return 0;

   avail = h3531_audio_write_avail(ctx);
   if (avail > len)
      avail = len;
   return avail;
}

static size_t h3531_audio_frames_consumed(void *data)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   uint32_t total = 0;
   uint32_t free_blocks = 0;
   uint32_t busy_blocks = 0;
   uint64_t submitted;
   uint64_t queued;
   uint64_t consumed;

   if (!ctx)
      return 0;

   if (data != h3531_clock_ctx)
   {
      h3531_clock_ctx = data;
      h3531_clock_floor = 0;
      h3531_clock_reads = 0;
   }

   if (h3531_query(ctx, &total, &free_blocks, &busy_blocks) != 0)
      return h3531_clock_floor;

   submitted = ctx->blocks_sent * (uint64_t)H3531_AO_SAMPLES;
   queued = (uint64_t)busy_blocks * (uint64_t)H3531_AO_SAMPLES;
   consumed = submitted > queued ? submitted - queued : 0;

   /* QueryChnState is block-granular. The public device clock must never move
    * backwards, so clamp a transient boundary observation. */
   if (consumed < (uint64_t)h3531_clock_floor)
      consumed = (uint64_t)h3531_clock_floor;
   else
      h3531_clock_floor = (size_t)consumed;

   ++h3531_clock_reads;
   if ((h3531_clock_reads % 4U) == 0U)
      RARCH_LOG("[H3531 AO] CLOCK consumed=%llu submitted=%llu queued_blocks=%u free=%u/%u\n",
            (unsigned long long)consumed,
            (unsigned long long)submitted,
            busy_blocks, free_blocks, total);

   return (size_t)consumed;
}

audio_driver_t audio_oss = {
   h3531_audio_init,
   h3531_audio_write,
   h3531_audio_stop,
   h3531_audio_start,
   h3531_audio_alive,
   h3531_audio_set_nonblock,
   h3531_audio_free,
   h3531_audio_use_float,
   "h3531",
   NULL, /* device_list_new */
   NULL, /* device_list_free */
   h3531_audio_write_avail,
   h3531_audio_buffer_size,
   NULL, /* write_raw */
   h3531_audio_wait_writable_stage39,
   h3531_audio_frames_consumed,
   NULL, /* underruns */
   h3531_audio_layout
};
