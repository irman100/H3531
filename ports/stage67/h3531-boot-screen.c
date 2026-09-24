#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_KBDS 8
#define MAX_EVENTS 32
#define BPL (8U * (unsigned)sizeof(unsigned long))
#define NBITS(x) (((x) + BPL - 1U) / BPL)
#define TBIT(bit, arr) (((arr)[(unsigned)(bit) / BPL] >> ((unsigned)(bit) % BPL)) & 1UL)

typedef unsigned char hi_u8;
typedef enum { HI_FALSE = 0, HI_TRUE = 1 } hi_bool;
typedef struct {
    hi_bool alpha_en;
    hi_bool alpha_chn_en;
    hi_u8 alpha0;
    hi_u8 alpha1;
    hi_u8 global_alpha;
    hi_u8 reserved;
} hi_fb_alpha;

#define IOC_TYPE_HIFB 'F'
#define FBIOGET_ALPHA_HIFB _IOR(IOC_TYPE_HIFB, 92, hi_fb_alpha)
#define FBIOPUT_ALPHA_HIFB _IOW(IOC_TYPE_HIFB, 93, hi_fb_alpha)

struct glyph {
    char c;
    uint8_t r[7];
};

static const struct glyph font5x7[] = {
    {' ',{0,0,0,0,0,0,0}}, {'-',{0,0,0,31,0,0,0}}, {'.',{0,0,0,0,0,12,12}},
    {':',{0,12,12,0,12,12,0}}, {'/',{1,2,4,8,16,0,0}}, {'[',{14,8,8,8,8,8,14}},
    {']',{14,2,2,2,2,2,14}}, {'0',{14,17,19,21,25,17,14}},
    {'1',{4,12,4,4,4,4,14}}, {'2',{14,17,1,2,4,8,31}},
    {'3',{30,1,1,14,1,1,30}}, {'4',{2,6,10,18,31,2,2}},
    {'5',{31,16,16,30,1,1,30}}, {'6',{14,16,16,30,17,17,14}},
    {'7',{31,1,2,4,8,8,8}}, {'8',{14,17,17,14,17,17,14}},
    {'9',{14,17,17,15,1,1,14}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{28,18,17,17,17,18,28}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}}
};

struct fbctx {
    int fd;
    uint8_t *mem;
    size_t len;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
};

static const uint8_t *glyph_rows(char c)
{
    size_t i;
    static const uint8_t unknown[7] = {31,1,2,4,0,4,0};
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (i = 0; i < sizeof(font5x7)/sizeof(font5x7[0]); ++i)
        if (font5x7[i].c == c) return font5x7[i].r;
    return unknown;
}

static uint32_t chan(unsigned value, unsigned len, unsigned off)
{
    uint32_t maxv;
    if (!len) return 0;
    maxv = (1U << len) - 1U;
    return ((value * maxv / 255U) & maxv) << off;
}

static uint32_t rgb(struct fbctx *fb, unsigned r, unsigned g, unsigned b)
{
    uint32_t p = chan(r, fb->v.red.length, fb->v.red.offset) |
                 chan(g, fb->v.green.length, fb->v.green.offset) |
                 chan(b, fb->v.blue.length, fb->v.blue.offset);
    if (fb->v.transp.length)
        p |= chan(255, fb->v.transp.length, fb->v.transp.offset);
    return p;
}

static void force_opaque(struct fbctx *fb)
{
    hi_fb_alpha a;
    memset(&a, 0, sizeof(a));
    if (ioctl(fb->fd, FBIOGET_ALPHA_HIFB, &a) < 0)
        return;
    a.alpha_en = HI_TRUE;
    a.alpha_chn_en = HI_TRUE;
    a.alpha0 = 255;
    a.alpha1 = 255;
    a.global_alpha = 255;
    (void)ioctl(fb->fd, FBIOPUT_ALPHA_HIFB, &a);
}

static void px(struct fbctx *fb, int x, int y, uint32_t p)
{
    unsigned bpp = fb->v.bits_per_pixel / 8;
    uint8_t *d;
    if (x < 0 || y < 0 || x >= (int)fb->v.xres || y >= (int)fb->v.yres) return;
    d = fb->mem + (size_t)y * fb->f.line_length + (size_t)x * bpp;
    if (bpp == 2) *(uint16_t *)d = (uint16_t)p;
    else if (bpp == 4) *(uint32_t *)d = p;
    else if (bpp == 3) { d[0]=(uint8_t)p; d[1]=(uint8_t)(p>>8); d[2]=(uint8_t)(p>>16); }
}

static void clearfb(struct fbctx *fb, uint32_t p)
{
    int x,y;
    unsigned bpp = fb->v.bits_per_pixel / 8;
    if (bpp == 2) {
        for (y=0; y<(int)fb->v.yres; ++y) {
            uint16_t *row = (uint16_t *)(fb->mem + (size_t)y * fb->f.line_length);
            for (x=0; x<(int)fb->v.xres; ++x) row[x]=(uint16_t)p;
        }
        return;
    }
    if (bpp == 4) {
        for (y=0; y<(int)fb->v.yres; ++y) {
            uint32_t *row = (uint32_t *)(fb->mem + (size_t)y * fb->f.line_length);
            for (x=0; x<(int)fb->v.xres; ++x) row[x]=p;
        }
        return;
    }
    for (y=0; y<(int)fb->v.yres; ++y)
        for (x=0; x<(int)fb->v.xres; ++x)
            px(fb,x,y,p);
}

static void draw_char(struct fbctx *fb, int x, int y, int scale, char c, uint32_t fg)
{
    const uint8_t *r = glyph_rows(c);
    int yy,xx,sy,sx;
    for (yy=0; yy<7; ++yy)
        for (xx=0; xx<5; ++xx)
            if (r[yy] & (1U << (4-xx)))
                for (sy=0; sy<scale; ++sy)
                    for (sx=0; sx<scale; ++sx)
                        px(fb, x+xx*scale+sx, y+yy*scale+sy, fg);
}

static void draw_text(struct fbctx *fb, int x, int y, int scale, const char *s, uint32_t fg)
{
    while (*s) {
        draw_char(fb,x,y,scale,*s,fg);
        x += 6*scale;
        ++s;
    }
}

static int fbopen(struct fbctx *fb)
{
    memset(fb,0,sizeof(*fb));
    fb->fd = -1;
    fb->fd = open("/dev/fb0", O_RDWR);
    if (fb->fd < 0) return -1;
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->f) < 0) return -1;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->v) < 0) return -1;
    if (fb->v.bits_per_pixel != 16 && fb->v.bits_per_pixel != 24 && fb->v.bits_per_pixel != 32)
        return -1;
    fb->len = (size_t)fb->f.line_length * fb->v.yres_virtual;
    fb->mem = mmap(NULL, fb->len, PROT_READ|PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) { fb->mem=NULL; return -1; }
    return 0;
}

static void fbclose(struct fbctx *fb)
{
    if (fb->mem) munmap(fb->mem, fb->len);
    if (fb->fd >= 0) close(fb->fd);
}

static int keyboard_capable(int fd)
{
    unsigned long ev[NBITS(EV_MAX+1)], keys[NBITS(KEY_MAX+1)];
    memset(ev,0,sizeof(ev)); memset(keys,0,sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(0,sizeof(ev)), ev) < 0) return 0;
    if (!TBIT(EV_KEY,ev)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY,sizeof(keys)), keys) < 0) return 0;
    return TBIT(KEY_A,keys) && TBIT(KEY_ENTER,keys) && TBIT(KEY_F2,keys);
}

static int mouse_capable(int fd)
{
    unsigned long ev[NBITS(EV_MAX+1)], rel[NBITS(REL_MAX+1)];
    memset(ev,0,sizeof(ev)); memset(rel,0,sizeof(rel));
    if (ioctl(fd, EVIOCGBIT(0,sizeof(ev)), ev) < 0) return 0;
    if (!TBIT(EV_REL,ev)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_REL,sizeof(rel)), rel) < 0) return 0;
    return TBIT(REL_X,rel) && TBIT(REL_Y,rel);
}

static void scan_input(int *kbd_fds, int *kbd_count, int *have_mouse)
{
    int i;
    *kbd_count=0; *have_mouse=0;
    for (i=0;i<MAX_EVENTS;++i) {
        char p[64];
        int fd;
        snprintf(p,sizeof(p),"/dev/input/event%d",i);
        fd=open(p,O_RDONLY|O_NONBLOCK);
        if (fd<0) continue;
        if (keyboard_capable(fd) && *kbd_count<MAX_KBDS) {
            kbd_fds[*kbd_count]=fd;
            (*kbd_count)++;
            continue;
        }
        if (mouse_capable(fd)) *have_mouse=1;
        close(fd);
    }
}

static int wait_f2(int *fds, int n, int timeout_ms)
{
    struct pollfd pfd[MAX_KBDS];
    int i,rc;
    for(i=0;i<n;++i){pfd[i].fd=fds[i];pfd[i].events=POLLIN;pfd[i].revents=0;}
    if(n<=0){ usleep((unsigned)timeout_ms*1000U); return 0; }
    rc=poll(pfd,(nfds_t)n,timeout_ms);
    if(rc<=0) return 0;
    for(i=0;i<n;++i){
        if(pfd[i].revents&POLLIN){
            struct input_event ev[16];
            ssize_t got=read(pfd[i].fd,ev,sizeof(ev));
            int k,count;
            if(got<=0) continue;
            count=(int)(got/(ssize_t)sizeof(struct input_event));
            for(k=0;k<count;++k)
                if(ev[k].type==EV_KEY && ev[k].code==KEY_F2 && ev[k].value==1)
                    return 2;
        }
    }
    return 0;
}

static void status_line(struct fbctx *fb, int y, const char *name, int ok, uint32_t white)
{
    char line[96];
    snprintf(line,sizeof(line),"%-22s [%s]",name,ok?"OK":"WARN");
    draw_text(fb,48,y,2,line,white);
}

static void draw_header(struct fbctx *fb, uint32_t black, uint32_t white)
{
    clearfb(fb,black);
    draw_text(fb,48,38,3,"STAYPLAYTION SYSTEM LOADER",white);
}

static int run_menu(struct fbctx *fb, int timeout_ms, uint32_t black, uint32_t white)
{
    int kfds[MAX_KBDS], kn=0, mouse=0, i, rc, critical=0;
    int storage_ok, x_ok, desktop_ok, monitor_ok;

    force_opaque(fb);
    draw_header(fb,black,white);
    draw_text(fb,48,78,2,"HI3531 STARTUP PREFLIGHT",white);

    storage_ok = access("/mnt/usb/H3531",R_OK)==0;
    x_ok = access("/mnt/usb/H3531/APPS/x11-debian/bin/Xfbdev-shadow-live",X_OK)==0;
    desktop_ok = access("/mnt/usb/H3531/APPS/x11-debian/START-DESKTOP.sh",X_OK)==0;
    monitor_ok = access("/mnt/usb/H3531/SYSTEM/MONITOR.ORIGINAL.APP",X_OK)==0;
    scan_input(kfds,&kn,&mouse);

    status_line(fb,135,"FRAMEBUFFER",1,white);
    status_line(fb,165,"SYSTEM STORAGE",storage_ok,white);
    status_line(fb,195,"X11 RUNTIME",x_ok,white);
    status_line(fb,225,"DESKTOP CORE",desktop_ok,white);
    status_line(fb,255,"USB KEYBOARD",kn>0,white);
    status_line(fb,285,"USB MOUSE",mouse,white);
    status_line(fb,315,"RESCUE MONITOR",monitor_ok,white);

    if(!storage_ok || !x_ok || !desktop_ok || !monitor_ok) critical=1;

    if(critical) {
        draw_text(fb,48,380,2,"CRITICAL PREFLIGHT FAILURE",white);
        draw_text(fb,48,410,2,"STARTING RESCUE MONITOR",white);
        sleep(2);
        rc=10;
    } else {
        draw_text(fb,48,380,2,"F2  RESCUE MONITOR",white);
        draw_text(fb,48,410,2,"AUTO START DESKTOP",white);
        draw_text(fb,48,440,2,"WAITING FOR STARTUP SELECTION",white);
        rc=wait_f2(kfds,kn,timeout_ms);
        if(rc==2) {
            draw_text(fb,48,490,2,"F2 PRESSED - STARTING MONITOR",white);
            usleep(500000);
        } else {
            draw_text(fb,48,490,2,"STARTUP SELECTION COMPLETE",white);
            usleep(500000);
        }
    }

    for(i=0;i<kn;++i) close(kfds[i]);
    return rc;
}

static void draw_warmup(struct fbctx *fb, uint32_t black, uint32_t white)
{
    draw_header(fb,black,white);
    draw_text(fb,48,88,2,"HARDWARE COMPATIBILITY PREINIT",white);
    status_line(fb,150,"HDMI/HIFB",1,white);
    status_line(fb,180,"INPUT DRIVER",1,white);
    draw_text(fb,48,245,2,"INITIALIZING VIDEO HARDWARE",white);
    draw_text(fb,48,280,2,"COMPATIBILITY LAYER ACTIVE",white);
    draw_text(fb,48,345,2,"PLEASE WAIT",white);
}

static void run_warmup_mask(struct fbctx *fb, int timeout_ms, uint32_t black, uint32_t white)
{
    int elapsed=0;
    while(elapsed < timeout_ms) {
        force_opaque(fb);
        draw_warmup(fb,black,white);
        usleep(100000);
        elapsed += 100;
    }
}

static void run_ready(struct fbctx *fb, int timeout_ms, uint32_t black, uint32_t white)
{
    force_opaque(fb);
    draw_header(fb,black,white);
    draw_text(fb,48,105,2,"HARDWARE INITIALIZED",white);
    status_line(fb,175,"HDMI/HIFB",1,white);
    status_line(fb,205,"INPUT DRIVER",1,white);
    status_line(fb,235,"DESKTOP CORE",1,white);
    draw_text(fb,48,315,2,"STARTING DESKTOP",white);
    usleep((unsigned)timeout_ms*1000U);
}

int main(int argc, char **argv)
{
    struct fbctx fb;
    uint32_t black,white;
    const char *mode="menu";
    int timeout_ms=7000;
    int rc=0;

    if(argc>1) {
        if(!strcmp(argv[1],"menu") || !strcmp(argv[1],"warmup") || !strcmp(argv[1],"ready")) {
            mode=argv[1];
            if(argc>2) timeout_ms=atoi(argv[2]);
        } else {
            timeout_ms=atoi(argv[1]);
        }
    }

    if(timeout_ms<500) timeout_ms=500;
    if(timeout_ms>15000) timeout_ms=15000;

    if(fbopen(&fb)<0) return 10;
    black=rgb(&fb,0,0,0);
    white=rgb(&fb,255,255,255);

    if(!strcmp(mode,"warmup")) {
        run_warmup_mask(&fb,timeout_ms,black,white);
        rc=0;
    } else if(!strcmp(mode,"ready")) {
        run_ready(&fb,timeout_ms,black,white);
        rc=0;
    } else {
        rc=run_menu(&fb,timeout_ms,black,white);
    }

    fbclose(&fb);
    return rc;
}
