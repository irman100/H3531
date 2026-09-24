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

struct glyph { char c; uint8_t r[7]; };
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
    static const uint8_t unknown[7]={31,1,2,4,0,4,0};
    if(c>='a'&&c<='z') c=(char)(c-'a'+'A');
    for(i=0;i<sizeof(font5x7)/sizeof(font5x7[0]);++i)
        if(font5x7[i].c==c) return font5x7[i].r;
    return unknown;
}
static uint32_t chan(unsigned v,unsigned len,unsigned off)
{
    uint32_t maxv;
    if(!len) return 0;
    maxv=(1U<<len)-1U;
    return ((v*maxv/255U)&maxv)<<off;
}
static uint32_t rgb(struct fbctx *fb,unsigned r,unsigned g,unsigned b)
{
    uint32_t p=chan(r,fb->v.red.length,fb->v.red.offset)|
               chan(g,fb->v.green.length,fb->v.green.offset)|
               chan(b,fb->v.blue.length,fb->v.blue.offset);
    if(fb->v.transp.length) p|=chan(255,fb->v.transp.length,fb->v.transp.offset);
    return p;
}
static void force_opaque(struct fbctx *fb)
{
    hi_fb_alpha a;
    memset(&a,0,sizeof(a));
    if(ioctl(fb->fd,FBIOGET_ALPHA_HIFB,&a)<0) return;
    a.alpha_en=HI_TRUE; a.alpha_chn_en=HI_TRUE;
    a.alpha0=255; a.alpha1=255; a.global_alpha=255;
    (void)ioctl(fb->fd,FBIOPUT_ALPHA_HIFB,&a);
}
static int fbopen_ctx(struct fbctx *fb)
{
    memset(fb,0,sizeof(*fb));
    fb->fd=open("/dev/fb0",O_RDWR);
    if(fb->fd<0) return -1;
    if(ioctl(fb->fd,FBIOGET_FSCREENINFO,&fb->f)<0) return -1;
    if(ioctl(fb->fd,FBIOGET_VSCREENINFO,&fb->v)<0) return -1;
    fb->len=(size_t)fb->f.line_length*fb->v.yres_virtual;
    fb->mem=mmap(NULL,fb->len,PROT_READ|PROT_WRITE,MAP_SHARED,fb->fd,0);
    if(fb->mem==MAP_FAILED){fb->mem=NULL;return -1;}
    return 0;
}
static void fbclose_ctx(struct fbctx *fb)
{
    if(fb->mem) munmap(fb->mem,fb->len);
    if(fb->fd>=0) close(fb->fd);
}
static void px(struct fbctx *fb,int x,int y,uint32_t p)
{
    unsigned bpp=fb->v.bits_per_pixel/8;
    uint8_t *d;
    if(x<0||y<0||x>=(int)fb->v.xres||y>=(int)fb->v.yres) return;
    d=fb->mem+(size_t)y*fb->f.line_length+(size_t)x*bpp;
    if(bpp==2) *(uint16_t*)d=(uint16_t)p;
    else if(bpp==4) *(uint32_t*)d=p;
}
static void clearfb(struct fbctx *fb,uint32_t p)
{
    int x,y;
    for(y=0;y<(int)fb->v.yres;++y)
        for(x=0;x<(int)fb->v.xres;++x)
            px(fb,x,y,p);
}
static void draw_char(struct fbctx *fb,int x,int y,int s,char c,uint32_t fg)
{
    const uint8_t *r=glyph_rows(c);
    int yy,xx,sy,sx;
    for(yy=0;yy<7;++yy) for(xx=0;xx<5;++xx)
        if(r[yy]&(1U<<(4-xx)))
            for(sy=0;sy<s;++sy) for(sx=0;sx<s;++sx)
                px(fb,x+xx*s+sx,y+yy*s+sy,fg);
}
static void draw_text(struct fbctx *fb,int x,int y,int s,const char *t,uint32_t fg)
{
    while(*t){draw_char(fb,x,y,s,*t,fg);x+=6*s;++t;}
}
static int keyboard_capable(int fd)
{
    unsigned long ev[NBITS(EV_MAX+1)],keys[NBITS(KEY_MAX+1)];
    memset(ev,0,sizeof(ev));memset(keys,0,sizeof(keys));
    if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)),ev)<0||!TBIT(EV_KEY,ev)) return 0;
    if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(keys)),keys)<0) return 0;
    return TBIT(KEY_A,keys)&&TBIT(KEY_ENTER,keys)&&TBIT(KEY_F2,keys);
}
static int mouse_capable(int fd)
{
    unsigned long ev[NBITS(EV_MAX+1)],rel[NBITS(REL_MAX+1)];
    memset(ev,0,sizeof(ev));memset(rel,0,sizeof(rel));
    if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)),ev)<0||!TBIT(EV_REL,ev)) return 0;
    if(ioctl(fd,EVIOCGBIT(EV_REL,sizeof(rel)),rel)<0) return 0;
    return TBIT(REL_X,rel)&&TBIT(REL_Y,rel);
}
static void scan_input(int *fds,int *n,int *mouse)
{
    int i;
    *n=0;*mouse=0;
    for(i=0;i<MAX_EVENTS;++i){
        char p[64];int fd;
        snprintf(p,sizeof(p),"/dev/input/event%d",i);
        fd=open(p,O_RDONLY|O_NONBLOCK);
        if(fd<0) continue;
        if(keyboard_capable(fd)&&*n<MAX_KBDS){fds[*n]=fd;(*n)++;continue;}
        if(mouse_capable(fd))*mouse=1;
        close(fd);
    }
}
static int wait_f2(int *fds,int n,int ms)
{
    struct pollfd p[MAX_KBDS];
    int i,rc;
    for(i=0;i<n;++i){p[i].fd=fds[i];p[i].events=POLLIN;p[i].revents=0;}
    if(n<=0){usleep((unsigned)ms*1000U);return 0;}
    rc=poll(p,(nfds_t)n,ms);
    if(rc<=0)return 0;
    for(i=0;i<n;++i) if(p[i].revents&POLLIN){
        struct input_event e[16];ssize_t got=read(p[i].fd,e,sizeof(e));int k,c;
        if(got<=0)continue;c=(int)(got/(ssize_t)sizeof(struct input_event));
        for(k=0;k<c;++k) if(e[k].type==EV_KEY&&e[k].code==KEY_F2&&e[k].value==1) return 2;
    }
    return 0;
}
static unsigned mem_mb(void)
{
    FILE *f=fopen("/proc/meminfo","r");char line[128];unsigned long kb=0;
    if(!f)return 0;
    while(fgets(line,sizeof(line),f)) if(sscanf(line,"MemTotal: %lu kB",&kb)==1)break;
    fclose(f);return (unsigned)(kb/1024UL);
}
static void status(struct fbctx *fb,int y,const char *n,int ok,uint32_t w)
{
    char b[96];snprintf(b,sizeof(b),"%-20s [%s]",n,ok?"OK":"WARN");draw_text(fb,48,y,2,b,w);
}
static void info(struct fbctx *fb,int y,const char *n,const char *v,uint32_t w)
{
    char b[112];snprintf(b,sizeof(b),"%-16s %s",n,v);draw_text(fb,48,y,2,b,w);
}
static void header(struct fbctx *fb,uint32_t k,uint32_t w)
{
    clearfb(fb,k);draw_text(fb,48,30,3,"STAYPLAYTION FIRMWARE 6.8.1",w);
}
int main(int argc,char **argv)
{
    struct fbctx fb;uint32_t black,white;const char *mode="menu";int ms=5000;
    if(argc>1)mode=argv[1];if(argc>2)ms=atoi(argv[2]);
    if(fbopen_ctx(&fb)<0)return 10;
    force_opaque(&fb);black=rgb(&fb,0,0,0);white=rgb(&fb,255,255,255);header(&fb,black,white);

    if(!strcmp(mode,"menu")){
        int fds[MAX_KBDS],n=0,mouse=0,i,rc;char m[32],v[64];
        scan_input(fds,&n,&mouse);
        snprintf(m,sizeof(m),"%u MB",mem_mb());
        snprintf(v,sizeof(v),"%ux%u %u BPP",fb.v.xres,fb.v.yres,fb.v.bits_per_pixel);
        draw_text(&fb,48,72,2,"HI3531 FIRMWARE ROOT",white);
        info(&fb,120,"CPU","HISILICON HI3531 V100",white);
        info(&fb,150,"MEMORY",m,white);
        info(&fb,180,"VIDEO",v,white);
        status(&fb,235,"KERNEL DRIVERS",1,white);
        status(&fb,265,"FRAMEBUFFER",1,white);
        status(&fb,295,"USB KEYBOARD",n>0,white);
        status(&fb,325,"USB MOUSE",mouse,white);
        status(&fb,355,"FIRMWARE ROOT",1,white);
        draw_text(&fb,48,420,2,"F2  RESCUE MONITOR",white);
        draw_text(&fb,48,450,2,"AUTO BOOT EXTERNAL OS",white);
        rc=wait_f2(fds,n,ms);
        for(i=0;i<n;++i)close(fds[i]);
        fbclose_ctx(&fb);return rc;
    }

    if(!strcmp(mode,"handoff")){
        draw_text(&fb,48,105,2,"EXTERNAL STAYPLAYTION OS FOUND",white);
        status(&fb,175,"ROOT FILESYSTEM",1,white);
        status(&fb,205,"DEVICE HANDOFF",1,white);
        draw_text(&fb,48,280,2,"TRANSFERRING ROOT FILESYSTEM",white);
    } else if(!strcmp(mode,"success")){
        draw_text(&fb,48,105,2,"STAYPLAYTION OS",white);
        draw_text(&fb,48,165,3,"EXTERNAL ROOT ACTIVE",white);
        status(&fb,245,"FIRMWARE HANDOFF",1,white);
        draw_text(&fb,48,315,2,"STAGE 6.8.1 ROOT TEST PASSED",white);
    } else {
        draw_text(&fb,48,105,2,"NO COMPATIBLE EXTERNAL OS",white);
        draw_text(&fb,48,165,2,"ENTERING RESCUE MONITOR",white);
    }

    if(ms>0)usleep((unsigned)ms*1000U);
    fbclose_ctx(&fb);return 0;
}
