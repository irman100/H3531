#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage426_snes_gba_video.py INPUT OUTPUT')

src_path, out_path = map(Path, sys.argv[1:])
src = src_path.read_text(encoding='utf-8')

anchor = 'static void h3531_present_rgb565('
if anchor not in src:
    raise SystemExit('h3531_present_rgb565 anchor missing')

helper = r'''
static bool h3531_stage426_integer_fastpath(FbVideo *v, const uint16_t *src,
      unsigned width, unsigned height, size_t pitch)
{
   if (!v || !v->fb || !src || pitch < width * 2U)
      return false;

   const unsigned src_pitch_px = (unsigned)(pitch / 2U);
   const unsigned fb_w = (unsigned)v->fb_width;
   const unsigned fb_h = (unsigned)v->fb_height;

   /* GBA: native 240x160 -> exactly 960x640 (4x), centered vertically. */
   if (width == 240 && height == 160 && fb_w >= 960 && fb_h >= 640)
   {
      const unsigned dst_w = 960, dst_h = 640;
      const unsigned x0 = (fb_w - dst_w) / 2U;
      const unsigned y0 = (fb_h - dst_h) / 2U;
      for (unsigned sy = 0; sy < 160; ++sy)
      {
         const uint16_t *s = src + (size_t)sy * src_pitch_px;
         uint16_t *rows[4];
         for (unsigned ry = 0; ry < 4; ++ry)
            rows[ry] = (uint16_t *)((uint8_t *)v->fb +
                  (size_t)(y0 + sy * 4U + ry) * v->fb_pitch) + x0;
         for (unsigned sx = 0; sx < 240; ++sx)
         {
            const uint16_t p = s[sx];
            const unsigned dx = sx * 4U;
            for (unsigned ry = 0; ry < 4; ++ry)
            {
               rows[ry][dx + 0] = p;
               rows[ry][dx + 1] = p;
               rows[ry][dx + 2] = p;
               rows[ry][dx + 3] = p;
            }
         }
      }
      if (!v->stage426_gba_logged)
      {
         RARCH_LOG("[H3531] Stage4.26 GBA RGB565 integer4x fastpath active: 240x160 -> 960x640\n");
         v->stage426_gba_logged = true;
      }
      return true;
   }

   /* SNES common mode: 256x224 -> exactly 768x672 (3x), centered. */
   if (width == 256 && height == 224 && fb_w >= 768 && fb_h >= 672)
   {
      const unsigned dst_w = 768, dst_h = 672;
      const unsigned x0 = (fb_w - dst_w) / 2U;
      const unsigned y0 = (fb_h - dst_h) / 2U;
      for (unsigned sy = 0; sy < 224; ++sy)
      {
         const uint16_t *s = src + (size_t)sy * src_pitch_px;
         uint16_t *rows[3];
         for (unsigned ry = 0; ry < 3; ++ry)
            rows[ry] = (uint16_t *)((uint8_t *)v->fb +
                  (size_t)(y0 + sy * 3U + ry) * v->fb_pitch) + x0;
         for (unsigned sx = 0; sx < 256; ++sx)
         {
            const uint16_t p = s[sx];
            const unsigned dx = sx * 3U;
            for (unsigned ry = 0; ry < 3; ++ry)
            {
               rows[ry][dx + 0] = p;
               rows[ry][dx + 1] = p;
               rows[ry][dx + 2] = p;
            }
         }
      }
      if (!v->stage426_snes_logged)
      {
         RARCH_LOG("[H3531] Stage4.26 SNES RGB565 integer3x fastpath active: 256x224 -> 768x672\n");
         v->stage426_snes_logged = true;
      }
      return true;
   }

   return false;
}

'''

src = src.replace(anchor, helper + anchor, 1)

# Add one-shot diagnostic flags to the driver state. Keep this patch anchored
# to the existing H3531 video struct used by Stage4.22 V4.
struct_anchor = 'bool stage422_direct_logged;'
if struct_anchor in src:
    src = src.replace(struct_anchor,
        struct_anchor + '\n   bool stage426_gba_logged;\n   bool stage426_snes_logged;', 1)
else:
    # fallback for older state layout: add immediately after framebuffer pitch
    struct_anchor = 'size_t fb_pitch;'
    if struct_anchor not in src:
        raise SystemExit('video state anchor missing')
    src = src.replace(struct_anchor,
        struct_anchor + '\n   bool stage426_gba_logged;\n   bool stage426_snes_logged;', 1)

# Dispatch before the generic scaler/direct path. This only accepts exact
# RGB565 source modes and otherwise returns false, preserving all old behavior.
call_anchor = 'if (!frame || width == 0 || height == 0)'
pos = src.find(call_anchor, src.find(anchor))
if pos < 0:
    raise SystemExit('frame validity anchor missing')
# insert after the validity block's immediate return statement
needle = 'return true;\n   }'
end = src.find(needle, pos)
if end < 0:
    raise SystemExit('frame validity block end missing')
end += len(needle)
src = src[:end] + '\n\n   if (h3531_stage426_integer_fastpath(v, (const uint16_t *)frame, width, height, pitch))\n      return true;' + src[end:]

out_path.write_text(src, encoding='utf-8')
print(f'STAGE426_VIDEO_PATCH_OK {src_path} -> {out_path}')
