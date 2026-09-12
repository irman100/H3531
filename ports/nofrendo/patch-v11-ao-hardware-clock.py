from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Distinct hardware-test identity.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v10: frame-linked APU + AO queue backpressure + RT no-catch-up 60Hz"
new = "H3531 Nofrendo v11: AO 48k hardware master clock + exact 800-sample frames + v7 video"
if s.count(old) != 1:
    raise SystemExit(f"v10 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

# ---------------------------------------------------------------------------
# AO: request a short eight-block hardware queue. Each block is 160 samples,
# so a full queue is 26.67 ms at 48 kHz.  The transport ABI itself is unchanged.
# If the driver reports a different total after EnableChn, v11 uses that actual
# total when priming and still paces from real hardware backpressure.
# ---------------------------------------------------------------------------
p = root / "platform" / "h3531-ao.c"
s = p.read_text()
old = "    attr->u32FrmNum = 30;\n"
new = "    attr->u32FrmNum = 8;       /* v11: short queue for hardware-clock pacing */\n"
if s.count(old) != 1:
    raise SystemExit(f"AO frame-count marker count={s.count(old)}")
s = s.replace(old, new, 1)
s = s.replace(
    "H3531 AO v6 ready: 48000 Hz S16 mono AO5/ch0 30x160, proven-frame=320 bytes",
    "H3531 AO v11 ready: 48000 Hz S16 mono AO5/ch0 requested=8x160, proven-frame=320 bytes",
    1,
)
p.write_text(s)

# ---------------------------------------------------------------------------
# Platform: retain the v10 fallback timer, but AO becomes the master clock once
# the hardware queue has been successfully queried and primed.  While AO master
# clock is active, fallback timer deadline misses are intentionally not logged.
# ---------------------------------------------------------------------------
p = root / "platform" / "osd_linux.c"
s = p.read_text()

marker = "static uint64_t h3531_now_ns(void)\n"
if s.count(marker) != 1:
    raise SystemExit(f"h3531_now_ns marker count={s.count(marker)}")
s = s.replace(marker, "static volatile int h3531_audio_master_clock = 0;\n\n" + marker, 1)

old = '  printf("H3531 NES timer v10: RT no-catch-up %d Hz sched_fifo=%s\\n",\n         param->frequency, rt_rc == 0 ? "on" : "off");'
new = '  printf("H3531 NES timer v11: fallback %d Hz sched_fifo=%s; AO clock takes priority when armed\\n",\n         param->frequency, rt_rc == 0 ? "on" : "off");'
if s.count(old) != 1:
    raise SystemExit(f"v10 timer log marker count={s.count(old)}")
s = s.replace(old, new, 1)

old = "      if (late_resets < 12) {\n"
new = "      if (!h3531_audio_master_clock && late_resets < 12) {\n"
if s.count(old) != 1:
    raise SystemExit(f"v10 late-reset marker count={s.count(old)}")
s = s.replace(old, new, 1)

# Replace the complete v10 audio block.
audio_start_marker = "/*\n** Audio v10: frame-linked Nofrendo APU -> exact 22100->48000 resampler -> AO5\n*/"
audio_start = s.find(audio_start_marker)
audio_end_marker = "/* --- End Audio --- */"
audio_end = s.find(audio_end_marker, audio_start)
if audio_start < 0 or audio_end < 0:
    raise SystemExit("v10 audio block not found")
audio_end += len(audio_end_marker)

new_audio = r'''/*
** Audio v11: frame-linked APU -> continuous exact resampler -> exactly
** 800 hardware samples (5 x 160) per emulated NES frame.  A full short AO
** queue supplies the real 48-kHz hardware backpressure/master clock.
*/
#define H3531_V11_HW_SAMPLES_PER_FRAME 800
#define H3531_V11_HW_BLOCKS_PER_FRAME 5
#define H3531_V11_RESAMP_TMP 808

static void (*audio_callback)(void *buffer, int length) = NULL;
static int ao_available = 0;
static int audio_started = 0;
static int16_t audio_frame[(DEFAULT_SAMPLERATE / NES_REFRESH_RATE) + 2];
static int16_t resamp_tmp[H3531_V11_RESAMP_TMP];
static int16_t resamp_carry[8];
static unsigned resamp_carry_count = 0;
static uint32_t resample_acc = 0;
static uint32_t apu_frame_acc = 0;
static unsigned audio_diag_frames = 0;
static uint64_t audio_diag_start_ns = 0;
static uint32_t ao_queue_total = 0;

static void h3531_audio_clock_fail(const char *why)
{
  fprintf(stderr, "H3531 AO clock v11: %s; falling back to software timer\n", why);
  h3531_audio_master_clock = 0;
  if (ao_available) {
    h3531_ao_stop();
    ao_available = 0;
  }
}

/* Preserve the exact continuous 22100->48000 phase accumulator, but collect
   its 799/799/802-ish per-frame output into a tiny carry buffer so transport
   to AO is exactly 800 samples every NES frame. */
static int h3531_resample_exact_frame(const int16_t *src, int count,
                                      int16_t *dst800)
{
  unsigned out = 0;
  unsigned i;

  for (i = 0; i < resamp_carry_count; ++i)
    resamp_tmp[out++] = resamp_carry[i];

  for (i = 0; i < (unsigned)count; ++i) {
    resample_acc += 48000U;
    while (resample_acc >= (uint32_t)DEFAULT_SAMPLERATE) {
      if (out >= H3531_V11_RESAMP_TMP)
        return -1;
      resamp_tmp[out++] = src[i];
      resample_acc -= (uint32_t)DEFAULT_SAMPLERATE;
    }
  }

  if (out < H3531_V11_HW_SAMPLES_PER_FRAME || out > H3531_V11_RESAMP_TMP)
    return -1;

  memcpy(dst800, resamp_tmp,
         H3531_V11_HW_SAMPLES_PER_FRAME * sizeof(int16_t));

  resamp_carry_count = out - H3531_V11_HW_SAMPLES_PER_FRAME;
  if (resamp_carry_count > (sizeof(resamp_carry) / sizeof(resamp_carry[0])))
    return -1;
  if (resamp_carry_count)
    memcpy(resamp_carry,
           &resamp_tmp[H3531_V11_HW_SAMPLES_PER_FRAME],
           resamp_carry_count * sizeof(int16_t));
  return 0;
}

static int h3531_send_hw_frame(const int16_t *pcm800)
{
  int block;
  for (block = 0; block < H3531_V11_HW_BLOCKS_PER_FRAME; ++block) {
    if (h3531_ao_send_160(&pcm800[block * 160]) != 0)
      return -1;
  }
  return 0;
}

static void h3531_audio_diag_tick(void)
{
  audio_diag_frames++;
  if (audio_diag_frames == 600U) {
    uint32_t total = 0, free_blocks = 0, busy_blocks = 0;
    uint64_t now_ns = h3531_now_ns();
    uint64_t elapsed_ns = (now_ns > audio_diag_start_ns) ?
                          (now_ns - audio_diag_start_ns) : 0;
    uint64_t fps_x100 = elapsed_ns ?
        (600ULL * 100ULL * 1000000000ULL) / elapsed_ns : 0;

    if (ao_available &&
        h3531_ao_query_state(&total, &free_blocks, &busy_blocks) == 0) {
      printf("H3531 AO clock v11: 600 frames elapsed_ms=%lu fps=%lu.%02lu total=%u free=%u busy=%u carry=%u\n",
             (unsigned long)(elapsed_ns / 1000000ULL),
             (unsigned long)(fps_x100 / 100ULL),
             (unsigned long)(fps_x100 % 100ULL),
             total, free_blocks, busy_blocks, resamp_carry_count);
    }
    audio_diag_frames = 0;
    audio_diag_start_ns = now_ns;
  }
}

void osd_frameaudio(void)
{
  int samples;
  int16_t pcm800[H3531_V11_HW_SAMPLES_PER_FRAME];

  if (!audio_callback || !h3531_audio_master_clock)
    return;

  apu_frame_acc += (uint32_t)DEFAULT_SAMPLERATE;
  samples = (int)(apu_frame_acc / (uint32_t)NES_REFRESH_RATE);
  apu_frame_acc %= (uint32_t)NES_REFRESH_RATE;

  audio_callback(audio_frame, samples);
  if (h3531_resample_exact_frame(audio_frame, samples, pcm800) != 0) {
    h3531_audio_clock_fail("resampler frame accounting failed");
    return;
  }

  if (h3531_send_hw_frame(pcm800) != 0) {
    h3531_audio_clock_fail("five-block hardware frame send failed");
    return;
  }

  h3531_audio_diag_tick();
}

void osd_frameaudio_silence(void)
{
  static int16_t silence800[H3531_V11_HW_SAMPLES_PER_FRAME];

  if (!h3531_audio_master_clock)
    return;
  if (h3531_send_hw_frame(silence800) != 0) {
    h3531_audio_clock_fail("pause/silence hardware frame send failed");
    return;
  }
  h3531_audio_diag_tick();
}

int osd_audio_clock_active(void)
{
  return h3531_audio_master_clock ? 1 : 0;
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
  audio_callback = playfunc;

  if (!audio_started && playfunc != NULL) {
    ao_available = (h3531_ao_start() == 0);
    h3531_audio_master_clock = 0;
    ao_queue_total = 0;

    if (!ao_available) {
      fprintf(stderr, "H3531 AO clock v11: AO unavailable; using fallback timer\n");
    } else {
      uint32_t total = 0, free_blocks = 0, busy_blocks = 0;
      static int16_t silence160[160];
      uint32_t prime;

      if (h3531_ao_query_state(&total, &free_blocks, &busy_blocks) != 0 ||
          total < H3531_V11_HW_BLOCKS_PER_FRAME || total > 64U) {
        h3531_audio_clock_fail("invalid/unavailable AO queue state");
      } else {
        ao_queue_total = total;
        for (prime = busy_blocks; prime < total; ++prime) {
          if (h3531_ao_send_160(silence160) != 0) {
            h3531_audio_clock_fail("failed to prime AO queue to full");
            break;
          }
        }
        if (ao_available) {
          if (h3531_ao_query_state(&total, &free_blocks, &busy_blocks) == 0) {
            printf("H3531 AO clock v11: armed 48000-Hz master queue total=%u free=%u busy=%u; 5x160 samples per NES frame\n",
                   total, free_blocks, busy_blocks);
          }
          h3531_audio_master_clock = 1;
        }
      }
    }

    audio_started = 1;
    resample_acc = 0;
    apu_frame_acc = 0;
    audio_diag_frames = 0;
    audio_diag_start_ns = h3531_now_ns();

    /* Two leading zero samples are the tiny reservoir needed to absorb the
       continuous resampler's 799/799/802 output pattern while draining exactly
       800 samples on every hardware-paced frame. */
    resamp_carry_count = 2;
    resamp_carry[0] = 0;
    resamp_carry[1] = 0;

    printf("H3531 NES audio v11: continuous 22100->48000 resampler; exact transport=800 samples/5 blocks per frame; carry=2 samples\n");
  }
}

static void osd_stopsound(void)
{
  audio_callback = NULL;
  audio_started = 0;
  h3531_audio_master_clock = 0;
  resample_acc = 0;
  apu_frame_acc = 0;
  resamp_carry_count = 0;
  ao_queue_total = 0;
  if (ao_available) {
    h3531_ao_stop();
    ao_available = 0;
  }
}

static int osd_init_sound(void)
{
  memset(audio_frame, 0, sizeof(audio_frame));
  memset(resamp_tmp, 0, sizeof(resamp_tmp));
  memset(resamp_carry, 0, sizeof(resamp_carry));
  resample_acc = 0;
  apu_frame_acc = 0;
  resamp_carry_count = 0;
  audio_diag_frames = 0;
  audio_diag_start_ns = 0;
  ao_queue_total = 0;
  h3531_audio_master_clock = 0;
  printf("H3531 NES audio v11: early init safe; AO hardware clock deferred until emulation\n");
  return 0;
}

void osd_getsoundinfo(sndinfo_t *info)
{
  info->sample_rate = DEFAULT_SAMPLERATE;
  info->bps = 16;
}
/* --- End Audio --- */'''

s = s[:audio_start] + new_audio + s[audio_end:]
p.write_text(s)

# ---------------------------------------------------------------------------
# Core: when AO hardware clock is armed, execute exactly one emulated frame,
# then submit exactly one 800-sample audio frame.  The five AO blocks naturally
# block until five 160-sample hardware periods have elapsed.  If AO pacing ever
# drops out, fall through to the already-tested v10 no-catch-up timer path.
# ---------------------------------------------------------------------------
p = root / "core" / "nes" / "nes.c"
s = p.read_text()

marker = "extern void osd_frameaudio(void);\n"
if s.count(marker) != 1:
    raise SystemExit(f"osd_frameaudio extern marker count={s.count(marker)}")
s = s.replace(marker,
              marker +
              "extern void osd_frameaudio_silence(void);\n"
              "extern int osd_audio_clock_active(void);\n",
              1)

loop_marker = "   while (false == nes.poweroff)\n   {\n"
if s.count(loop_marker) != 1:
    raise SystemExit(f"nes main while marker count={s.count(loop_marker)}")
insert = r'''   while (false == nes.poweroff)
   {
      if (osd_audio_clock_active())
      {
         /* AO5 drains exactly five 160-sample blocks per NES frame.  This is
            the master clock; software timer ticks are ignored in this mode. */
         gui_tick(1);
         if (true == nes.pause)
         {
            osd_frameaudio_silence();
            system_video(true);
         }
         else
         {
            nes_renderframe(true);
            osd_frameaudio();
            system_video(true);
         }
         last_ticks = nofrendo_ticks;
         frames_to_render = 0;
         continue;
      }
'''
s = s.replace(loop_marker, insert, 1)

# The v10 fallback path already calls osd_frameaudio().  In fallback mode that
# function is intentionally a no-op because the AO master-clock flag is clear.
p.write_text(s)

print("H3531 Nofrendo v11 AO hardware-master-clock patch applied")
