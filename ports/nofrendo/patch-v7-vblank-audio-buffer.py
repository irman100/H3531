from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

# Unmistakable hardware-test identity.
p = root / "main.c"
s = p.read_text()
old = "H3531 Nofrendo v6: proven AO frame + shadow/vsync video + right-edge crop + exact 60Hz"
new = "H3531 Nofrendo v7: source snapshots + native HIFB vblank + AO prebuffer + exact 60Hz"
if s.count(old) != 1:
    raise SystemExit(f"v6 main identity marker count={s.count(old)}")
p.write_text(s.replace(old, new, 1))

p = root / "platform" / "osd_linux.c"
s = p.read_text()

# ---------------------------------------------------------------------------
# 1) Stable source snapshots.
#
# v4 made the 256x224 bitmap persistent to eliminate use-after-free.  That was
# necessary, but the video worker then read the same bitmap while the emulator
# could already be drawing the next frame into it.  v7 snapshots each complete
# NES frame into one of two small (~57 KiB) buffers before handing it to the
# asynchronous video worker.  The worker never reads the live PPU bitmap.
# ---------------------------------------------------------------------------
marker = "#define DEFAULT_HEIGHT    NES_VISIBLE_HEIGHT\n"
insert = r'''#define DEFAULT_HEIGHT    NES_VISIBLE_HEIGHT

#define H3531_VIDEO_SLOTS 2
#define H3531_VIDEO_FRAME_BYTES (DEFAULT_WIDTH * DEFAULT_HEIGHT)
static uint8_t h3531_video_slot[H3531_VIDEO_SLOTS][H3531_VIDEO_FRAME_BYTES];
/* 0 free, 1 ready, 2 being displayed */
static int h3531_video_slot_state[H3531_VIDEO_SLOTS];
static unsigned h3531_video_drop_count;
'''
if s.count(marker) != 1:
    raise SystemExit(f"video macro marker count={s.count(marker)}")
s = s.replace(marker, insert, 1)

old_custom = r'''static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects) {
  pthread_mutex_lock(&vid_mutex);

  // 如果上一帧还没画完，就丢弃这一帧（避免卡顿）
  if (g_bmp == NULL) { 
    g_bmp = bmp; // 把新帧放入队列
    pthread_cond_signal(&vid_cond); // 唤醒 videoTask_linux
  }

  pthread_mutex_unlock(&vid_mutex);

  // do_audio_frame(); // 声音被 stubbed, 注释掉
}
'''
new_custom = r'''static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects) {
  int slot = -1;
  int y;
  int i;
  (void)num_dirties;
  (void)dirty_rects;

  if (!bmp)
    return;

  pthread_mutex_lock(&vid_mutex);
  for (i = 0; i < H3531_VIDEO_SLOTS; ++i) {
    if (h3531_video_slot_state[i] == 0) {
      slot = i;
      /* Reserve it while we copy the live PPU image. */
      h3531_video_slot_state[i] = 2;
      break;
    }
  }
  pthread_mutex_unlock(&vid_mutex);

  if (slot < 0) {
    /* Never stall emulation waiting for video.  A later complete frame wins. */
    h3531_video_drop_count++;
    return;
  }

  /* custom_blit runs before Nofrendo can start drawing the next frame, so this
     short 57-KiB copy is an atomic source snapshot from the emulator's point
     of view. */
  for (y = 0; y < DEFAULT_HEIGHT; ++y)
    memcpy(&h3531_video_slot[slot][y * DEFAULT_WIDTH],
           bmp->line[y], DEFAULT_WIDTH);

  pthread_mutex_lock(&vid_mutex);
  h3531_video_slot_state[slot] = 1;
  pthread_cond_signal(&vid_cond);
  pthread_mutex_unlock(&vid_mutex);
}
'''
if s.count(old_custom) != 1:
    raise SystemExit(f"custom_blit marker count={s.count(old_custom)}")
s = s.replace(old_custom, new_custom, 1)

start = s.find("static void* videoTask_linux(void *arg) {")
end_marker = "/* --- End Video --- */"
end = s.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit("videoTask_linux boundaries not found")
old_video_block = s[start:end]
new_video_block = r'''static void* videoTask_linux(void *arg) {
    int screen_width = SCREEN_WIDTH;
    int screen_height = SCREEN_HEIGHT;
    int nes_width = DEFAULT_WIDTH;
    int nes_height = DEFAULT_HEIGHT;
    int x = (screen_width - DEFAULT_WIDTH) / 2;
    int y = (screen_height - DEFAULT_HEIGHT) / 2;
    uint8_t *lines[DEFAULT_HEIGHT];
    int row;
    (void)arg;

    printf("Centering NES (%dx%d) on screen (%dx%d) at offset (%d, %d)\n",
           nes_width, nes_height, screen_width, screen_height, x, y);
    printf("H3531 NES video v7: double source snapshot queue ready (%u bytes each)\n",
           (unsigned)H3531_VIDEO_FRAME_BYTES);

    for (;;) {
      int slot = -1;
      int i;

      pthread_mutex_lock(&vid_mutex);
      for (;;) {
        for (i = 0; i < H3531_VIDEO_SLOTS; ++i) {
          if (h3531_video_slot_state[i] == 1) {
            slot = i;
            h3531_video_slot_state[i] = 2;
            break;
          }
        }
        if (slot >= 0)
          break;
        pthread_cond_wait(&vid_cond, &vid_mutex);
      }
      pthread_mutex_unlock(&vid_mutex);

      for (row = 0; row < DEFAULT_HEIGHT; ++row)
        lines[row] = &h3531_video_slot[slot][row * DEFAULT_WIDTH];

      fb_blit_lines(x, y, nes_width, nes_height, lines);

      pthread_mutex_lock(&vid_mutex);
      h3531_video_slot_state[slot] = 0;
      pthread_mutex_unlock(&vid_mutex);
    }
    return NULL;
}
'''
s = s[:start] + new_video_block + s[end:]

# ---------------------------------------------------------------------------
# 2) AO jitter reserve.
#
# Three 160-sample silent blocks = 10 ms at 48 kHz.  This is deliberately
# small (well under one NES frame) but gives AO enough queued data to survive a
# short Linux scheduler delay without an audible underrun click.
# ---------------------------------------------------------------------------
old_audio = r'''    ao_available = (h3531_ao_start() == 0);
    if (!ao_available)
      fprintf(stderr, "H3531 AO: unavailable; NES continues with paced silent audio\n");
    audio_running = 1;
    ao_fill = 0;
    resample_acc = 0;
    if (pthread_create(&audio_thread, NULL, audio_thread_func, NULL) == 0) {
'''
new_audio = r'''    ao_available = (h3531_ao_start() == 0);
    if (!ao_available)
      fprintf(stderr, "H3531 AO: unavailable; NES continues with paced silent audio\n");
    ao_fill = 0;
    resample_acc = 0;
    if (ao_available) {
      int prime;
      memset(ao_block, 0, sizeof(ao_block));
      for (prime = 0; prime < 3; ++prime) {
        if (h3531_ao_send_160(ao_block) != 0) {
          fprintf(stderr, "H3531 NES audio v7: AO prebuffer failed at block %d\n", prime);
          h3531_ao_stop();
          ao_available = 0;
          break;
        }
      }
      if (ao_available)
        printf("H3531 NES audio v7: AO prebuffer=3x160 samples (10 ms)\n");
    }
    audio_running = 1;
    if (pthread_create(&audio_thread, NULL, audio_thread_func, NULL) == 0) {
'''
if s.count(old_audio) != 1:
    raise SystemExit(f"audio prebuffer marker count={s.count(old_audio)}")
s = s.replace(old_audio, new_audio, 1)

# ---------------------------------------------------------------------------
# 3) Remove per-key UART logging from the hot path.  It was useful during
# bring-up, but synchronous serial printf can itself create 60-Hz tick bursts.
# ---------------------------------------------------------------------------
for noisy in [
    '    // **** 添加日志：打印当前状态和变化 ****\n    printf("DEBUG: my_input = 0x%02X, changed_bits = 0x%02X\\n", my_input, chg);\n    // **** 日志结束 ****\n\n',
    '            printf("DEBUG: Bit %d changed. Event ID: %d. Handler found: %p\\n",\n                   bit_index, event_id, handler);\n\n',
    '                printf("DEBUG: Sending event for bit %d (Event ID: %d): %s\\n",\n                       bit_index, event_id, pressed ? "Pressed (MAKE)" : "Released (BREAK)");\n\n',
]:
    if s.count(noisy) != 1:
        raise SystemExit("expected input debug block not found")
    s = s.replace(noisy, "", 1)

p.write_text(s)

# Remove the second per-event debug source in nesinput.c.
p = root / "core" / "nes" / "nesinput.c"
s = p.read_text()
noisy = r'''   // Debug
   printf("========= HANDLER =========\n");
   printf("DEBUG: input_event: input->data changed to 0x%02X (state=%d, value=0x%02X)\n",
           input->data, state, value);
   printf("========= HANDLER =========\n\n");
'''
if s.count(noisy) != 1:
    raise SystemExit(f"nesinput debug marker count={s.count(noisy)}")
s = s.replace(noisy, "", 1)
p.write_text(s)

print("H3531 Nofrendo v7 vblank/audio/source-snapshot patch applied")
