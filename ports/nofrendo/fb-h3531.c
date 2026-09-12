#include "fb.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define H3531_NES_CROP_RIGHT 8

/*
 * Stock Hi3531 hifb.ko does not implement Linux FBIO_WAITFORVSYNC.
 * Its own FBIOGET_VBLANK_HIFB command is _IO('F', 100) == 0x4664.
 * Static inspection of the supplied hifb.ko shows command 0x4664 reaching
 * hifb_wait_regconfig_work(), matching the vendor vblank path.
 */
#define H3531_FBIOGET_VBLANK_HIFB 0x00004664UL

static int fbfd = -1;
static uint8_t *fbp = NULL;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static size_t fb_len = 0;
static int line_length = 0;
static uint16_t *shadow_frame = NULL;
static size_t shadow_pixels = 0;
static int hifb_vblank_state = 0; /* 0 unknown, 1 works, -1 unavailable */

extern uint16_t myPalette[256];

static uint16_t *visible_row(int y)
{
    size_t off;
    if (!fbp || y < 0 || y >= (int)vinfo.yres)
        return NULL;
    off = (size_t)(vinfo.yoffset + (unsigned)y) * (size_t)line_length +
          (size_t)vinfo.xoffset * 2U;
    if (off + (size_t)vinfo.xres * 2U > fb_len)
        return NULL;
    return (uint16_t *)(fbp + off);
}

static void fb_wait_hifb_vblank(void)
{
    int rc;

    if (hifb_vblank_state < 0 || fbfd < 0)
        return;

    errno = 0;
    rc = ioctl(fbfd, H3531_FBIOGET_VBLANK_HIFB, 0);
    if (rc == 0) {
        if (hifb_vblank_state == 0)
            printf("H3531 NES video v7: native HIFB vblank 0x4664 active\n");
        hifb_vblank_state = 1;
    } else {
        if (hifb_vblank_state == 0)
            printf("H3531 NES video v7: native HIFB vblank unavailable rc=%d errno=%d; fast-copy fallback\n",
                   rc, errno);
        hifb_vblank_state = -1;
    }
}

void fb_init(void)
{
    const char *dev = getenv("FRAMEBUFFER");
    if (!dev || !dev[0])
        dev = "/dev/fb0";

    fbfd = open(dev, O_RDWR);
    if (fbfd < 0) {
        fprintf(stderr, "H3531 NES: cannot open %s: %s\n", dev, strerror(errno));
        exit(1);
    }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
        ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        fprintf(stderr, "H3531 NES: framebuffer ioctl failed: %s\n", strerror(errno));
        exit(2);
    }
    if (vinfo.bits_per_pixel != 16) {
        fprintf(stderr, "H3531 NES: expected 16bpp framebuffer, got %u\n",
                vinfo.bits_per_pixel);
        exit(3);
    }

    fb_len = (size_t)finfo.smem_len;
    line_length = (int)finfo.line_length;
    fbp = (uint8_t *)mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp == MAP_FAILED) {
        fbp = NULL;
        fprintf(stderr, "H3531 NES: framebuffer mmap failed: %s\n", strerror(errno));
        exit(4);
    }

    shadow_pixels = (size_t)vinfo.xres * (size_t)vinfo.yres;
    shadow_frame = (uint16_t *)malloc(shadow_pixels * sizeof(uint16_t));
    if (shadow_frame)
        printf("H3531 NES video v7: shadow compositor ready (%lu bytes)\n",
               (unsigned long)(shadow_pixels * sizeof(uint16_t)));
    else
        fprintf(stderr, "H3531 NES video v7: shadow allocation failed; direct-VRAM fallback\n");

    printf("H3531 NES video: %ux%u virtual=%ux%u off=%u,%u stride=%d bpp=%u smem=%lu\n",
           vinfo.xres, vinfo.yres, vinfo.xres_virtual, vinfo.yres_virtual,
           vinfo.xoffset, vinfo.yoffset, line_length, vinfo.bits_per_pixel,
           (unsigned long)fb_len);
    printf("H3531 NES pixel format: R%u@%u G%u@%u B%u@%u A%u@%u\n",
           vinfo.red.length, vinfo.red.offset,
           vinfo.green.length, vinfo.green.offset,
           vinfo.blue.length, vinfo.blue.offset,
           vinfo.transp.length, vinfo.transp.offset);
    printf("H3531 NES video v7: crop right=%d source pixels before scaling\n",
           H3531_NES_CROP_RIGHT);
}

void fb_cleanup(void)
{
    if (shadow_frame) {
        free(shadow_frame);
        shadow_frame = NULL;
        shadow_pixels = 0;
    }
    if (fbp) {
        munmap(fbp, fb_len);
        fbp = NULL;
    }
    if (fbfd >= 0) {
        close(fbfd);
        fbfd = -1;
    }
}

void fb_clear(void)
{
    int y, x;
    for (y = 0; y < (int)vinfo.yres; ++y) {
        uint16_t *dst = visible_row(y);
        if (!dst)
            continue;
        for (x = 0; x < (int)vinfo.xres; ++x)
            dst[x] = 0x8000U;
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static void fb_blit_direct(int x0, int y0, int visible_width, int height,
                           int scale, uint8_t **lines)
{
    int sy, sx, vy;
    const int out_w = visible_width * scale;

    for (sy = 0; sy < height; ++sy) {
        const uint8_t *src = lines[sy];
        uint16_t *first = visible_row(y0 + sy * scale);
        if (!src || !first)
            continue;
        first += x0;

        if (scale == 3) {
            for (sx = 0; sx < visible_width; ++sx) {
                const uint16_t c = myPalette[src[sx]];
                const int dx = sx * 3;
                first[dx] = c;
                first[dx + 1] = c;
                first[dx + 2] = c;
            }
        } else if (scale == 2) {
            for (sx = 0; sx < visible_width; ++sx) {
                const uint16_t c = myPalette[src[sx]];
                const int dx = sx * 2;
                first[dx] = c;
                first[dx + 1] = c;
            }
        } else {
            for (sx = 0; sx < visible_width; ++sx)
                first[sx] = myPalette[src[sx]];
        }

        for (vy = 1; vy < scale; ++vy) {
            uint16_t *copy = visible_row(y0 + sy * scale + vy);
            if (copy)
                memcpy(copy + x0, first, (size_t)out_w * sizeof(uint16_t));
        }
    }
}

void fb_blit_lines(int ignored_x, int ignored_y, int width, int height, uint8_t **lines)
{
    int scale = 3;
    int visible_width;
    int out_w, out_h, x0, y0;
    int sy, sx, vy, oy;
    (void)ignored_x;
    (void)ignored_y;

    if (!fbp || !lines || width <= 0 || height <= 0)
        return;

    visible_width = width;
    if (width > H3531_NES_CROP_RIGHT + 16)
        visible_width = width - H3531_NES_CROP_RIGHT;

    while (scale > 1 &&
           (visible_width * scale > (int)vinfo.xres || height * scale > (int)vinfo.yres))
        --scale;

    out_w = visible_width * scale;
    out_h = height * scale;
    x0 = ((int)vinfo.xres - out_w) / 2;
    y0 = ((int)vinfo.yres - out_h) / 2;

    if (!shadow_frame || (size_t)out_w * (size_t)out_h > shadow_pixels) {
        fb_blit_direct(x0, y0, visible_width, height, scale, lines);
#if defined(__arm__)
        __asm__ volatile("dmb" ::: "memory");
#endif
        return;
    }

    /* Complete palette conversion/scaling in normal RAM before scanout. */
    for (sy = 0; sy < height; ++sy) {
        const uint8_t *src = lines[sy];
        uint16_t *first = shadow_frame + (size_t)(sy * scale) * (size_t)out_w;
        if (!src)
            continue;

        if (scale == 3) {
            for (sx = 0; sx < visible_width; ++sx) {
                const uint16_t c = myPalette[src[sx]];
                const int dx = sx * 3;
                first[dx] = c;
                first[dx + 1] = c;
                first[dx + 2] = c;
            }
        } else if (scale == 2) {
            for (sx = 0; sx < visible_width; ++sx) {
                const uint16_t c = myPalette[src[sx]];
                const int dx = sx * 2;
                first[dx] = c;
                first[dx + 1] = c;
            }
        } else {
            for (sx = 0; sx < visible_width; ++sx)
                first[sx] = myPalette[src[sx]];
        }

        for (vy = 1; vy < scale; ++vy)
            memcpy(first + (size_t)vy * (size_t)out_w,
                   first, (size_t)out_w * sizeof(uint16_t));
    }

    /* Use the native HiSilicon HIFB vblank primitive before the short VRAM copy. */
    fb_wait_hifb_vblank();
    for (oy = 0; oy < out_h; ++oy) {
        uint16_t *dst = visible_row(y0 + oy);
        if (dst)
            memcpy(dst + x0,
                   shadow_frame + (size_t)oy * (size_t)out_w,
                   (size_t)out_w * sizeof(uint16_t));
    }

#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}
