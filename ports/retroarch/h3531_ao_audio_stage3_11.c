/* H3531 RetroArch Stage 3.11 audio integration.
 *
 * Stage3.10 is physically proven for A/V speed. This stage keeps the exact
 * core timer and proven AO transport unchanged, but exposes only a 64 ms
 * working window of the 100 ms hardware FIFO to RetroArch. The hardware queue
 * itself remains 30 x 160 frames for ABI compatibility; the frontend sees a
 * 20-block effective queue (~66.7 ms at 48 kHz).
 *
 * This follows the normal audio_driver contract: buffer_size/write_avail/
 * wait_writable describe the usable playback buffer. No core timing or sample
 * rate constants are changed.
 */

#define audio_oss audio_oss_stage38_transport
#include "h3531_ao_audio_base.inc"
#undef audio_oss

#define H3531_STAGE311_TARGET_BLOCKS 20U
#define H3531_STAGE311_BLOCK_BYTES \
   (H3531_AO_SAMPLES * H3531_RA_STEREO_FRAME_BYTES)
#define H3531_STAGE311_WAIT_NS 1000000L
#define H3531_STAGE311_WAIT_LAPS 250U

static void *h3531_audio_init_stage311(const char *device,
      unsigned rate, unsigned latency, unsigned *new_out_rate)
{
   void *ctx = h3531_audio_init(device, rate, latency, new_out_rate);
   if (ctx)
      RARCH_LOG("[H3531 AO] Stage3.11 bounded-latency transport active: target=%u blocks %.1fms; AO is not the emulation clock\n",
            H3531_STAGE311_TARGET_BLOCKS,
            (double)(H3531_STAGE311_TARGET_BLOCKS * H3531_AO_SAMPLES) *
               1000.0 / (double)H3531_AO_SAMPLE_RATE);
   return ctx;
}

static size_t h3531_audio_write_avail_stage311(void *data)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   uint32_t total = 0;
   uint32_t free_blocks = 0;
   uint32_t busy_blocks = 0;
   size_t capacity_frames;
   size_t queued_frames;
   size_t avail_frames;

   if (!ctx)
      return 0;

   capacity_frames = (size_t)H3531_STAGE311_TARGET_BLOCKS * H3531_AO_SAMPLES;

   if (h3531_query(ctx, &total, &free_blocks, &busy_blocks) != 0)
      return H3531_STAGE311_BLOCK_BYTES;

   queued_frames = (size_t)busy_blocks * H3531_AO_SAMPLES + ctx->mono_fill;
   if (queued_frames >= capacity_frames)
      return 0;

   avail_frames = capacity_frames - queued_frames;
   return avail_frames * H3531_RA_STEREO_FRAME_BYTES;
}

static size_t h3531_audio_buffer_size_stage311(void *data)
{
   (void)data;
   return (size_t)H3531_STAGE311_TARGET_BLOCKS * H3531_AO_SAMPLES *
      H3531_RA_STEREO_FRAME_BYTES;
}

static size_t h3531_audio_wait_writable_stage311(void *data, size_t len)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   struct timespec ts;
   unsigned lap;

   if (!ctx)
      return 0;

   ts.tv_sec = 0;
   ts.tv_nsec = H3531_STAGE311_WAIT_NS;

   for (lap = 0; lap < H3531_STAGE311_WAIT_LAPS; ++lap)
   {
      size_t avail = h3531_audio_write_avail_stage311(ctx);
      size_t need = len < H3531_STAGE311_BLOCK_BYTES ? len : H3531_STAGE311_BLOCK_BYTES;

      if (avail >= need)
         return avail < len ? avail : len;

      if (ctx->nonblock)
         return 0;

      ++ctx->waits;
      while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
         ;
      ts.tv_sec = 0;
      ts.tv_nsec = H3531_STAGE311_WAIT_NS;
   }

   RARCH_WARN("[H3531 AO] bounded-latency wait timed out: avail=%zu target_blocks=%u busy=%u\n",
         h3531_audio_write_avail_stage311(ctx),
         H3531_STAGE311_TARGET_BLOCKS, ctx->last_busy);
   return 0;
}

audio_driver_t audio_oss = {
   h3531_audio_init_stage311,
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
   h3531_audio_write_avail_stage311,
   h3531_audio_buffer_size_stage311,
   NULL, /* write_raw */
   h3531_audio_wait_writable_stage311,
   NULL, /* frames_consumed: exact RetroArch frame timer owns core pacing */
   NULL, /* underruns */
   h3531_audio_layout
};
