/* H3531 SDL 1.2 backend patch.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fixed-fbdev backend for HiSilicon Hi3531.
 * Normal applications keep the existing 32-bit logical surface. FBZX can
 * explicitly request a native A1R5G5B5 16-bit logical surface with
 * H3531_FBZX_FAST16=1 so the hot flip path does not reconvert every pixel.
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

static int fast16_requested(void) {
    const char *e=SDL_getenv("H3531_FBZX_FAST16");
    return e && e[0]=='1' && e[1]=='\0';
}

static int DUMMY_Available(void) {
    const char *e = SDL_getenv("SDL_VIDEODRIVER");
    return (e && SDL_strcmp(e, DUMMYVID_DRIVER_NAME) == 0) ? 1 : 0;
}

static void free_scale_tables(_THIS) {
    if(this->hidden->xmap) { SDL_free(this->hidden->xmap); this->hidden->xmap=NULL; }
    if(this->hidden->ymap) { SDL_free(this->hidden->ymap); this->hidden->ymap=NULL; }
}

static void free_shadow_buffers(_THIS) {
    if(this->hidden->buffer) {
        SDL_free(this->hidden->buffer);
        this->hidden->buffer=NULL;
    }
    if(this->hidden->prev_buffer) {
        SDL_free(this->hidden->prev_buffer);
        this->hidden->prev_buffer=NULL;
    }
    this->hidden->prev_valid=0;
}

static void DUMMY_DeleteDevice(SDL_VideoDevice *device) {
    SDL_free(device->hidden);
    SDL_free(device);
}

static SDL_VideoDevice *DUMMY_CreateDevice(int devindex) {
    SDL_VideoDevice *d=(SDL_VideoDevice*)SDL_malloc(sizeof(SDL_VideoDevice));
    (void)devindex;
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

static Uint16 *visible_base(_THIS) {
    Uint16 *base=(Uint16*)this->hidden->fb_mem;
    int stride=this->hidden->fb_pitch/2;
    if(!base) return NULL;
    return base + this->hidden->fb_yoff*stride + this->hidden->fb_xoff;
}

static void clear_fb(_THIS) {
    int x,y;
    Uint16 *base=visible_base(this);
    int stride=this->hidden->fb_pitch/2;
    if(!base) return;
    /* HIFB A1R5G5B5: 0x8000 is opaque black. Clear the ACTIVE viewport,
       not blindly page zero: vendor HIFB may expose a panned virtual surface. */
    for(y=0; y<this->hidden->fb_h; ++y) {
        Uint16 *d=base+y*stride;
        for(x=0; x<this->hidden->fb_w; ++x) d[x]=0x8000u;
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static int build_scale_tables(_THIS) {
    int i;
    int sw=this->hidden->w, sh=this->hidden->h;
    int pw=this->hidden->fb_w, ph=this->hidden->fb_h;
    int outw,outh;

    if((long long)pw*sh <= (long long)ph*sw) {
        outw=pw;
        outh=(int)((long long)pw*sh/sw);
    } else {
        outh=ph;
        outw=(int)((long long)ph*sw/sh);
    }
    if(outw<1) outw=1;
    if(outh<1) outh=1;

    this->hidden->outw=outw;
    this->hidden->outh=outh;
    this->hidden->ox=(pw-outw)/2;
    this->hidden->oy=(ph-outh)/2;

    free_scale_tables(this);
    this->hidden->xmap=(int*)SDL_malloc((size_t)outw*sizeof(int));
    this->hidden->ymap=(int*)SDL_malloc((size_t)outh*sizeof(int));
    if(!this->hidden->xmap || !this->hidden->ymap) {
        free_scale_tables(this);
        SDL_SetError("H3531: scaler table allocation failed");
        return -1;
    }

    for(i=0;i<outw;i++) this->hidden->xmap[i]=(int)((long long)i*sw/outw);
    for(i=0;i<outh;i++) this->hidden->ymap[i]=(int)((long long)i*sh/outh);
    return 0;
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
    this->hidden->fb_xoff=v.xoffset;
    this->hidden->fb_yoff=v.yoffset;
    this->hidden->fb_pitch=f.line_length;
    this->hidden->fb_bpp=v.bits_per_pixel;
    this->hidden->fb_len=f.smem_len;
    if((unsigned long)(this->hidden->fb_yoff+this->hidden->fb_h)*(unsigned long)this->hidden->fb_pitch > this->hidden->fb_len) {
        SDL_SetError("H3531: visible framebuffer viewport exceeds mapped memory");
        return -1;
    }
    this->hidden->fb_mem=mmap(NULL,this->hidden->fb_len,PROT_READ|PROT_WRITE,MAP_SHARED,this->hidden->fb_fd,0);
    if(this->hidden->fb_mem==MAP_FAILED) {
        this->hidden->fb_mem=NULL;
        SDL_SetError("H3531: framebuffer mmap failed");
        return -1;
    }
    if(fast16_requested()) {
        vformat->BitsPerPixel=16;
        vformat->BytesPerPixel=2;
    } else {
        vformat->BitsPerPixel=32;
        vformat->BytesPerPixel=4;
    }
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
    int fast16=fast16_requested();
    int bytespp=fast16 ? 2 : 4;
    (void)bpp;

    free_shadow_buffers(this);
    bytes=(size_t)width*(size_t)height*(size_t)bytespp;
    this->hidden->buffer=SDL_malloc(bytes);
    if(!this->hidden->buffer) {
        SDL_SetError("H3531: logical framebuffer allocation failed");
        return NULL;
    }
    SDL_memset(this->hidden->buffer,0,bytes);

    if(fast16) {
        this->hidden->prev_buffer=SDL_malloc(bytes);
        if(!this->hidden->prev_buffer) {
            free_shadow_buffers(this);
            SDL_SetError("H3531: fast16 previous-frame allocation failed");
            return NULL;
        }
        if(!SDL_ReallocFormat(current,16,0x7C00u,0x03E0u,0x001Fu,0x8000u)) {
            free_shadow_buffers(this);
            return NULL;
        }
    } else {
        if(!SDL_ReallocFormat(current,32,0x00FF0000,0x0000FF00,0x000000FF,0)) {
            free_shadow_buffers(this);
            return NULL;
        }
    }

    current->flags=(flags&SDL_FULLSCREEN)|SDL_SWSURFACE|SDL_PREALLOC;
    this->hidden->w=current->w=width;
    this->hidden->h=current->h=height;
    current->pitch=width*bytespp;
    current->pixels=this->hidden->buffer;
    if(build_scale_tables(this)<0) {
        free_shadow_buffers(this);
        return NULL;
    }
    clear_fb(this);

    if(fast16) {
        fprintf(stderr,
                "H3531 video16: native A1R5G5B5 logical surface %dx%d -> viewport %dx%d, dirty-row scaler enabled\n",
                width,height,this->hidden->outw,this->hidden->outh);
    }
    return current;
}

static Uint16 rgb1555(Uint32 p) {
    Uint32 r=(p>>16)&255u, g=(p>>8)&255u, b=p&255u;
    return (Uint16)(0x8000u|((r>>3)<<10)|((g>>3)<<5)|(b>>3));
}

/* Exact nearest-neighbour 3:2 horizontal expansion used by FBZX:
   source pixels [a,b,c,d] become [a,a,b,c,c,d]. */
static void scale_row_640_to_960_1555(const Uint16 *src, Uint16 *dst) {
    int sx,dx;
    for(sx=0,dx=0; sx<640; sx+=2,dx+=3) {
        Uint16 a=src[sx];
        Uint16 b=src[sx+1];
        dst[dx]=a;
        dst[dx+1]=a;
        dst[dx+2]=b;
    }
}

/* FBZX fast path. 640x480 -> 960x720 is exactly 3:2 in both axes.
   The framebuffer is exclusively owned by the graphical app, so unchanged
   logical rows can safely keep their previous physical pixels. */
static int fast16_update_fbzx(_THIS) {
    Uint16 *src=(Uint16*)this->hidden->buffer;
    Uint16 *prev=(Uint16*)this->hidden->prev_buffer;
    Uint16 *fb=visible_base(this);
    int stride=this->hidden->fb_pitch/2;
    int ox=this->hidden->ox, oy=this->hidden->oy;
    int pair;
    unsigned changed=0;
    static unsigned diag_frames=0;
    static unsigned diag_changed_rows=0;

    if(!src || !prev || !fb) return 0;
    if(this->hidden->w!=640 || this->hidden->h!=480 ||
       this->hidden->outw!=960 || this->hidden->outh!=720)
        return 0;

    for(pair=0; pair<240; ++pair) {
        int sy0=pair*2;
        int sy1=sy0+1;
        int dy=oy+pair*3;
        const Uint16 *s0=src+sy0*640;
        const Uint16 *s1=src+sy1*640;
        Uint16 *p0=prev+sy0*640;
        Uint16 *p1=prev+sy1*640;
        Uint16 *d0=fb+dy*stride+ox;
        Uint16 *d1=d0+stride;
        Uint16 *d2=d1+stride;

        if(!this->hidden->prev_valid || SDL_memcmp(s0,p0,640u*2u)!=0) {
            scale_row_640_to_960_1555(s0,d0);
            SDL_memcpy(d1,d0,960u*2u);
            SDL_memcpy(p0,s0,640u*2u);
            ++changed;
        }
        if(!this->hidden->prev_valid || SDL_memcmp(s1,p1,640u*2u)!=0) {
            scale_row_640_to_960_1555(s1,d2);
            SDL_memcpy(p1,s1,640u*2u);
            ++changed;
        }
    }

    this->hidden->prev_valid=1;
    ++diag_frames;
    diag_changed_rows+=changed;
    if(diag_frames>=250u) {
        fprintf(stderr,
                "H3531 scale16: frames=%u changed_rows=%u/%u avg_rows_x100=%u\n",
                diag_frames,diag_changed_rows,diag_frames*480u,
                (diag_changed_rows*100u)/diag_frames);
        diag_frames=0;
        diag_changed_rows=0;
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
    return 1;
}

static void DUMMY_UpdateRects(_THIS, int numrects, SDL_Rect *rects) {
    int n;
    int sw=this->hidden->w, sh=this->hidden->h;
    int outw=this->hidden->outw, outh=this->hidden->outh;
    int ox=this->hidden->ox, oy=this->hidden->oy;
    Uint16 *fb=visible_base(this);
    int stride=this->hidden->fb_pitch/2;
    int *xmap=this->hidden->xmap;
    int *ymap=this->hidden->ymap;
    int fast16=fast16_requested();

    if(fast16 && fast16_update_fbzx(this))
        return;

    if(!fb || !xmap || !ymap || sw<=0 || sh<=0) return;

    if(fast16) {
        Uint16 *src=(Uint16*)this->hidden->buffer;
        if(!src) return;
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
                int sy=ymap[y-oy];
                Uint16 *srow=src+sy*sw;
                Uint16 *d=fb+y*stride;
                for(x=dx0; x<dx1; ++x)
                    d[x]=srow[xmap[x-ox]];
            }
        }
    } else {
        Uint32 *src=(Uint32*)this->hidden->buffer;
        if(!src) return;
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
                int sy=ymap[y-oy];
                Uint32 *srow=src+sy*sw;
                Uint16 *d=fb+y*stride;
                for(x=dx0; x<dx1; ++x)
                    d[x]=rgb1555(srow[xmap[x-ox]]);
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
    free_scale_tables(this);
    free_shadow_buffers(this);
    if(this->hidden->fb_mem) {
        munmap(this->hidden->fb_mem,this->hidden->fb_len);
        this->hidden->fb_mem=NULL;
    }
    if(this->hidden->fb_fd>=0) {
        close(this->hidden->fb_fd);
        this->hidden->fb_fd=-1;
    }
}
