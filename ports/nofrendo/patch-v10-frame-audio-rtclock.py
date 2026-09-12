from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Distinct hardware-test identity.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v7: source snapshots + native HIFB vblank + AO prebuffer + exact 60Hz"
new = "H3531 Nofrendo v10: frame-linked APU + AO queue backpressure + RT no-catch-up 60Hz"
if s.count(old) != 1:
    raise SystemExit(f"v7 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

# ---------------------------------------------------------------------------
# Platform timer: keep a dedicated 60-Hz clock thread, but NEVER catch up by
# issuing back-to-back ticks after a scheduler delay. Prefer SCHED_FIFO for
# the tiny timer thread when root/kernel permit it; fall back safely otherwise.
# ---------------------------------------------------------------------------
p = root / "platform" / "osd_linux.c"
s = p.read_text()

include_marker = "#include <pthread.h>\n"
if include_marker not in s:
    raise SystemExit("pthread include marker missing")
if "#include <sched.h>\n" not in s:
    s = s.replace(include_marker, include_marker + "#include <sched.h>\n", 1)

# patch-h3531.py emits timer_thread_func followed directly by osd_installtimer.
start = s.find("static void* timer_thread_func(void* arg) {")
end = s.find("\nint osd_installtimer(", start)
if start < 0 or end < 0:
    raise SystemExit(f"timer_thread_func boundaries not found start={start} end={end}")

new_timer = r'''static void* timer_thread_func(void* arg) {
  timer_param_t* param = (timer_param_t*)arg;
  const uint64_t period_ns = 1000000000ULL / (uint64_t)param->frequency;
  uint64_t deadline = h3531_now_ns() + period_ns;
  unsigned late_resets = 0;
  struct sched_param sp;
  int rt_rc;

  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = 10;
  rt_rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
  printf("H3531 NES timer v10: RT no-catch-up %d Hz sched_fifo=%s\n",
         param->frequency, rt_rc == 0 ? "on" : "off");

  for (;;) {
    struct timespec abs_time;
    uint64_t now;
    int sleep_rc;

    abs_time.tv_sec = (time_t)(deadline / 1000000000ULL);
    abs_time.tv_nsec = (long)(deadline % 1000000000ULL);
    do {
      sleep_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_time, NULL);
    } while (sleep_rc == EINTR);

    param->func();

    now = h3531_now_ns();
    deadline += period_ns;
    if (now >= deadline) {
      if (late_resets < 12) {
        fprintf(stderr,
                "H3531 NES timer v10: missed deadline by %lu us; phase reset without catch-up\n",
                (unsigned long)((now - deadline) / 1000ULL));
        late_resets++;
      }
      deadline = now + period_ns;
    }
  }
  return NULL;
}
'''
s = s[:start] + new_timer + s[end:]

# ---------------------------------------------------------------------------
# Audio: remove the independent nanosleep-driven APU worker. v10 generates
# exactly one share of the 22100-Hz APU stream per emulated NES frame. The
# exact phase accumulator converts it to 48 kHz; over 60 frames this is exactly
# 48000 output samples, i.e. 800 hardware samples/frame on average.
# ---------------------------------------------------------------------------
audio_start_marker = "/*\n** Audio (Nofrendo 22100 Hz -> resampler -> Hi3531 AO 48000 Hz)\n*/"
audio_start = s.find(audio_start_marker)
audio_end_marker = "/* --- End Audio --- */"
audio_end = s.find(audio_end_marker, audio_start)
if audio_start < 0 or audio_end < 0:
    raise SystemExit("v7 audio block not found")
audio_end += len(audio_end_marker)

new_audio = r'''/*
** Audio v10: frame-linked Nofrendo APU -> exact 22100->48000 resampler -> AO5
*/
static void (*audio_callback)(void *buffer, int length) = NULL;
static int ao_available = 0;
static int audio_started = 0;
static int16_t audio_frame[(DEFAULT_SAMPLERATE / NES_REFRESH_RATE) + 2];
static int16_t ao_block[160];
static unsigned ao_fill = 0;
static uint32_t resample_acc = 0;
static uint32_t apu_frame_acc = 0;
static unsigned audio_diag_frames = 0;

static void h3531_push_resampled(const int16_t *src, int count)
{
  int i;
  if (!ao_available)
    return;

  for (i = 0; i < count; ++i) {
    resample_acc += 48000U;
    while (resample_acc >= (uint32_t)DEFAULT_SAMPLERATE) {
      ao_block[ao_fill++] = src[i];
      resample_acc -= (uint32_t)DEFAULT_SAMPLERATE;
      if (ao_fill == 160U) {
        if (h3531_ao_send_160(ao_block) != 0) {
          fprintf(stderr, "H3531 AO v10: disabling after SendFrame/backpressure failure\n");
          h3531_ao_stop();
          ao_available = 0;
          ao_fill = 0;
          return;
        }
        ao_fill = 0;
      }
    }
  }
}

void osd_frameaudio(void)
{
  int samples;

  if (!audio_callback)
    return;

  apu_frame_acc += (uint32_t)DEFAULT_SAMPLERATE;
  samples = (int)(apu_frame_acc / (uint32_t)NES_REFRESH_RATE);
  apu_frame_acc %= (uint32_t)NES_REFRESH_RATE;

  audio_callback(audio_frame, samples);
  h3531_push_resampled(audio_frame, samples);

  audio_diag_frames++;
  if (audio_diag_frames == 600U) {
    uint32_t total = 0, free_blocks = 0, busy_blocks = 0;
    if (ao_available &&
        h3531_ao_query_state(&total, &free_blocks, &busy_blocks) == 0) {
      printf("H3531 AO v10 state: total=%u free=%u busy=%u\n",
             total, free_blocks, busy_blocks);
    }
    audio_diag_frames = 0;
  }
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
  audio_callback = playfunc;

  if (!audio_started && playfunc != NULL) {
    ao_available = (h3531_ao_start() == 0);
    if (!ao_available) {
      fprintf(stderr, "H3531 AO v10: unavailable; NES continues without audio\n");
    } else {
      int prime;
      memset(ao_block, 0, sizeof(ao_block));
      for (prime = 0; prime < 3; ++prime) {
        if (h3531_ao_send_160(ao_block) != 0) {
          fprintf(stderr, "H3531 NES audio v10: AO prebuffer failed at block %d\n", prime);
          h3531_ao_stop();
          ao_available = 0;
          break;
        }
      }
    }

    audio_started = 1;
    ao_fill = 0;
    resample_acc = 0;
    apu_frame_acc = 0;
    audio_diag_frames = 0;
    printf("H3531 NES audio v10: frame-linked APU 22100->48000; 800 output samples/frame average; prebuffer=10 ms\n");
  }
}

static void osd_stopsound(void)
{
  audio_callback = NULL;
  audio_started = 0;
  ao_fill = 0;
  resample_acc = 0;
  apu_frame_acc = 0;
  if (ao_available) {
    h3531_ao_stop();
    ao_available = 0;
  }
}

static int osd_init_sound(void)
{
  memset(audio_frame, 0, sizeof(audio_frame));
  memset(ao_block, 0, sizeof(ao_block));
  ao_fill = 0;
  resample_acc = 0;
  apu_frame_acc = 0;
  audio_diag_frames = 0;
  printf("H3531 NES audio v10: early init safe; AO/APU deferred until emulation\n");
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
# Core: cap a delayed timer observation to one pending frame. Never perform
# Nofrendo's old burst catch-up. Generate one APU share for every frame that
# is actually emulated, including a hidden frame if one ever occurs.
# ---------------------------------------------------------------------------
p = root / "core" / "nes" / "nes.c"
s = p.read_text()

include_marker = "#include <sched.h>\n"
if include_marker not in s:
    raise SystemExit("v6 sched include not found")
if "extern void osd_frameaudio(void);\n" not in s:
    s = s.replace(include_marker, include_marker + "extern void osd_frameaudio(void);\n", 1)

old_tick = r'''      if (nofrendo_ticks != last_ticks)
      {
         int tick_diff = nofrendo_ticks - last_ticks;

         if (tick_diff > 1 && h3531_tick_burst_logs < 12)
         {
            fprintf(stderr,
                    "H3531 NES timing v6: tick burst=%d (frameskip pressure)\n",
                    tick_diff);
            h3531_tick_burst_logs++;
         }

         frames_to_render += tick_diff;
         gui_tick(tick_diff);
         last_ticks = nofrendo_ticks;
      }
'''
new_tick = r'''      if (nofrendo_ticks != last_ticks)
      {
         int tick_diff = nofrendo_ticks - last_ticks;

         if (tick_diff > 1 && h3531_tick_burst_logs < 12)
         {
            fprintf(stderr,
                    "H3531 NES timing v10: observed %d ticks; dropping catch-up and rendering one frame\n",
                    tick_diff);
            h3531_tick_burst_logs++;
         }

         frames_to_render = 1;
         gui_tick(1);
         last_ticks = nofrendo_ticks;
      }
'''
if s.count(old_tick) != 1:
    raise SystemExit(f"v6 tick block marker count={s.count(old_tick)}")
s = s.replace(old_tick, new_tick, 1)

old_hidden = '''         frames_to_render--;\n         nes_renderframe(false);\n         system_video(false);\n'''
new_hidden = '''         frames_to_render--;\n         nes_renderframe(false);\n         osd_frameaudio();\n         system_video(false);\n'''
if s.count(old_hidden) != 1:
    raise SystemExit(f"hidden frame marker count={s.count(old_hidden)}")
s = s.replace(old_hidden, new_hidden, 1)

old_visible = '''         frames_to_render = 0;\n         nes_renderframe(true);\n         system_video(true);\n'''
new_visible = '''         frames_to_render = 0;\n         nes_renderframe(true);\n         osd_frameaudio();\n         system_video(true);\n'''
if s.count(old_visible) != 1:
    raise SystemExit(f"visible frame marker count={s.count(old_visible)}")
s = s.replace(old_visible, new_visible, 1)

p.write_text(s)

print("H3531 Nofrendo v10 frame-audio / RT no-catch-up patch applied")
