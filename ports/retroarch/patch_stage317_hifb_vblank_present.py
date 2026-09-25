#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage317_hifb_vblank_present.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        raise SystemExit("function not found: " + signature)
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("opening brace not found: " + signature)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + replacement.rstrip() + text[i+1:]
    raise SystemExit("unterminated function: " + signature)

inc_anchor = '#include <time.h>\n'
if inc_anchor not in src:
    raise SystemExit("time include anchor missing")
src = src.replace(inc_anchor,
'''#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
''', 1)

insert_anchor = 'static bool h3531_present_rgb565_3x(h3531_fb_t *h,\n'
if insert_anchor not in src:
    raise SystemExit("PS1 fastpath insertion anchor missing")

helpers = r'''
/* Stage3.17: the stock Hi3531 HIFB driver does not implement Linux
 * FBIO_WAITFORVSYNC, but the physically proven vendor command _IO('F',100)
 * (0x4664) reaches hifb_wait_regconfig_work().  It is a presentation-sync
 * hint only; RetroArch's exact-content timer remains the emulation clock. */
#define H3531_FBIOGET_VBLANK_HIFB 0x00004664UL

static uint16_t *h3531_vblank_shadow = NULL;
static size_t h3531_vblank_shadow_cap = 0;
static int h3531_vblank_state = 0; /* 0 unknown, 1 works, -1 unavailable */
static uint64_t h3531_vblank_frames = 0;
static uint64_t h3531_vblank_wait_ns = 0;
static uint64_t h3531_vblank_copy_ns = 0;

static bool h3531_vblank_reserve(size_t pixels)
{
   uint16_t *p;
   if (pixels <= h3531_vblank_shadow_cap)
      return true;

   p = (uint16_t*)realloc(h3531_vblank_shadow,
         pixels * sizeof(*h3531_vblank_shadow));
   if (!p)
      return false;

   h3531_vblank_shadow = p;
   h3531_vblank_shadow_cap = pixels;
   return true;
}

static void h3531_wait_hifb_vblank(h3531_fb_t *h)
{
   struct timespec t0, t1;

   if (!h || h->fd < 0 || h3531_vblank_state < 0)
      return;

   clock_gettime(CLOCK_MONOTONIC, &t0);
   errno = 0;
   if (ioctl(h->fd, H3531_FBIOGET_VBLANK_HIFB, 0) == 0)
   {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      h3531_vblank_wait_ns += h3531_timespec_ns(&t1) -
            h3531_timespec_ns(&t0);

      if (h3531_vblank_state == 0)
         RARCH_LOG("[H3531] HIFB native vblank 0x4664 active; viewport-shadow presentation enabled\n");
      h3531_vblank_state = 1;
   }
   else
   {
      if (h3531_vblank_state == 0)
         RARCH_WARN("[H3531] HIFB native vblank 0x4664 unavailable errno=%d; shadow-copy fallback\n",
               errno);
      h3531_vblank_state = -1;
   }
}

static bool h3531_commit_vblank_shadow(h3531_fb_t *h,
      unsigned dx, unsigned dy, unsigned dw, unsigned dh)
{
   struct timespec t0, t1;
   unsigned y;

   if (!h || !h->mem || !h3531_vblank_shadow || !dw || !dh)
      return false;

   h3531_wait_hifb_vblank(h);

   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (y = 0; y < dh; ++y)
   {
      uint16_t *dst = (uint16_t*)(h->mem +
            (size_t)(dy + y) * h->stride) + dx;
      memcpy(dst,
            h3531_vblank_shadow + (size_t)y * dw,
            (size_t)dw * sizeof(*dst));
   }
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   clock_gettime(CLOCK_MONOTONIC, &t1);

   h3531_vblank_copy_ns += h3531_timespec_ns(&t1) -
         h3531_timespec_ns(&t0);
   ++h3531_vblank_frames;

   if (h3531_vblank_frames >= 300U)
   {
      const double wait_ms = (double)h3531_vblank_wait_ns /
            (double)h3531_vblank_frames / 1000000.0;
      const double copy_ms = (double)h3531_vblank_copy_ns /
            (double)h3531_vblank_frames / 1000000.0;
      RARCH_LOG("[H3531] VBLANK-PRESENT frames=%llu wait_avg=%.3fms copy_avg=%.3fms state=%s\n",
            (unsigned long long)h3531_vblank_frames,
            wait_ms, copy_ms,
            h3531_vblank_state > 0 ? "native" : "fallback");
      h3531_vblank_frames = 0;
      h3531_vblank_wait_ns = 0;
      h3531_vblank_copy_ns = 0;
   }

   return true;
}

'''
src = src.replace(insert_anchor, helpers + insert_anchor, 1)

ps1_3x = r'''static bool h3531_present_rgb565_3x(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   unsigned sy;
   const unsigned dw = 960U;
   const unsigned dh = 720U;

   if (!h3531_reserve_ps1_row(dw) ||
       !h3531_vblank_reserve((size_t)dw * dh))
      return false;

   for (sy = 0; sy < 240U; ++sy)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)sy * src_pitch);
      unsigned sx;
      unsigned rep;

      for (sx = 0; sx < 320U; ++sx)
      {
         const uint16_t p = h3531_fast_rgb565_to_a1r5g5b5(srow[sx]);
         const unsigned ox = sx * 3U;
         h3531_row[ox + 0U] = p;
         h3531_row[ox + 1U] = p;
         h3531_row[ox + 2U] = p;
      }

      for (rep = 0; rep < 3U; ++rep)
         memcpy(h3531_vblank_shadow +
                  (size_t)(sy * 3U + rep) * dw,
               h3531_row, (size_t)dw * sizeof(*h3531_row));
   }

   return h3531_commit_vblank_shadow(h, dx, dy, dw, dh);
}'''
src = replace_function(src,
      "static bool h3531_present_rgb565_3x(h3531_fb_t *h,",
      ps1_3x)

ps1_1x = r'''static bool h3531_present_rgb565_1x(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   const unsigned dw = 640U;
   const unsigned dh = 480U;
   unsigned y;

   if (!h3531_vblank_reserve((size_t)dw * dh))
      return false;

   for (y = 0; y < dh; ++y)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)y * src_pitch);
      uint16_t *dst = h3531_vblank_shadow + (size_t)y * dw;
      unsigned x;
      for (x = 0; x < dw; ++x)
         dst[x] = h3531_fast_rgb565_to_a1r5g5b5(srow[x]);
   }

   return h3531_commit_vblank_shadow(h, dx, dy, dw, dh);
}'''
src = replace_function(src,
      "static bool h3531_present_rgb565_1x(h3531_fb_t *h,",
      ps1_1x)

ps1_2x1 = r'''static bool h3531_present_rgb565_2x1(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   const unsigned dw = 640U;
   const unsigned dh = 480U;
   unsigned y;

   if (!h3531_vblank_reserve((size_t)dw * dh))
      return false;

   for (y = 0; y < dh; ++y)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)y * src_pitch);
      uint16_t *dst = h3531_vblank_shadow + (size_t)y * dw;
      unsigned x;

      for (x = 0; x < 320U; ++x)
      {
         const uint16_t p = h3531_fast_rgb565_to_a1r5g5b5(srow[x]);
         dst[x * 2U + 0U] = p;
         dst[x * 2U + 1U] = p;
      }
   }

   return h3531_commit_vblank_shadow(h, dx, dy, dw, dh);
}'''
src = replace_function(src,
      "static bool h3531_present_rgb565_2x1(h3531_fb_t *h,",
      ps1_2x1)

generic = r'''static bool h3531_present_rowreuse_rgb565(h3531_fb_t *h,
      const uint8_t *src, unsigned src_w, unsigned src_h,
      unsigned src_pitch, unsigned dx, unsigned dy,
      unsigned dw, unsigned dh)
{
   uint32_t cached_sy = UINT32_MAX;
   unsigned y;

   if (!h3531_reserve_maps(src_w, src_h, dw, dh) ||
       !h3531_vblank_reserve((size_t)dw * dh))
      return false;

   for (y = 0; y < dh; ++y)
   {
      uint32_t sy = h3531_ymap[y];
      uint16_t *dst = h3531_vblank_shadow + (size_t)y * dw;

      if (sy != cached_sy)
      {
         const uint16_t *srow =
            (const uint16_t*)(src + (size_t)sy * src_pitch);
         h3531_build_scaled_row_rgb565(srow, src_w, dw);
         cached_sy = sy;
      }

      memcpy(dst, h3531_row, (size_t)dw * sizeof(*dst));
   }

   return h3531_commit_vblank_shadow(h, dx, dy, dw, dh);
}'''
src = replace_function(src,
      "static bool h3531_present_rowreuse_rgb565(h3531_fb_t *h,",
      generic)

free_sig = "static void h3531_free_fast(void *data)"
free_start = src.find(free_sig)
if free_start < 0:
    raise SystemExit("free function missing")
free_brace = src.find("{", free_start)
insert_free = '''{
   free(h3531_vblank_shadow);
   h3531_vblank_shadow = NULL;
   h3531_vblank_shadow_cap = 0;
   h3531_vblank_state = 0;
   h3531_vblank_frames = 0;
   h3531_vblank_wait_ns = 0;
   h3531_vblank_copy_ns = 0;
'''
src = src[:free_brace] + insert_free + src[free_brace+1:]

required = [
   "H3531_FBIOGET_VBLANK_HIFB 0x00004664UL",
   "HIFB native vblank 0x4664 active; viewport-shadow presentation enabled",
   "VBLANK-PRESENT frames=%llu wait_avg=%.3fms copy_avg=%.3fms state=%s",
   "h3531_commit_vblank_shadow(h, dx, dy, dw, dh)",
   "PS1 RGB565 integer3x fastpath active",
   "RGB565 direct1x fastpath active",
   "RGB565 horizontal2x fastpath active",
]
for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage3.17 marker: " + marker)

out_path.write_text(src, encoding="utf-8")
print("STAGE317_HIFB_VBLANK_PRESENT_PATCH_OK")
