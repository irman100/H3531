/* RetroArch H3531 framebuffer PoC.
 *
 * This file intentionally occupies RetroArch's existing FPGA software-video
 * build slot in the first proof-of-concept.  It does not use FPGA hardware.
 * The slot lets us keep upstream registration changes to zero while proving
 * direct HIFB rendering on the real Hi3531 board.
 *
 * Target framebuffer proven by H3531 Monitor/Nofrendo:
 *   /dev/fb0, 1280x720, 16 bpp A1R5G5B5, stride normally 2560 bytes.
 *
 * Stage-1 scope: RetroArch RGUI/menu only.  No cores and no H3531 audio yet.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../../driver.h"
#include "../../configuration.h"
#include "../../verbosity.h"

#define H3531_DEFAULT_FB "/dev/fb0"

typedef struct h3531_fb
{
   int fd;
   uint8_t *mem;
   size_t mem_len;
   struct fb_fix_screeninfo fix;
   struct fb_var_screeninfo var;

   unsigned screen_w;
   unsigned screen_h;
   unsigned stride;

   unsigned char *menu_frame;
   size_t menu_frame_cap;
   unsigned menu_width;
   unsigned menu_height;
   unsigned menu_pitch;
   unsigned menu_bits;

   unsigned frame_width;
   unsigned frame_height;
   unsigned frame_pitch;
   unsigned frame_bits;

   unsigned last_x;
   unsigned last_y;
   unsigned last_w;
   unsigned last_h;
} h3531_fb_t;

static uint16_t h3531_pack_a1r5g5b5(unsigned r, unsigned g, unsigned b)
{
   return (uint16_t)(0x8000U
         | (((r >> 3) & 0x1fU) << 10)
         | (((g >> 3) & 0x1fU) << 5)
         |  ((b >> 3) & 0x1fU));
}

static uint16_t h3531_from_rgb565(uint16_t p)
{
   unsigned r5 = (p >> 11) & 0x1fU;
   unsigned g6 = (p >> 5)  & 0x3fU;
   unsigned b5 = p & 0x1fU;
   unsigned r  = (r5 << 3) | (r5 >> 2);
   unsigned g  = (g6 << 2) | (g6 >> 4);
   unsigned b  = (b5 << 3) | (b5 >> 2);
   return h3531_pack_a1r5g5b5(r, g, b);
}

/* RGUI's software menu texture in 16-bit mode is RGBX4444. */
static uint16_t h3531_from_rgbx4444(uint16_t p)
{
   unsigned r4 = (p >> 12) & 0x0fU;
   unsigned g4 = (p >> 8)  & 0x0fU;
   unsigned b4 = (p >> 4)  & 0x0fU;
   return h3531_pack_a1r5g5b5(r4 * 17U, g4 * 17U, b4 * 17U);
}

static uint16_t h3531_from_xrgb8888(uint32_t p)
{
   unsigned r = (p >> 16) & 0xffU;
   unsigned g = (p >> 8)  & 0xffU;
   unsigned b = p & 0xffU;
   return h3531_pack_a1r5g5b5(r, g, b);
}

static void h3531_clear(h3531_fb_t *h)
{
   unsigned y;
   if (!h || !h->mem)
      return;

   for (y = 0; y < h->screen_h; ++y)
      memset(h->mem + (size_t)y * h->stride, 0, h->stride);
}

static bool h3531_format_is_expected(const h3531_fb_t *h)
{
   return h->var.bits_per_pixel == 16
      && h->var.red.offset   == 10 && h->var.red.length   == 5
      && h->var.green.offset == 5  && h->var.green.length == 5
      && h->var.blue.offset  == 0  && h->var.blue.length  == 5
      && h->var.transp.offset == 15 && h->var.transp.length == 1;
}

static void h3531_calc_rect(h3531_fb_t *h, unsigned src_w, unsigned src_h,
      unsigned *dx, unsigned *dy, unsigned *dw, unsigned *dh)
{
   uint64_t lhs;
   uint64_t rhs;

   *dx = *dy = 0;
   *dw = h->screen_w;
   *dh = h->screen_h;

   if (!src_w || !src_h || !h->screen_w || !h->screen_h)
      return;

   lhs = (uint64_t)h->screen_w * (uint64_t)src_h;
   rhs = (uint64_t)h->screen_h * (uint64_t)src_w;

   if (lhs > rhs)
      *dw = (unsigned)(((uint64_t)h->screen_h * src_w) / src_h);
   else if (lhs < rhs)
      *dh = (unsigned)(((uint64_t)h->screen_w * src_h) / src_w);

   if (!*dw) *dw = 1;
   if (!*dh) *dh = 1;

   *dx = (h->screen_w - *dw) / 2U;
   *dy = (h->screen_h - *dh) / 2U;
}

static void h3531_present(h3531_fb_t *h, const void *src,
      unsigned src_w, unsigned src_h, unsigned src_pitch,
      unsigned bits, bool menu_texture)
{
   unsigned dx, dy, dw, dh;
   unsigned y;

   if (!h || !h->mem || !src || !src_w || !src_h || !src_pitch)
      return;

   h3531_calc_rect(h, src_w, src_h, &dx, &dy, &dw, &dh);

   if (dx != h->last_x || dy != h->last_y ||
       dw != h->last_w || dh != h->last_h)
   {
      h3531_clear(h);
      h->last_x = dx;
      h->last_y = dy;
      h->last_w = dw;
      h->last_h = dh;
   }

   for (y = 0; y < dh; ++y)
   {
      unsigned x;
      unsigned sy = (unsigned)(((uint64_t)y * src_h) / dh);
      const uint8_t *srow = (const uint8_t*)src + (size_t)sy * src_pitch;
      uint16_t *drow = (uint16_t*)(h->mem + (size_t)(dy + y) * h->stride) + dx;

      if (bits == 16)
      {
         const uint16_t *s16 = (const uint16_t*)srow;
         for (x = 0; x < dw; ++x)
         {
            unsigned sx = (unsigned)(((uint64_t)x * src_w) / dw);
            uint16_t p = s16[sx];
            drow[x] = menu_texture
               ? h3531_from_rgbx4444(p)
               : h3531_from_rgb565(p);
         }
      }
      else if (bits == 32)
      {
         const uint32_t *s32 = (const uint32_t*)srow;
         for (x = 0; x < dw; ++x)
         {
            unsigned sx = (unsigned)(((uint64_t)x * src_w) / dw);
            drow[x] = h3531_from_xrgb8888(s32[sx]);
         }
      }
   }
}

static void *h3531_init(const video_info_t *video,
      const input_driver_t **input, void **input_data)
{
   const char *fb_path = getenv("RETROARCH_FB");
   h3531_fb_t *h = (h3531_fb_t*)calloc(1, sizeof(*h));
   size_t fallback_len;

   if (!h)
      return NULL;

   h->fd = -1;
   *input = NULL;
   *input_data = NULL;

   if (!fb_path || !fb_path[0])
      fb_path = H3531_DEFAULT_FB;

   h->fd = open(fb_path, O_RDWR);
   if (h->fd < 0)
   {
      RARCH_ERR("[H3531] cannot open %s: %s\n", fb_path, strerror(errno));
      free(h);
      return NULL;
   }

   if (ioctl(h->fd, FBIOGET_FSCREENINFO, &h->fix) < 0 ||
       ioctl(h->fd, FBIOGET_VSCREENINFO, &h->var) < 0)
   {
      RARCH_ERR("[H3531] framebuffer info ioctl failed: %s\n", strerror(errno));
      close(h->fd);
      free(h);
      return NULL;
   }

   h->screen_w = h->var.xres;
   h->screen_h = h->var.yres;
   h->stride   = h->fix.line_length;

   fallback_len = (size_t)h->stride *
      (size_t)(h->var.yres_virtual ? h->var.yres_virtual : h->var.yres);
   h->mem_len = h->fix.smem_len ? (size_t)h->fix.smem_len : fallback_len;

   if (h->var.bits_per_pixel != 16 || !h->screen_w || !h->screen_h ||
       h->stride < h->screen_w * 2U || !h->mem_len)
   {
      RARCH_ERR("[H3531] unsupported framebuffer: %ux%u %ubpp stride=%u len=%lu\n",
            h->screen_w, h->screen_h, h->var.bits_per_pixel, h->stride,
            (unsigned long)h->mem_len);
      close(h->fd);
      free(h);
      return NULL;
   }

   h->mem = (uint8_t*)mmap(NULL, h->mem_len,
         PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
   if (h->mem == MAP_FAILED)
   {
      RARCH_ERR("[H3531] mmap(%s) failed: %s\n", fb_path, strerror(errno));
      h->mem = NULL;
      close(h->fd);
      free(h);
      return NULL;
   }

   h->frame_width  = video->width;
   h->frame_height = video->height;
   h->frame_bits   = video->rgb32 ? 32U : 16U;
   h->frame_pitch  = video->width * (video->rgb32 ? 4U : 2U);

   h3531_clear(h);

   RARCH_LOG("[H3531] framebuffer ready: %s %ux%u %ubpp stride=%u len=%lu\n",
         fb_path, h->screen_w, h->screen_h, h->var.bits_per_pixel,
         h->stride, (unsigned long)h->mem_len);
   RARCH_LOG("[H3531] bitfields R%u/%u G%u/%u B%u/%u A%u/%u\n",
         h->var.red.offset, h->var.red.length,
         h->var.green.offset, h->var.green.length,
         h->var.blue.offset, h->var.blue.length,
         h->var.transp.offset, h->var.transp.length);

   if (!h3531_format_is_expected(h))
      RARCH_WARN("[H3531] framebuffer bitfields differ from proven A1R5G5B5 layout; PoC conversion still assumes A1R5G5B5\n");

   return h;
}

static bool h3531_frame(void *data, const void *frame,
      unsigned frame_width, unsigned frame_height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   h3531_fb_t *h = (h3531_fb_t*)data;
   const void *src = frame;
   unsigned width = frame_width;
   unsigned height = frame_height;
   unsigned src_pitch = pitch;
   unsigned bits = h ? h->frame_bits : 16U;
   bool menu_texture = false;
#ifdef HAVE_MENU
   bool menu_is_alive = video_info &&
      (video_info->menu_st_flags & MENU_ST_FLAG_ALIVE);
#else
   bool menu_is_alive = false;
#endif

   (void)frame_count;
   (void)msg;

   if (!h)
      return false;

#ifdef HAVE_MENU
   menu_driver_frame(menu_is_alive, video_info);

   if (h->menu_frame && menu_is_alive)
   {
      src          = h->menu_frame;
      width        = h->menu_width;
      height       = h->menu_height;
      src_pitch    = h->menu_pitch;
      bits         = h->menu_bits;
      menu_texture = true;
   }
   else
#endif
   {
      if (frame_width > 4 && frame_height > 4)
      {
         h->frame_width  = frame_width;
         h->frame_height = frame_height;
         h->frame_pitch  = pitch;
      }

      width     = h->frame_width;
      height    = h->frame_height;
      src_pitch = h->frame_pitch;

      if (menu_is_alive && frame_width <= 4 && frame_height <= 4)
         return true;
   }

   if (src && width && height && src_pitch)
      h3531_present(h, src, width, height, src_pitch, bits, menu_texture);

   return true;
}

static void h3531_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   (void)data;
   (void)toggle;
   (void)adaptive_vsync_enabled;
   (void)swap_interval;
}

static bool h3531_alive(void *data) { (void)data; return true; }
static bool h3531_focus(void *data) { (void)data; return true; }
static bool h3531_suppress_screensaver(void *data, bool enable)
{ (void)data; (void)enable; return false; }
static bool h3531_has_windowed(void *data) { (void)data; return false; }
static bool h3531_set_shader(void *data, enum rarch_shader_type type,
      const char *path)
{ (void)data; (void)type; (void)path; return false; }
static void h3531_set_rotation(void *data, unsigned rotation)
{ (void)data; (void)rotation; }
static void h3531_set_viewport(void *data, unsigned vp_width,
      unsigned vp_height, bool force_full, bool allow_rotate)
{ (void)data; (void)vp_width; (void)vp_height; (void)force_full; (void)allow_rotate; }

static void h3531_free(void *data)
{
   h3531_fb_t *h = (h3531_fb_t*)data;
   if (!h)
      return;

   h3531_clear(h);

   if (h->mem)
      munmap(h->mem, h->mem_len);
   if (h->fd >= 0)
      close(h->fd);
   free(h->menu_frame);
   free(h);
}

static void h3531_set_texture_frame(void *data,
      const void *frame, bool rgb32, unsigned width, unsigned height,
      float alpha)
{
   h3531_fb_t *h = (h3531_fb_t*)data;
   unsigned pitch;
   size_t required;
   unsigned char *tmp;

   (void)alpha;

   if (!h || !frame || !width || !height)
      return;

   pitch = width * (rgb32 ? 4U : 2U);
   required = (size_t)pitch * height;

   if (required > h->menu_frame_cap)
   {
      tmp = (unsigned char*)realloc(h->menu_frame, required);
      if (!tmp)
         return;
      h->menu_frame = tmp;
      h->menu_frame_cap = required;
   }

   memcpy(h->menu_frame, frame, required);
   h->menu_width  = width;
   h->menu_height = height;
   h->menu_pitch  = pitch;
   h->menu_bits   = rgb32 ? 32U : 16U;
}

static void h3531_set_osd_msg(void *data, const char *msg, size_t msg_len,
      const struct font_params *params, void *font)
{ (void)data; (void)msg; (void)msg_len; (void)params; (void)font; }

static void h3531_get_video_output_size(void *data,
      unsigned *width, unsigned *height, char *desc, size_t desc_len)
{
   h3531_fb_t *h = (h3531_fb_t*)data;
   if (width)  *width  = h ? h->screen_w : 0;
   if (height) *height = h ? h->screen_h : 0;
   if (desc && desc_len)
      snprintf(desc, desc_len, "H3531 HIFB");
}

static void h3531_get_video_output_prev(void *data) { (void)data; }
static void h3531_get_video_output_next(void *data) { (void)data; }
static void h3531_set_video_mode(void *data, unsigned width, unsigned height,
      bool fullscreen)
{ (void)data; (void)width; (void)height; (void)fullscreen; }

static const video_poke_interface_t h3531_poke_interface = {
   NULL,
   NULL,
   NULL,
   h3531_set_video_mode,
   NULL,
   NULL,
   h3531_get_video_output_size,
   h3531_get_video_output_prev,
   h3531_get_video_output_next,
   NULL,
   NULL,
   NULL,
   NULL,
#ifdef HAVE_MENU
   h3531_set_texture_frame,
   NULL,
   h3531_set_osd_msg,
   NULL,
#else
   NULL,
   NULL,
   NULL,
   NULL,
#endif
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL
};

static void h3531_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &h3531_poke_interface;
}

/* Kept as video_fpga so the first PoC can reuse upstream's existing
 * HAVE_FPGA registration slot.  The public driver name is "h3531". */
video_driver_t video_fpga = {
   h3531_init,
   h3531_frame,
   h3531_set_nonblock_state,
   h3531_alive,
   h3531_focus,
   h3531_suppress_screensaver,
   h3531_has_windowed,
   h3531_set_shader,
   h3531_free,
   "h3531",
   h3531_set_viewport,
   h3531_set_rotation,
   NULL,
   NULL,
   NULL,
#ifdef HAVE_OVERLAY
   NULL,
#endif
   h3531_get_poke_interface,
   NULL,
   NULL,
   NULL,
#ifdef HAVE_GFX_WIDGETS
   NULL
#endif
};
