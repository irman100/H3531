#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage426_snes_gba_video.py INPUT OUTPUT')

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding='utf-8')

# Input is the Stage4.22 V3 PS1-patched H3531 framebuffer driver.
insert_anchor = 'static bool h3531_present_rgb565_3x(h3531_fb_t *h,\n'
if insert_anchor not in src:
    raise SystemExit('Stage4.22 V3 RGB565 3x anchor missing')

helpers = r'''static bool h3531_present_rgb565_gba_4x(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   unsigned sy;
   if (!h3531_reserve_ps1_row(960U))
      return false;

   for (sy = 0; sy < 160U; ++sy)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)sy * src_pitch);
      unsigned sx;
      for (sx = 0; sx < 240U; ++sx)
      {
         const uint16_t p = h3531_fast_rgb565_to_a1r5g5b5(srow[sx]);
         const unsigned ox = sx * 4U;
         h3531_row[ox + 0U] = p;
         h3531_row[ox + 1U] = p;
         h3531_row[ox + 2U] = p;
         h3531_row[ox + 3U] = p;
      }
      {
         unsigned rep;
         for (rep = 0; rep < 4U; ++rep)
         {
            uint16_t *dst = (uint16_t*)(h->mem +
                  (size_t)(dy + sy * 4U + rep) * h->stride) + dx;
            memcpy(dst, h3531_row, 960U * sizeof(*h3531_row));
         }
      }
   }
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   return true;
}

static bool h3531_present_rgb565_snes_3x(h3531_fb_t *h,
      const uint8_t *src, unsigned src_pitch,
      unsigned dx, unsigned dy)
{
   unsigned sy;
   if (!h3531_reserve_ps1_row(768U))
      return false;

   for (sy = 0; sy < 224U; ++sy)
   {
      const uint16_t *srow = (const uint16_t*)(src + (size_t)sy * src_pitch);
      unsigned sx;
      for (sx = 0; sx < 256U; ++sx)
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
            memcpy(dst, h3531_row, 768U * sizeof(*h3531_row));
         }
      }
   }
#if defined(__arm__)
   __asm__ volatile("dmb" ::: "memory");
#endif
   return true;
}

static bool h3531_stage426_gba_4x_logged = false;
static bool h3531_stage426_snes_3x_logged = false;

'''
src = src.replace(insert_anchor, helpers + insert_anchor, 1)

ps1_if = '      if (src_w == 320U && src_h == 240U && dw == 960U && dh == 720U)\n'
if ps1_if not in src:
    raise SystemExit('Stage4.22 V3 dispatch anchor missing')

new_dispatch = '''      if (src_w == 240U && src_h == 160U && dw == 960U && dh == 640U)\n      {\n         presented = h3531_present_rgb565_gba_4x(h, (const uint8_t*)src,\n               src_pitch, dx, dy);\n         if (presented && !h3531_stage426_gba_4x_logged)\n         {\n            RARCH_LOG("[H3531] Stage4.26 GBA RGB565 integer4x fastpath active: 240x160 -> 960x640\\n");\n            h3531_stage426_gba_4x_logged = true;\n         }\n      }\n      else if (src_w == 256U && src_h == 224U && dw == 768U && dh == 672U)\n      {\n         presented = h3531_present_rgb565_snes_3x(h, (const uint8_t*)src,\n               src_pitch, dx, dy);\n         if (presented && !h3531_stage426_snes_3x_logged)\n         {\n            RARCH_LOG("[H3531] Stage4.26 SNES RGB565 integer3x fastpath active: 256x224 -> 768x672\\n");\n            h3531_stage426_snes_3x_logged = true;\n         }\n      }\n      else if (src_w == 320U && src_h == 240U && dw == 960U && dh == 720U)\n'''
src = src.replace(ps1_if, new_dispatch, 1)

reset_anchor = '   h3531_ps1_horizontal2x_logged = false;\n'
if reset_anchor not in src:
    raise SystemExit('Stage4.22 V3 log-reset anchor missing')
src = src.replace(reset_anchor, reset_anchor +
      '   h3531_stage426_gba_4x_logged = false;\n'
      '   h3531_stage426_snes_3x_logged = false;\n', 1)

out_path.write_text(src, encoding='utf-8')
print(f'STAGE426_SNES_GBA_VIDEO_PATCH_OK {src_path} -> {out_path}')
