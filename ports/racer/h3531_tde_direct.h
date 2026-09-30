#ifndef H3531_TDE_DIRECT_H
#define H3531_TDE_DIRECT_H

/*
 * Minimal Hi3531 TDE userspace ABI used by Racer.
 *
 * This is intentionally not a copy of the vendor SDK header.  It contains
 * only the fixed-width layout required for full-frame ARGB1555 resize and the
 * ioctl request values verified against the Hi3531 userspace wrapper ABI.
 */
#include <stdint.h>
#include <sys/ioctl.h>

enum {
    H3531_TDE_COLOR_FMT_ARGB1555 = 12
};

typedef struct {
    uint32_t phy_addr;
    int32_t color_fmt;
    uint32_t height;
    uint32_t width;
    uint32_t stride;
    uint32_t clut_phy_addr;
    int32_t ycbcr_clut;
    int32_t alpha_max_255;
    int32_t alpha_ext_1555;
    uint8_t alpha0;
    uint8_t alpha1;
    uint16_t reserved_align;
    uint32_t cbcr_phy_addr;
    uint32_t cbcr_stride;
} h3531_tde_surface_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} h3531_tde_rect_t;

typedef struct {
    int32_t handle;
    h3531_tde_surface_t src;
    h3531_tde_rect_t src_rect;
    h3531_tde_surface_t dst;
    h3531_tde_rect_t dst_rect;
} h3531_tde_resize_cmd_t;

typedef struct {
    int32_t handle;
    int32_t sync;
    int32_t block;
    uint32_t timeout_10ms;
} h3531_tde_end_cmd_t;

/*
 * Exact Hi3531 TDE requests recovered from the Hi3531 userspace ABI.
 * ioctl type 0x74 ('t'):
 *   1  begin job, 5 resize, 9 submit/end job.
 */
#define H3531_TDE_IOC_BEGIN_JOB   0x40047401UL
#define H3531_TDE_IOC_RESIZE      0x40847405UL
#define H3531_TDE_IOC_END_JOB     0x40107409UL

_Static_assert(sizeof(h3531_tde_surface_t) == 48, "Hi3531 TDE surface ABI");
_Static_assert(sizeof(h3531_tde_rect_t) == 16, "Hi3531 TDE rect ABI");
_Static_assert(sizeof(h3531_tde_resize_cmd_t) == 132, "Hi3531 TDE resize ABI");
_Static_assert(sizeof(h3531_tde_end_cmd_t) == 16, "Hi3531 TDE end ABI");

#endif
