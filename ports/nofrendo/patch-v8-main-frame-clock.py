from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Hardware-test identity.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v7: source snapshots + native HIFB vblank + AO prebuffer + exact 60Hz"
new = "H3531 Nofrendo v8: main-loop frame clock + source snapshots + native HIFB vblank + AO prebuffer"
if s.count(old) != 1:
    raise SystemExit(f"v7 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

# Stop creating the asynchronous 60-Hz timer thread.  v8 paces the NES core
# directly from nes_emulate(), so there is no timer-thread -> nofrendo_ticks ->
# polling -> catch-up frameskip chain.
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
  printf("H3531 NES timer v8: external timer disabled; main loop owns %d-Hz frame clock\n",
         frequency);
  return 0;
}
'''
s = s[:start] + replacement + s[end:]
p.write_text(s)

# Replace the tick-polling / autoframeskip loop with one directly paced frame
# per iteration.  Keep an absolute CLOCK_MONOTONIC deadline and preserve the
# fractional nanoseconds (1e9 % refresh_rate), so long-term cadence is exact.
# Mild lateness is absorbed by the next deadline; lateness above half a frame
# resynchronizes without rendering a burst of catch-up frames.
p = root / "core" / "nes" / "nes.c"
s = p.read_text()

inc = "#include <sched.h>\n"
if inc not in s:
    raise SystemExit("v6 sched include not present")
s = s.replace(inc, inc + "#include <errno.h>\n#include <stdint.h>\n#include <time.h>\n", 1)

start_marker = "/* main emulation loop */\nvoid nes_emulate(void)\n"
start = s.find(start_marker)
end = s.find("\nstatic void mem_trash", start)
if start < 0 or end < 0:
    raise SystemExit("nes_emulate boundaries not found")

new_block = r'''/* H3531 v8: direct main-loop frame clock. */
static uint64_t h3531_nes_now_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void h3531_nes_sleep_until(uint64_t deadline)
{
   for (;;)
   {
      uint64_t now = h3531_nes_now_ns();
      uint64_t left;
      struct timespec req;

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

/* main emulation loop */
void nes_emulate(void)
{
   const uint64_t base_ns = 1000000000ULL / (uint64_t)NES_REFRESH_RATE;
   const unsigned rem_step = (unsigned)(1000000000ULL % (uint64_t)NES_REFRESH_RATE);
   uint64_t deadline;
   uint64_t max_late_ns = 0;
   unsigned rem_acc = 0;
   unsigned frame_count = 0;
   unsigned late_count = 0;
   unsigned resync_count = 0;

   osd_setsound(nes.apu->process);

   nes.scanline_cycles = 0;
   nes.fiq_cycles = (int) NES_FIQ_PERIOD;
   deadline = h3531_nes_now_ns();

   printf("H3531 NES clock v8: main-loop absolute %d Hz, no catch-up frameskip\n",
          NES_REFRESH_RATE);

   while (false == nes.poweroff)
   {
      uint64_t now;
      uint64_t late_ns = 0;

      /* One logical GUI/NES tick maps to exactly one emulated frame. */
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

      /* Advance the absolute deadline by exactly 1 / refresh_rate seconds,
         including the fractional nanosecond remainder. */
      deadline += base_ns;
      rem_acc += rem_step;
      if (rem_acc >= (unsigned)NES_REFRESH_RATE)
      {
         deadline += 1ULL;
         rem_acc -= (unsigned)NES_REFRESH_RATE;
      }

      h3531_nes_sleep_until(deadline);
      now = h3531_nes_now_ns();

      if (now > deadline)
      {
         late_ns = now - deadline;
         late_count++;
         if (late_ns > max_late_ns)
            max_late_ns = late_ns;

         /* Never render two frames back-to-back just because Linux woke us
            late.  Large lateness starts a new phase instead. */
         if (late_ns > base_ns / 2ULL)
         {
            deadline = now;
            rem_acc = 0;
            resync_count++;
         }
      }

      frame_count++;
      if (frame_count == 600)
      {
         printf("H3531 NES clock v8: 600 frames late=%u max_late_us=%lu resync=%u\n",
                late_count,
                (unsigned long)(max_late_ns / 1000ULL),
                resync_count);
         frame_count = 0;
         late_count = 0;
         max_late_ns = 0;
         resync_count = 0;
      }
   }
}
'''

s = s[:start] + new_block + s[end:]
p.write_text(s)

print("H3531 Nofrendo v8 main-frame-clock patch applied")
