/* H3531 RetroArch Stage 3.9 audio integration.
 *
 * Keep the Stage3.7 AO transport and threaded-pipeline behaviour that was
 * physically better during game startup/intros, but add the standard
 * RetroArch device-clock callback. The Hi3531 driver exposes queued AO blocks,
 * so consumed frames are the normal queue-driver calculation:
 *
 *   submitted frames - frames still queued in hardware
 *
 * This is the same model used by upstream OSS/ALSA-style queue drivers. It
 * gives RetroArch a real hardware-clock observation instead of asking the
 * frontend to guess long-term rate from nominal 48 kHz alone.
 *
 * The Stage3.7 implementation is included as a build-time base. Rename its
 * exported vtable only; all of its static transport helpers remain in this
 * translation unit and are reused below.
 */

#define audio_oss audio_oss_stage37_transport
#include "h3531_ao_audio_base.inc"
#undef audio_oss

static void *h3531_clock_ctx;
static size_t h3531_clock_floor;
static unsigned h3531_clock_reads;

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

   /* blocks_sent is incremented only after AO accepted a 160-frame block. */
   submitted = ctx->blocks_sent * (uint64_t)H3531_AO_SAMPLES;
   queued = (uint64_t)busy_blocks * (uint64_t)H3531_AO_SAMPLES;
   consumed = submitted > queued ? submitted - queued : 0;

   /* frames_consumed() is required to be monotonic. QueryChnState is block
    * granular, so clamp a transient boundary observation rather than letting
    * the frontend see the device clock move backwards. */
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
   h3531_audio_wait_writable,
   h3531_audio_frames_consumed,
   NULL, /* underruns */
   h3531_audio_layout
};
