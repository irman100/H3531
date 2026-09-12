from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")

p = root / "platform" / "osd_linux.c"
s = p.read_text()

old = '''static void shutdown(void)\n{\n}\n'''
new = '''static void shutdown(void)\n{\n  if (myBitmap) {\n    printf("H3531 NES video: destroying persistent hardware bitmap\\n");\n    bmp_destroy(&myBitmap);\n  }\n}\n'''
if s.count(old) != 1:
    raise SystemExit(f"shutdown marker count={s.count(old)}")
s = s.replace(old, new, 1)

old = '''static bitmap_t *lock_write(void)\n{\n    // 1. 使用我们新的 "ppu_frame_buffer"\n    // 2. 修正 stride (步长): 我们的缓冲区是 8-bit (uint8_t)，所以一行的字节数\n    //    就是 DEFAULT_WIDTH (256)，而不是 DEFAULT_WIDTH*2 (512)。\n    myBitmap = bmp_createhw(ppu_frame_buffer, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH);\n    return myBitmap;\n}\n'''
new = '''static bitmap_t *lock_write(void)\n{\n    /*\n     * The Nofrendo core keeps the pointer returned by lock_write() after\n     * free_write() returns during vid_findmode().  Therefore the hardware\n     * bitmap wrapper must outlive a lock/unlock pair.  The previous port\n     * destroyed it in free_write(), leaving vid_findmode() with a dangling\n     * screen pointer and causing a use-after-free during vid_init().\n     */\n    if (myBitmap == NULL) {\n        myBitmap = bmp_createhw(ppu_frame_buffer, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH);\n        printf("H3531 NES video: persistent hw bitmap created %p (%dx%d pitch=%d)\\n",\n               (void *)myBitmap, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH);\n    }\n    return myBitmap;\n}\n'''
if s.count(old) != 1:
    raise SystemExit(f"lock_write marker count={s.count(old)}")
s = s.replace(old, new, 1)

old = '''static void free_write(int num_dirties, rect_t *dirty_rects)\n{\n bmp_destroy(&myBitmap);\n}\n'''
new = '''static void free_write(int num_dirties, rect_t *dirty_rects)\n{\n  (void)num_dirties;\n  (void)dirty_rects;\n  /* Do not destroy myBitmap here: vid_findmode() still dereferences it. */\n  printf("H3531 NES video: free_write keeps hw bitmap alive\\n");\n}\n'''
if s.count(old) != 1:
    raise SystemExit(f"free_write marker count={s.count(old)}")
s = s.replace(old, new, 1)
p.write_text(s)

# Add one-shot diagnostics inside the core video initialization path.
p = root / "core" / "vid_drv.c"
s = p.read_text()
old = '''static int vid_findmode(int width, int height, viddriver_t *osd_driver)\n{\n   if (osd_driver->init(width, height))\n'''
new = '''static int vid_findmode(int width, int height, viddriver_t *osd_driver)\n{\n   printf("H3531 vid_findmode: driver init begin\\n");\n   if (osd_driver->init(width, height))\n'''
if s.count(old) != 1:
    raise SystemExit(f"vid_findmode start marker count={s.count(old)}")
s = s.replace(old, new, 1)

old = '''   /* we got our driver */\n   driver = osd_driver;\n\n   /* re-assert dimensions, clear the surface */\n   screen = driver->lock_write();\n'''
new = '''   /* we got our driver */\n   driver = osd_driver;\n   printf("H3531 vid_findmode: driver init done\\n");\n\n   /* re-assert dimensions, clear the surface */\n   screen = driver->lock_write();\n   printf("H3531 vid_findmode: lock_write returned %p\\n", (void *)screen);\n   if (screen == NULL) {\n      printf("H3531 vid_findmode: lock_write failed\\n");\n      driver = NULL;\n      return -1;\n   }\n'''
if s.count(old) != 1:
    raise SystemExit(f"vid lock marker count={s.count(old)}")
s = s.replace(old, new, 1)

old = '''   /* release surface */\n   if (driver->free_write)\n      driver->free_write(-1, NULL);\n\n   log_printf("video driver: %s at %dx%d\\n", driver->name,\n              screen->width, screen->height);\n'''
new = '''   /* release surface */\n   if (driver->free_write)\n      driver->free_write(-1, NULL);\n\n   printf("H3531 vid_findmode: after free_write screen=%p %dx%d\\n",\n          (void *)screen, screen->width, screen->height);\n   log_printf("video driver: %s at %dx%d\\n", driver->name,\n              screen->width, screen->height);\n'''
if s.count(old) != 1:
    raise SystemExit(f"vid free marker count={s.count(old)}")
s = s.replace(old, new, 1)
p.write_text(s)

# Give the physical test binary an unmistakable identity.
p = root / "main.c"
s = p.read_text()
s2 = s.replace(
    'H3531 Nofrendo v3: stable init + exact 60Hz + deferred AO/resampler + A1R5G5B5',
    'H3531 Nofrendo v4: persistent video bitmap + exact 60Hz + deferred AO/resampler + A1R5G5B5',
    1,
)
if s2 == s:
    raise SystemExit("v3 main identity marker not found")
p.write_text(s2)

print("H3531 Nofrendo v4 video-lifetime patch applied")
