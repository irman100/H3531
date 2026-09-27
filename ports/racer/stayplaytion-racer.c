/*
 * Stayplaytion Racer Stage 6 - Hybrid 3D Track
 *
 * Native Hi3531 hybrid pseudo-3D + true low-poly 3D arcade racer.
 * No SDL/OpenGL/X11 while native framebuffer lease is active.
 *
 * Visual architecture:
 *   - 640x360 internal A1R5G5B5 render target
 *   - exact 2x present to 1280x720 HIFB
 *   - CPU0 game/render, CPU1 framebuffer presenter
 *   - OutRun-style projected road + true low-poly 3D cars/buildings
 *   - build-time packed CC0 sky/billboard art from OpenMRac-data
 *
 * Future:
 *   - TDE scaling/blit backend
 *   - H.264 VDEC/VO feeding selected billboard surfaces
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "racer_assets.h"

#define RW 640
#define RH 360
#define OW 1280
#define OH 720
#define HORIZON 118
#define H3531_FBIOGET_VBLANK_HIFB 0x00004664UL
#define MAX_PAD_NODES 12

#define TRACK_SEGMENTS 1400
#define SEG_LEN 180.0f
#define DRAW_DISTANCE 220
#define ROAD_WIDTH 1900.0f
#define CAMERA_HEIGHT 950.0f
#define CAMERA_DEPTH 0.86f
#define MAX_SPEED 90.0f
#define REVERSE_SPEED 36.0f
#define REVERSE_ACCEL 0.52f
#define ACCEL 0.78f
#define BRAKE 1.75f
#define DECEL 0.24f
#define OFFROAD_DECEL 1.05f
#define TARGET_FPS 60
#define FRAME_NS 16666667ULL
#define MAX_SIM_CATCHUP 4

typedef struct {
    int fd;
    uint8_t *mem;
    size_t len;
    unsigned stride;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    uint16_t *canvas[2];
    uint16_t *base;
    uint16_t *row2x;
    int vblank_state;
    pthread_t presenter;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t free_cv;
    int pending;
    int busy[2];
    int stop;
    unsigned presented;
} video_t;

#define BPL (8U*(unsigned)sizeof(unsigned long))
#define NBITS(n) (((unsigned)(n)+BPL-1U)/BPL)
#define TBIT(bit,a) (((a)[(unsigned)(bit)/BPL]>>((unsigned)(bit)%BPL))&1UL)

typedef struct {
    int fd;
    char path[64];
    char name[128];
    unsigned long absbits[NBITS(ABS_MAX+1)];
    struct input_absinfo absinfo[ABS_MAX+1];
    int center_raw[ABS_MAX+1];
    uint8_t have_abs[ABS_MAX+1];
    int16_t axis[ABS_MAX+1];
    uint8_t key_down[KEY_MAX+1];
    int sx_code, sy_code;
} pad_node_t;

typedef struct {
    int kfd;
    int left,right,up,down;
    int key_gas,key_brake;
    int gas,brake;
    int steer;
    int steer_node;
    int start_down,select_down;
    pad_node_t pads[MAX_PAD_NODES];
    int pad_count;
} input_t;

typedef struct {
    float curve;
    float y;
    unsigned flags;
} track_seg_t;

enum {
    TF_NONE=0,
    TF_BILLBOARD_L=1,
    TF_BILLBOARD_R=2,
    TF_GRANDSTAND_L=4,
    TF_GRANDSTAND_R=8,
    TF_TREES=16,
    TF_CITY=32,
    TF_FINISH=64
};

typedef struct {
    float x,y,w,scale;
    float world_x,world_y,z;
    int visible;
    int seg_index;
} proj_t;

typedef struct {
    float pos;
    float offset;
    float speed;
    float lane_phase;
    float wheel_spin;
    float steer_angle;
    int lane;
} traffic_t;

typedef struct { float x,y,z; } v3f_t;
typedef struct { float u,v; } v2f_t;
typedef struct { uint16_t a,b,c; uint8_t material; } tri3d_t;

#include "kenney_vehicle.h"
#include "sports_vehicle.h"
#include "track_texture.h"

typedef struct {
    float sx,sy,z;
    int valid;
} sv3_t;

typedef struct {
    float depth;
    int x0,y0,x1,y1,x2,y2;
    uint16_t color;
} drawtri_t;

typedef struct {
    float depth;
    int x0,y0,x1,y1,x2,y2;
    float u0,v0,u1,v1,u2,v2;
    float light;
} textri_t;


static volatile sig_atomic_t g_stop=0;
static uint16_t *g_canvas=NULL;
static track_seg_t g_track[TRACK_SEGMENTS];
static proj_t g_proj[DRAW_DISTANCE+1];
static traffic_t g_traffic[8];
static uint32_t g_frame=0;
static float g_position=0.0f;
static float g_speed=0.0f;
static float g_player_x=0.0f;
static float g_steer_visual=0.0f;
static float g_vehicle_heading=0.0f;
static float g_vehicle_slip=0.0f;
static float g_steer_angle=0.0f;
static float g_steer_fl=0.0f;
static float g_steer_fr=0.0f;
static float g_wheel_spin=0.0f;
static float g_body_roll=0.0f;
static float g_body_pitch=0.0f;
static float g_prev_speed=0.0f;
static int g_lap=1;



/* Compact true 3D car: lower body + cabin. Local axes: X right, Y up, Z forward. */
static const v3f_t g_car_v[]={
    {-260,0,-450},{260,0,-450},{260,0,450},{-260,0,450},
    {-260,210,-450},{260,210,-450},{260,210,450},{-260,210,450},
    {-165,210,-170},{165,210,-170},{165,210,235},{-165,210,235},
    {-140,410,-110},{140,410,-110},{140,410,170},{-140,410,170}
};

static const tri3d_t g_car_t[]={
    {0,1,5,0},{0,5,4,0},{1,2,6,1},{1,6,5,1},
    {2,3,7,0},{2,7,6,0},{3,0,4,1},{3,4,7,1},
    {4,5,6,0},{4,6,7,0},{0,3,2,2},{0,2,1,2},
    {8,9,13,3},{8,13,12,3},{9,10,14,3},{9,14,13,3},
    {10,11,15,3},{10,15,14,3},{11,8,12,3},{11,12,15,3},
    {12,13,14,4},{12,14,15,4},{8,11,10,0},{8,10,9,0}
};

static const tri3d_t g_box_t[]={
    {0,1,5,0},{0,5,4,0},{1,2,6,1},{1,6,5,1},
    {2,3,7,2},{2,7,6,2},{3,0,4,1},{3,4,7,1},
    {4,5,6,3},{4,6,7,3},{0,3,2,4},{0,2,1,4}
};

#define CAR_TRI_COUNT ((int)(sizeof(g_car_t)/sizeof(g_car_t[0])))
#define MAX_MESH_VERTS 4096
#define MAX_DRAW_TRIS 8192
static v3f_t g_mesh_rv[MAX_MESH_VERTS];
static sv3_t g_mesh_sv[MAX_MESH_VERTS];
static drawtri_t g_mesh_out[MAX_DRAW_TRIS];
static textri_t g_tex_out[MAX_DRAW_TRIS];
#if KENNEY_BODY_VERTEX_COUNT > MAX_MESH_VERTS
#error "Kenney body exceeds Racer mesh scratch budget"
#endif
#if KENNEY_BODY_TRIANGLE_COUNT > MAX_DRAW_TRIS
#error "Kenney body exceeds Racer triangle scratch budget"
#endif
#if SPORTS_BODY_VERTEX_COUNT > MAX_MESH_VERTS
#error "Sports body exceeds Racer mesh scratch budget"
#endif
#if SPORTS_BODY_TRIANGLE_COUNT > MAX_DRAW_TRIS
#error "Sports body exceeds Racer triangle scratch budget"
#endif

static uint16_t C_SKY,C_GRASS1,C_GRASS2,C_ROAD1,C_ROAD2,C_RUMBLE1,C_RUMBLE2,C_LANE,C_WHITE,C_BLACK,C_RED,C_BLUE,C_GLASS;

static uint16_t pack1555(unsigned r,unsigned g,unsigned b)
{
    if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;
    return (uint16_t)(0x8000U|((r>>3)<<10)|((g>>3)<<5)|(b>>3));
}

static void init_colors(void)
{
    C_SKY=pack1555(40,90,145);
    C_GRASS1=pack1555(32,112,58);
    C_GRASS2=pack1555(25,95,48);
    C_ROAD1=pack1555(58,61,66);
    C_ROAD2=pack1555(66,69,73);
    C_RUMBLE1=pack1555(235,235,225);
    C_RUMBLE2=pack1555(190,35,35);
    C_LANE=pack1555(235,220,165);
    C_WHITE=pack1555(240,240,240);
    C_BLACK=pack1555(8,10,12);
    C_RED=pack1555(220,45,35);
    C_BLUE=pack1555(35,105,215);
    C_GLASS=pack1555(60,150,205);
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
    struct timespec ts;
    ts.tv_sec=(time_t)(ns/1000000000ULL);
    ts.tv_nsec=(long)(ns%1000000000ULL);
    while(nanosleep(&ts,&ts)<0 && errno==EINTR){}
}

static void pace_until(uint64_t target)
{
    uint64_t now=mono_ns();
    if(now<target)sleep_ns(target-now);
}

static void pin_thread(int cpu,const char *name)
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(cpu,&set);
    if(pthread_setaffinity_np(pthread_self(),sizeof(set),&set)==0)
        fprintf(stderr,"[racer] %s pinned cpu=%d\n",name,cpu);
    else
        fprintf(stderr,"[racer] %s affinity unavailable\n",name);
#else
    (void)cpu;(void)name;
#endif
}

static void putpx(int x,int y,uint16_t c)
{
    if((unsigned)x<RW&&(unsigned)y<RH)g_canvas[(size_t)y*RW+x]=c;
}

static void hline(int x0,int x1,int y,uint16_t c)
{
    int x;
    if((unsigned)y>=RH)return;
    if(x0>x1){int t=x0;x0=x1;x1=t;}
    if(x1<0||x0>=RW)return;
    if(x0<0)x0=0;if(x1>=RW)x1=RW-1;
    for(x=x0;x<=x1;++x)g_canvas[(size_t)y*RW+x]=c;
}

static void fill_rect(int x,int y,int w,int h,uint16_t c)
{
    int yy;
    for(yy=0;yy<h;++yy)hline(x,x+w-1,y+yy,c);
}

static void line2(int x0,int y0,int x1,int y1,uint16_t c)
{
    int dx=abs(x1-x0),sx=x0<x1?1:-1;
    int dy=-abs(y1-y0),sy=y0<y1?1:-1;
    int err=dx+dy;
    for(;;){
        putpx(x0,y0,c);
        if(x0==x1&&y0==y1)break;
        {
            int e2=2*err;
            if(e2>=dy){err+=dy;x0+=sx;}
            if(e2<=dx){err+=dx;y0+=sy;}
        }
    }
}


static uint16_t shade1555(uint16_t c,float k)
{
    unsigned r=(c>>10)&31U,g=(c>>5)&31U,b=c&31U;
    if(k<0.18f)k=0.18f;if(k>1.25f)k=1.25f;
    r=(unsigned)(r*k);g=(unsigned)(g*k);b=(unsigned)(b*k);
    if(r>31)r=31;if(g>31)g=31;if(b>31)b=31;
    return (uint16_t)(0x8000U|(r<<10)|(g<<5)|b);
}

static void fill_tri2d(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t color)
{
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int area;
    int e0dx,e0dy,e1dx,e1dy,e2dx,e2dy;
    int row0,row1,row2;

    if(x1<minx)minx=x1;if(x2<minx)minx=x2;
    if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
    if(y1<miny)miny=y1;if(y2<miny)miny=y2;
    if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
    if(maxx<0||minx>=RW||maxy<0||miny>=RH)return;
    if(minx<0)minx=0;if(maxx>=RW)maxx=RW-1;
    if(miny<0)miny=0;if(maxy>=RH)maxy=RH-1;

    area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
    if(area==0)return;

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);

    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        if(area>0){
            for(x=minx;x<=maxx;++x){
                if(w0>=0&&w1>=0&&w2>=0)dst[x]=color;
                w0+=e0dx;w1+=e1dx;w2+=e2dx;
            }
        }else{
            for(x=minx;x<=maxx;++x){
                if(w0<=0&&w1<=0&&w2<=0)dst[x]=color;
                w0+=e0dx;w1+=e1dx;w2+=e2dx;
            }
        }
        row0+=e0dy;row1+=e1dy;row2+=e2dy;
    }
}

static void fill_tri_textured(
    int x0,int y0,float u0,float v0,
    int x1,int y1,float u1,float v1,
    int x2,int y2,float u2,float v2,
    float light,const uint16_t *texture,int tex_w,int tex_h)
{
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int area;
    int e0dx,e0dy,e1dx,e1dy,e2dx,e2dy;
    int row0,row1,row2;
    float inv_area,du_dx,du_dy,dv_dx,dv_dy,row_u,row_v;

    if(x1<minx)minx=x1;if(x2<minx)minx=x2;
    if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
    if(y1<miny)miny=y1;if(y2<miny)miny=y2;
    if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
    if(maxx<0||minx>=RW||maxy<0||miny>=RH)return;
    if(minx<0)minx=0;if(maxx>=RW)maxx=RW-1;
    if(miny<0)miny=0;if(maxy>=RH)maxy=RH-1;

    area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
    if(area==0)return;
    inv_area=1.0f/(float)area;

    du_dx=((u1-u0)*(float)(y2-y0)-(u2-u0)*(float)(y1-y0))*inv_area;
    du_dy=((u2-u0)*(float)(x1-x0)-(u1-u0)*(float)(x2-x0))*inv_area;
    dv_dx=((v1-v0)*(float)(y2-y0)-(v2-v0)*(float)(y1-y0))*inv_area;
    dv_dy=((v2-v0)*(float)(x1-x0)-(v1-v0)*(float)(x2-x0))*inv_area;

    row_u=u0+du_dx*((float)minx-x0)+du_dy*((float)miny-y0);
    row_v=v0+dv_dx*((float)minx-x0)+dv_dy*((float)miny-y0);

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);

    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        float uu=row_u,vv=row_v;
        uint16_t *dst=g_canvas+(size_t)y*RW;

        for(x=minx;x<=maxx;++x){
            int inside=(area>0)?(w0>=0&&w1>=0&&w2>=0):(w0<=0&&w1<=0&&w2<=0);
            if(inside){
                int tx=(int)(uu+0.5f),ty=(int)(vv+0.5f);
                uint16_t tex;
                if(tx<0)tx=0;if(tx>=tex_w)tx=tex_w-1;
                if(ty<0)ty=0;if(ty>=tex_h)ty=tex_h-1;
                tex=texture[ty*tex_w+tx];
                if(tex&0x8000U)dst[x]=shade1555(tex,light);
            }
            w0+=e0dx;w1+=e1dx;w2+=e2dx;
            uu+=du_dx;vv+=dv_dx;
        }

        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_u+=du_dy;row_v+=dv_dy;
    }
}

static int cmp_textri_far_first(const void *aa,const void *bb)
{
    const textri_t *a=(const textri_t*)aa,*b=(const textri_t*)bb;
    if(a->depth<b->depth)return 1;
    if(a->depth>b->depth)return -1;
    return 0;
}

static int cmp_drawtri_far_first(const void *aa,const void *bb)
{
    const drawtri_t *a=(const drawtri_t*)aa,*b=(const drawtri_t*)bb;
    if(a->depth<b->depth)return 1;
    if(a->depth>b->depth)return -1;
    return 0;
}

static uint16_t car_material_color(int material,int variant)
{
    static const uint16_t dummy=0;
    uint16_t body;
    (void)dummy;
    if(variant%4==0)body=pack1555(220,48,36);
    else if(variant%4==1)body=pack1555(38,105,220);
    else if(variant%4==2)body=pack1555(235,178,42);
    else body=pack1555(210,210,218);

    switch(material){
        case 0:return body;
        case 1:return shade1555(body,0.72f);
        case 2:return pack1555(24,26,30);
        case 3:return pack1555(48,108,138);
        case 4:return pack1555(92,165,195);
        default:return body;
    }
}

static void rotate_y(v3f_t in,float yaw,v3f_t *out)
{
    float cs=cosf(yaw),sn=sinf(yaw);
    out->x=in.x*cs-in.z*sn;
    out->y=in.y;
    out->z=in.x*sn+in.z*cs;
}

static void rotate_xyz(v3f_t in,float pitch,float yaw,float roll,v3f_t *out)
{
    float sx=sinf(pitch),cx=cosf(pitch);
    float sy=sinf(yaw),cy=cosf(yaw);
    float sz=sinf(roll),cz=cosf(roll);
    float x=in.x,y=in.y,z=in.z;
    float y1=y*cx-z*sx;
    float z1=y*sx+z*cx;
    float x2=x*cy-z1*sy;
    float z2=x*sy+z1*cy;
    out->x=x2*cz-y1*sz;
    out->y=x2*sz+y1*cz;
    out->z=z2;
}

static float approachf(float cur,float target,float step)
{
    float d=target-cur;
    if(d>step)return cur+step;
    if(d<-step)return cur-step;
    return target;
}

static int project_cam(float x,float y,float z,float camx,float camy,sv3_t *o)
{
    float s;
    if(z<35.0f){o->valid=0;return 0;}
    s=CAMERA_DEPTH/z;
    o->sx=RW*0.5f+s*(x-camx)*RW*0.5f;
    o->sy=RH*0.5f-s*(y-camy)*RH*0.5f;
    o->z=z;o->valid=1;
    return 1;
}

static void render_mesh3d(
    const v3f_t *verts,int vcount,const tri3d_t *tris,int tcount,
    float ox,float oy,float oz,float yaw,float scale,
    float camx,float camy,int variant,int is_car,uint16_t override_color)
{
    v3f_t *rv=g_mesh_rv;
    sv3_t *sv=g_mesh_sv;
    drawtri_t *out=g_mesh_out;
    int i,n=0;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_DRAW_TRIS)return;

    for(i=0;i<vcount;++i){
        v3f_t q,p=verts[i];
        p.x*=scale;p.y*=scale;p.z*=scale;
        rotate_y(p,yaw,&q);
        rv[i]=q;
        project_cam(ox+q.x,oy+q.y,oz+q.z,camx,camy,&sv[i]);
    }

    for(i=0;i<tcount;++i){
        const tri3d_t *t=&tris[i];
        v3f_t a=rv[t->a],b=rv[t->b],d=rv[t->c];
        float ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z;
        float vx=d.x-a.x,vy=d.y-a.y,vz=d.z-a.z;
        float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
        float mag=sqrtf(nx*nx+ny*ny+nz*nz);
        float light=0.72f;
        uint16_t base;

        if(!sv[t->a].valid||!sv[t->b].valid||!sv[t->c].valid)continue;
        if(mag>0.001f){
            nx/=mag;ny/=mag;nz/=mag;
            light=0.44f+0.42f*fabsf(nx*0.25f+ny*0.82f+nz*(-0.45f));
        }

        if(override_color)base=override_color;
        else if(is_car)base=car_material_color(t->material,variant);
        else{
            uint16_t wall=(variant&1)?pack1555(156,145,132):pack1555(112,130,150);
            switch(t->material){
                case 0:base=wall;break;
                case 1:base=shade1555(wall,0.72f);break;
                case 2:base=shade1555(wall,0.86f);break;
                case 3:base=shade1555(wall,1.08f);break;
                default:base=pack1555(42,48,56);break;
            }
        }

        out[n].depth=(sv[t->a].z+sv[t->b].z+sv[t->c].z)/3.0f;
        out[n].x0=(int)sv[t->a].sx;out[n].y0=(int)sv[t->a].sy;
        out[n].x1=(int)sv[t->b].sx;out[n].y1=(int)sv[t->b].sy;
        out[n].x2=(int)sv[t->c].sx;out[n].y2=(int)sv[t->c].sy;
        out[n].color=shade1555(base,light);
        n++;
    }

    qsort(out,(size_t)n,sizeof(out[0]),cmp_drawtri_far_first);
    for(i=0;i<n;++i)
        fill_tri2d(out[i].x0,out[i].y0,out[i].x1,out[i].y1,out[i].x2,out[i].y2,out[i].color);
}

static void queue_vehicle_part3d(
    const v3f_t *verts,const v2f_t *uvs,int vcount,
    const tri3d_t *tris,int tcount,
    v3f_t pivot,float part_steer,float wheel_spin,int is_wheel,
    float ox,float oy,float oz,
    float body_pitch,float body_yaw,float body_roll,float scale,
    float camx,float camy,int variant,int tex_w,int tex_h,int *queued)
{
    v3f_t *rv=g_mesh_rv;
    sv3_t *sv=g_mesh_sv;
    textri_t *out=g_tex_out;
    int i,n=*queued;
    (void)variant;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_DRAW_TRIS)return;
    if(n>=MAX_DRAW_TRIS)return;

    for(i=0;i<vcount;++i){
        v3f_t p=verts[i],q,r;
        p.x*=scale;p.y*=scale;p.z*=scale;

        if(is_wheel){
            rotate_xyz(p,wheel_spin,part_steer,0.0f,&q);
            q.x+=pivot.x*scale;
            q.y+=pivot.y*scale;
            q.z+=pivot.z*scale;
        }else{
            q=p;
        }

        rotate_xyz(q,body_pitch,body_yaw,body_roll,&r);
        rv[i]=r;
        project_cam(ox+r.x,oy+r.y,oz+r.z,camx,camy,&sv[i]);
    }

    for(i=0;i<tcount&&n<MAX_DRAW_TRIS;++i){
        const tri3d_t *t=&tris[i];
        v3f_t a=rv[t->a],b=rv[t->b],d=rv[t->c];
        float ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z;
        float vx=d.x-a.x,vy=d.y-a.y,vz=d.z-a.z;
        float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
        float mag=sqrtf(nx*nx+ny*ny+nz*nz);
        float light=0.76f;

        if(!sv[t->a].valid||!sv[t->b].valid||!sv[t->c].valid)continue;

        if(mag>0.001f){
            nx/=mag;ny/=mag;nz/=mag;
            light=0.48f+0.48f*fabsf(nx*0.24f+ny*0.84f+nz*(-0.42f));
        }

        out[n].depth=(sv[t->a].z+sv[t->b].z+sv[t->c].z)/3.0f;
        out[n].x0=(int)sv[t->a].sx;out[n].y0=(int)sv[t->a].sy;
        out[n].x1=(int)sv[t->b].sx;out[n].y1=(int)sv[t->b].sy;
        out[n].x2=(int)sv[t->c].sx;out[n].y2=(int)sv[t->c].sy;
        out[n].u0=uvs[t->a].u*(tex_w-1);
        out[n].v0=uvs[t->a].v*(tex_h-1);
        out[n].u1=uvs[t->b].u*(tex_w-1);
        out[n].v1=uvs[t->b].v*(tex_h-1);
        out[n].u2=uvs[t->c].u*(tex_w-1);
        out[n].v2=uvs[t->c].v*(tex_h-1);
        out[n].light=light;
        n++;
    }

    *queued=n;
}

static void render_kenney_vehicle(
    float ox,float oy,float oz,
    float body_pitch,float body_yaw,float body_roll,
    float steer_fl,float steer_fr,float wheel_spin,
    float scale,float camx,float camy,int variant)
{
    int i,n=0;

    /*
     * One depth queue for body + all four wheels.
     *
     * Earlier stages sorted each mesh separately and then drew wheels after the
     * body, so a wheel could overwrite body pixels and look visible through the
     * car. Every textured triangle now participates in the SAME far-to-near
     * ordering before a single raster pass.
     */
    queue_vehicle_part3d(kenney_body_v,kenney_body_uv,KENNEY_BODY_VERTEX_COUNT,
                         kenney_body_t,KENNEY_BODY_TRIANGLE_COUNT,
                         (v3f_t){0,0,0},0,0,0,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,variant,KENNEY_COLORMAP_W,KENNEY_COLORMAP_H,&n);

    queue_vehicle_part3d(kenney_wheel_rl_v,kenney_wheel_rl_uv,KENNEY_WHEEL_RL_VERTEX_COUNT,
                         kenney_wheel_rl_t,KENNEY_WHEEL_RL_TRIANGLE_COUNT,
                         kenney_wheel_rl_pivot,0,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,variant,KENNEY_COLORMAP_W,KENNEY_COLORMAP_H,&n);
    queue_vehicle_part3d(kenney_wheel_rr_v,kenney_wheel_rr_uv,KENNEY_WHEEL_RR_VERTEX_COUNT,
                         kenney_wheel_rr_t,KENNEY_WHEEL_RR_TRIANGLE_COUNT,
                         kenney_wheel_rr_pivot,0,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,variant,KENNEY_COLORMAP_W,KENNEY_COLORMAP_H,&n);
    queue_vehicle_part3d(kenney_wheel_fl_v,kenney_wheel_fl_uv,KENNEY_WHEEL_FL_VERTEX_COUNT,
                         kenney_wheel_fl_t,KENNEY_WHEEL_FL_TRIANGLE_COUNT,
                         kenney_wheel_fl_pivot,-steer_fl,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,variant,KENNEY_COLORMAP_W,KENNEY_COLORMAP_H,&n);
    queue_vehicle_part3d(kenney_wheel_fr_v,kenney_wheel_fr_uv,KENNEY_WHEEL_FR_VERTEX_COUNT,
                         kenney_wheel_fr_t,KENNEY_WHEEL_FR_TRIANGLE_COUNT,
                         kenney_wheel_fr_pivot,-steer_fr,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,variant,KENNEY_COLORMAP_W,KENNEY_COLORMAP_H,&n);

    qsort(g_tex_out,(size_t)n,sizeof(g_tex_out[0]),cmp_textri_far_first);
    for(i=0;i<n;++i)
        fill_tri_textured(
            g_tex_out[i].x0,g_tex_out[i].y0,g_tex_out[i].u0,g_tex_out[i].v0,
            g_tex_out[i].x1,g_tex_out[i].y1,g_tex_out[i].u1,g_tex_out[i].v1,
            g_tex_out[i].x2,g_tex_out[i].y2,g_tex_out[i].u2,g_tex_out[i].v2,
            g_tex_out[i].light,kenney_colormap,KENNEY_COLORMAP_W,KENNEY_COLORMAP_H);
}


static void render_sports_vehicle(
    float ox,float oy,float oz,
    float body_pitch,float body_yaw,float body_roll,
    float steer_fl,float steer_fr,float wheel_spin,
    float scale,float camx,float camy)
{
    int i,n=0;

    queue_vehicle_part3d(sports_body_v,sports_body_uv,SPORTS_BODY_VERTEX_COUNT,
                         sports_body_t,SPORTS_BODY_TRIANGLE_COUNT,
                         (v3f_t){0,0,0},0,0,0,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,0,SPORTS_COLORMAP_W,SPORTS_COLORMAP_H,&n);

    queue_vehicle_part3d(sports_wheel_rl_v,sports_wheel_rl_uv,SPORTS_WHEEL_RL_VERTEX_COUNT,
                         sports_wheel_rl_t,SPORTS_WHEEL_RL_TRIANGLE_COUNT,
                         sports_wheel_rl_pivot,0,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,0,SPORTS_COLORMAP_W,SPORTS_COLORMAP_H,&n);
    queue_vehicle_part3d(sports_wheel_rr_v,sports_wheel_rr_uv,SPORTS_WHEEL_RR_VERTEX_COUNT,
                         sports_wheel_rr_t,SPORTS_WHEEL_RR_TRIANGLE_COUNT,
                         sports_wheel_rr_pivot,0,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,0,SPORTS_COLORMAP_W,SPORTS_COLORMAP_H,&n);
    queue_vehicle_part3d(sports_wheel_fl_v,sports_wheel_fl_uv,SPORTS_WHEEL_FL_VERTEX_COUNT,
                         sports_wheel_fl_t,SPORTS_WHEEL_FL_TRIANGLE_COUNT,
                         sports_wheel_fl_pivot,-steer_fl,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,0,SPORTS_COLORMAP_W,SPORTS_COLORMAP_H,&n);
    queue_vehicle_part3d(sports_wheel_fr_v,sports_wheel_fr_uv,SPORTS_WHEEL_FR_VERTEX_COUNT,
                         sports_wheel_fr_t,SPORTS_WHEEL_FR_TRIANGLE_COUNT,
                         sports_wheel_fr_pivot,-steer_fr,wheel_spin,1,
                         ox,oy,oz,body_pitch,body_yaw,body_roll,
                         scale,camx,camy,0,SPORTS_COLORMAP_W,SPORTS_COLORMAP_H,&n);

    qsort(g_tex_out,(size_t)n,sizeof(g_tex_out[0]),cmp_textri_far_first);
    for(i=0;i<n;++i)
        fill_tri_textured(
            g_tex_out[i].x0,g_tex_out[i].y0,g_tex_out[i].u0,g_tex_out[i].v0,
            g_tex_out[i].x1,g_tex_out[i].y1,g_tex_out[i].u1,g_tex_out[i].v1,
            g_tex_out[i].x2,g_tex_out[i].y2,g_tex_out[i].u2,g_tex_out[i].v2,
            g_tex_out[i].light,sports_colormap,SPORTS_COLORMAP_W,SPORTS_COLORMAP_H);
}

static void make_box_vertices(float w,float h,float d,v3f_t v[8])
{
    float x=w*0.5f,z=d*0.5f;
    v[0]=(v3f_t){-x,0,-z};v[1]=(v3f_t){x,0,-z};
    v[2]=(v3f_t){x,0,z};v[3]=(v3f_t){-x,0,z};
    v[4]=(v3f_t){-x,h,-z};v[5]=(v3f_t){x,h,-z};
    v[6]=(v3f_t){x,h,z};v[7]=(v3f_t){-x,h,z};
}


static void render_box3d(
    float ox,float oy,float oz,float yaw,
    float w,float h,float d,float scale,
    float camx,float camy,int variant,uint16_t color)
{
    v3f_t v[8];
    make_box_vertices(w,h,d,v);
    render_mesh3d(v,8,g_box_t,12,ox,oy,oz,yaw,scale,camx,camy,variant,0,color);
}

static void child_offset_yaw(float yaw,float lx,float lz,float *wx,float *wz)
{
    float cs=cosf(yaw),sn=sinf(yaw);
    *wx=lx*cs-lz*sn;
    *wz=lx*sn+lz*cs;
}

static void render_car3d(
    float ox,float oy,float oz,float yaw,float scale,
    float camx,float camy,int variant)
{
    float dx,dz;
    uint16_t tire=pack1555(18,20,22);
    uint16_t trim=pack1555(34,38,44);

    render_mesh3d(g_car_v,16,g_car_t,CAR_TRI_COUNT,
                  ox,oy,oz,yaw,scale,camx,camy,variant,1,0);

    /* Four true-3D wheels. They are deliberately boxy at this resolution but
       give the silhouette much more volume than the Stage2 wedge. */
    child_offset_yaw(yaw,-235.0f*scale,-285.0f*scale,&dx,&dz);
    render_box3d(ox+dx,oy+42.0f*scale,oz+dz,yaw,
                 105,120,95,scale,camx,camy,0,tire);
    child_offset_yaw(yaw,235.0f*scale,-285.0f*scale,&dx,&dz);
    render_box3d(ox+dx,oy+42.0f*scale,oz+dz,yaw,
                 105,120,95,scale,camx,camy,0,tire);
    child_offset_yaw(yaw,-235.0f*scale,290.0f*scale,&dx,&dz);
    render_box3d(ox+dx,oy+42.0f*scale,oz+dz,yaw,
                 105,120,95,scale,camx,camy,0,tire);
    child_offset_yaw(yaw,235.0f*scale,290.0f*scale,&dx,&dz);
    render_box3d(ox+dx,oy+42.0f*scale,oz+dz,yaw,
                 105,120,95,scale,camx,camy,0,tire);

    /* Rear bumper / spoiler: separate geometry makes the rear view read as car. */
    child_offset_yaw(yaw,0,-410.0f*scale,&dx,&dz);
    render_box3d(ox+dx,oy+185.0f*scale,oz+dz,yaw,
                 520,38,80,scale,camx,camy,0,trim);
}

/* ---------- framebuffer ---------- */

static void build_base(video_t *v)
{
    int x,y;
    for(y=0;y<RH;++y){
        for(x=0;x<RW;++x){
            uint16_t c;
            if(y<180){
                int sx=x/2;
                int sy=y/2;
                if(sx>=RACER_SKY_W)sx=RACER_SKY_W-1;
                if(sy>=RACER_SKY_H)sy=RACER_SKY_H-1;
                c=racer_sky[sy*RACER_SKY_W+sx];
            }else{
                c=C_GRASS1;
            }
            v->base[(size_t)y*RW+x]=c;
        }
    }


}

static int video_open(video_t *v)
{
    size_t fallback;
    memset(v,0,sizeof(*v));
    v->fd=-1;v->pending=-1;

    v->fd=open("/dev/fb0",O_RDWR);
    if(v->fd<0){fprintf(stderr,"[racer] open fb: %s\n",strerror(errno));return -1;}
    if(ioctl(v->fd,FBIOGET_FSCREENINFO,&v->fix)<0 ||
       ioctl(v->fd,FBIOGET_VSCREENINFO,&v->var)<0)return -1;

    v->stride=v->fix.line_length;
    fallback=(size_t)v->stride*(v->var.yres_virtual?v->var.yres_virtual:v->var.yres);
    v->len=v->fix.smem_len?v->fix.smem_len:fallback;
    if(v->var.xres!=OW||v->var.yres!=OH||v->var.bits_per_pixel!=16||v->stride<OW*2U){
        fprintf(stderr,"[racer] unsupported fb %ux%u %ubpp stride=%u\n",
            v->var.xres,v->var.yres,v->var.bits_per_pixel,v->stride);
        return -1;
    }

    v->mem=(uint8_t*)mmap(NULL,v->len,PROT_READ|PROT_WRITE,MAP_SHARED,v->fd,0);
    if(v->mem==MAP_FAILED){v->mem=NULL;return -1;}

    v->canvas[0]=(uint16_t*)malloc((size_t)RW*RH*2U);
    v->canvas[1]=(uint16_t*)malloc((size_t)RW*RH*2U);
    v->base=(uint16_t*)malloc((size_t)RW*RH*2U);
    v->row2x=(uint16_t*)malloc((size_t)OW*2U);
    if(!v->canvas[0]||!v->canvas[1]||!v->base||!v->row2x)return -1;

    pthread_mutex_init(&v->lock,NULL);
    pthread_cond_init(&v->ready,NULL);
    pthread_cond_init(&v->free_cv,NULL);
    g_canvas=v->canvas[0];
    build_base(v);

    fprintf(stderr,"[racer] HIFB ready 1280x720 <- 640x360 exact2x sports-car free-drive reverse fixed60\n");
    return 0;
}

static void video_close(video_t *v)
{
    if(v->mem)munmap(v->mem,v->len);
    if(v->fd>=0)close(v->fd);
    free(v->canvas[0]);free(v->canvas[1]);free(v->base);free(v->row2x);
    pthread_cond_destroy(&v->ready);pthread_cond_destroy(&v->free_cv);
    pthread_mutex_destroy(&v->lock);
    memset(v,0,sizeof(*v));v->fd=-1;g_canvas=NULL;
}

static void video_present_buffer(video_t *v,const uint16_t *src)
{
    int y,x;
    if(v->vblank_state>=0){
        errno=0;
        if(ioctl(v->fd,H3531_FBIOGET_VBLANK_HIFB,0)==0){
            if(v->vblank_state==0)fprintf(stderr,"[racer] HIFB vblank 0x4664 active\n");
            v->vblank_state=1;
        }else v->vblank_state=-1;
    }

    for(y=0;y<RH;++y){
        const uint16_t *s=src+(size_t)y*RW;
        uint16_t *r=v->row2x;
        uint8_t *d0=v->mem+(size_t)(y*2)*v->stride;
        uint8_t *d1=v->mem+(size_t)(y*2+1)*v->stride;
        for(x=0;x<RW;++x){r[x*2]=s[x];r[x*2+1]=s[x];}
        memcpy(d0,r,OW*2U);memcpy(d1,r,OW*2U);
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static void *presenter_main(void *arg)
{
    video_t *v=(video_t*)arg;
    pin_thread(1,"presenter");
    for(;;){
        int idx;
        pthread_mutex_lock(&v->lock);
        while(v->pending<0&&!v->stop)pthread_cond_wait(&v->ready,&v->lock);
        if(v->pending<0&&v->stop){pthread_mutex_unlock(&v->lock);break;}
        idx=v->pending;v->pending=-1;
        pthread_cond_broadcast(&v->free_cv);
        pthread_mutex_unlock(&v->lock);

        video_present_buffer(v,v->canvas[idx]);

        pthread_mutex_lock(&v->lock);
        v->busy[idx]=0;v->presented++;
        pthread_cond_broadcast(&v->free_cv);
        pthread_mutex_unlock(&v->lock);
    }
    return NULL;
}

static int video_start(video_t *v)
{
    if(pthread_create(&v->presenter,NULL,presenter_main,v)!=0)return -1;
    fprintf(stderr,"[racer] dual-core render/present pipeline active\n");
    return 0;
}

static void video_acquire(video_t *v,int idx)
{
    pthread_mutex_lock(&v->lock);
    while(v->busy[idx]&&!v->stop)pthread_cond_wait(&v->free_cv,&v->lock);
    pthread_mutex_unlock(&v->lock);
    g_canvas=v->canvas[idx];
}

static void video_submit(video_t *v,int idx)
{
    pthread_mutex_lock(&v->lock);
    while(v->pending>=0&&!v->stop)pthread_cond_wait(&v->free_cv,&v->lock);
    v->busy[idx]=1;v->pending=idx;
    pthread_cond_signal(&v->ready);
    pthread_mutex_unlock(&v->lock);
}

static void video_stop(video_t *v)
{
    pthread_mutex_lock(&v->lock);
    while(v->pending>=0||v->busy[0]||v->busy[1])pthread_cond_wait(&v->free_cv,&v->lock);
    v->stop=1;pthread_cond_signal(&v->ready);
    pthread_mutex_unlock(&v->lock);
    pthread_join(v->presenter,NULL);
}

/* ---------- input ---------- */

static int16_t scale_abs_centered(const struct input_absinfo *i,int center,int v)
{
    int64_t mn=i->minimum,mx=i->maximum,c=center,out;
    if(mx<=mn)return 0;
    if(v<(int)c){
        int64_t d=c-mn;if(d<=0)return 0;
        out=((int64_t)v-c)*32768/d;if(out<-32768)out=-32768;
    }else{
        int64_t d=mx-c;if(d<=0)return 0;
        out=((int64_t)v-c)*32767/d;if(out>32767)out=32767;
    }
    return (int16_t)out;
}

static int shape_axis(int v)
{
    const int dead=7000;
    int a=v<0?-v:v,lin,curve;
    if(a<=dead)return 0;
    lin=(a-dead)*32767/(32767-dead);
    if(lin>32767)lin=32767;
    curve=(int)(((int64_t)lin*lin)/32767);
    curve=(lin+curve*2)/3;
    return v<0?-curve:curve;
}

static int center_quality(const struct input_absinfo *i,int c)
{
    int range=i->maximum-i->minimum,mid=(i->minimum+i->maximum)/2;
    int edge,off;
    if(range<=0)return -100000;
    edge=c-i->minimum;if(i->maximum-c<edge)edge=i->maximum-c;
    off=abs(c-mid);
    return edge*100/range-off*40/range;
}

static int pad_quality(const pad_node_t *p)
{
    if(p->sx_code<0||p->sy_code<0)return -100000;
    return center_quality(&p->absinfo[p->sx_code],p->center_raw[p->sx_code])+
           center_quality(&p->absinfo[p->sy_code],p->center_raw[p->sy_code]);
}

static int pad_capable(int fd)
{
    unsigned long ev[NBITS(EV_MAX+1)],key[NBITS(KEY_MAX+1)],ab[NBITS(ABS_MAX+1)];
    int buttons=0,axes=0,c;
    memset(ev,0,sizeof(ev));memset(key,0,sizeof(key));memset(ab,0,sizeof(ab));
    if(ioctl(fd,EVIOCGBIT(0,sizeof(ev)),ev)<0)return 0;
    if(TBIT(EV_KEY,ev))ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(key)),key);
    if(TBIT(EV_ABS,ev))ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(ab)),ab);
    for(c=BTN_JOYSTICK;c<=KEY_MAX;++c)if(TBIT(c,key))buttons++;
    for(c=0;c<=ABS_MAX;++c)if(TBIT(c,ab))axes++;
    return ((TBIT(ABS_X,ab)&&TBIT(ABS_Y,ab)) || (buttons>=2&&axes>=2));
}

static void input_scan(input_t *in)
{
    const char *kbd=getenv("H3531_NATIVE_KEYBOARD");
    int n;
    for(n=0;n<64&&in->pad_count<MAX_PAD_NODES;++n){
        char path[64],name[128];
        int fd,c;
        pad_node_t *p;
        snprintf(path,sizeof(path),"/dev/input/event%d",n);
        if(kbd&&strcmp(kbd,path)==0)continue;
        fd=open(path,O_RDONLY|O_NONBLOCK);
        if(fd<0)continue;
        if(!pad_capable(fd)){close(fd);continue;}
        p=&in->pads[in->pad_count++];
        memset(p,0,sizeof(*p));p->fd=fd;p->sx_code=-1;p->sy_code=-1;
        snprintf(p->path,sizeof(p->path),"%s",path);
        memset(name,0,sizeof(name));
        if(ioctl(fd,EVIOCGNAME(sizeof(name)-1),name)<0)snprintf(name,sizeof(name),"event%d",n);
        snprintf(p->name,sizeof(p->name),"%s",name);
        ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(p->absbits)),p->absbits);
        for(c=0;c<=ABS_MAX;++c){
            if(TBIT(c,p->absbits)&&ioctl(fd,EVIOCGABS(c),&p->absinfo[c])==0){
                p->have_abs[c]=1;p->center_raw[c]=p->absinfo[c].value;p->axis[c]=0;
            }
        }
        if(p->have_abs[ABS_X]&&p->have_abs[ABS_Y]){p->sx_code=ABS_X;p->sy_code=ABS_Y;}
        else if(p->have_abs[ABS_RX]&&p->have_abs[ABS_RY]){p->sx_code=ABS_RX;p->sy_code=ABS_RY;}
        fprintf(stderr,"[racer] gamepad node %s name=%s center=%d/%d quality=%d\n",
            p->path,p->name,
            p->sx_code>=0?p->center_raw[p->sx_code]:0,
            p->sy_code>=0?p->center_raw[p->sy_code]:0,
            pad_quality(p));
    }

    {
        int i,best=-1,bq=-100000;
        for(i=0;i<in->pad_count;++i){
            int q=pad_quality(&in->pads[i]);
            if(q>bq){bq=q;best=i;}
        }
        in->steer_node=best;
        if(best>=0)fprintf(stderr,"[racer] primary steering node=%d path=%s quality=%d\n",
            best,in->pads[best].path,bq);
    }
}

static void input_open(input_t *in)
{
    const char *kbd=getenv("H3531_NATIVE_KEYBOARD");
    int i;
    memset(in,0,sizeof(*in));in->kfd=-1;in->steer_node=-1;
    for(i=0;i<MAX_PAD_NODES;++i)in->pads[i].fd=-1;
    if(kbd&&*kbd)in->kfd=open(kbd,O_RDONLY|O_NONBLOCK);
    input_scan(in);
    fprintf(stderr,"[racer] keyboard=%s fd=%d gamepad_nodes=%d\n",
        (kbd&&*kbd)?kbd:"<none>",in->kfd,in->pad_count);
}

static void input_close(input_t *in)
{
    int i;
    if(in->kfd>=0)close(in->kfd);
    for(i=0;i<in->pad_count;++i)if(in->pads[i].fd>=0)close(in->pads[i].fd);
}

static void input_poll(input_t *in)
{
    int i,steer=0,pad_gas=0,pad_brake=0;
    if(in->kfd>=0){
        struct input_event e;
        while(read(in->kfd,&e,sizeof(e))==(ssize_t)sizeof(e)){
            int d=e.value!=0;
            if(e.type!=EV_KEY)continue;
            if(e.code==KEY_LEFT)in->left=d;
            else if(e.code==KEY_RIGHT)in->right=d;
            else if(e.code==KEY_UP||e.code==KEY_W)in->key_gas=d;
            else if(e.code==KEY_DOWN||e.code==KEY_S)in->key_brake=d;
            else if((e.code==KEY_ESC||e.code==KEY_F12)&&d)g_stop=1;
        }
    }

    in->start_down=0;in->select_down=0;
    for(i=0;i<in->pad_count;++i){
        pad_node_t *p=&in->pads[i];
        struct input_event e;
        while(read(p->fd,&e,sizeof(e))==(ssize_t)sizeof(e)){
            if(e.type==EV_ABS&&e.code<=ABS_MAX&&p->have_abs[e.code])
                p->axis[e.code]=scale_abs_centered(&p->absinfo[e.code],p->center_raw[e.code],e.value);
            else if(e.type==EV_KEY&&e.code<=KEY_MAX)
                p->key_down[e.code]=(uint8_t)(e.value!=0);
        }
        if(i==in->steer_node&&p->sx_code>=0)steer=shape_axis(p->axis[p->sx_code]);
        if(p->key_down[BTN_DPAD_LEFT]||p->key_down[KEY_LEFT])steer=-32768;
        if(p->key_down[BTN_DPAD_RIGHT]||p->key_down[KEY_RIGHT])steer=32767;
        if(p->key_down[BTN_SOUTH]||p->key_down[BTN_TRIGGER]||p->key_down[BTN_THUMB])pad_gas=1;
        if(p->key_down[BTN_EAST]||p->key_down[BTN_TOP]||p->key_down[BTN_THUMB2])pad_brake=1;
        if(p->key_down[BTN_START])in->start_down=1;
        if(p->key_down[BTN_SELECT])in->select_down=1;
    }
    if(in->left)steer=-32768;if(in->right)steer=32767;
    in->steer=steer;
    in->gas=in->key_gas||pad_gas;
    in->brake=in->key_brake||pad_brake;
    if(in->start_down&&in->select_down)g_stop=1;
}

/* ---------- level ---------- */

static void add_curve(int a,int b,float curve)
{
    int i;if(a<0)a=0;if(b>TRACK_SEGMENTS)b=TRACK_SEGMENTS;
    for(i=a;i<b;++i)g_track[i].curve=curve;
}

static void add_flag(int seg,unsigned flag)
{
    if(seg>=0&&seg<TRACK_SEGMENTS)g_track[seg].flags|=flag;
}

static void build_level(void)
{
    int i;
    memset(g_track,0,sizeof(g_track));

    /* Boulevard Sprint:
       launch straight -> fast right -> S -> hill -> urban chicane ->
       long sweep -> descent -> final straight. */
    add_curve(90,185,0.34f);
    add_curve(185,255,-0.48f);
    add_curve(255,315,0.24f);
    add_curve(380,465,-0.62f);
    add_curve(465,535,0.66f);
    add_curve(610,760,0.24f);
    add_curve(760,850,-0.30f);
    add_curve(900,1025,0.53f);
    add_curve(1025,1110,-0.44f);
    add_curve(1180,1280,0.19f);

    for(i=0;i<TRACK_SEGMENTS;++i){
        float t=(float)i;
        float hill=0.0f;
        if(i>260&&i<560)hill+=180.0f*sinf((t-260.0f)*3.1415926f/300.0f);
        if(i>700&&i<980)hill-=135.0f*sinf((t-700.0f)*3.1415926f/280.0f);
        if(i>1040&&i<1260)hill+=90.0f*sinf((t-1040.0f)*3.1415926f/220.0f);
        hill+=25.0f*sinf(t*0.035f);
        g_track[i].y=hill;
        if((i%23)==0)g_track[i].flags|=TF_TREES;
        if(i>330&&i<560&&(i%17)==0)g_track[i].flags|=TF_CITY;
    }

    add_flag(65,TF_BILLBOARD_R);
    add_flag(220,TF_BILLBOARD_L);
    add_flag(430,TF_BILLBOARD_R);
    add_flag(705,TF_BILLBOARD_L);
    add_flag(980,TF_BILLBOARD_R);
    add_flag(1190,TF_BILLBOARD_L);
    add_flag(30,TF_GRANDSTAND_L|TF_GRANDSTAND_R);
    add_flag(45,TF_GRANDSTAND_L|TF_GRANDSTAND_R);
    add_flag(1360,TF_FINISH|TF_GRANDSTAND_L|TF_GRANDSTAND_R);

    for(i=0;i<8;++i){
        g_traffic[i].pos=(110+i*135)*SEG_LEN;
        g_traffic[i].offset=((i%3)-1)*0.46f;
        g_traffic[i].speed=48.0f+(float)(i*3);
        g_traffic[i].lane_phase=(float)i*0.77f;
        g_traffic[i].wheel_spin=0.0f;
        g_traffic[i].steer_angle=0.0f;
        g_traffic[i].lane=i%3;
    }
}

static float track_length(void){return TRACK_SEGMENTS*SEG_LEN;}

static int seg_index_from_pos(float p)
{
    int i=(int)(p/SEG_LEN)%TRACK_SEGMENTS;
    if(i<0)i+=TRACK_SEGMENTS;
    return i;
}

static float lerpf(float a,float b,float t){return a+(b-a)*t;}

static void project_point(float worldx,float worldy,float z,float camx,float camy,proj_t *p)
{
    float scale;
    if(z<1.0f){p->visible=0;return;}
    scale=CAMERA_DEPTH/z;
    p->scale=scale;
    p->x=RW*0.5f + scale*(worldx-camx)*RW*0.5f;
    p->y=RH*0.5f - scale*(worldy-camy)*RH*0.5f;
    p->w=scale*ROAD_WIDTH*RW*0.5f;
    p->visible=1;
}

static void build_projection(void)
{
    float base_percent=fmodf(g_position,SEG_LEN)/SEG_LEN;
    int base=seg_index_from_pos(g_position);
    float player_y=lerpf(g_track[base].y,g_track[(base+1)%TRACK_SEGMENTS].y,base_percent)+CAMERA_HEIGHT;
    float camera_x=g_player_x*ROAD_WIDTH;
    float x=0.0f;
    float dx=-g_track[base].curve*base_percent;
    int n;

    for(n=0;n<=DRAW_DISTANCE;++n){
        int idx=(base+n)%TRACK_SEGMENTS;
        float z=n*SEG_LEN-fmodf(g_position,SEG_LEN);
        g_proj[n].seg_index=idx;
        g_proj[n].world_x=-x;
        g_proj[n].world_y=g_track[idx].y;
        g_proj[n].z=z;
        project_point(-x,g_track[idx].y,z,camera_x,player_y,&g_proj[n]);
        x+=dx;
        dx+=g_track[idx].curve;
    }
}


static void draw_dynamic_sky(void)
{
    int x,y;
    int shift=((int)(g_position*0.018f))%(RACER_SKY_W*2);
    if(shift<0)shift+=RACER_SKY_W*2;
    for(y=0;y<180;++y){
        int sy=y/2;
        if(sy>=RACER_SKY_H)sy=RACER_SKY_H-1;
        for(x=0;x<RW;++x){
            int wrapped=(x+shift)%(RACER_SKY_W*2);
            int sx=wrapped/2;
            putpx(x,y,racer_sky[sy*RACER_SKY_W+sx]);
        }
    }
}

static void road_scanline(int y,float cx,float roadw,int stripe)
{
    int rw=(int)roadw;
    int rumble=rw/12;
    int lanes=3;
    int lane_w=(rw*2)/lanes;
    int left=(int)cx-rw;
    int right=(int)cx+rw;
    uint16_t grass=(stripe&1)?C_GRASS1:C_GRASS2;
    uint16_t road=(stripe&1)?C_ROAD1:C_ROAD2;
    uint16_t rum=(stripe&1)?C_RUMBLE1:C_RUMBLE2;
    int i;

    hline(0,RW-1,y,grass);
    hline(left-rumble,left-1,y,rum);
    hline(right+1,right+rumble,y,rum);
    hline(left,right,y,road);

    if((stripe&3)==0&&rw>40){
        for(i=1;i<lanes;++i){
            int lx=left+(lane_w*i);
            hline(lx-1,lx+1,y,C_LANE);
        }
    }
}

static void draw_road(void)
{
    int n,maxy=RH-1;
    for(n=1;n<DRAW_DISTANCE;++n){
        proj_t *a=&g_proj[n-1],*b=&g_proj[n];
        int y0,y1,y;
        if(!a->visible||!b->visible)continue;
        y0=(int)a->y;y1=(int)b->y;
        if(y0<=y1)continue;
        if(y1>=maxy)continue;
        if(y0>maxy)y0=maxy;
        if(y1<0)y1=0;
        if(y0>=RH)y0=RH-1;

        for(y=y1;y<=y0;++y){
            float t=(y0==y1)?0.0f:(float)(y-y1)/(float)(y0-y1);
            float cx=lerpf(b->x,a->x,t);
            float rw=lerpf(b->w,a->w,t);
            road_scanline(y,cx,rw,(g_proj[n].seg_index/3)&7);
        }
        maxy=y1;
        if(maxy<=HORIZON)break;
    }
}

/* ---------- sprites ---------- */

static void draw_billboard_scaled(int cx,int bottom,int w,int h,unsigned phase)
{
    int x,y;
    if(w<8||h<5)return;
    if(w>220)w=220;if(h>124)h=124;
    for(y=0;y<h;++y){
        int sy=y*RACER_BILLBOARD_H/h;
        int yy=bottom-h+y;
        if((unsigned)yy>=RH)continue;
        for(x=0;x<w;++x){
            int sx=x*RACER_BILLBOARD_W/w;
            uint16_t c=racer_billboard[sy*RACER_BILLBOARD_W+sx];
            /* moving scanline makes this visibly "alive"; later VDEC replaces pixels */
            if(((y+(int)phase)&15)==0){
                unsigned r=((c>>10)&31),g=((c>>5)&31),b=(c&31);
                r=(r*3)/4;g=(g*3)/4;b=(b*3)/4;
                c=(uint16_t)(0x8000|(r<<10)|(g<<5)|b);
            }
            putpx(cx-w/2+x,yy,c);
        }
    }
    hline(cx-w/2-3,cx+w/2+3,bottom-h-3,C_BLACK);
    hline(cx-w/2-3,cx+w/2+3,bottom+2,C_BLACK);
    line2(cx-w/2-3,bottom-h-3,cx-w/2-3,bottom+2,C_BLACK);
    line2(cx+w/2+3,bottom-h-3,cx+w/2+3,bottom+2,C_BLACK);
    fill_rect(cx-w/2,bottom+3,4,h/3,C_BLACK);
    fill_rect(cx+w/2-4,bottom+3,4,h/3,C_BLACK);
}

static void draw_tree(int x,int bottom,int scale)
{
    int h=scale*3/2,w=scale;
    if(scale<3)return;
    fill_rect(x-1,bottom-h/3,3,h/3,pack1555(85,52,27));
    fill_rect(x-w/3,bottom-h,w*2/3,h*2/3,pack1555(22,100,42));
    fill_rect(x-w/2,bottom-h*3/4,w,h/3,pack1555(30,126,50));
}

static void draw_building3d_at(int n,int side,int variant,float size_mul)
{
    proj_t *p=&g_proj[n];
    float road_center=p->world_x;
    float ox=road_center + side*ROAD_WIDTH*1.70f;
    float w=760.0f*size_mul;
    float h=(1250.0f+(variant%4)*310.0f)*size_mul;
    float d=720.0f*size_mul;
    float camx=g_player_x*ROAD_WIDTH;
    int base=seg_index_from_pos(g_position);
    float base_percent=fmodf(g_position,SEG_LEN)/SEG_LEN;
    float camy=lerpf(g_track[base].y,g_track[(base+1)%TRACK_SEGMENTS].y,base_percent)+CAMERA_HEIGHT;
    uint16_t roof=(variant&1)?pack1555(76,78,84):pack1555(52,66,78);
    uint16_t glass=pack1555(50,105,132);

    render_box3d(ox,p->world_y,p->z,0.0f,w,h,d,1.0f,camx,camy,variant,0);
    /* setback roof volume */
    render_box3d(ox,p->world_y+h,p->z,0.0f,w*0.64f,h*0.20f,d*0.62f,
                 1.0f,camx,camy,variant,roof);
    /* glass entrance slab toward the road; tiny depth but true perspective */
    render_box3d(ox-side*w*0.52f,p->world_y+90.0f*size_mul,p->z,
                 0.0f,50.0f,380.0f*size_mul,d*0.58f,
                 1.0f,camx,camy,variant,glass);
}

static void draw_grandstand3d_at(int n,int side,int variant)
{
    proj_t *p=&g_proj[n];
    v3f_t v[8];
    float ox=p->world_x+side*ROAD_WIDTH*1.45f;
    float camx=g_player_x*ROAD_WIDTH;
    int base=seg_index_from_pos(g_position);
    float bp=fmodf(g_position,SEG_LEN)/SEG_LEN;
    float camy=lerpf(g_track[base].y,g_track[(base+1)%TRACK_SEGMENTS].y,bp)+CAMERA_HEIGHT;
    make_box_vertices(1400.0f,850.0f,1800.0f,v);
    render_mesh3d(v,8,g_box_t,12,ox,p->world_y,p->z,0.0f,1.0f,camx,camy,variant,0,0);
}

static void draw_roadside(void)
{
    int n;
    for(n=DRAW_DISTANCE-1;n>=2;--n){
        proj_t *p=&g_proj[n];
        int idx=p->seg_index;
        unsigned f=g_track[idx].flags;
        float roadw=p->w;
        int scale=(int)(roadw*0.20f);
        int bottom=(int)p->y;
        if(!p->visible||bottom<0||bottom>=RH||roadw<3)continue;

        if(f&TF_BILLBOARD_L)draw_billboard_scaled((int)(p->x-roadw*1.55f),bottom,scale*5,scale*3,g_frame);
        if(f&TF_BILLBOARD_R)draw_billboard_scaled((int)(p->x+roadw*1.55f),bottom,scale*5,scale*3,g_frame);
        if(f&TF_TREES){
            draw_tree((int)(p->x-roadw*1.35f),bottom,scale);
            draw_tree((int)(p->x+roadw*1.40f),bottom,scale);
        }
        if(f&TF_CITY){
            /* True low-poly 3D close/mid buildings; distant objects naturally
               become tiny through projection and cost only a handful of pixels. */
            draw_building3d_at(n,-1,(idx>>4)&3,1.0f);
            if(idx&1)draw_building3d_at(n,1,(idx>>3)&3,0.82f);
        }
        if(f&TF_GRANDSTAND_L)draw_grandstand3d_at(n,-1,1);
        if(f&TF_GRANDSTAND_R)draw_grandstand3d_at(n,1,0);
        if(f&TF_FINISH&&scale>8){
            int y=bottom-scale*2;
            hline((int)(p->x-roadw),(int)(p->x+roadw),y,C_WHITE);
            fill_rect((int)(p->x-roadw),y-6,(int)(roadw*2),4,C_BLACK);
        }
    }
}

static void update_traffic(void)
{
    int i;
    float len=track_length();
    for(i=0;i<8;++i){
        g_traffic[i].pos+=g_traffic[i].speed;
        g_traffic[i].lane_phase+=0.012f+(float)i*0.0007f;
        {
            int seg=seg_index_from_pos(g_traffic[i].pos);
            float target=-g_track[seg].curve*0.34f+sinf(g_traffic[i].lane_phase)*0.025f;
            g_traffic[i].steer_angle=approachf(g_traffic[i].steer_angle,target,0.018f);
            g_traffic[i].wheel_spin+=g_traffic[i].speed/
                (KENNEY_VEHICLE_WHEEL_RADIUS>1.0f?KENNEY_VEHICLE_WHEEL_RADIUS:90.0f);
            if(g_traffic[i].wheel_spin>6.2831853f)g_traffic[i].wheel_spin-=6.2831853f;
        }
        while(g_traffic[i].pos>=len)g_traffic[i].pos-=len;
        while(g_traffic[i].pos<0)g_traffic[i].pos+=len;
    }
}

static void draw_traffic(void)
{
    int n,i,base=seg_index_from_pos(g_position);
    float base_percent=fmodf(g_position,SEG_LEN)/SEG_LEN;
    float camx=g_player_x*ROAD_WIDTH;
    float camy=lerpf(g_track[base].y,g_track[(base+1)%TRACK_SEGMENTS].y,base_percent)+CAMERA_HEIGHT;

    /* Distance order gives us a cheap object-level painter without a full-frame
       Z-buffer. Each car itself is true 3D and face-sorted. */
    for(n=DRAW_DISTANCE-1;n>=4;--n){
        int idx=(base+n)%TRACK_SEGMENTS;
        for(i=0;i<8;++i){
            int ti=seg_index_from_pos(g_traffic[i].pos);
            if(ti==idx){
                float percent=fmodf(g_traffic[i].pos,SEG_LEN)/SEG_LEN;
                float oz=g_proj[n].z+percent*SEG_LEN;
                float lane=g_traffic[i].offset+sinf(g_traffic[i].lane_phase)*0.035f;
                float ox=g_proj[n].world_x+lane*ROAD_WIDTH*0.78f;
                float oy=lerpf(g_track[idx].y,g_track[(idx+1)%TRACK_SEGMENTS].y,percent);
                float yaw=-g_track[idx].curve*0.18f+sinf(g_traffic[i].lane_phase)*0.018f;
                float lod=(n<34)?1.02f:0.86f;
                if(n<72){
                    float d=g_traffic[i].steer_angle;
                    render_kenney_vehicle(ox,oy,oz,0.0f,yaw,0.0f,
                                          d,d,g_traffic[i].wheel_spin,
                                          lod,camx,camy,i);
                }else{
                    render_car3d(ox,oy,oz,yaw,lod,camx,camy,i);
                }
            }
        }
    }
}

static void draw_player_car3d(void)
{
    float camx=0.0f,camy=0.0f;
    float ox=0.0f;
    float oy=-1515.0f;
    float oz=1660.0f;

    /* Player uses the higher-detail J-Toastie sports-car mesh.
       Heading is fully free: the renderer does not force the nose back to road center. */
    render_sports_vehicle(
        ox,oy,oz,
        g_body_pitch,-g_vehicle_heading,g_body_roll,
        g_steer_fl,g_steer_fr,g_wheel_spin,
        1.08f,camx,camy);
}

static void draw_hud(void)
{
    int bar=(int)(fabsf(g_speed)/MAX_SPEED*120.0f);
    int i;
    fill_rect(16,14,130,15,pack1555(10,17,22));
    for(i=0;i<bar;++i)putpx(20+i,20,pack1555(45+(unsigned)i,190,85));
    hline(20,140,18,C_WHITE);
    hline(20,140,25,C_WHITE);
    /* reverse indicator */
    if(g_speed<0.0f)fill_rect(20,30,18,4,C_RED);

    /* center road aiming marker */
    hline(RW/2-8,RW/2+8,HORIZON+14,pack1555(235,235,210));
    putpx(RW/2,HORIZON+13,C_WHITE);
    putpx(RW/2,HORIZON+15,C_WHITE);

    /* lap/finish indicator without text rendering */
    fill_rect(RW-52,14,36,8,pack1555(20,24,28));
    fill_rect(RW-48,17,g_lap==1?10:20,2,C_WHITE);
}

/* ---------- game ---------- */

static void update_ackermann(float steer)
{
    float wb=SPORTS_VEHICLE_WHEELBASE;
    float tw=SPORTS_VEHICLE_TRACK;
    float a=fabsf(steer);
    if(wb<100.0f)wb=520.0f;
    if(tw<80.0f)tw=360.0f;

    if(a<0.002f){
        g_steer_fl=steer;
        g_steer_fr=steer;
        return;
    }

    {
        float r=wb/tanf(a);
        float inner=atanf(wb/fmaxf(20.0f,r-tw*0.5f));
        float outer=atanf(wb/(r+tw*0.5f));
        if(steer>0.0f){
            g_steer_fr=inner;
            g_steer_fl=outer;
        }else{
            g_steer_fl=-inner;
            g_steer_fr=-outer;
        }
    }
}

static void game_update(input_t *in)
{
    float steer_input=(float)in->steer/32767.0f;
    float previous=g_speed;
    float abs_ratio=fabsf(g_speed)/MAX_SPEED;
    float max_steer,target_steer,steer_rate;
    float travel,yaw_delta,longitudinal,lateral;
    float accel;
    float wb=SPORTS_VEHICLE_WHEELBASE;
    float wheel_r=SPORTS_VEHICLE_WHEEL_RADIUS;

    /*
     * Signed drivetrain:
     * - gas accelerates forward;
     * - brake first brakes a forward-moving car;
     * - holding brake at/near zero engages reverse;
     * - gas while reversing brakes the reverse motion before going forward.
     */
    if(in->gas && !in->brake){
        if(g_speed<-1.5f)g_speed+=BRAKE;
        else g_speed+=ACCEL;
    }else if(in->brake && !in->gas){
        if(g_speed>1.5f)g_speed-=BRAKE;
        else g_speed-=REVERSE_ACCEL;
    }else{
        if(g_speed>0.0f){g_speed-=DECEL;if(g_speed<0.0f)g_speed=0.0f;}
        else if(g_speed<0.0f){g_speed+=DECEL;if(g_speed>0.0f)g_speed=0.0f;}
    }

    if(g_speed>MAX_SPEED)g_speed=MAX_SPEED;
    if(g_speed<-REVERSE_SPEED)g_speed=-REVERSE_SPEED;
    abs_ratio=fabsf(g_speed)/MAX_SPEED;

    /*
     * Free steering. There is no target heading and no road-centre attraction.
     * Steering changes yaw only through tyre geometry and travelled distance.
     */
    max_steer=0.11f+0.46f*(1.0f-abs_ratio)*(1.0f-0.25f*abs_ratio);
    target_steer=steer_input*max_steer;
    steer_rate=0.042f-0.010f*abs_ratio;
    if(steer_rate<0.022f)steer_rate=0.022f;

    g_steer_angle=approachf(g_steer_angle,target_steer,steer_rate);
    if(fabsf(steer_input)<0.01f)
        g_steer_angle=approachf(g_steer_angle,0.0f,steer_rate*1.45f);

    update_ackermann(g_steer_angle);

    if(wb<100.0f)wb=600.0f;
    if(wheel_r<10.0f)wheel_r=110.0f;

    travel=g_speed;

    /*
     * Kinematic bicycle in a free local world.
     * Signed travel makes steering naturally reverse while backing up.
     * Heading is persistent and may rotate through a full 360 degrees.
     */
    yaw_delta=(travel/wb)*tanf(g_steer_angle);
    g_vehicle_heading+=yaw_delta;
    while(g_vehicle_heading>3.14159265f)g_vehicle_heading-=6.2831853f;
    while(g_vehicle_heading<-3.14159265f)g_vehicle_heading+=6.2831853f;

    longitudinal=travel*cosf(g_vehicle_heading);
    lateral=travel*sinf(g_vehicle_heading);

    g_position+=longitudinal;
    g_player_x+=lateral/ROAD_WIDTH;

    /* Wide grass field rather than a road clamp. This is only a numeric guard. */
    if(g_player_x>8.0f)g_player_x=8.0f;
    if(g_player_x<-8.0f)g_player_x=-8.0f;

    /* Grass adds rolling resistance but never steers the car back to the road. */
    if(fabsf(g_player_x)>1.05f){
        float drag=OFFROAD_DECEL*0.18f;
        if(g_speed>0.0f){g_speed-=drag;if(g_speed<0.0f)g_speed=0.0f;}
        else if(g_speed<0.0f){g_speed+=drag;if(g_speed>0.0f)g_speed=0.0f;}
    }

    accel=g_speed-previous;
    g_body_pitch+=(fmaxf(-0.075f,fminf(0.075f,-accel*0.0075f))-g_body_pitch)*0.14f;
    g_body_roll+=(fmaxf(-0.10f,fminf(0.10f,-g_steer_angle*abs_ratio*0.30f))-g_body_roll)*0.13f;

    /* Exact path-to-wheel coupling, including backwards rotation in reverse. */
    g_wheel_spin+=travel/wheel_r;
    while(g_wheel_spin>6.2831853f)g_wheel_spin-=6.2831853f;
    while(g_wheel_spin<-6.2831853f)g_wheel_spin+=6.2831853f;

    g_vehicle_slip=0.0f;
    g_steer_visual+=(steer_input-g_steer_visual)*0.13f;

    while(g_position>=track_length()){
        g_position-=track_length();
        g_lap++;
        if(g_lap>2)g_lap=1;
    }
    while(g_position<0.0f){
        g_position+=track_length();
        g_lap--;
        if(g_lap<1)g_lap=2;
    }

    g_prev_speed=g_speed;
    update_traffic();
}

static void render_frame(video_t *v,int idx)
{
    memcpy(v->canvas[idx],v->base,(size_t)RW*RH*2U);
    draw_dynamic_sky();
    build_projection();
    draw_road();
    draw_roadside();
    draw_traffic();
    draw_player_car3d();
    draw_hud();
}

static int selftest(void)
{
    video_t v;
    unsigned i,changed=0;
    memset(&v,0,sizeof(v));
    init_colors();
    v.canvas[0]=(uint16_t*)calloc((size_t)RW*RH,sizeof(uint16_t));
    v.base=(uint16_t*)calloc((size_t)RW*RH,sizeof(uint16_t));
    if(!v.canvas[0]||!v.base)return 2;
    g_canvas=v.canvas[0];
    build_level();

    /* synthetic base for test, no framebuffer or external decode needed */
    for(i=0;i<(unsigned)(RW*RH);++i)v.base[i]=C_SKY;
    render_frame(&v,0);
    for(i=0;i<(unsigned)(RW*RH);++i)if(v.canvas[0][i]!=C_SKY){changed++;if(changed>5000)break;}
    free(v.canvas[0]);free(v.base);g_canvas=NULL;
    if(changed<=5000){
        fprintf(stderr,"RACER_SELFTEST_FAIL changed=%u\n",changed);
        return 3;
    }
    printf("RACER_SELFTEST_OK changed>%u track=%d draw=%d\n",changed,TRACK_SEGMENTS,DRAW_DISTANCE);
    return 0;
}

int main(int argc,char **argv)
{
    video_t v;
    input_t in;
    int idx=0;
    uint64_t perf;
    unsigned frames=0;

    if(argc>1&&strcmp(argv[1],"--selftest")==0)return selftest();

    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGHUP,on_signal);
    init_colors();
    build_level();

    if(video_open(&v)<0){video_close(&v);return 10;}
    input_open(&in);
    pin_thread(0,"renderer");
    if(video_start(&v)<0){input_close(&in);video_close(&v);return 11;}

    {
        uint64_t last_sim=mono_ns();
        uint64_t accumulator=0;
        uint64_t next_frame=last_sim+FRAME_NS;
        perf=last_sim;

        fprintf(stderr,"[racer] fixed simulation/present target=60Hz free-drive reverse sports-texture=%dx%d\n",
            SPORTS_COLORMAP_W,SPORTS_COLORMAP_H);

        while(!g_stop){
            uint64_t now=mono_ns();
            uint64_t elapsed=now-last_sim;
            int sim_steps=0;

            if(elapsed>FRAME_NS*MAX_SIM_CATCHUP)elapsed=FRAME_NS*MAX_SIM_CATCHUP;
            last_sim=now;
            accumulator+=elapsed;

            input_poll(&in);

            while(accumulator>=FRAME_NS && sim_steps<MAX_SIM_CATCHUP){
                game_update(&in);
                accumulator-=FRAME_NS;
                sim_steps++;
            }

            video_acquire(&v,idx);
            render_frame(&v,idx);
            video_submit(&v,idx);
            idx^=1;
            g_frame++;frames++;

            now=mono_ns();
            if(frames>=300){
                double sec=(double)(now-perf)/1000000000.0;
                fprintf(stderr,
                    "[racer] PERF freedrive fps=%.2f speed=%.1f pos=%.0f seg=%d input=%d rack=%.3f ack=%.3f/%.3f heading=%.3f slip=%.3f wheel=%.3f roll=%.3f pitch=%.3f x=%.3f lap=%d pads=%d presented=%u\n",
                    sec>0.0?(double)frames/sec:0.0,g_speed,g_position,
                    seg_index_from_pos(g_position),in.steer,g_steer_angle,
                    g_steer_fl,g_steer_fr,g_vehicle_heading,g_vehicle_slip,
                    g_wheel_spin,g_body_roll,g_body_pitch,g_player_x,g_lap,
                    in.pad_count,v.presented);
                perf=now;frames=0;
            }

            pace_until(next_frame);
            now=mono_ns();
            if(now>next_frame+FRAME_NS*2)next_frame=now+FRAME_NS;
            else next_frame+=FRAME_NS;
        }
    }

    video_stop(&v);
    fprintf(stderr,"[racer] exit frame=%u presented=%u\n",g_frame,v.presented);
    input_close(&in);video_close(&v);
    return 0;
}
