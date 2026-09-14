/* H3531 RetroArch Stage 3.10 audio integration.
 *
 * Goal: keep the physically working AO transport and RetroArch's standard
 * threaded audio pipeline, but stop AO queue transitions from owning emulation
 * speed. The runloop is paced separately by RetroArch's exact-content timer.
 *
 * Stage3.8 supplies the corrected AO queue accounting and transport. Stage3.10
 * adds only wait_writable() so the standard threaded pipeline can host it.
 * frames_consumed() is deliberately NULL: the coarse 160-frame AO queue state
 * is useful for write_avail/rate control, but it is not used as the core clock.
 */

#define audio_oss audio_oss_stage38_transport
#include "h3531_ao_audio_base.inc"
#undef audio_oss

static void *h3531_audio_init_stage310(const char *device,
      unsigned rate, unsigned latency, unsigned *new_out_rate)
{
   void *ctx = h3531_audio_init(device, rate, latency, new_out_rate);
   if (ctx)
      RARCH_LOG("[H3531 AO] Stage3.10 exact-core-timer transport active; AO is not the emulation clock\n");
   return ctx;
}

static size_t h3531_audio_wait_writable_stage310(void *data, size_t len)
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

audio_driver_t audio_oss = {
   h3531_audio_init_stage310,
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
   h3531_audio_wait_writable_stage310,
   NULL, /* frames_consumed: exact RetroArch frame timer owns core pacing */
   NULL, /* underruns */
   h3531_audio_layout
};
