/*
 * Stayplaytion Space3D Stage 2
 * Native Hi3531 third-person 3D exploration prototype.
 *
 * Renderer: small3dlib (CC0) + native HIFB A1R5G5B5 output.
 * Player: true 3D transform, yaw/pitch/roll, forward motion, chase camera.
 * World: CC0 OBJ ship, station, rotating panel, asteroids, faceted planet.
 * Input: keyboard + every gamepad-capable evdev node (multi-interface safe).
 *
 * No OpenGL/SDL/X11 is required while the native lease is active.
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

#define RENDER_W 800
#define RENDER_H 450
#define OUTPUT_W 1280
#define OUTPUT_H 720
#define H3531_FBIOGET_VBLANK_HIFB 0x00004664UL
#define MAX_PAD_NODES 12
#define STAR_COUNT 700

static void draw3d_pixel();

#define S3L_RESOLUTION_X RENDER_W
#define S3L_RESOLUTION_Y RENDER_H
#define S3L_PIXEL_FUNCTION draw3d_pixel
#define S3L_Z_BUFFER 2
#define S3L_REDUCED_Z_BUFFER_GRANULARITY 4
#define S3L_SORT 0
#define S3L_STENCIL_BUFFER 0
#define S3L_PERSPECTIVE_CORRECTION 0
#define S3L_COMPUTE_DEPTH 1
#define S3L_NEAR_CROSS_STRATEGY 0
#define S3L_USE_WIDER_TYPES 0
#include "vendor/small3dlib.h"
#include "cc0_ship_model.h"

#define U S3L_F

typedef struct {
    int fd;
    uint8_t *mem;
    size_t len;
    unsigned stride;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    uint16_t *canvas;
    uint16_t *base;
    uint16_t *scaled_row;
    uint16_t xmap[OUTPUT_W];
    uint16_t ymap[OUTPUT_H];
    int vblank_state;
} video_t;

typedef struct {
    int fd;
    char path[64];
    char name[128];
    unsigned long absbits[(ABS_MAX + (8*sizeof(unsigned long))) / (8*sizeof(unsigned long))];
    struct input_absinfo absinfo[ABS_MAX + 1];
    uint8_t have_abs[ABS_MAX + 1];
    int16_t axis[ABS_MAX + 1];
    uint8_t key_down[KEY_MAX + 1];
} pad_node_t;

typedef struct {
    int kfd;
    int left,right,up,down;
    pad_node_t pads[MAX_PAD_NODES];
    int pad_count;
    int ctrl_x,ctrl_y;
    int start_down,select_down;
} input_t;

typedef struct {
    S3L_Unit x,y,z;
    uint8_t br;
} star_t;

static volatile sig_atomic_t g_stop=0;
static uint16_t *g_canvas=NULL;
static uint32_t g_frame=0;
static S3L_Scene g_scene;
static S3L_Model3D g_models[10];
static star_t g_stars[STAR_COUNT];
static uint32_t g_rng=0x35313531U;
static int g_panel_near=0;

enum {
    MODEL_SHIP=0,
    MODEL_PANEL=1,
    MODEL_PANEL_FRAME=2,
    MODEL_STATION=3,
    MODEL_RING=4,
    MODEL_ASTEROID0=5,
    MODEL_ASTEROID1=6,
    MODEL_ASTEROID2=7,
    MODEL_PLANET=8,
    MODEL_BEACON=9
};

static uint32_t xrnd(void)
{
    uint32_t x=g_rng;
    x^=x<<13; x^=x>>17; x^=x<<5;
    g_rng=x;
    return x;
}

static uint16_t pack1555(unsigned r,unsigned g,unsigned b)
{
    if(r>255) r=255; if(g>255) g=255; if(b>255) b=255;
    return (uint16_t)(0x8000U|((r>>3)<<10)|((g>>3)<<5)|(b>>3));
}

static void on_signal(int sig){(void)sig;g_stop=1;}

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}

static void sleep_ns(uint64_t ns)
{
    struct timespec r;
    r.tv_sec=(time_t)(ns/1000000000ULL);
    r.tv_nsec=(long)(ns%1000000000ULL);
    while(nanosleep(&r,&r)<0 && errno==EINTR && !g_stop){}
}

static void putpx(int x,int y,uint16_t c)
{
    if((unsigned)x<RENDER_W && (unsigned)y<RENDER_H)
        g_canvas[(size_t)y*RENDER_W+x]=c;
}

static void hline(int x0,int x1,int y,uint16_t c)
{
    int x;
    if((unsigned)y>=RENDER_H) return;
    if(x0>x1){int t=x0;x0=x1;x1=t;}
    if(x1<0||x0>=RENDER_W) return;
    if(x0<0)x0=0; if(x1>=RENDER_W)x1=RENDER_W-1;
    for(x=x0;x<=x1;++x) putpx(x,y,c);
}

static void vline(int x,int y0,int y1,uint16_t c)
{
    int y;
    if((unsigned)x>=RENDER_W) return;
    if(y0>y1){int t=y0;y0=y1;y1=t;}
    if(y1<0||y0>=RENDER_H) return;
    if(y0<0)y0=0; if(y1>=RENDER_H)y1=RENDER_H-1;
    for(y=y0;y<=y1;++y) putpx(x,y,c);
}

static void build_background(video_t *v)
{
    int x,y;
    for(y=0;y<RENDER_H;++y){
        for(x=0;x<RENDER_W;++x){
            int r=2,g=5,b=13;
            int dx=x-610,dy=y-105;
            int d=(dx*dx)/9+(dy*dy)/4;
            int neb=0;
            if(d<36000) neb=(36000-d)/1800;
            r+=neb/2; g+=neb; b+=neb*2;
            dx=x-160;dy=y-310;
            d=(dx*dx)/7+(dy*dy)/5;
            if(d<25000){
                int n=(25000-d)/1900;
                r+=n; g+=n/3; b+=n*2;
            }
            /* very cheap deterministic fine grain, precomputed once */
            {
                unsigned n=(unsigned)((x*1103515245u+y*2654435761u)>>27)&3u;
                b+=(int)n;
            }
            v->base[(size_t)y*RENDER_W+x]=pack1555((unsigned)r,(unsigned)g,(unsigned)b);
        }
    }
}

static int video_open(video_t *v)
{
    size_t fallback;
    int x,y;
    memset(v,0,sizeof(*v));
    v->fd=-1;

    v->fd=open("/dev/fb0",O_RDWR);
    if(v->fd<0){fprintf(stderr,"[space3d] open fb: %s\n",strerror(errno));return -1;}
    if(ioctl(v->fd,FBIOGET_FSCREENINFO,&v->fix)<0 ||
       ioctl(v->fd,FBIOGET_VSCREENINFO,&v->var)<0){
        fprintf(stderr,"[space3d] fb info: %s\n",strerror(errno));return -1;
    }
    v->stride=v->fix.line_length;
    fallback=(size_t)v->stride*(v->var.yres_virtual?v->var.yres_virtual:v->var.yres);
    v->len=v->fix.smem_len?v->fix.smem_len:fallback;

    if(v->var.xres!=OUTPUT_W||v->var.yres!=OUTPUT_H||
       v->var.bits_per_pixel!=16||v->stride<OUTPUT_W*2U){
        fprintf(stderr,"[space3d] unsupported fb %ux%u %ubpp stride=%u\n",
            v->var.xres,v->var.yres,v->var.bits_per_pixel,v->stride);
        return -1;
    }

    v->mem=(uint8_t*)mmap(NULL,v->len,PROT_READ|PROT_WRITE,MAP_SHARED,v->fd,0);
    if(v->mem==MAP_FAILED){v->mem=NULL;return -1;}

    v->canvas=(uint16_t*)malloc((size_t)RENDER_W*RENDER_H*2U);
    v->base=(uint16_t*)malloc((size_t)RENDER_W*RENDER_H*2U);
    v->scaled_row=(uint16_t*)malloc((size_t)OUTPUT_W*2U);
    if(!v->canvas||!v->base||!v->scaled_row) return -1;

    for(x=0;x<OUTPUT_W;++x)
        v->xmap[x]=(uint16_t)((uint32_t)x*RENDER_W/OUTPUT_W);
    for(y=0;y<OUTPUT_H;++y)
        v->ymap[y]=(uint16_t)((uint32_t)y*RENDER_H/OUTPUT_H);

    build_background(v);
    g_canvas=v->canvas;
    fprintf(stderr,"[space3d] HIFB ready %ux%u <- %ux%u\n",
        OUTPUT_W,OUTPUT_H,RENDER_W,RENDER_H);
    return 0;
}

static void video_close(video_t *v)
{
    if(v->mem) munmap(v->mem,v->len);
    if(v->fd>=0) close(v->fd);
    free(v->canvas);free(v->base);free(v->scaled_row);
    memset(v,0,sizeof(*v));v->fd=-1;g_canvas=NULL;
}

static void video_present(video_t *v)
{
    int oy,ox,last_sy=-1;
    if(v->vblank_state>=0){
        errno=0;
        if(ioctl(v->fd,H3531_FBIOGET_VBLANK_HIFB,0)==0){
            if(v->vblank_state==0) fprintf(stderr,"[space3d] HIFB vblank 0x4664 active\n");
            v->vblank_state=1;
        }else{
            if(v->vblank_state==0) fprintf(stderr,"[space3d] vblank unavailable errno=%d\n",errno);
            v->vblank_state=-1;
        }
    }

    for(oy=0;oy<OUTPUT_H;++oy){
        int sy=v->ymap[oy];
        if(sy!=last_sy){
            const uint16_t *src=v->canvas+(size_t)sy*RENDER_W;
            for(ox=0;ox<OUTPUT_W;++ox) v->scaled_row[ox]=src[v->xmap[ox]];
            last_sy=sy;
        }
        memcpy(v->mem+(size_t)oy*v->stride,v->scaled_row,OUTPUT_W*2U);
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

/* ---------- all-event-node gamepad transport ---------- */

#define BPL (8U*(unsigned)sizeof(unsigned long))
#define NBITS(n) (((unsigned)(n)+BPL-1U)/BPL)
#define TBIT(bit,a) (((a)[(unsigned)(bit)/BPL]>>((unsigned)(bit)%BPL))&1UL)

static int16_t scale_abs(const struct input_absinfo *i,int v)
{
    int64_t mn=i->minimum,mx=i->maximum,c=(mn+mx)/2;
    if(mx<=mn) return 0;
    if(v<(int)c){
        int64_t d=c-mn,o=((int64_t)v-c)*32768;
        if(d<=0)return 0;o/=d;if(o<-32768)o=-32768;return(int16_t)o;
    }else{
        int64_t d=mx-c,o=((int64_t)v-c)*32767;
        if(d<=0)return 0;o/=d;if(o>32767)o=32767;return(int16_t)o;
    }
}

static int pad_capable(int fd)
{
    unsigned long ev[NBITS(EV_MAX+1)],key[NBITS(KEY_MAX+1)],ab[NBITS(ABS_MAX+1)];
    int buttons=0,axes=0,code;
    memset(ev,0,sizeof(ev));memset(key,0,sizeof(key));memset(ab,0,sizeof(ab));
    if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)),ev)<0) return 0;
    if(TBIT(EV_KEY,ev)) ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(key)),key);
    if(TBIT(EV_ABS,ev)) ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(ab)),ab);
    for(code=BTN_JOYSTICK;code<=KEY_MAX;++code) if(TBIT(code,key)) buttons++;
    for(code=0;code<=ABS_MAX;++code) if(TBIT(code,ab)) axes++;
    if((TBIT(ABS_X,ab)&&TBIT(ABS_Y,ab)) ||
       (TBIT(ABS_RX,ab)&&TBIT(ABS_RY,ab)) ||
       (TBIT(ABS_HAT0X,ab)&&TBIT(ABS_HAT0Y,ab)))
        return axes>=2;
    return buttons>=2 && axes>=2;
}

static void input_scan_pads(input_t *in)
{
    int n;
    const char *kbd=getenv("H3531_NATIVE_KEYBOARD");
    for(n=0;n<64 && in->pad_count<MAX_PAD_NODES;++n){
        char p[64],name[128];
        int fd,code;
        pad_node_t *node;
        snprintf(p,sizeof(p),"/dev/input/event%d",n);
        if(kbd&&strcmp(kbd,p)==0) continue;
        fd=open(p,O_RDONLY|O_NONBLOCK);
        if(fd<0) continue;
        if(!pad_capable(fd)){close(fd);continue;}

        node=&in->pads[in->pad_count++];
        memset(node,0,sizeof(*node));node->fd=fd;
        snprintf(node->path,sizeof(node->path),"%s",p);
        memset(name,0,sizeof(name));
        if(ioctl(fd,EVIOCGNAME(sizeof(name)-1),name)<0) snprintf(name,sizeof(name),"event%d",n);
        snprintf(node->name,sizeof(node->name),"%s",name);
        ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(node->absbits)),node->absbits);
        for(code=0;code<=ABS_MAX;++code){
            if(TBIT(code,node->absbits) && ioctl(fd,EVIOCGABS(code),&node->absinfo[code])==0){
                node->have_abs[code]=1;
                node->axis[code]=scale_abs(&node->absinfo[code],node->absinfo[code].value);
            }
        }
        fprintf(stderr,"[space3d] gamepad node %s name=%s\n",node->path,node->name);
    }
}

static void input_open(input_t *in)
{
    const char *kbd=getenv("H3531_NATIVE_KEYBOARD");
    int i;
    memset(in,0,sizeof(*in));in->kfd=-1;
    for(i=0;i<MAX_PAD_NODES;++i) in->pads[i].fd=-1;
    if(kbd&&*kbd) in->kfd=open(kbd,O_RDONLY|O_NONBLOCK);
    input_scan_pads(in);
    fprintf(stderr,"[space3d] keyboard=%s fd=%d gamepad_nodes=%d\n",
        (kbd&&*kbd)?kbd:"<none>",in->kfd,in->pad_count);
}

static void input_close(input_t *in)
{
    int i;
    if(in->kfd>=0) close(in->kfd);
    for(i=0;i<in->pad_count;++i) if(in->pads[i].fd>=0) close(in->pads[i].fd);
}

static void input_poll(input_t *in)
{
    int i,bestx=0,besty=0;
    if(in->kfd>=0){
        struct input_event e;
        while(read(in->kfd,&e,sizeof(e))==(ssize_t)sizeof(e)){
            int d=e.value!=0;
            if(e.type!=EV_KEY) continue;
            if(e.code==KEY_LEFT)in->left=d;
            else if(e.code==KEY_RIGHT)in->right=d;
            else if(e.code==KEY_UP)in->up=d;
            else if(e.code==KEY_DOWN)in->down=d;
            else if((e.code==KEY_ESC||e.code==KEY_F12)&&d)g_stop=1;
        }
    }

    in->start_down=0;in->select_down=0;
    for(i=0;i<in->pad_count;++i){
        pad_node_t *p=&in->pads[i];
        struct input_event e;
        while(read(p->fd,&e,sizeof(e))==(ssize_t)sizeof(e)){
            if(e.type==EV_ABS && e.code<=ABS_MAX && p->have_abs[e.code])
                p->axis[e.code]=scale_abs(&p->absinfo[e.code],e.value);
            else if(e.type==EV_KEY && e.code<=KEY_MAX)
                p->key_down[e.code]=(uint8_t)(e.value!=0);
        }

        /* Accept either stick pair on any interface. This is deliberate for
         * composite/twin USB pads whose sticks may live on separate eventN. */
        if(p->have_abs[ABS_X]&&p->have_abs[ABS_Y]){
            if(abs(p->axis[ABS_X])>abs(bestx)) bestx=p->axis[ABS_X];
            if(abs(p->axis[ABS_Y])>abs(besty)) besty=p->axis[ABS_Y];
        }
        if(p->have_abs[ABS_RX]&&p->have_abs[ABS_RY]){
            if(abs(p->axis[ABS_RX])>abs(bestx)) bestx=p->axis[ABS_RX];
            if(abs(p->axis[ABS_RY])>abs(besty)) besty=p->axis[ABS_RY];
        }
        if(p->have_abs[ABS_HAT0X] && abs(p->axis[ABS_HAT0X])>12000) bestx=p->axis[ABS_HAT0X];
        if(p->have_abs[ABS_HAT0Y] && abs(p->axis[ABS_HAT0Y])>12000) besty=p->axis[ABS_HAT0Y];

        if(p->key_down[BTN_DPAD_LEFT]||p->key_down[KEY_LEFT]) bestx=-32768;
        if(p->key_down[BTN_DPAD_RIGHT]||p->key_down[KEY_RIGHT]) bestx=32767;
        if(p->key_down[BTN_DPAD_UP]||p->key_down[KEY_UP]) besty=-32768;
        if(p->key_down[BTN_DPAD_DOWN]||p->key_down[KEY_DOWN]) besty=32767;
        if(p->key_down[BTN_START])in->start_down=1;
        if(p->key_down[BTN_SELECT])in->select_down=1;
    }

    if(in->left)bestx=-32768;if(in->right)bestx=32767;
    if(in->up)besty=-32768;if(in->down)besty=32767;
    if(abs(bestx)<5000)bestx=0;if(abs(besty)<5000)besty=0;
    in->ctrl_x=bestx;in->ctrl_y=besty;
    if(in->start_down&&in->select_down)g_stop=1;
}

/* ---------- models ---------- */

static const S3L_Unit cubeV[]={
    -U,-U,-U, U,-U,-U, U,U,-U, -U,U,-U,
    -U,-U,U,  U,-U,U,  U,U,U,  -U,U,U
};
static const S3L_Index cubeT[]={
    0,2,1,0,3,2, 4,5,6,4,6,7,
    0,1,5,0,5,4, 3,7,6,3,6,2,
    1,2,6,1,6,5, 0,4,7,0,7,3
};

static const S3L_Unit panelV[]={
    -U,-U/2,0, U,-U/2,0, U,U/2,0, -U,U/2,0
};
static const S3L_Index panelT[]={0,1,2,0,2,3};

static const S3L_Unit octV[]={
    0,U,0, U,0,0, 0,0,U, -U,0,0, 0,0,-U, 0,-U,0
};
static const S3L_Index octT[]={
    0,2,1,0,3,2,0,4,3,0,1,4,
    5,1,2,5,2,3,5,3,4,5,4,1
};

#define RING_SEG 24
static S3L_Unit ringV[RING_SEG*2*3];
static S3L_Index ringT[RING_SEG*4*3];

#define SP_LAT 8
#define SP_LON 16
static S3L_Unit sphereV[(SP_LAT+1)*SP_LON*3];
static S3L_Index sphereT[SP_LAT*SP_LON*2*3];

static void build_ring_mesh(void)
{
    int i,t=0;
    for(i=0;i<RING_SEG;++i){
        S3L_Unit a=(S3L_Unit)(i*S3L_F/RING_SEG);
        S3L_Unit cs=S3L_cos(a),sn=S3L_sin(a);
        S3L_Unit r0=U*2,r1=U*3;
        ringV[(i*2+0)*3+0]=cs*r0/U;
        ringV[(i*2+0)*3+1]=sn*r0/U;
        ringV[(i*2+0)*3+2]=0;
        ringV[(i*2+1)*3+0]=cs*r1/U;
        ringV[(i*2+1)*3+1]=sn*r1/U;
        ringV[(i*2+1)*3+2]=0;
    }
    for(i=0;i<RING_SEG;++i){
        int n=(i+1)%RING_SEG;
        S3L_Index a=(S3L_Index)(i*2),b=(S3L_Index)(i*2+1);
        S3L_Index c=(S3L_Index)(n*2),d=(S3L_Index)(n*2+1);
        ringT[t++]=a;ringT[t++]=b;ringT[t++]=d;
        ringT[t++]=a;ringT[t++]=d;ringT[t++]=c;
        /* duplicate reverse face so ring is visible from both sides */
        ringT[t++]=d;ringT[t++]=b;ringT[t++]=a;
        ringT[t++]=c;ringT[t++]=d;ringT[t++]=a;
    }
}

static void build_sphere_mesh(void)
{
    int lat,lon,t=0;
    for(lat=0;lat<=SP_LAT;++lat){
        S3L_Unit pitch=(S3L_Unit)(-S3L_F/4 + lat*(S3L_F/2)/SP_LAT);
        S3L_Unit cp=S3L_cos(pitch),sp=S3L_sin(pitch);
        for(lon=0;lon<SP_LON;++lon){
            S3L_Unit yaw=(S3L_Unit)(lon*S3L_F/SP_LON);
            S3L_Unit cy=S3L_cos(yaw),sy=S3L_sin(yaw);
            int idx=(lat*SP_LON+lon)*3;
            sphereV[idx+0]=cp*sy/U;
            sphereV[idx+1]=sp;
            sphereV[idx+2]=cp*cy/U;
        }
    }
    for(lat=0;lat<SP_LAT;++lat){
        for(lon=0;lon<SP_LON;++lon){
            int n=(lon+1)%SP_LON;
            S3L_Index a=(S3L_Index)(lat*SP_LON+lon);
            S3L_Index b=(S3L_Index)(lat*SP_LON+n);
            S3L_Index c=(S3L_Index)((lat+1)*SP_LON+lon);
            S3L_Index d=(S3L_Index)((lat+1)*SP_LON+n);
            sphereT[t++]=a;sphereT[t++]=c;sphereT[t++]=d;
            sphereT[t++]=a;sphereT[t++]=d;sphereT[t++]=b;
        }
    }
}

static void set_transform(S3L_Model3D *m,S3L_Unit x,S3L_Unit y,S3L_Unit z,
    S3L_Unit sx,S3L_Unit sy,S3L_Unit sz)
{
    S3L_transform3DInit(&m->transform);
    m->transform.translation.x=x;m->transform.translation.y=y;m->transform.translation.z=z;
    m->transform.scale.x=sx;m->transform.scale.y=sy;m->transform.scale.z=sz;
}

static void world_init(void)
{
    build_ring_mesh();build_sphere_mesh();
    cc0ShipModelInit();
    g_models[MODEL_SHIP]=cc0ShipModel;
    S3L_model3DInit(panelV,4,panelT,2,&g_models[MODEL_PANEL]);
    S3L_model3DInit(cubeV,8,cubeT,12,&g_models[MODEL_PANEL_FRAME]);
    S3L_model3DInit(cubeV,8,cubeT,12,&g_models[MODEL_STATION]);
    S3L_model3DInit(ringV,RING_SEG*2,ringT,RING_SEG*4,&g_models[MODEL_RING]);
    S3L_model3DInit(octV,6,octT,8,&g_models[MODEL_ASTEROID0]);
    S3L_model3DInit(octV,6,octT,8,&g_models[MODEL_ASTEROID1]);
    S3L_model3DInit(octV,6,octT,8,&g_models[MODEL_ASTEROID2]);
    S3L_model3DInit(sphereV,(SP_LAT+1)*SP_LON,sphereT,SP_LAT*SP_LON*2,&g_models[MODEL_PLANET]);
    S3L_model3DInit(cubeV,8,cubeT,12,&g_models[MODEL_BEACON]);

    set_transform(&g_models[MODEL_SHIP],0,0,0,U,U,U);
    set_transform(&g_models[MODEL_PANEL],U*6,U*1,U*24,U*4,U*4,U*4);
    set_transform(&g_models[MODEL_PANEL_FRAME],U*6,U*1,U*24+U/4,U*5,U*3,U/5);
    set_transform(&g_models[MODEL_STATION],-U*10,U*1,U*42,U*3,U*3,U*5);
    set_transform(&g_models[MODEL_RING],-U*10,U*1,U*42,U*2,U*2,U*2);
    set_transform(&g_models[MODEL_ASTEROID0],U*4,-U*2,U*33,U*2,U*3,U*2);
    set_transform(&g_models[MODEL_ASTEROID1],-U*5,U*5,U*52,U*3,U*2,U*2);
    set_transform(&g_models[MODEL_ASTEROID2],U*10,U*3,U*66,U*4,U*3,U*5);
    set_transform(&g_models[MODEL_PLANET],U*30,U*8,U*105,U*18,U*18,U*18);
    set_transform(&g_models[MODEL_BEACON],0,U*6,U*78,U/2,U*8,U/2);

    g_models[MODEL_PANEL].config.backfaceCulling=0;
    g_models[MODEL_RING].config.backfaceCulling=0;

    S3L_sceneInit(g_models,10,&g_scene);
    g_scene.camera.focalLength=U*2;
}

static void stars_init(void)
{
    int i;
    for(i=0;i<STAR_COUNT;++i){
        g_stars[i].x=((int)(xrnd()%240U)-120)*U;
        g_stars[i].y=((int)(xrnd()%120U)-60)*U;
        g_stars[i].z=((int)(xrnd()%170U)+8)*U;
        g_stars[i].br=(uint8_t)(100+(xrnd()%156U));
    }
}

static void stars_draw(S3L_Unit ship_z)
{
    int i;
    for(i=0;i<STAR_COUNT;++i){
        S3L_Vec4 p,q;
        while(g_stars[i].z<ship_z-U*10) g_stars[i].z+=U*180;
        p.x=g_stars[i].x;p.y=g_stars[i].y;p.z=g_stars[i].z;p.w=U/32;
        S3L_project3DPointToScreen(p,g_scene.camera,&q);
        if(q.w>0 && q.x>=0&&q.x<RENDER_W&&q.y>=0&&q.y<RENDER_H){
            unsigned br=g_stars[i].br;
            /* exactly one render pixel: never cross-shaped, never snowflake */
            putpx((int)q.x,(int)q.y,pack1555(br,br,(br+255)/2));
        }
    }
}

/* ---------- shading ---------- */

static inline void draw3d_pixel(S3L_PixelInfo *p)
{
    unsigned r=120,g=150,b=180;
    unsigned facet=(unsigned)((p->triangleIndex*37U+p->modelIndex*53U)&63U);
    unsigned fog;
    switch(p->modelIndex){
        case MODEL_SHIP:
            r=105+facet;g=155+facet/2;b=190+facet;
            break;
        case MODEL_PANEL:
            /* emissive pseudo-video placeholder */
            r=10+((p->x+g_frame*2U)&31U);
            g=110+((p->y*3U+g_frame*4U)&95U);
            b=170+((p->x+p->y+g_frame*5U)&63U);
            if(((p->y+(int)g_frame)&7)==0){r/=2;g/=2;b/=2;}
            break;
        case MODEL_PANEL_FRAME:
            r=18;g=95+facet;b=125+facet;
            break;
        case MODEL_STATION:
        case MODEL_RING:
            r=75+facet;g=88+facet;b=100+facet;
            break;
        case MODEL_ASTEROID0:
        case MODEL_ASTEROID1:
        case MODEL_ASTEROID2:
            r=70+facet;g=58+facet/2;b=48+facet/3;
            break;
        case MODEL_PLANET:
            r=20+facet/3;g=55+facet;b=105+facet*2;
            break;
        case MODEL_BEACON:
            r=220;g=100+facet;b=25;
            break;
        default: break;
    }
    if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;

    fog=(unsigned)(p->depth>0?p->depth:0);
    fog=fog/(U*2);
    if(fog>150)fog=150;
    r=(r*(255-fog)+3*fog)/255;
    g=(g*(255-fog)+6*fog)/255;
    b=(b*(255-fog)+15*fog)/255;
    putpx(p->x,p->y,pack1555(r,g,b));
}

static S3L_Unit dist_manhattan3(S3L_Vec4 a,S3L_Vec4 b)
{
    return S3L_abs(a.x-b.x)+S3L_abs(a.y-b.y)+S3L_abs(a.z-b.z);
}

static void draw_target_brackets(S3L_Vec4 world,uint16_t color)
{
    S3L_Vec4 q;
    int x,y,s=8;
    world.w=U/8;
    S3L_project3DPointToScreen(world,g_scene.camera,&q);
    if(q.w<=0)return;
    x=(int)q.x;y=(int)q.y;
    hline(x-s,x-3,y-s,color);vline(x-s,y-s,y-3,color);
    hline(x+3,x+s,y-s,color);vline(x+s,y-s,y-3,color);
    hline(x-s,x-3,y+s,color);vline(x-s,y+3,y+s,color);
    hline(x+3,x+s,y+s,color);vline(x+s,y+3,y+s,color);
}

static void update_world(int cx,int cy)
{
    S3L_Model3D *ship=&g_models[MODEL_SHIP];
    S3L_Vec4 fwd,up;
    int target_yaw_rate=(cx*5)/32768;
    int target_pitch_rate=(cy*4)/32768;
    static int yaw_rate=0,pitch_rate=0;
    S3L_Unit target_roll;

    yaw_rate+=(target_yaw_rate-yaw_rate)/3;
    pitch_rate+=(target_pitch_rate-pitch_rate)/3;

    ship->transform.rotation.y=S3L_wrap(ship->transform.rotation.y+yaw_rate,U);
    ship->transform.rotation.x+=pitch_rate;
    if(ship->transform.rotation.x>U/7)ship->transform.rotation.x=U/7;
    if(ship->transform.rotation.x<-U/7)ship->transform.rotation.x=-U/7;

    target_roll=(S3L_Unit)(-(int64_t)cx*(U/12)/32768);
    ship->transform.rotation.z+=(target_roll-ship->transform.rotation.z)/6;

    S3L_rotationToDirections(ship->transform.rotation,U/9,&fwd,NULL,NULL);
    ship->transform.translation.x+=fwd.x;
    ship->transform.translation.y+=fwd.y;
    ship->transform.translation.z+=fwd.z;

    /* chase camera, true 3D direction vectors */
    S3L_rotationToDirections(ship->transform.rotation,U*5,&fwd,NULL,NULL);
    S3L_rotationToDirections(ship->transform.rotation,U*2,NULL,NULL,&up);
    g_scene.camera.transform.translation.x=ship->transform.translation.x-fwd.x+up.x;
    g_scene.camera.transform.translation.y=ship->transform.translation.y-fwd.y+up.y;
    g_scene.camera.transform.translation.z=ship->transform.translation.z-fwd.z+up.z;
    g_scene.camera.transform.rotation=ship->transform.rotation;
    g_scene.camera.transform.rotation.z/=2;

    /* object life */
    g_models[MODEL_PANEL].transform.rotation.y+=(S3L_Unit)1;
    g_models[MODEL_PANEL_FRAME].transform.rotation.y=g_models[MODEL_PANEL].transform.rotation.y;
    g_models[MODEL_RING].transform.rotation.z+=(S3L_Unit)2;
    g_models[MODEL_ASTEROID0].transform.rotation.x+=1;
    g_models[MODEL_ASTEROID0].transform.rotation.y+=2;
    g_models[MODEL_ASTEROID1].transform.rotation.z+=1;
    g_models[MODEL_ASTEROID2].transform.rotation.y-=1;
    g_models[MODEL_PLANET].transform.rotation.y+=1;

    g_panel_near=dist_manhattan3(ship->transform.translation,
        g_models[MODEL_PANEL].transform.translation)<U*10;
}

static int selftest(void)
{
    uint16_t *tmp;
    unsigned i,nonzero=0;
    tmp=(uint16_t*)calloc((size_t)RENDER_W*RENDER_H,sizeof(uint16_t));
    if(!tmp)return 2;
    g_canvas=tmp;
    world_init();
    stars_init();
    g_scene.camera.transform.translation.z=-U*5;
    S3L_newFrame();
    S3L_drawScene(g_scene);
    for(i=0;i<(unsigned)(RENDER_W*RENDER_H);++i) if(tmp[i]){nonzero++;if(nonzero>100)break;}
    free(tmp);g_canvas=NULL;
    if(CC0SHIP_VERTEX_COUNT<4||CC0SHIP_TRIANGLE_COUNT<2||nonzero<=100){
        fprintf(stderr,"SPACE3D_SELFTEST_FAIL shipV=%d shipT=%d pixels=%u\n",
            CC0SHIP_VERTEX_COUNT,CC0SHIP_TRIANGLE_COUNT,nonzero);
        return 3;
    }
    printf("SPACE3D_SELFTEST_OK shipV=%d shipT=%d pixels>%u\n",
        CC0SHIP_VERTEX_COUNT,CC0SHIP_TRIANGLE_COUNT,nonzero);
    return 0;
}

int main(int argc,char **argv)
{
    video_t v;
    input_t in;
    uint64_t perf,last;
    unsigned perf_frames=0;

    if(argc>1&&strcmp(argv[1],"--selftest")==0)return selftest();

    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGHUP,on_signal);
    if(video_open(&v)<0){video_close(&v);return 10;}
    input_open(&in);
    world_init();stars_init();

    perf=mono_ns();last=perf;
    while(!g_stop){
        uint64_t now;
        S3L_Vec4 panelCenter;

        input_poll(&in);
        update_world(in.ctrl_x,in.ctrl_y);

        memcpy(v.canvas,v.base,(size_t)RENDER_W*RENDER_H*2U);
        stars_draw(g_models[MODEL_SHIP].transform.translation.z);

        S3L_newFrame();
        S3L_drawScene(g_scene);

        panelCenter=g_models[MODEL_PANEL].transform.translation;
        if(g_panel_near) draw_target_brackets(panelCenter,pack1555(80,255,220));
        else draw_target_brackets(panelCenter,pack1555(30,115,135));

        video_present(&v);
        g_frame++;perf_frames++;
        now=mono_ns();

        if(perf_frames>=300){
            double sec=(double)(now-perf)/1000000000.0;
            fprintf(stderr,
                "[space3d] PERF fps=%.2f render=%dx%d pads=%d pos=%d,%d,%d panel_near=%d\n",
                sec>0.0?(double)perf_frames/sec:0.0,RENDER_W,RENDER_H,in.pad_count,
                (int)g_models[MODEL_SHIP].transform.translation.x,
                (int)g_models[MODEL_SHIP].transform.translation.y,
                (int)g_models[MODEL_SHIP].transform.translation.z,g_panel_near);
            perf=now;perf_frames=0;
        }

        if(v.vblank_state<0){
            uint64_t target=last+16666667ULL;
            if(now<target)sleep_ns(target-now);
            last=target;
            if(now>target+50000000ULL)last=now;
        }else last=now;
    }

    fprintf(stderr,"[space3d] exit frame=%u\n",g_frame);
    input_close(&in);video_close(&v);
    return 0;
}
