from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Distinct hardware-test identity.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v7: source snapshots + native HIFB vblank + AO prebuffer + exact 60Hz"
new = "H3531 Nofrendo v9: native HIFB vblank master clock + synchronous presentation + AO prebuffer"
if s.count(old) != 1:
    raise SystemExit(f"v7 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

# ---------------------------------------------------------------------------
# Platform: disable the independent software timer and make video presentation
# acknowledgement synchronous.  The video worker still performs palette/scale
# work and waits on HiSilicon HIFB vblank; the emulation thread waits until the
# submitted snapshot has actually been copied to visible VRAM before starting
# the next NES frame.
# ---------------------------------------------------------------------------
p = root / "platform" / "osd_linux.c"
s = p.read_text()

start = s.find("int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)")
end = s.find("\n}\n\n/*\n** Audio", start)
if start < 0 or end < 0:
    raise SystemExit("osd_installtimer boundary not found")
end += 3
replacement = r'''int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
  (void)func;
  (void)funcsize;
  (void)counter;
  (void)countersize;
  printf("H3531 NES timer v9: software timer disabled; native HIFB vblank is master clock (%d-Hz NES target)\n",
         frequency);
  return 0;
}
'''
s = s[:start] + replacement + s[end:]

custom_start = s.find("static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects) {")
custom_end = s.find("\n}\n\n/* --- Original videoTask", custom_start)
if custom_start < 0 or custom_end < 0:
    raise SystemExit("custom_blit boundary not found")
custom_end += 3
new_custom = r'''static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects) {
  int slot = -1;
  int y;
  int i;
  (void)num_dirties;
  (void)dirty_rects;

  if (!bmp)
    return;

  /* v9 never drops a completed NES frame.  Wait for a free snapshot slot. */
  pthread_mutex_lock(&vid_mutex);
  for (;;) {
    for (i = 0; i < H3531_VIDEO_SLOTS; ++i) {
      if (h3531_video_slot_state[i] == 0) {
        slot = i;
        h3531_video_slot_state[i] = 2; /* reserved for producer copy */
        break;
      }
    }
    if (slot >= 0)
      break;
    pthread_cond_wait(&vid_cond, &vid_mutex);
  }
  pthread_mutex_unlock(&vid_mutex);

  for (y = 0; y < DEFAULT_HEIGHT; ++y)
    memcpy(&h3531_video_slot[slot][y * DEFAULT_WIDTH],
           bmp->line[y], DEFAULT_WIDTH);

  /* Submit the immutable snapshot, then wait for the video worker to complete
     its native-vblank synchronized visible-VRAM copy.  This acknowledgement is
     the only frame clock used by the emulator in v9. */
  pthread_mutex_lock(&vid_mutex);
  h3531_video_slot_state[slot] = 1;
  pthread_cond_signal(&vid_cond);
  while (h3531_video_slot_state[slot] != 0)
    pthread_cond_wait(&vid_cond, &vid_mutex);
  pthread_mutex_unlock(&vid_mutex);
}
'''
s = s[:custom_start] + new_custom + s[custom_end:]

old_release = r'''      pthread_mutex_lock(&vid_mutex);
      h3531_video_slot_state[slot] = 0;
      pthread_mutex_unlock(&vid_mutex);
'''
new_release = r'''      pthread_mutex_lock(&vid_mutex);
      h3531_video_slot_state[slot] = 0;
      pthread_cond_broadcast(&vid_cond);
      pthread_mutex_unlock(&vid_mutex);
'''
if s.count(old_release) != 1:
    raise SystemExit(f"video release marker count={s.count(old_release)}")
s = s.replace(old_release, new_release, 1)

s = s.replace(
    'printf("H3531 NES video v7: double source snapshot queue ready (%u bytes each)\\n",',
    'printf("H3531 NES video v9: synchronous snapshot/presentation queue ready (%u bytes each)\\n",',
    1,
)

p.write_text(s)

# ---------------------------------------------------------------------------
# Core: one emulated frame per one completed hardware presentation.
# No nofrendo_ticks polling, no catch-up frames, no nanosleep.  CLOCK_MONOTONIC
# is used only for low-frequency diagnostics every 600 presented frames.
# ---------------------------------------------------------------------------
p = root / "core" / "nes" / "nes.c"
s = p.read_text()

inc = "#include <sched.h>\n"
if inc not in s:
    raise SystemExit("v6 sched include not present")
s = s.replace(inc, inc + "#include <stdint.h>\n#include <time.h>\n", 1)

start_marker = "/* main emulation loop */\nvoid nes_emulate(void)\n"
start = s.find(start_marker)
end = s.find("\nstatic void mem_trash", start)
if start < 0 or end < 0:
    raise SystemExit("nes_emulate boundaries not found")

new_loop = r'''static uint64_t h3531_v9_now_us(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* main emulation loop */
void nes_emulate(void)
{
   unsigned presented = 0;
   uint64_t batch_start_us;

   osd_setsound(nes.apu->process);

   nes.scanline_cycles = 0;
   nes.fiq_cycles = (int) NES_FIQ_PERIOD;
   batch_start_us = h3531_v9_now_us();

   printf("H3531 NES clock v9: one NES frame per acknowledged native HIFB vblank presentation\n");

   while (false == nes.poweroff)
   {
      gui_tick(1);

      if (true == nes.pause)
      {
         system_video(true);
      }
      else
      {
         nes_renderframe(true);
         system_video(true);
      }

      presented++;
      if (presented == 600)
      {
         uint64_t now_us = h3531_v9_now_us();
         uint64_t elapsed_us = (now_us > batch_start_us) ? (now_us - batch_start_us) : 0;
         uint64_t fps_x100 = elapsed_us ? (600ULL * 100ULL * 1000000ULL) / elapsed_us : 0;

         printf("H3531 NES clock v9: 600 presented frames elapsed_ms=%lu fps=%lu.%02lu\n",
                (unsigned long)(elapsed_us / 1000ULL),
                (unsigned long)(fps_x100 / 100ULL),
                (unsigned long)(fps_x100 % 100ULL));
         presented = 0;
         batch_start_us = now_us;
      }
   }
}
'''

s = s[:start] + new_loop + s[end:]
p.write_text(s)

print("H3531 Nofrendo v9 native-vblank master-clock patch applied")
