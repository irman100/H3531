/* H3531 SDL 1.2 backend patch.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Reuses SDL 1.2's dummy-driver slot as a fixed-fbdev backend for the
 * HiSilicon Hi3531 board. The application keeps a 32-bit logical surface;
 * dirty rectangles are scaled to the board's fixed 16-bit A1R5G5B5 fb0.
 */
#include "SDL_config.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "SDL_nullvideo.h"
#include "SDL_nullevents_c.h"
#include "SDL_nullmouse_c.h"

#define DUMMYVID_DRIVER_NAME "h3531"
static int DUMMY_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **DUMMY_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *DUMMY_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int DUMMY_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void DUMMY_VideoQuit(_THIS);
static int DUMMY_AllocHWSurface(_THIS, SDL_Surface *surface);
static int DUMMY_LockHWSurface(_THIS, SDL_Surface *surface);
static void DUMMY_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void DUMMY_FreeHWSurface(_THIS, SDL_Surface *surface);
static void DUMMY_UpdateRects(_THIS, int numrects, SDL_Rect *rects);

static int DUMMY_Available(void) {
    const char *e = SDL_getenv("SDL_VIDEODRIVER");
    return (e && SDL_strcmp(e, DUMMYVID_DRIVER_NAME) == 0) ? 1 : 0;
}

static void DUMMY_DeleteDevice(SDL_VideoDevice *device) {
    SDL_free(device->hidden);
    SDL_free(device);
}

static SDL_VideoDevice *DUMMY_CreateDevice(int devindex) {
    SDL_VideoDevice *d=(SDL_VideoDevice*)SDL_malloc(sizeof(SDL_VideoDevice));
    if(d) {
        SDL_memset(d,0,sizeof(*d));
        d->hidden=(struct SDL_PrivateVideoData*)SDL_malloc(sizeof(*d->hidden));
    }
    if(!d || !d->hidden) {
        if(d) SDL_free(d);
        SDL_OutOfMemory();
        return 0;
    }
    SDL_memset(d->hidden,0,sizeof(*d->hidden));
    d->hidden->fb_fd=-1;
    d->VideoInit=DUMMY_VideoInit;
    d->ListModes=DUMMY_ListModes;
    d->SetVideoMode=DUMMY_SetVideoMode;
    d->CreateYUVOverlay=NULL;
    d->SetColors=DUMMY_SetColors;
    d->UpdateRects=DUMMY_UpdateRects;
    d->VideoQuit=DUMMY_VideoQuit;
    d->AllocHWSurface=DUMMY_AllocHWSurface;
    d->CheckHWBlit=NULL;
    d->FillHWRect=NULL;
    d->SetHWColorKey=NULL;
    d->SetHWAlpha=NULL;
    d->LockHWSurface=DUMMY_LockHWSurface;
    d->UnlockHWSurface=DUMMY_UnlockHWSurface;
    d->FlipHWSurface=NULL;
    d->FreeHWSurface=DUMMY_FreeHWSurface;
    d->SetCaption=NULL;
    d->SetIcon=NULL;
    d->IconifyWindow=NULL;
    d->GrabInput=NULL;
    d->GetWMInfo=NULL;
    d->InitOSKeymap=DUMMY_InitOSKeymap;
    d->PumpEvents=DUMMY_PumpEvents;
    d->free=DUMMY_DeleteDevice;
    return d;
}

VideoBootStrap DUMMY_bootstrap = {
    DUMMYVID_DRIVER_NAME,
    "H3531 fixed framebuffer video driver",
    DUMMY_Available,
    DUMMY_CreateDevice
};

static void clear_fb(_THIS) {
    int x,y;
    Uint16 *base;
    int stride;
    if(!this->hidden->fb_mem) return;
    base=(Uint16*)this->hidden->fb_mem;
    stride=this->hidden->fb_pitch/2;
    /* HIFB uses the top bit as alpha in A1R5G5B5. 0x0000 is transparent,
       so clear to opaque black (0x8000), not memset(0). */
    for(y=0; y<this->hidden->fb_h; ++y) {
        Uint16 *d=base+y*stride;
        for(x=0; x<this->hidden->fb_w; ++x) d[x]=0x8000u;
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static int DUMMY_VideoInit(_THIS, SDL_PixelFormat *vformat) {
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    const char *dev=SDL_getenv("SDL_FBDEV");
    if(!dev) dev="/dev/fb0";
    this->hidden->fb_fd=open(dev,O_RDWR,0);
    if(this->hidden->fb_fd<0) {
        SDL_SetError("H3531: cannot open %s",dev);
        return -1;
    }
    if(ioctl(this->hidden->fb_fd,FBIOGET_VSCREENINFO,&v)<0 ||
       ioctl(this->hidden->fb_fd,FBIOGET_FSCREENINFO,&f)<0) {
        SDL_SetError("H3531: framebuffer ioctl failed");
        return -1;
    }
    if(v.bits_per_pixel!=16) {
        SDL_SetError("H3531: expected 16bpp fb, got %u",v.bits_per_pixel);
        return -1;
    }
    this->hidden->fb_w=v.xres;
    this->hidden->fb_h=v.yres;
    this->hidden->fb_pitch=f.line_length;
    this->hidden->fb_bpp=v.bits_per_pixel;
    this->hidden->fb_len=f.smem_len;
    this->hidden->fb_mem=mmap(NULL,this->hidden->fb_len,PROT_READ|PROT_WRITE,MAP_SHARED,this->hidden->fb_fd,0);
    if(this->hidden->fb_mem==MAP_FAILED) {
        this->hidden->fb_mem=NULL;
        SDL_SetError("H3531: framebuffer mmap failed");
        return -1;
    }
    vformat->BitsPerPixel=32;
    vformat->BytesPerPixel=4;
    this->info.current_w=this->hidden->fb_w;
    this->info.current_h=this->hidden->fb_h;
    this->info.wm_available=0;
    this->info.hw_available=0;
    clear_fb(this);
    return 0;
}

static SDL_Rect **DUMMY_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags) {
    (void)this; (void)format; (void)flags;
    return (SDL_Rect**)-1;
}

static SDL_Surface *DUMMY_SetVideoMode(_THIS, SDL_Surface *current,
        int width, int height, int bpp, Uint32 flags) {
    size_t bytes;
    (void)bpp;
    if(this->hidden->buffer) {
        SDL_free(this->hidden->buffer);
        this->hidden->buffer=NULL;
    }
    bytes=(size_t)width*(size_t)height*4u;
    this->hidden->buffer=SDL_malloc(bytes);
    if(!this->hidden->buffer) {
        SDL_SetError("H3531: logical framebuffer allocation failed");
        return NULL;
    }
    SDL_memset(this->hidden->buffer,0,bytes);
    if(!SDL_ReallocFormat(current,32,0x00FF0000,0x0000FF00,0x000000FF,0)) {
        SDL_free(this->hidden->buffer);
        this->hidden->buffer=NULL;
        return NULL;
    }
    /* Pixels are owned and freed by this backend in VideoQuit(). */
    current->flags=(flags&SDL_FULLSCREEN)|SDL_SWSURFACE|SDL_PREALLOC;
    this->hidden->w=current->w=width;
    this->hidden->h=current->h=height;
    current->pitch=width*4;
    current->pixels=this->hidden->buffer;
    clear_fb(this);
    return current;
}

static Uint16 rgb1555(Uint32 p) {
    Uint32 r=(p>>16)&255u, g=(p>>8)&255u, b=p&255u;
    return (Uint16)(0x8000u|((r>>3)<<10)|((g>>3)<<5)|(b>>3));
}

static void DUMMY_UpdateRects(_THIS, int numrects, SDL_Rect *rects) {
    int n;
    int sw=this->hidden->w, sh=this->hidden->h;
    int pw=this->hidden->fb_w, ph=this->hidden->fb_h;
    Uint32 *src=(Uint32*)this->hidden->buffer;
    Uint16 *fb=(Uint16*)this->hidden->fb_mem;
    int stride=this->hidden->fb_pitch/2;
    int outw,outh,ox,oy;
    if(!src || !fb || sw<=0 || sh<=0) return;

    if((long long)pw*sh <= (long long)ph*sw) {
        outw=pw;
        outh=(int)((long long)pw*sh/sw);
    } else {
        outh=ph;
        outw=(int)((long long)ph*sw/sh);
    }
    if(outw<1) outw=1;
    if(outh<1) outh=1;
    ox=(pw-outw)/2;
    oy=(ph-outh)/2;

    for(n=0; n<numrects; ++n) {
        int sx0=rects[n].x, sy0=rects[n].y;
        int sx1=sx0+rects[n].w, sy1=sy0+rects[n].h;
        int dx0,dy0,dx1,dy1,x,y;
        if(sx0<0) sx0=0;
        if(sy0<0) sy0=0;
        if(sx1>sw) sx1=sw;
        if(sy1>sh) sy1=sh;
        if(sx1<=sx0 || sy1<=sy0) continue;
        dx0=ox+(int)((long long)sx0*outw/sw);
        dy0=oy+(int)((long long)sy0*outh/sh);
        dx1=ox+(int)(((long long)sx1*outw+sw-1)/sw);
        dy1=oy+(int)(((long long)sy1*outh+sh-1)/sh);
        if(dx0<ox) dx0=ox;
        if(dy0<oy) dy0=oy;
        if(dx1>ox+outw) dx1=ox+outw;
        if(dy1>oy+outh) dy1=oy+outh;
        for(y=dy0; y<dy1; ++y) {
            int sy=(int)((long long)(y-oy)*sh/outh);
            Uint16 *d=fb+y*stride;
            for(x=dx0; x<dx1; ++x) {
                int sx=(int)((long long)(x-ox)*sw/outw);
                d[x]=rgb1555(src[sy*sw+sx]);
            }
        }
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static int DUMMY_AllocHWSurface(_THIS, SDL_Surface *s) {(void)this;(void)s;return -1;}
static void DUMMY_FreeHWSurface(_THIS, SDL_Surface *s) {(void)this;(void)s;}
static int DUMMY_LockHWSurface(_THIS, SDL_Surface *s) {(void)this;(void)s;return 0;}
static void DUMMY_UnlockHWSurface(_THIS, SDL_Surface *s) {(void)this;(void)s;}
static int DUMMY_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors) {(void)this;(void)firstcolor;(void)ncolors;(void)colors;return 1;}

static void DUMMY_VideoQuit(_THIS) {
    if(this->hidden->buffer) {
        SDL_free(this->hidden->buffer);
        this->hidden->buffer=NULL;
    }
    if(this->hidden->fb_mem) {
        munmap(this->hidden->fb_mem,this->hidden->fb_len);
        this->hidden->fb_mem=NULL;
    }
    if(this->hidden->fb_fd>=0) {
        close(this->hidden->fb_fd);
        this->hidden->fb_fd=-1;
    }
}
