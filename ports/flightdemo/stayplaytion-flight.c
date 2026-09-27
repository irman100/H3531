/*
 * Stayplaytion Flight Core Stage 1
 * HiSilicon Hi3531 native framebuffer demo/game prototype.
 *
 * Rendering:
 *   - internal 640x360 A1R5G5B5 canvas
 *   - integer 2x present to proven 1280x720 HIFB /dev/fb0
 *   - vendor HIFB vblank ioctl 0x4664 when available
 *
 * Input:
 *   - native keyboard passed by h3531-native-run
 *   - /dev/input/js0 or js1 gamepad
 *
 * Safety:
 *   - no SPI, U-Boot, MMZ or firmware writes
 *   - h3531-native-run owns framebuffer lease/desktop restoration
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define INTERNAL_W 640
#define INTERNAL_H 360
#define OUTPUT_W 1280
#define OUTPUT_H 720
#define STAR_COUNT 320
#define PARTICLE_COUNT 96
#define H3531_FBIOGET_VBLANK_HIFB 0x00004664UL

typedef struct {
    int fd;
    uint8_t *mem;
    size_t len;
    unsigned stride;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    uint16_t *canvas;
    uint16_t *base;
    uint16_t *row2x;
    int vblank_state;
} video_t;

typedef struct {
    int x, y, z;
    unsigned seed;
} star_t;

typedef struct {
    int x, y;
    int vx, vy;
    int life;
} particle_t;

typedef struct {
    int kfd;
    int jfd;
    int left, right, up, down;
    int16_t axis_x, axis_y;
    uint8_t buttons[32];
} input_t;

static volatile sig_atomic_t g_stop = 0;
static uint32_t g_rng = 0x3531c0deU;
static star_t g_stars[STAR_COUNT];
static particle_t g_particles[PARTICLE_COUNT];
static unsigned g_particle_cursor = 0;

static uint32_t xrnd(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(0x8000U |
        (((r >> 3) & 31U) << 10) |
        (((g >> 3) & 31U) << 5) |
        ((b >> 3) & 31U));
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t ns)
{
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ULL);
    req.tv_nsec = (long)(ns % 1000000000ULL);
    while (nanosleep(&req, &req) < 0 && errno == EINTR && !g_stop) {}
}

static void putpx(uint16_t *c, int x, int y, uint16_t color)
{
    if ((unsigned)x < INTERNAL_W && (unsigned)y < INTERNAL_H)
        c[(size_t)y * INTERNAL_W + x] = color;
}

static void hline(uint16_t *c, int x0, int x1, int y, uint16_t color)
{
    int x;
    if ((unsigned)y >= INTERNAL_H)
        return;
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (x1 < 0 || x0 >= INTERNAL_W)
        return;
    if (x0 < 0) x0 = 0;
    if (x1 >= INTERNAL_W) x1 = INTERNAL_W - 1;
    for (x=x0; x<=x1; ++x)
        c[(size_t)y * INTERNAL_W + x] = color;
}

static void vline(uint16_t *c, int x, int y0, int y1, uint16_t color)
{
    int y;
    if ((unsigned)x >= INTERNAL_W)
        return;
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    if (y1 < 0 || y0 >= INTERNAL_H)
        return;
    if (y0 < 0) y0 = 0;
    if (y1 >= INTERNAL_H) y1 = INTERNAL_H - 1;
    for (y=y0; y<=y1; ++y)
        c[(size_t)y * INTERNAL_W + x] = color;
}

static void line(uint16_t *c, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
    int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        putpx(c,x0,y0,color);
        if (x0==x1 && y0==y1) break;
        {
            int e2 = 2*err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

static void fill_rect(uint16_t *c, int x, int y, int w, int h, uint16_t color)
{
    int yy;
    if (w <= 0 || h <= 0)
        return;
    for (yy=0; yy<h; ++yy)
        hline(c, x, x+w-1, y+yy, color);
}

static void reset_star(star_t *s, int farz)
{
    s->x = (int)(xrnd()%6001U) - 3000;
    s->y = (int)(xrnd()%3401U) - 1700;
    s->z = farz ? (3200 + (int)(xrnd()%2400U)) : (400 + (int)(xrnd()%5000U));
    s->seed = xrnd();
}

static void init_scene(void)
{
    int i;
    memset(g_particles, 0, sizeof(g_particles));
    for (i=0; i<STAR_COUNT; ++i)
        reset_star(&g_stars[i], 0);
}

static void build_base(video_t *v)
{
    int y,x;
    for (y=0; y<INTERNAL_H; ++y) {
        unsigned r,g,b;
        if (y < 220) {
            r = 2 + (unsigned)(y * 4 / 220);
            g = 5 + (unsigned)(y * 7 / 220);
            b = 15 + (unsigned)(y * 18 / 220);
        } else {
            unsigned t=(unsigned)(y-220);
            r = 4 + t/35;
            g = 4 + t/30;
            b = 10 + t/18;
        }
        for (x=0; x<INTERNAL_W; ++x)
            v->base[(size_t)y*INTERNAL_W+x]=rgb(r,g,b);
    }
}

static void draw_stars(uint16_t *c)
{
    int i;
    for (i=0; i<STAR_COUNT; ++i) {
        star_t *s=&g_stars[i];
        int sx,sy,br;
        s->z -= 34;
        if (s->z < 96) {
            reset_star(s,1);
            continue;
        }
        sx = INTERNAL_W/2 + (s->x * 260) / s->z;
        sy = 150 + (s->y * 260) / s->z;
        if (sx<1 || sx>=INTERNAL_W-1 || sy<1 || sy>=INTERNAL_H-1) {
            reset_star(s,1);
            continue;
        }
        br = 255 - (s->z * 180 / 5600);
        if (br < 70) br=70;
        putpx(c,sx,sy,rgb((unsigned)br,(unsigned)br,(unsigned)(br+((s->seed>>3)&31U))));
        if (s->z < 850) {
            putpx(c,sx-1,sy,rgb((unsigned)br/2,(unsigned)br/2,(unsigned)br));
            putpx(c,sx+1,sy,rgb((unsigned)br/2,(unsigned)br/2,(unsigned)br));
        }
    }
}

static void draw_grid(uint16_t *c, unsigned frame)
{
    const int horizon=218;
    int i;
    uint16_t dim=rgb(5,30,42);
    uint16_t bright=rgb(8,58,72);

    hline(c,0,INTERNAL_W-1,horizon,bright);

    for (i=-8;i<=8;++i) {
        int xb=INTERNAL_W/2 + i*58;
        line(c,INTERNAL_W/2,horizon,xb,INTERNAL_H-1,(i&1)?dim:bright);
    }

    for (i=0;i<13;++i) {
        unsigned q=(frame*5U + (unsigned)i*64U) & 1023U;
        unsigned p=(q*q)>>10;
        int y=horizon + (int)(p*140U/1024U);
        if (y>horizon && y<INTERNAL_H)
            hline(c,0,INTERNAL_W-1,y,(i&1)?dim:bright);
    }
}

static void spawn_exhaust(int sx,int sy)
{
    particle_t *p=&g_particles[g_particle_cursor++%PARTICLE_COUNT];
    p->x=sx*256;
    p->y=sy*256;
    p->vx=((int)(xrnd()%161U)-80);
    p->vy=180+(int)(xrnd()%160U);
    p->life=18+(int)(xrnd()%24U);
}

static void draw_particles(uint16_t *c)
{
    int i;
    for(i=0;i<PARTICLE_COUNT;++i) {
        particle_t *p=&g_particles[i];
        int x,y;
        unsigned br;
        if(p->life<=0) continue;
        p->x+=p->vx;
        p->y+=p->vy;
        p->life--;
        x=p->x>>8; y=p->y>>8;
        br=(unsigned)(60+p->life*5);
        if(br>255) br=255;
        putpx(c,x,y,rgb(br/2,br,255));
        if(p->life>18) putpx(c,x,y+1,rgb(br/4,br/2,200));
    }
}

static void draw_ship(uint16_t *c,int x,int y,unsigned frame)
{
    uint16_t hull=rgb(170,205,235);
    uint16_t edge=rgb(35,210,255);
    uint16_t dark=rgb(18,40,65);
    uint16_t glow=rgb(255,145+(frame&31U),55);

    line(c,x,y-13,x-18,y+12,edge);
    line(c,x,y-13,x+18,y+12,edge);
    line(c,x-18,y+12,x+18,y+12,edge);
    fill_rect(c,x-7,y-2,15,13,hull);
    fill_rect(c,x-3,y-8,7,8,dark);
    hline(c,x-14,x-7,y+8,hull);
    hline(c,x+7,x+14,y+8,hull);
    putpx(c,x-5,y+13,glow);
    putpx(c,x,y+15,glow);
    putpx(c,x+5,y+13,glow);
}

static void draw_holo_panel(uint16_t *c,unsigned frame)
{
    /* Stage 1 placeholder for the future hardware-decoded video surface. */
    unsigned cycle=frame%360U;
    int z=360-(int)cycle;
    int w,h,x,y,i;
    uint16_t border=rgb(30,190,220);
    uint16_t dark=rgb(4,18,25);
    uint16_t scan=rgb(8,70,82);

    if(z<45) z+=315;
    w=26 + (360-z)/5;
    h=w*9/16;
    if(w>170) w=170;
    if(h>96) h=96;
    x=470 - (360-z)/10;
    y=82 + (360-z)/14;

    fill_rect(c,x,y,w,h,dark);
    hline(c,x,x+w-1,y,border);
    hline(c,x,x+w-1,y+h-1,border);
    vline(c,x,y,y+h-1,border);
    vline(c,x+w-1,y,y+h-1,border);

    for(i=3;i<h-3;i+=5)
        hline(c,x+3,x+w-4,y+i,scan);

    {
        int bar=(int)((frame*3U)%(unsigned)(w>8?w-8:1));
        vline(c,x+4+bar,y+4,y+h-5,rgb(80,230,255));
    }
}

static int video_open(video_t *v)
{
    size_t fallback;
    memset(v,0,sizeof(*v));
    v->fd=-1;
    v->vblank_state=0;

    v->fd=open("/dev/fb0",O_RDWR);
    if(v->fd<0) {
        fprintf(stderr,"[flight] open /dev/fb0: %s\n",strerror(errno));
        return -1;
    }
    if(ioctl(v->fd,FBIOGET_FSCREENINFO,&v->fix)<0 ||
       ioctl(v->fd,FBIOGET_VSCREENINFO,&v->var)<0) {
        fprintf(stderr,"[flight] framebuffer info: %s\n",strerror(errno));
        return -1;
    }
    v->stride=v->fix.line_length;
    fallback=(size_t)v->stride*(v->var.yres_virtual?v->var.yres_virtual:v->var.yres);
    v->len=v->fix.smem_len?v->fix.smem_len:fallback;

    if(v->var.xres!=OUTPUT_W || v->var.yres!=OUTPUT_H ||
       v->var.bits_per_pixel!=16 || v->stride<OUTPUT_W*2U) {
        fprintf(stderr,"[flight] unsupported fb %ux%u %ubpp stride=%u\n",
            v->var.xres,v->var.yres,v->var.bits_per_pixel,v->stride);
        return -1;
    }

    v->mem=(uint8_t*)mmap(NULL,v->len,PROT_READ|PROT_WRITE,MAP_SHARED,v->fd,0);
    if(v->mem==MAP_FAILED) {
        v->mem=NULL;
        fprintf(stderr,"[flight] mmap: %s\n",strerror(errno));
        return -1;
    }

    v->canvas=(uint16_t*)malloc((size_t)INTERNAL_W*INTERNAL_H*2U);
    v->base=(uint16_t*)malloc((size_t)INTERNAL_W*INTERNAL_H*2U);
    v->row2x=(uint16_t*)malloc((size_t)OUTPUT_W*2U);
    if(!v->canvas || !v->base || !v->row2x) {
        fprintf(stderr,"[flight] out of memory\n");
        return -1;
    }

    build_base(v);
    fprintf(stderr,"[flight] HIFB ready %ux%u stride=%u internal=%ux%u\n",
        v->var.xres,v->var.yres,v->stride,INTERNAL_W,INTERNAL_H);
    return 0;
}

static void video_close(video_t *v)
{
    if(!v) return;
    if(v->mem) munmap(v->mem,v->len);
    if(v->fd>=0) close(v->fd);
    free(v->canvas);
    free(v->base);
    free(v->row2x);
    memset(v,0,sizeof(*v));
    v->fd=-1;
}

static void video_present(video_t *v)
{
    int y,x;
    if(v->vblank_state>=0) {
        errno=0;
        if(ioctl(v->fd,H3531_FBIOGET_VBLANK_HIFB,0)==0) {
            if(v->vblank_state==0)
                fprintf(stderr,"[flight] native HIFB vblank 0x4664 active\n");
            v->vblank_state=1;
        } else {
            if(v->vblank_state==0)
                fprintf(stderr,"[flight] HIFB vblank unavailable errno=%d; timed fallback\n",errno);
            v->vblank_state=-1;
        }
    }

    for(y=0;y<INTERNAL_H;++y) {
        const uint16_t *src=v->canvas+(size_t)y*INTERNAL_W;
        uint16_t *r=v->row2x;
        uint8_t *d0=v->mem+(size_t)(y*2)*v->stride;
        uint8_t *d1=v->mem+(size_t)(y*2+1)*v->stride;
        for(x=0;x<INTERNAL_W;++x) {
            uint16_t p=src[x];
            r[x*2]=p;
            r[x*2+1]=p;
        }
        memcpy(d0,r,OUTPUT_W*2U);
        memcpy(d1,r,OUTPUT_W*2U);
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static int open_first_joystick(void)
{
    int fd=open("/dev/input/js0",O_RDONLY|O_NONBLOCK);
    if(fd>=0) return fd;
    return open("/dev/input/js1",O_RDONLY|O_NONBLOCK);
}

static void input_open(input_t *in)
{
    const char *kbd=getenv("H3531_NATIVE_KEYBOARD");
    memset(in,0,sizeof(*in));
    in->kfd=-1; in->jfd=-1;

    if(kbd && *kbd)
        in->kfd=open(kbd,O_RDONLY|O_NONBLOCK);
    in->jfd=open_first_joystick();

    fprintf(stderr,"[flight] input keyboard=%s fd=%d joystick_fd=%d\n",
        (kbd&&*kbd)?kbd:"<none>",in->kfd,in->jfd);
}

static void input_close(input_t *in)
{
    if(in->kfd>=0) close(in->kfd);
    if(in->jfd>=0) close(in->jfd);
}

static void input_poll(input_t *in)
{
    if(in->kfd>=0) {
        struct input_event ev;
        while(read(in->kfd,&ev,sizeof(ev))==(ssize_t)sizeof(ev)) {
            int down=ev.value!=0;
            if(ev.type!=EV_KEY) continue;
            switch(ev.code) {
                case KEY_LEFT: in->left=down; break;
                case KEY_RIGHT: in->right=down; break;
                case KEY_UP: in->up=down; break;
                case KEY_DOWN: in->down=down; break;
                case KEY_ESC:
                case KEY_F12:
                    if(down) g_stop=1;
                    break;
                default: break;
            }
        }
    }

    if(in->jfd>=0) {
        struct js_event je;
        while(read(in->jfd,&je,sizeof(je))==(ssize_t)sizeof(je)) {
            unsigned type=je.type&~JS_EVENT_INIT;
            if(type==JS_EVENT_AXIS) {
                if(je.number==0) in->axis_x=je.value;
                if(je.number==1) in->axis_y=je.value;
            } else if(type==JS_EVENT_BUTTON && je.number<32) {
                in->buttons[je.number]=(uint8_t)(je.value?1:0);
            }
        }
        if((in->buttons[6]&&in->buttons[7]) ||
           (in->buttons[8]&&in->buttons[9]))
            g_stop=1;
    }
}

static int selftest(void)
{
    uint16_t p=rgb(255,0,0);
    star_t s;
    g_rng=0x3531c0deU;
    reset_star(&s,1);
    if((p&0x8000U)==0 || s.z<3200) {
        fprintf(stderr,"STAYPLAYTION_FLIGHT_SELFTEST_FAIL\n");
        return 2;
    }
    printf("STAYPLAYTION_FLIGHT_SELFTEST_OK\n");
    return 0;
}

int main(int argc,char **argv)
{
    video_t v;
    input_t in;
    unsigned frame=0;
    int ship_x=INTERNAL_W/2;
    int ship_y=INTERNAL_H-52;
    uint64_t perf_start,last_frame;

    if(argc>1 && strcmp(argv[1],"--selftest")==0)
        return selftest();

    signal(SIGINT,on_signal);
    signal(SIGTERM,on_signal);
    signal(SIGHUP,on_signal);

    if(video_open(&v)<0) {
        video_close(&v);
        return 10;
    }
    input_open(&in);
    init_scene();

    perf_start=mono_ns();
    last_frame=perf_start;

    while(!g_stop) {
        uint64_t now;
        int dx=0,dy=0;

        input_poll(&in);
        if(in.left || in.axis_x<-9000) dx=-3;
        if(in.right || in.axis_x>9000) dx=3;
        if(in.up || in.axis_y<-9000) dy=-3;
        if(in.down || in.axis_y>9000) dy=3;

        ship_x+=dx;
        ship_y+=dy;
        if(ship_x<25) ship_x=25;
        if(ship_x>INTERNAL_W-26) ship_x=INTERNAL_W-26;
        if(ship_y<235) ship_y=235;
        if(ship_y>INTERNAL_H-28) ship_y=INTERNAL_H-28;

        memcpy(v.canvas,v.base,(size_t)INTERNAL_W*INTERNAL_H*2U);
        draw_stars(v.canvas);
        draw_grid(v.canvas,frame);
        draw_holo_panel(v.canvas,frame);
        spawn_exhaust(ship_x-5,ship_y+15);
        spawn_exhaust(ship_x+5,ship_y+15);
        draw_particles(v.canvas);
        draw_ship(v.canvas,ship_x,ship_y,frame);
        video_present(&v);

        ++frame;
        now=mono_ns();

        if(frame%300U==0U) {
            double sec=(double)(now-perf_start)/1000000000.0;
            double fps=sec>0.0?300.0/sec:0.0;
            fprintf(stderr,"[flight] PERF fps=%.2f vblank=%s ship=%d,%d\n",
                fps,v.vblank_state>0?"native":"fallback",ship_x,ship_y);
            perf_start=now;
        }

        if(v.vblank_state<0) {
            uint64_t target=last_frame+16666667ULL;
            if(now<target) sleep_ns(target-now);
            last_frame=target;
            if(now>target+50000000ULL) last_frame=now;
        } else {
            last_frame=now;
        }
    }

    fprintf(stderr,"[flight] exit frame=%u\n",frame);
    input_close(&in);
    video_close(&v);
    return 0;
}
