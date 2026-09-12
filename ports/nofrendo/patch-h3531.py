from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
port_dir = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parent)

# Keep the physically verified v1 framebuffer path.
(root / "platform" / "fb.c").write_text((port_dir / "fb-h3531.c").read_text())
(root / "platform" / "h3531-ao.c").write_text((port_dir / "h3531-ao.c").read_text())
(root / "platform" / "h3531-ao.h").write_text((port_dir / "h3531-ao.h").read_text())

p = root / "platform" / "osd_linux.c"
s = p.read_text()

# Hi3531 HIFB is A1R5G5B5, not RGB565.
old = "c=(pal[i].b>>3)+((pal[i].g>>2)<<5)+((pal[i].r>>3)<<11);"
new = "c=(uint16)(0x8000U | ((pal[i].r>>3)<<10) | ((pal[i].g>>3)<<5) | (pal[i].b>>3));"
if s.count(old) != 1:
    raise SystemExit(f"palette marker count={s.count(old)}")
s = s.replace(old, new, 1)
s = s.replace("#define SCREEN_WIDTH 320", "#define SCREEN_WIDTH 1280", 1)
s = s.replace("#define SCREEN_HEIGHT 240", "#define SCREEN_HEIGHT 720", 1)

# IMPORTANT: preserve the Nofrendo port's proven APU contract.
# v2 changed these to 48k/160 too early. v3 leaves the core at 22100/128 and
# resamples only at the Hi3531 hardware boundary.
if '#include <time.h>' not in s:
    s = s.replace('#include <unistd.h> // for usleep\n', '#include <unistd.h> // for usleep\n#include <time.h>\n', 1)
s = s.replace('#include "input.h"    // Your evdev input driver\n',
              '#include "input.h"    // Your evdev input driver\n#include "h3531-ao.h"\n', 1)

# Exact monotonic 60-Hz timer. This code is not reached until after vid_init,
# so it is independent from the v2 early-init crash.
start = s.find('typedef struct {\n  void (*func)(void);\n  int frequency;\n} timer_param_t;')
end_marker = '/*\n** Audio (ALSA playback for Card1)\n*/'
end = s.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit('timer/audio boundary not found')

timer_code = r'''typedef struct {
  void (*func)(void);
  int frequency;
} timer_param_t;

static uint64_t h3531_now_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void h3531_sleep_until(uint64_t deadline)
{
  for (;;) {
    uint64_t now = h3531_now_ns();
    struct timespec req;
    uint64_t left;
    if (now == 0 || now >= deadline)
      return;
    left = deadline - now;
    req.tv_sec = (time_t)(left / 1000000000ULL);
    req.tv_nsec = (long)(left % 1000000000ULL);
    if (nanosleep(&req, &req) == 0)
      return;
    if (errno != EINTR)
      return;
  }
}

static void* timer_thread_func(void* arg)
{
  timer_param_t* param = (timer_param_t*)arg;
  const uint64_t period_ns = 1000000000ULL / (uint64_t)param->frequency;
  uint64_t deadline = h3531_now_ns() + period_ns;

  for (;;) {
    uint64_t now;
    param->func();
    now = h3531_now_ns();
    if (now > deadline + period_ns * 2ULL)
      deadline = now + period_ns;
    else {
      h3531_sleep_until(deadline);
      deadline += period_ns;
    }
  }
  return NULL;
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
  static timer_param_t param;
  pthread_t timer_thread;
  (void)funcsize;
  (void)counter;
  (void)countersize;
  printf("H3531 NES timer: absolute-deadline %d Hz\n", frequency);
  param.func = (void (*)(void))func;
  param.frequency = frequency;
  if (pthread_create(&timer_thread, NULL, timer_thread_func, &param) != 0)
    return -1;
  pthread_detach(timer_thread);
  return 0;
}

'''
s = s[:start] + timer_code + s[end:]

# Replace ALSA with deferred Hi3531 AO. Do not touch AO during osd_init().
# The hardware path starts only when Nofrendo has created its APU and calls
# osd_setsound(). Native 22100-Hz S16 mono is resampled to the proven 48-kHz
# 160-sample AO ABI with a phase accumulator.
audio_start = s.find(end_marker)
audio_end_marker = '/* --- End Audio --- */'
audio_end = s.find(audio_end_marker, audio_start)
if audio_start < 0 or audio_end < 0:
    raise SystemExit('ALSA block not found')
audio_end += len(audio_end_marker)

audio_code = r'''/*
** Audio (Nofrendo 22100 Hz -> resampler -> Hi3531 AO 48000 Hz)
*/
static void (*audio_callback)(void *buffer, int length) = NULL;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;
static pthread_t audio_thread;
static int audio_running = 0;
static int audio_thread_created = 0;
static int ao_available = 0;
static int16_t audio_frame[DEFAULT_FRAGSIZE];
static int16_t ao_block[160];
static unsigned ao_fill = 0;
static uint32_t resample_acc = 0;

static void h3531_push_resampled(const int16_t *src, int count)
{
  int i;
  if (!ao_available)
    return;

  for (i = 0; i < count; ++i) {
    /* Zero-order hold resampler. The accumulator preserves exact long-term
       22100 -> 48000 timing and is intentionally cheap on ARMv7. */
    resample_acc += 48000U;
    while (resample_acc >= (uint32_t)DEFAULT_SAMPLERATE) {
      ao_block[ao_fill++] = src[i];
      resample_acc -= (uint32_t)DEFAULT_SAMPLERATE;
      if (ao_fill == 160U) {
        if (h3531_ao_send_160(ao_block) != 0) {
          fprintf(stderr, "H3531 AO: disabling after SendFrame failure\n");
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

static void *audio_thread_func(void *arg)
{
  const uint64_t period_ns =
      ((uint64_t)DEFAULT_FRAGSIZE * 1000000000ULL) / (uint64_t)DEFAULT_SAMPLERATE;
  uint64_t deadline = h3531_now_ns() + period_ns;
  (void)arg;

  for (;;) {
    void (*cb)(void *, int);
    uint64_t now;

    pthread_mutex_lock(&audio_mutex);
    while (audio_running && audio_callback == NULL)
      pthread_cond_wait(&audio_cond, &audio_mutex);
    if (!audio_running) {
      pthread_mutex_unlock(&audio_mutex);
      break;
    }
    cb = audio_callback;
    pthread_mutex_unlock(&audio_mutex);

    cb(audio_frame, DEFAULT_FRAGSIZE);
    h3531_push_resampled(audio_frame, DEFAULT_FRAGSIZE);

    now = h3531_now_ns();
    if (now > deadline + period_ns * 2ULL)
      deadline = now + period_ns;
    else {
      h3531_sleep_until(deadline);
      deadline += period_ns;
    }
  }
  return NULL;
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
  pthread_mutex_lock(&audio_mutex);
  audio_callback = playfunc;

  if (!audio_thread_created && playfunc != NULL) {
    /* This occurs after NES/APU creation, not during early OSD/video init. */
    ao_available = (h3531_ao_start() == 0);
    if (!ao_available)
      fprintf(stderr, "H3531 AO: unavailable; NES continues with paced silent audio\n");
    audio_running = 1;
    ao_fill = 0;
    resample_acc = 0;
    if (pthread_create(&audio_thread, NULL, audio_thread_func, NULL) == 0) {
      audio_thread_created = 1;
      printf("H3531 NES audio: deferred APU start, 22100->48000 resampler active\n");
    } else {
      fprintf(stderr, "H3531 NES audio: failed to create audio thread\n");
      audio_running = 0;
      if (ao_available) {
        h3531_ao_stop();
        ao_available = 0;
      }
    }
  }

  pthread_cond_signal(&audio_cond);
  pthread_mutex_unlock(&audio_mutex);
}

static void osd_stopsound(void)
{
  pthread_mutex_lock(&audio_mutex);
  audio_running = 0;
  audio_callback = NULL;
  pthread_cond_broadcast(&audio_cond);
  pthread_mutex_unlock(&audio_mutex);

  if (audio_thread_created) {
    pthread_join(audio_thread, NULL);
    audio_thread_created = 0;
  }
  if (ao_available) {
    h3531_ao_stop();
    ao_available = 0;
  }
}

static int osd_init_sound(void)
{
  memset(audio_frame, 0, sizeof(audio_frame));
  ao_fill = 0;
  resample_acc = 0;
  printf("H3531 NES audio: early init safe; AO deferred until APU callback\n");
  return 0;
}

void osd_getsoundinfo(sndinfo_t *info)
{
  info->sample_rate = DEFAULT_SAMPLERATE;
  info->bps = 16;
}
/* --- End Audio --- */'''
s = s[:audio_start] + audio_code + s[audio_end:]
p.write_text(s)

# Deterministic evdev override; otherwise keep upstream keyboard scan.
p = root / "platform" / "input.c"
s = p.read_text()
needle = "static int find_keyboard_device(void) {\n    DIR *dir;"
insert = '''static int find_keyboard_device(void) {\n    const char *forced = getenv("NOFRENDO_INPUT");\n    if (forced && forced[0]) {\n        int fd = open(forced, O_RDONLY | O_NONBLOCK);\n        if (fd >= 0) {\n            printf("H3531 NES input: forced evdev %s\\n", forced);\n            return fd;\n        }\n        fprintf(stderr, "H3531 NES input: cannot open %s\\n", forced);\n    }\n\n    DIR *dir;'''
if s.count(needle) != 1:
    raise SystemExit(f"input marker count={s.count(needle)}")
s = s.replace(needle, insert, 1)
p.write_text(s)

# Add narrow stage markers around the exact interval where v2 crashed.
p = root / "core" / "nofrendo.c"
s = p.read_text()
s = s.replace('   if (gui_init())\n      return -1;\n\n   osd_getvideoinfo(&video);\n   if (vid_init(video.default_width, video.default_height, video.driver))\n      return -1;\n\tprintf("vid_init done\\n");',
'''   printf("H3531 stage: gui_init begin\\n");\n   if (gui_init())\n      return -1;\n   printf("H3531 stage: gui_init done\\n");\n\n   osd_getvideoinfo(&video);\n   printf("H3531 stage: vid_init begin %dx%d\\n", video.default_width, video.default_height);\n   if (vid_init(video.default_width, video.default_height, video.driver))\n      return -1;\n   printf("H3531 stage: vid_init done\\n");''', 1)
p.write_text(s)

# Build identity.
p = root / "main.c"
s = p.read_text()
needle = "int main(int argc, char *argv[])\n{\n"
insert = 'int main(int argc, char *argv[])\n{\n    printf("H3531 Nofrendo v3: stable init + exact 60Hz + deferred AO/resampler + A1R5G5B5\\n");\n'
if s.count(needle) != 1:
    raise SystemExit(f"main marker count={s.count(needle)}")
s = s.replace(needle, insert, 1)
p.write_text(s)

print("H3531 Nofrendo v3 patch applied")
