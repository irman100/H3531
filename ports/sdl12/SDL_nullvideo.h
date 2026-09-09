/* H3531 SDL 1.2 backend patch.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "SDL_config.h"
#ifndef _SDL_nullvideo_h
#define _SDL_nullvideo_h
#include "../SDL_sysvideo.h"
#define _THIS SDL_VideoDevice *this
struct SDL_PrivateVideoData {
    int w, h;
    void *buffer;
    int fb_fd;
    void *fb_mem;
    unsigned long fb_len;
    int fb_w, fb_h, fb_pitch;
    int fb_bpp;
};
#endif
