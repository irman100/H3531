#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage422_v3_ps1_video.py INPUT OUTPUT')

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding='utf-8')

insert_anchor = 'static bool h3531_present_rowreuse_rgb565(h3531_fb_t *h,\n'
if insert_anchor not in src:
    raise SystemExit('row-reuse anchor missing')

helpers = r'''static bool h3531_reserve_ps1_row(unsigned dw)
{
   uint16_t *new_row;
   if (dw <= h3531_row_cap)
      return true;
   new_row = (uint16_t*)realloc(h3531_row, (size_t)dw * sizeof(*h3531_row));
   if (!new_row)
      return false;
   h3531_row = new_row;
   h3531_row_cap = dw;
   return true;
}

static bool h3531_present_rgb565_3x(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   unsigned sy;
   if (!h3531_reserve_ps1_row(960U))
      return false;

   for (sy = 0; sy < 240U; ++sy)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)sy * src_pitch);
      unsigned sx;
      for (sx = 0; sx < 320U; ++sx)
      {
         const uint16_t p = h3531_fast_rgb565_to_a1r5g5b5(srow[sx]);
         const unsigned ox = sx * 3U;
         h3531_row[ox + 0U] = p;
         h3531_row[ox + 1U] = p;
         h3531_row[ox + 2U] = p;
      }

      {
         unsigned rep;
         for (rep = 0; rep < 3U; ++rep)
         {
            uint16_t *dst = (uint16_t*)(h->mem +
                  (size_t)(dy + sy * 3U + rep) * h->stride) + dx;
            memcpy(dst, h3531_row, 960U * sizeof(*h3531_row));
         }
      }
   }
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   return true;
}

static bool h3531_present_rgb565_1x(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   unsigned y;
   for (y = 0; y < 480U; ++y)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)y * src_pitch);
      uint16_t *dst = (uint16_t*)(h->mem + (size_t)(dy + y) * h->stride) + dx;
      unsigned x;
      for (x = 0; x < 640U; ++x)
         dst[x] = h3531_fast_rgb565_to_a1r5g5b5(srow[x]);
   }
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   return true;
}

static bool h3531_present_rgb565_2x1(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   unsigned y;
   for (y = 0; y < 480U; ++y)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)y * src_pitch);
      uint16_t *dst = (uint16_t*)(h->mem + (size_t)(dy + y) * h->stride) + dx;
      unsigned x;
      for (x = 0; x < 320U; ++x)
      {
         const uint16_t p = h3531_fast_rgb565_to_a1r5g5b5(srow[x]);
         dst[x * 2U + 0U] = p;
         dst[x * 2U + 1U] = p;
      }
   }
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   return true;
}

static bool h3531_ps1_integer3x_logged = false;
static bool h3531_ps1_direct1x_logged = false;
static bool h3531_ps1_horizontal2x_logged = false;

'''
src = src.replace(insert_anchor, helpers + insert_anchor, 1)

old = '''   if (!h3531_present_rowreuse_rgb565(h, (const uint8_t*)src,
            src_w, src_h, src_pitch, dx, dy, dw, dh))
   {
      h3531_present_reference(h, src, src_w, src_h, src_pitch,
            bits, menu_texture);
      return;
   }
'''
if old not in src:
    raise SystemExit('presentation fallback block missing')

new = '''   {
      bool presented = false;

      /* Preserve the exact viewport. These paths only remove generic scaler
       * overhead; no PS1 picture dimensions are reduced. */
      if (src_w == 320U && src_h == 240U && dw == 960U && dh == 720U)
      {
         presented = h3531_present_rgb565_3x(h, (const uint8_t*)src,
               src_pitch, dx, dy);
         if (presented && !h3531_ps1_integer3x_logged)
         {
            RARCH_LOG("[H3531] PS1 RGB565 integer3x fastpath active: 320x240 -> 960x720\\n");
            h3531_ps1_integer3x_logged = true;
         }
      }
      else if (src_w == 640U && src_h == 480U && dw == 640U && dh == 480U)
      {
         presented = h3531_present_rgb565_1x(h, (const uint8_t*)src,
               src_pitch, dx, dy);
         if (presented && !h3531_ps1_direct1x_logged)
         {
            RARCH_LOG("[H3531] RGB565 direct1x fastpath active: 640x480 -> 640x480\\n");
            h3531_ps1_direct1x_logged = true;
         }
      }
      else if (src_w == 320U && src_h == 480U && dw == 640U && dh == 480U)
      {
         presented = h3531_present_rgb565_2x1(h, (const uint8_t*)src,
               src_pitch, dx, dy);
         if (presented && !h3531_ps1_horizontal2x_logged)
         {
            RARCH_LOG("[H3531] RGB565 horizontal2x fastpath active: 320x480 -> 640x480\\n");
            h3531_ps1_horizontal2x_logged = true;
         }
      }

      if (!presented)
         presented = h3531_present_rowreuse_rgb565(h, (const uint8_t*)src,
               src_w, src_h, src_pitch, dx, dy, dw, dh);

      if (!presented)
      {
         h3531_present_reference(h, src, src_w, src_h, src_pitch,
               bits, menu_texture);
         return;
      }
   }
'''
src = src.replace(old, new, 1)

free_anchor = '   h3531_fastpath_logged = false;\n'
if free_anchor not in src:
    raise SystemExit('free reset anchor missing')
src = src.replace(free_anchor, free_anchor + '''   h3531_ps1_integer3x_logged = false;
   h3531_ps1_direct1x_logged = false;
   h3531_ps1_horizontal2x_logged = false;
''', 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE422_V3_PS1_VIDEO_PATCH_OK {src_path} -> {out_path}')
