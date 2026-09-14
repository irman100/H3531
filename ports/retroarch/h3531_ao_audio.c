/* H3531 AO audio driver for RetroArch.
 *
 * Integration policy:
 *   - implements RetroArch's standard audio_driver_t interface;
 *   - receives native-endian S16 stereo PCM from RetroArch;
 *   - downmixes to the physically proven Hi3531 AO S16 mono transport;
 *   - keeps the proven AO5/ch0, 48 kHz, 160-sample / 320-byte ABI;
 *   - mirrors the PCM virtual address into both transport slots, matching
 *     the known-good Nofrendo implementation on this board;
 *   - recovers AO5 state that Monitor/Sofia may leave enabled.
 *
 * This file is copied into RetroArch's OSS build slot for the H3531 target,
 * so the exported symbol remains audio_oss while the public driver id is
 * "h3531". No OSS ABI is used at runtime.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../audio_driver.h"
#include "../../verbosity.h"

#define H3531_AO_CONTEXT_INDEX 80U
#define H3531_AO_SAMPLE_RATE 48000U
#define H3531_AO_SAMPLES 160U
#define H3531_AO_MONO_BYTES (H3531_AO_SAMPLES * sizeof(int16_t))
#define H3531_RA_STEREO_FRAME_BYTES (2U * sizeof(int16_t))
#define H3531_AO_QUEUE_BLOCKS 30U

#define H3531_IOCTL_INIT_CONTEXT 0x40045800UL
#define H3531_IOCTL_SET_PUB_ATTR 0x40245801UL
#define H3531_IOCTL_ENABLE_DEV   0x00005803UL
#define H3531_IOCTL_DISABLE_DEV  0x00005804UL
#define H3531_IOCTL_SEND_FRAME   0x40305805UL
#define H3531_IOCTL_ENABLE_CHN   0x00005808UL
#define H3531_IOCTL_DISABLE_CHN  0x00005809UL
#define H3531_IOCTL_CLEAR_ATTR   0x0000580BUL
#define H3531_IOCTL_QUERY_CHN    0x800C580FUL

#define H3531_AO_NOT_PERM ((int32_t)0xA0168009U)

struct h3531_aio_attr
{
   uint32_t enSamplerate;
   uint32_t enBitwidth;
   uint32_t enWorkmode;
   uint32_t enSoundmode;
   uint32_t u32EXFlag;
   uint32_t u32FrmNum;
   uint32_t u32PtNumPerFrm;
   uint32_t u32ChnCnt;
   uint32_t u32ClkSel;
};

struct h3531_ao_chn_state
{
   uint32_t total_blocks;
   uint32_t free_blocks;
   uint32_t busy_blocks;
};

/* Exact 48-byte transport record accepted by stock hi3531_ao.ko on the
 * physically proven target. Reserved words intentionally remain zero. */
struct h3531_audio_frame_raw
{
   uint32_t bit_width;
   uint32_t sound_mode;
   uint32_t vir_addr[2];
   uint32_t reserved0[5];
   uint32_t data_bytes;
   uint32_t reserved1[2];
};

typedef char h3531_attr_size_must_be_36[(sizeof(struct h3531_aio_attr) == 36) ? 1 : -1];
typedef char h3531_state_size_must_be_12[(sizeof(struct h3531_ao_chn_state) == 12) ? 1 : -1];
typedef char h3531_frame_size_must_be_48[(sizeof(struct h3531_audio_frame_raw) == 48) ? 1 : -1];
typedef char h3531_frame_len_offset_must_be_36[(offsetof(struct h3531_audio_frame_raw, data_bytes) == 36) ? 1 : -1];

typedef struct h3531_audio
{
   int fd;
   bool dev_enabled;
   bool chn_enabled;
   bool paused;
   bool nonblock;
   bool query_supported;

   int16_t mono_block[H3531_AO_SAMPLES];
   unsigned mono_fill;

   uint64_t write_calls;
   uint64_t input_frames;
   uint64_t blocks_sent;
   uint64_t waits;
   uint64_t query_failures;
   uint64_t send_failures;
   uint32_t send_max_us;
   uint32_t last_free;
   uint32_t last_busy;
} h3531_audio_t;

static uint64_t h3531_now_us(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int h3531_ioctl(h3531_audio_t *ctx, unsigned long request, void *arg)
{
   errno = 0;
   return ioctl(ctx->fd, request, arg);
}

static int h3531_ioctl_noarg(h3531_audio_t *ctx, unsigned long request)
{
   errno = 0;
   return ioctl(ctx->fd, request, 0);
}

static void h3531_make_attr(struct h3531_aio_attr *attr)
{
   memset(attr, 0, sizeof(*attr));
   attr->enSamplerate    = H3531_AO_SAMPLE_RATE;
   attr->enBitwidth      = 1U; /* AUDIO_BIT_WIDTH_16 */
   attr->enWorkmode      = 0U; /* I2S master */
   attr->enSoundmode     = 0U; /* mono */
   attr->u32EXFlag       = 0U;
   attr->u32FrmNum       = H3531_AO_QUEUE_BLOCKS;
   attr->u32PtNumPerFrm  = H3531_AO_SAMPLES;
   attr->u32ChnCnt       = 2U;
   attr->u32ClkSel       = 0U;
}

static void h3531_disable(h3531_audio_t *ctx)
{
   if (!ctx || ctx->fd < 0)
      return;

   if (ctx->chn_enabled)
   {
      (void)h3531_ioctl_noarg(ctx, H3531_IOCTL_DISABLE_CHN);
      ctx->chn_enabled = false;
   }
   if (ctx->dev_enabled)
   {
      (void)h3531_ioctl_noarg(ctx, H3531_IOCTL_DISABLE_DEV);
      ctx->dev_enabled = false;
   }
}

static void h3531_recover_inherited_state(h3531_audio_t *ctx)
{
   int rc;

   rc = h3531_ioctl_noarg(ctx, H3531_IOCTL_DISABLE_CHN);
   RARCH_LOG("[H3531 AO] recovery DisableChn rc=%d (0x%08x)\n", rc, (unsigned)rc);
   rc = h3531_ioctl_noarg(ctx, H3531_IOCTL_DISABLE_DEV);
   RARCH_LOG("[H3531 AO] recovery DisableDev rc=%d (0x%08x)\n", rc, (unsigned)rc);
   rc = h3531_ioctl_noarg(ctx, H3531_IOCTL_CLEAR_ATTR);
   RARCH_LOG("[H3531 AO] recovery ClearPubAttr rc=%d (0x%08x)\n", rc, (unsigned)rc);
   ctx->dev_enabled = false;
   ctx->chn_enabled = false;
}

static int h3531_query(h3531_audio_t *ctx,
      uint32_t *total, uint32_t *free_blocks, uint32_t *busy_blocks)
{
   struct h3531_ao_chn_state st;
   int rc;

   if (!ctx || ctx->fd < 0 || !ctx->chn_enabled || !ctx->query_supported)
      return -1;

   memset(&st, 0, sizeof(st));
   rc = h3531_ioctl(ctx, H3531_IOCTL_QUERY_CHN, &st);
   if (rc != 0)
   {
      ++ctx->query_failures;
      ctx->query_supported = false;
      RARCH_WARN("[H3531 AO] QueryChnState unavailable rc=%d (0x%08x) errno=%d; using proven direct SendFrame path\n",
            rc, (unsigned)rc, errno);
      return rc;
   }

   ctx->last_free = st.free_blocks;
   ctx->last_busy = st.busy_blocks;
   if (total)
      *total = st.total_blocks;
   if (free_blocks)
      *free_blocks = st.free_blocks;
   if (busy_blocks)
      *busy_blocks = st.busy_blocks;
   return 0;
}

/* Returns 0 when writable, 1 for a nonblocking would-block condition,
 * and -1 for a hard failure. */
static int h3531_wait_free(h3531_audio_t *ctx)
{
   uint32_t total = 0, free_blocks = 0, busy_blocks = 0;
   struct pollfd pfd;
   int rc;

   if (h3531_query(ctx, &total, &free_blocks, &busy_blocks) != 0)
      return 0; /* Preserve the physically proven direct SendFrame path. */

   while (free_blocks == 0U)
   {
      if (ctx->nonblock)
         return 1;

      ++ctx->waits;
      pfd.fd = ctx->fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      do
      {
         rc = poll(&pfd, 1, 100);
      } while (rc < 0 && errno == EINTR);

      if (rc <= 0)
      {
         RARCH_ERR("[H3531 AO] timeout waiting for free block total=%u busy=%u rc=%d errno=%d\n",
               total, busy_blocks, rc, errno);
         return -1;
      }

      if (h3531_query(ctx, &total, &free_blocks, &busy_blocks) != 0)
         return 0;
   }

   return 0;
}

static int h3531_send_block(h3531_audio_t *ctx)
{
   struct h3531_audio_frame_raw frame;
   uintptr_t pcm_addr;
   uint64_t before_us, after_us;
   int wait_rc;
   int rc;

   wait_rc = h3531_wait_free(ctx);
   if (wait_rc != 0)
      return wait_rc;

   pcm_addr = (uintptr_t)ctx->mono_block;
   if (sizeof(uintptr_t) > sizeof(uint32_t) && pcm_addr > UINT32_MAX)
   {
      RARCH_ERR("[H3531 AO] PCM address does not fit 32-bit AO ABI\n");
      return -1;
   }

   memset(&frame, 0, sizeof(frame));
   frame.bit_width   = 1U;
   frame.sound_mode  = 0U;
   frame.vir_addr[0] = (uint32_t)pcm_addr;
   frame.vir_addr[1] = (uint32_t)pcm_addr;
   frame.data_bytes  = (uint32_t)H3531_AO_MONO_BYTES;

   before_us = h3531_now_us();
   rc = h3531_ioctl(ctx, H3531_IOCTL_SEND_FRAME, &frame);
   after_us = h3531_now_us();

   if (before_us && after_us >= before_us)
   {
      uint64_t elapsed = after_us - before_us;
      if (elapsed > UINT32_MAX)
         elapsed = UINT32_MAX;
      if ((uint32_t)elapsed > ctx->send_max_us)
         ctx->send_max_us = (uint32_t)elapsed;
   }

   if (rc != 0)
   {
      ++ctx->send_failures;
      RARCH_ERR("[H3531 AO] SendFrame failed rc=%d (0x%08x) errno=%d\n",
            rc, (unsigned)rc, errno);
      return -1;
   }

   ++ctx->blocks_sent;
   if ((ctx->blocks_sent % 300U) == 0U)
      RARCH_LOG("[H3531 AO] STATS blocks=%llu input_frames=%llu writes=%llu waits=%llu free=%u busy=%u sendmax_us=%u query_fail=%llu\n",
            (unsigned long long)ctx->blocks_sent,
            (unsigned long long)ctx->input_frames,
            (unsigned long long)ctx->write_calls,
            (unsigned long long)ctx->waits,
            ctx->last_free,
            ctx->last_busy,
            ctx->send_max_us,
            (unsigned long long)ctx->query_failures);

   return 0;
}

static void *h3531_audio_init(const char *device,
      unsigned rate, unsigned latency, unsigned *new_out_rate)
{
   h3531_audio_t *ctx = (h3531_audio_t*)calloc(1, sizeof(*ctx));
   struct h3531_aio_attr attr;
   uint32_t context = H3531_AO_CONTEXT_INDEX;
   int rc;

   (void)device;

   if (!ctx)
      return NULL;

   ctx->fd = -1;
   ctx->query_supported = true;

   ctx->fd = open("/dev/ao", O_RDWR);
   if (ctx->fd < 0)
   {
      RARCH_ERR("[H3531 AO] open /dev/ao failed errno=%d\n", errno);
      free(ctx);
      return NULL;
   }

   h3531_make_attr(&attr);

   rc = h3531_ioctl(ctx, H3531_IOCTL_INIT_CONTEXT, &context);
   if (rc != 0)
   {
      RARCH_ERR("[H3531 AO] InitContext failed rc=%d (0x%08x) errno=%d\n",
            rc, (unsigned)rc, errno);
      goto error;
   }

   rc = h3531_ioctl(ctx, H3531_IOCTL_SET_PUB_ATTR, &attr);
   if ((int32_t)rc == H3531_AO_NOT_PERM)
   {
      RARCH_LOG("[H3531 AO] SetPubAttr returned NOT_PERM; recovering inherited AO5 state\n");
      h3531_recover_inherited_state(ctx);
      context = H3531_AO_CONTEXT_INDEX;
      rc = h3531_ioctl(ctx, H3531_IOCTL_INIT_CONTEXT, &context);
      if (rc == 0)
         rc = h3531_ioctl(ctx, H3531_IOCTL_SET_PUB_ATTR, &attr);
   }
   if (rc != 0)
   {
      RARCH_ERR("[H3531 AO] SetPubAttr failed rc=%d (0x%08x) errno=%d\n",
            rc, (unsigned)rc, errno);
      goto error;
   }

   rc = h3531_ioctl_noarg(ctx, H3531_IOCTL_ENABLE_DEV);
   if (rc != 0)
   {
      RARCH_ERR("[H3531 AO] EnableDev failed rc=%d (0x%08x) errno=%d\n",
            rc, (unsigned)rc, errno);
      goto error;
   }
   ctx->dev_enabled = true;

   rc = h3531_ioctl_noarg(ctx, H3531_IOCTL_ENABLE_CHN);
   if (rc != 0)
   {
      RARCH_ERR("[H3531 AO] EnableChn failed rc=%d (0x%08x) errno=%d\n",
            rc, (unsigned)rc, errno);
      goto error;
   }
   ctx->chn_enabled = true;

   if (new_out_rate && rate != H3531_AO_SAMPLE_RATE)
      *new_out_rate = H3531_AO_SAMPLE_RATE;

   RARCH_LOG("[H3531 AO] ready: requested=%uHz output=48000Hz latency=%ums S16 stereo->mono AO5/ch0 30x160 proven-frame=320B\n",
         rate, latency);
   return ctx;

error:
   h3531_disable(ctx);
   if (ctx->fd >= 0)
      close(ctx->fd);
   free(ctx);
   return NULL;
}

static ssize_t h3531_audio_write(void *data, const void *s, size_t len)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   const int16_t *src = (const int16_t*)s;
   size_t frames;
   size_t i;

   if (!ctx || !s)
      return -1;
   if (ctx->paused)
      return 0;
   if ((len % H3531_RA_STEREO_FRAME_BYTES) != 0U)
   {
      RARCH_ERR("[H3531 AO] unaligned RetroArch PCM write len=%lu\n",
            (unsigned long)len);
      return -1;
   }

   ++ctx->write_calls;

   /* A prior nonblocking write may have filled a complete AO block but
    * found no hardware slot. Flush it before consuming more frontend PCM. */
   if (ctx->mono_fill == H3531_AO_SAMPLES)
   {
      int rc = h3531_send_block(ctx);
      if (rc > 0)
         return 0;
      if (rc < 0)
         return -1;
      ctx->mono_fill = 0;
   }

   frames = len / H3531_RA_STEREO_FRAME_BYTES;
   for (i = 0; i < frames; ++i)
   {
      int32_t l = src[i * 2U];
      int32_t r = src[i * 2U + 1U];
      ctx->mono_block[ctx->mono_fill++] = (int16_t)((l + r) / 2);
      ++ctx->input_frames;

      if (ctx->mono_fill == H3531_AO_SAMPLES)
      {
         int rc = h3531_send_block(ctx);
         if (rc > 0)
            return (ssize_t)((i + 1U) * H3531_RA_STEREO_FRAME_BYTES);
         if (rc < 0)
            return -1;
         ctx->mono_fill = 0;
      }
   }

   return (ssize_t)len;
}

static bool h3531_audio_stop(void *data)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   if (!ctx)
      return false;
   ctx->paused = true;
   return true;
}

static bool h3531_audio_start(void *data, bool is_shutdown)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   (void)is_shutdown;
   if (!ctx)
      return false;
   ctx->paused = false;
   return true;
}

static bool h3531_audio_alive(void *data)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   return ctx && !ctx->paused;
}

static void h3531_audio_set_nonblock(void *data, bool state)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   if (ctx)
      ctx->nonblock = state;
}

static void h3531_audio_free(void *data)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   if (!ctx)
      return;

   RARCH_LOG("[H3531 AO] final: blocks=%llu input_frames=%llu writes=%llu waits=%llu send_fail=%llu query_fail=%llu sendmax_us=%u\n",
         (unsigned long long)ctx->blocks_sent,
         (unsigned long long)ctx->input_frames,
         (unsigned long long)ctx->write_calls,
         (unsigned long long)ctx->waits,
         (unsigned long long)ctx->send_failures,
         (unsigned long long)ctx->query_failures,
         ctx->send_max_us);

   h3531_disable(ctx);
   if (ctx->fd >= 0)
      close(ctx->fd);
   free(ctx);
}

static bool h3531_audio_use_float(void *data)
{
   (void)data;
   return false;
}

static size_t h3531_audio_write_avail(void *data)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   uint32_t total = 0, free_blocks = 0, busy_blocks = 0;
   size_t frames;

   if (!ctx)
      return 0;

   if (h3531_query(ctx, &total, &free_blocks, &busy_blocks) != 0)
      return H3531_AO_SAMPLES * H3531_RA_STEREO_FRAME_BYTES;

   frames = (size_t)free_blocks * H3531_AO_SAMPLES;
   if (ctx->mono_fill < H3531_AO_SAMPLES)
      frames += H3531_AO_SAMPLES - ctx->mono_fill;
   return frames * H3531_RA_STEREO_FRAME_BYTES;
}

static size_t h3531_audio_buffer_size(void *data)
{
   (void)data;
   return (size_t)H3531_AO_QUEUE_BLOCKS * H3531_AO_SAMPLES *
      H3531_RA_STEREO_FRAME_BYTES;
}

static size_t h3531_audio_wait_writable(void *data, size_t len)
{
   h3531_audio_t *ctx = (h3531_audio_t*)data;
   int rc;
   (void)len;
   if (!ctx)
      return 0;
   rc = h3531_wait_free(ctx);
   if (rc != 0)
      return 0;
   return h3531_audio_write_avail(ctx);
}

static uint32_t h3531_audio_layout(void *data)
{
   (void)data;
   return AUDIO_LAYOUT_STEREO;
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
   NULL, /* frames_consumed */
   NULL, /* underruns */
   h3531_audio_layout
};
