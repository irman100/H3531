/*
 * Stayplaytion Racer Stage 7.9 - Textured Vice City VCMAP2
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
#define HORIZON 110
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
#define TRACK_CURVE_SCALE 0.0075f
#define TRACK_3D_RANGE_BACK 72
#define TRACK_3D_RANGE_FRONT 170
#define CHASE_DISTANCE 1120.0f
#define CHASE_HEIGHT 760.0f
#define CHASE_NEAR_DISTANCE 1080.0f
#define CHASE_FAR_DISTANCE 1660.0f
#define CHASE_REVERSE_DISTANCE 1180.0f
#define CHASE_NEAR_HEIGHT 760.0f
#define CHASE_FAR_HEIGHT 920.0f
#define CHASE_POS_HZ 1.85f
#define CHASE_POS_DAMP 0.78f
#define CHASE_YAW_HZ 2.15f
#define CHASE_YAW_DAMP 0.82f
#define TRACK_FOCAL 258.0f
#define TRACK_SCREEN_Y 126.0f

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
    uint64_t present_ns_total;
    uint64_t present_ns_max;
    unsigned present_profile_count;
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

typedef struct {
    float x,y,z,yaw;
} track_world_t;

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

typedef struct { float x,y,z,u,v; } vc_vertex_t;
typedef struct { uint16_t a,b,c; uint8_t material,flags; } vc_tri_t;
typedef struct {
    uint16_t x,y,w,h;
    uint16_t fallback;
    uint8_t flags,pad;
} vc_material_t;

typedef struct {
    int16_t sx,sz;
    uint32_t vertex_base,vertex_count,tri_base,tri_count;
} vc_sector_t;

typedef struct {
    char magic[4];
    uint32_t version;
    float world_scale,sector_m;
    float spawn_x,spawn_y,spawn_z,spawn_yaw;
    float min_x,max_x,min_z,max_z;
    uint32_t vertex_count,tri_count,sector_count,material_count;
    uint32_t atlas_w,atlas_h;
} vcmap_header_t;

typedef struct {
    float world_scale,sector_m,sector_world;
    float spawn_x,spawn_y,spawn_z,spawn_yaw;
    float min_x,max_x,min_z,max_z;
    uint32_t vertex_count,tri_count,sector_count,material_count;
    uint32_t atlas_w,atlas_h;
    vc_vertex_t *verts;
    vc_tri_t *tris;
    vc_sector_t *sectors;
    vc_material_t *materials;
    uint16_t *atlas;
} vc_runtime_map_t;

#include "kenney_vehicle.h"
#include "sports_vehicle.h"
#include "track_texture.h"
#include "env_tree_default.h"
#include "env_tree_pine.h"
#include "env_house_suburban.h"
#include "env_house_mid.h"
#include "env_house_far.h"
#include "env_building_commercial.h"
#include "env_building_commercial_mid.h"
#include "env_building_commercial_far.h"
#include "env_street_light.h"
#include "env_warning_sign.h"
#include "osm_city_map.h"

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
    int x0,y0,x1,y1,x2,y2;
    float z0,z1,z2;
    uint16_t color;
} citytri_t;

typedef struct {
    float depth;
    int x0,y0,x1,y1,x2,y2;
    float u0,v0,u1,v1,u2,v2;
    float z0,z1,z2;
    float light;
} textri_t;

typedef struct {
    int x0,y0,x1,y1,x2,y2;
    float u0,v0,u1,v1,u2,v2;
    float z0,z1,z2;
    float light;
    uint8_t material;
} vc_textri_t;


static volatile sig_atomic_t g_stop=0;
static uint16_t *g_canvas=NULL;
static track_seg_t g_track[TRACK_SEGMENTS];
static track_world_t g_track_world[TRACK_SEGMENTS+1];
static proj_t g_proj[DRAW_DISTANCE+1];
static traffic_t g_traffic[8];
static uint32_t g_frame=0;
static float g_position=0.0f;
static float g_speed=0.0f;
static float g_player_x=0.0f;
static float g_world_x=OSM_CITY_SPAWN_X;
static float g_world_y=OSM_CITY_SPAWN_Y;
static float g_world_z=OSM_CITY_SPAWN_Z;
static int g_osm_city_mode=1;
static int g_vc_city_mode=0;
static int g_vc_debug_flat=0;
static int g_vc_last_queued=0;
static int g_vc_last_visible_sectors=0;
static int g_vc_last_cap_hit=0;
static vc_runtime_map_t g_vc_map;
static float g_vc_ground_y=0.0f;
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
static float g_camera_heading=0.0f;
static float g_camera_heading_vel=0.0f;
static float g_camera_arm_heading=0.0f;
static float g_camera_arm_heading_vel=0.0f;
static float g_camera_x=0.0f,g_camera_y=0.0f,g_camera_z=0.0f;
static float g_camera_distance=CHASE_NEAR_DISTANCE;
static float g_camera_distance_vel=0.0f;
static float g_camera_target_distance=CHASE_NEAR_DISTANCE;
static float g_camera_height=CHASE_NEAR_HEIGHT;
static float g_camera_height_vel=0.0f;
static float g_camera_target_height=CHASE_NEAR_HEIGHT;
static int g_camera_initialized=0;
static int g_lap=1;

typedef struct {
    uint64_t sky_ns;
    uint64_t track_ns;
    uint64_t props_ns;
    uint64_t shadow_ns;
    uint64_t car_ns;
    uint64_t hud_ns;
    uint64_t total_ns;
    uint64_t max_total_ns;
    uint64_t max_track_ns;
    uint64_t max_props_ns;
    uint64_t max_car_ns;
    unsigned frames;
} render_prof_t;

static render_prof_t g_prof;

static void build_world_track(void);



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
#define MAX_VC_DRAW_TRIS 16384
static v3f_t g_mesh_rv[MAX_MESH_VERTS];
static v3f_t g_mesh_cam[MAX_MESH_VERTS];
static sv3_t g_mesh_sv[MAX_MESH_VERTS];
static drawtri_t g_mesh_out[MAX_DRAW_TRIS];
static textri_t g_tex_out[MAX_DRAW_TRIS];
static vc_textri_t g_vc_tex_out[MAX_VC_DRAW_TRIS];
static citytri_t g_city_out[MAX_DRAW_TRIS];
static v2f_t g_vc_mesh_uv[MAX_MESH_VERTS];
static uint16_t g_city_zbuf[RW*RH];
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

static uint16_t C_SKY,C_VC_FOG,C_GRASS1,C_GRASS2,C_ROAD1,C_ROAD2,C_RUMBLE1,C_RUMBLE2,C_LANE,C_WHITE,C_BLACK,C_RED,C_BLUE,C_GLASS;
static uint16_t g_shade_lut[8][32768];
static uint16_t g_fog_lut[8][32768];

static uint16_t pack1555(unsigned r,unsigned g,unsigned b)
{
    if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;
    return (uint16_t)(0x8000U|((r>>3)<<10)|((g>>3)<<5)|(b>>3));
}

static void init_colors(void)
{
    C_SKY=pack1555(126,190,236);
    C_VC_FOG=pack1555(132,181,210);
    C_GRASS1=pack1555(55,132,67);
    C_GRASS2=pack1555(46,116,59);
    C_ROAD1=pack1555(63,66,70);
    C_ROAD2=pack1555(70,73,77);
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

static void probe_tde_backend(void)
{
    const char *devs[]={"/dev/tde","/dev/hi_tde","/dev/umap/tde"};
    const char *libs[]={"/lib/libtde.so","/usr/lib/libtde.so","/mnt/usb/H3531/LIB/libtde.so"};
    int i,dev_found=0,lib_found=0;
    const char *dev_path="-",*lib_path="-";
    for(i=0;i<(int)(sizeof(devs)/sizeof(devs[0]));++i){
        if(access(devs[i],F_OK)==0){dev_found=1;dev_path=devs[i];break;}
    }
    for(i=0;i<(int)(sizeof(libs)/sizeof(libs[0]));++i){
        if(access(libs[i],R_OK)==0){lib_found=1;lib_path=libs[i];break;}
    }
    fprintf(stderr,
        "[racer] TDE probe device=%s(%s) userspace=%s(%s) backend=cpu-exact2x\n",
        dev_found?"yes":"no",dev_path,lib_found?"yes":"no",lib_path);
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

static void init_shade_lut(void)
{
    int level,c;
    for(level=0;level<8;++level){
        float k=0.38f+(0.74f*(float)level/7.0f);
        for(c=0;c<32768;++c){
            uint16_t src=(uint16_t)(0x8000U|(unsigned)c);
            g_shade_lut[level][c]=shade1555(src,k);
        }
    }
}

static int shade_level(float light)
{
    int level=(int)((light-0.38f)*(7.0f/0.74f)+0.5f);
    if(level<0)level=0;
    if(level>7)level=7;
    return level;
}

static void init_fog_lut(void)
{
    int level,c;
    unsigned fr=(C_VC_FOG>>10)&31U,fg=(C_VC_FOG>>5)&31U,fb=C_VC_FOG&31U;
    for(level=0;level<8;++level){
        for(c=0;c<32768;++c){
            unsigned r=((unsigned)c>>10)&31U,g=((unsigned)c>>5)&31U,b=(unsigned)c&31U;
            unsigned inv=(unsigned)(7-level);
            r=(r*inv+fr*(unsigned)level+3U)/7U;
            g=(g*inv+fg*(unsigned)level+3U)/7U;
            b=(b*inv+fb*(unsigned)level+3U)/7U;
            g_fog_lut[level][c]=(uint16_t)(0x8000U|(r<<10)|(g<<5)|b);
        }
    }
}

static int vc_fog_level_for_z(float z)
{
    float s=g_vc_map.world_scale>1.0f?g_vc_map.world_scale:240.0f;
    float start=s*30.0f;
    float end=s*78.0f;
    int level;
    if(z<=start)return 0;
    if(z>=end)return 7;
    level=(int)(((z-start)/(end-start))*7.0f+0.5f);
    if(level<0)level=0;if(level>7)level=7;
    return level;
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

static void fill_tri2d_z(
    int x0,int y0,float z0,
    int x1,int y1,float z1,
    int x2,int y2,float z2,
    uint16_t color)
{
    const float DEPTH_SCALE=2949075.0f; /* 45 * 65535 */
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int area;
    int e0dx,e0dy,e1dx,e1dy,e2dx,e2dy;
    int row0,row1,row2;
    float inv_area,d0,d1,d2,ddx,ddy,rowd;
    int32_t ddx_fx,ddy_fx,row_fx;

    if(z0<=0.0f||z1<=0.0f||z2<=0.0f)return;
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

    d0=DEPTH_SCALE/z0;d1=DEPTH_SCALE/z1;d2=DEPTH_SCALE/z2;
    if(d0>65535.0f)d0=65535.0f;
    if(d1>65535.0f)d1=65535.0f;
    if(d2>65535.0f)d2=65535.0f;
    ddx=((d1-d0)*(float)(y2-y0)-(d2-d0)*(float)(y1-y0))*inv_area;
    ddy=((d2-d0)*(float)(x1-x0)-(d1-d0)*(float)(x2-x0))*inv_area;
    rowd=d0+ddx*((float)minx-x0)+ddy*((float)miny-y0);
    ddx_fx=(int32_t)(ddx*256.0f);
    ddy_fx=(int32_t)(ddy*256.0f);
    row_fx=(int32_t)(rowd*256.0f);

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);
    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        int32_t dfx=row_fx;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        uint16_t *zrow=g_city_zbuf+(size_t)y*RW;
        for(x=minx;x<=maxx;++x){
            int inside=(area>0)?(w0>=0&&w1>=0&&w2>=0):(w0<=0&&w1<=0&&w2<=0);
            if(inside){
                int di=dfx>>8;
                if(di<1)di=1;if(di>65535)di=65535;
                if((uint16_t)di>zrow[x]){
                    zrow[x]=(uint16_t)di;
                    dst[x]=color;
                }
            }
            w0+=e0dx;w1+=e1dx;w2+=e2dx;
            dfx+=ddx_fx;
        }
        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_fx+=ddy_fx;
    }
}


static float vc_wrap_uv(float v)
{
    if(v>=0.0f&&v<=1.0f)return v;
    v=v-floorf(v);
    if(v<0.0f)v+=1.0f;
    return v;
}

static void fill_tri_vc_textured_z(const vc_textri_t *t)
{
    enum { CORR_BLOCK=8 };
    const float DEPTH_SCALE=2949075.0f; /* 45 * 65535 */
    int x0=t->x0,y0=t->y0,x1=t->x1,y1=t->y1,x2=t->x2,y2=t->y2;
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y,area;
    int e0dx,e0dy,e1dx,e1dy,e2dx,e2dy,row0,row1,row2;
    float inv_area;
    float q0,q1,q2,uq0,uq1,uq2,vq0,vq1,vq2;
    float dq_dx,dq_dy,duq_dx,duq_dy,dvq_dx,dvq_dy;
    float row_q,row_uq,row_vq;
    const vc_material_t *mat;
    int level=shade_level(t->light);

    if(t->material>=g_vc_map.material_count)return;
    mat=&g_vc_map.materials[t->material];
    if(t->z0<=0.0f||t->z1<=0.0f||t->z2<=0.0f)return;

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

    q0=1.0f/t->z0;q1=1.0f/t->z1;q2=1.0f/t->z2;
    uq0=t->u0*q0;uq1=t->u1*q1;uq2=t->u2*q2;
    vq0=t->v0*q0;vq1=t->v1*q1;vq2=t->v2*q2;

#define VC_ATTR_GRAD(a0,a1,a2,dx,dy) do {     (dx)=(((a1)-(a0))*(float)(y2-y0)-((a2)-(a0))*(float)(y1-y0))*inv_area;     (dy)=(((a2)-(a0))*(float)(x1-x0)-((a1)-(a0))*(float)(x2-x0))*inv_area; } while(0)
    {
        float dq_dx0,dq_dy0,duq_dx0,duq_dy0,dvq_dx0,dvq_dy0;
        VC_ATTR_GRAD(q0,q1,q2,dq_dx0,dq_dy0);
        VC_ATTR_GRAD(uq0,uq1,uq2,duq_dx0,duq_dy0);
        VC_ATTR_GRAD(vq0,vq1,vq2,dvq_dx0,dvq_dy0);
        dq_dx=dq_dx0;dq_dy=dq_dy0;
        duq_dx=duq_dx0;duq_dy=duq_dy0;
        dvq_dx=dvq_dx0;dvq_dy=dvq_dy0;
    }
#undef VC_ATTR_GRAD

    row_q=q0+dq_dx*((float)minx-x0)+dq_dy*((float)miny-y0);
    row_uq=uq0+duq_dx*((float)minx-x0)+duq_dy*((float)miny-y0);
    row_vq=vq0+dvq_dx*((float)minx-x0)+dvq_dy*((float)miny-y0);

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);
    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        float q=row_q,uq=row_uq,vq=row_vq;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        uint16_t *zrow=g_city_zbuf+(size_t)y*RW;
        int corr_left=0;
        int fog_left=0,fog_now=0;
        float u_now=0.0f,v_now=0.0f,u_step=0.0f,v_step=0.0f;

        for(x=minx;x<=maxx;++x){
            int inside=(area>0)?(w0>=0&&w1>=0&&w2>=0):(w0<=0&&w1<=0&&w2<=0);
            if(inside){
                int di=(int)(q*DEPTH_SCALE);
                if(fog_left<=0){
                    float z_now=(fabsf(q)>1.0e-12f)?(1.0f/q):1.0e9f;
                    fog_now=vc_fog_level_for_z(z_now);
                    fog_left=CORR_BLOCK;
                }
                if(di<1)di=1;if(di>65535)di=65535;

                if((uint16_t)di>zrow[x]){
                    uint16_t out_color=0;
                    int opaque=1;

                    if((mat->flags&1U) && mat->w>0 && mat->h>0 && g_vc_map.atlas){
                        if(corr_left<=0){
                            float qn=q+dq_dx*(float)CORR_BLOCK;
                            float invq=(fabsf(q)>1.0e-12f)?(1.0f/q):0.0f;
                            float invqn=(fabsf(qn)>1.0e-12f)?(1.0f/qn):invq;
                            float un=(uq+duq_dx*(float)CORR_BLOCK)*invqn;
                            float vn=(vq+dvq_dx*(float)CORR_BLOCK)*invqn;
                            u_now=uq*invq;
                            v_now=vq*invq;
                            u_step=(un-u_now)*(1.0f/(float)CORR_BLOCK);
                            v_step=(vn-v_now)*(1.0f/(float)CORR_BLOCK);
                            corr_left=CORR_BLOCK;
                        }
                        {
                            float fu=vc_wrap_uv(u_now);
                            float fv=vc_wrap_uv(v_now);
                            int tx=(int)(fu*(float)(mat->w-1)+0.5f);
                            int ty=(int)(fv*(float)(mat->h-1)+0.5f);
                            uint16_t tex=g_vc_map.atlas[
                                ((unsigned)mat->y+(unsigned)ty)*g_vc_map.atlas_w+
                                ((unsigned)mat->x+(unsigned)tx)
                            ];
                            if(tex&0x8000U){
                                out_color=g_shade_lut[level][tex&0x7fffU];
                                if(fog_now>0)out_color=g_fog_lut[fog_now][out_color&0x7fffU];
                            }else opaque=0;
                        }
                    }else{
                        out_color=shade1555(mat->fallback,t->light);
                        if(fog_now>0)out_color=g_fog_lut[fog_now][out_color&0x7fffU];
                    }

                    if(opaque){
                        zrow[x]=(uint16_t)di;
                        dst[x]=out_color;
                    }
                }

                if((mat->flags&1U) && mat->w>0 && mat->h>0){
                    if(corr_left<=0){
                        float invq=(fabsf(q)>1.0e-12f)?(1.0f/q):0.0f;
                        u_now=uq*invq;v_now=vq*invq;
                        u_step=0.0f;v_step=0.0f;
                        corr_left=1;
                    }
                    u_now+=u_step;v_now+=v_step;
                    corr_left--;
                }
                fog_left--;
            }else{
                corr_left=0;
                fog_left=0;
            }

            w0+=e0dx;w1+=e1dx;w2+=e2dx;
            q+=dq_dx;uq+=duq_dx;vq+=dvq_dx;
        }
        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_q+=dq_dy;row_uq+=duq_dy;row_vq+=dvq_dy;
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
    float inv_area,du_dx,du_dy,dv_dx,dv_dy;
    int32_t du_dx_fx,du_dy_fx,dv_dx_fx,dv_dy_fx;
    int32_t row_u_fx,row_v_fx;
    int level=shade_level(light);

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

    du_dx_fx=(int32_t)(du_dx*65536.0f);
    du_dy_fx=(int32_t)(du_dy*65536.0f);
    dv_dx_fx=(int32_t)(dv_dx*65536.0f);
    dv_dy_fx=(int32_t)(dv_dy*65536.0f);
    row_u_fx=(int32_t)((u0+du_dx*((float)minx-x0)+du_dy*((float)miny-y0))*65536.0f);
    row_v_fx=(int32_t)((v0+dv_dx*((float)minx-x0)+dv_dy*((float)miny-y0))*65536.0f);

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);

    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        int32_t uu=row_u_fx,vv=row_v_fx;
        uint16_t *dst=g_canvas+(size_t)y*RW;

        for(x=minx;x<=maxx;++x){
            int inside=(area>0)?(w0>=0&&w1>=0&&w2>=0):(w0<=0&&w1<=0&&w2<=0);
            if(inside){
                int tx=(uu+32768)>>16,ty=(vv+32768)>>16;
                uint16_t tex;
                if(tx<0)tx=0;if(tx>=tex_w)tx=tex_w-1;
                if(ty<0)ty=0;if(ty>=tex_h)ty=tex_h-1;
                tex=texture[ty*tex_w+tx];
                if(tex&0x8000U)dst[x]=g_shade_lut[level][tex&0x7fffU];
            }
            w0+=e0dx;w1+=e1dx;w2+=e2dx;
            uu+=du_dx_fx;vv+=dv_dx_fx;
        }

        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_u_fx+=du_dy_fx;row_v_fx+=dv_dy_fx;
    }
}


/*
 * Perspective-correct textured triangle for the world-space road.
 *
 * The generic vehicle rasterizer intentionally remains affine for now because
 * it is small on screen and already costs ~12 ms/frame. The road, however,
 * spans a large depth range, so affine screen-space UVs visibly "swim" as the
 * chase camera moves. Interpolate 1/z, u/z and v/z, then recover u/v in short
 * 8-pixel blocks. This needs only about one reciprocal per eight covered
 * pixels after the first block, avoiding an expensive divide for every pixel.
 *
 * V wraps because the asphalt atlas repeats longitudinally; U stays clamped
 * across the physical road width.
 */
static void fill_tri_textured_perspective_wrap(
    int x0,int y0,float u0,float v0,float z0,
    int x1,int y1,float u1,float v1,float z1,
    int x2,int y2,float u2,float v2,float z2,
    float light,const uint16_t *texture,int tex_w,int tex_h)
{
    enum { CORR_BLOCK=8 };
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int area;
    int e0dx,e0dy,e1dx,e1dy,e2dx,e2dy;
    int row0,row1,row2;
    float inv_area;
    float q0,q1,q2,uq0,uq1,uq2,vq0,vq1,vq2;
    float dq_dx,dq_dy,duq_dx,duq_dy,dvq_dx,dvq_dy;
    float row_q,row_uq,row_vq;
    int level=shade_level(light);
    int wrap_mask=tex_h-1;
    float inv_block=1.0f/(float)CORR_BLOCK;

    if(z0<=0.0f||z1<=0.0f||z2<=0.0f)return;

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

    q0=1.0f/z0;q1=1.0f/z1;q2=1.0f/z2;
    uq0=u0*q0;uq1=u1*q1;uq2=u2*q2;
    vq0=v0*q0;vq1=v1*q1;vq2=v2*q2;

#define ATTR_GRAD(a0,a1,a2,dx,dy) do { \
    (dx)=(((a1)-(a0))*(float)(y2-y0)-((a2)-(a0))*(float)(y1-y0))*inv_area; \
    (dy)=(((a2)-(a0))*(float)(x1-x0)-((a1)-(a0))*(float)(x2-x0))*inv_area; \
} while(0)
    ATTR_GRAD(q0,q1,q2,dq_dx,dq_dy);
    ATTR_GRAD(uq0,uq1,uq2,duq_dx,duq_dy);
    ATTR_GRAD(vq0,vq1,vq2,dvq_dx,dvq_dy);
#undef ATTR_GRAD

    row_q=q0+dq_dx*((float)minx-x0)+dq_dy*((float)miny-y0);
    row_uq=uq0+duq_dx*((float)minx-x0)+duq_dy*((float)miny-y0);
    row_vq=vq0+dvq_dx*((float)minx-x0)+dvq_dy*((float)miny-y0);

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);
    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        float q=row_q,uq=row_uq,vq=row_vq;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        int corr_left=0;
        int32_t uu_fx=0,vv_fx=0,du_fx=0,dv_fx=0;

        for(x=minx;x<=maxx;++x){
            int inside=(area>0)?(w0>=0&&w1>=0&&w2>=0):(w0<=0&&w1<=0&&w2<=0);
            if(inside){
                if(corr_left<=0){
                    float qn=q+dq_dx*(float)CORR_BLOCK;
                    float u_now,v_now,u_next,v_next;
                    float invq=(fabsf(q)>1.0e-12f)?(1.0f/q):0.0f;
                    float invqn=(fabsf(qn)>1.0e-12f)?(1.0f/qn):invq;
                    u_now=uq*invq;
                    v_now=vq*invq;
                    u_next=(uq+duq_dx*(float)CORR_BLOCK)*invqn;
                    v_next=(vq+dvq_dx*(float)CORR_BLOCK)*invqn;
                    uu_fx=(int32_t)(u_now*65536.0f);
                    vv_fx=(int32_t)(v_now*65536.0f);
                    du_fx=(int32_t)((u_next-u_now)*inv_block*65536.0f);
                    dv_fx=(int32_t)((v_next-v_now)*inv_block*65536.0f);
                    corr_left=CORR_BLOCK;
                }
                {
                    int tx=(uu_fx+32768)>>16;
                    int ty=(vv_fx+32768)>>16;
                    uint16_t tex;
                    if(tx<0)tx=0;if(tx>=tex_w)tx=tex_w-1;
                    if((tex_h&(tex_h-1))==0)ty=(int)((uint32_t)ty&(uint32_t)wrap_mask);
                    else { ty%=tex_h;if(ty<0)ty+=tex_h; }
                    tex=texture[ty*tex_w+tx];
                    if(tex&0x8000U)dst[x]=g_shade_lut[level][tex&0x7fffU];
                }
                uu_fx+=du_fx;vv_fx+=dv_fx;
                corr_left--;
            }else{
                corr_left=0;
            }

            w0+=e0dx;w1+=e1dx;w2+=e2dx;
            q+=dq_dx;uq+=duq_dx;vq+=dvq_dx;
        }

        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_q+=dq_dy;row_uq+=duq_dy;row_vq+=dvq_dy;
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

typedef struct {
    float sx,cx,sy,cy,sz,cz;
} rotxyz_t;

static rotxyz_t make_rotxyz(float pitch,float yaw,float roll)
{
    rotxyz_t r;
    r.sx=sinf(pitch);r.cx=cosf(pitch);
    r.sy=sinf(yaw);r.cy=cosf(yaw);
    r.sz=sinf(roll);r.cz=cosf(roll);
    return r;
}

static void rotate_xyz_precomputed(v3f_t in,const rotxyz_t *rot,v3f_t *out)
{
    float x=in.x,y=in.y,z=in.z;
    float y1=y*rot->cx-z*rot->sx;
    float z1=y*rot->sx+z*rot->cx;
    float x2=x*rot->cy-z1*rot->sy;
    float z2=x*rot->sy+z1*rot->cy;
    out->x=x2*rot->cz-y1*rot->sz;
    out->y=x2*rot->sz+y1*rot->cz;
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
    rotxyz_t body_rot=make_rotxyz(body_pitch,body_yaw,body_roll);
    rotxyz_t wheel_rot=make_rotxyz(wheel_spin,part_steer,0.0f);
    int i,n=*queued;
    (void)variant;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_DRAW_TRIS)return;
    if(n>=MAX_DRAW_TRIS)return;

    for(i=0;i<vcount;++i){
        v3f_t p=verts[i],q,r;
        p.x*=scale;p.y*=scale;p.z*=scale;

        if(is_wheel){
            rotate_xyz_precomputed(p,&wheel_rot,&q);
            q.x+=pivot.x*scale;
            q.y+=pivot.y*scale;
            q.z+=pivot.z*scale;
        }else{
            q=p;
        }

        rotate_xyz_precomputed(q,&body_rot,&r);
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
                if(g_vc_city_mode && y>145){
                    int fl=(y-145)*7/35;
                    if(fl<0)fl=0;if(fl>7)fl=7;
                    c=g_fog_lut[fl][c&0x7fffU];
                }
            }else{
                c=g_vc_city_mode?C_VC_FOG:C_GRASS1;
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

    fprintf(stderr,"[racer] HIFB ready 1280x720 <- 640x360 exact2x Stage7.13 vcmap2-mask-fog92-nearfirst fullqueue alpha-test city-zbuffer fixed60\n");
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

        {
            uint64_t p0=mono_ns();
            uint64_t pns;
            video_present_buffer(v,v->canvas[idx]);
            pns=mono_ns()-p0;

            pthread_mutex_lock(&v->lock);
            v->present_ns_total+=pns;
            if(pns>v->present_ns_max)v->present_ns_max=pns;
            v->present_profile_count++;
        }
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
            else if(e.code==KEY_T && e.value==1 && g_vc_city_mode){
                g_vc_debug_flat=!g_vc_debug_flat;
                fprintf(stderr,"[racer] VC render mode=%s\n",
                        g_vc_debug_flat?"flat":"textured");
            }
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
        if((i%15)==0)g_track[i].flags|=TF_TREES;
        if(((i>165&&i<275)&&(i%29)==0) ||
           ((i>300&&i<650)&&(i%21)==0) ||
           ((i>835&&i<945)&&(i%29)==0) ||
           ((i>1015&&i<1245)&&(i%23)==0))g_track[i].flags|=TF_CITY;
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
    build_world_track();

    /* Stage7.4: start on a real OSM2World street in world coordinates. */
    g_world_x=OSM_CITY_SPAWN_X;
    g_world_y=OSM_CITY_SPAWN_Y;
    g_world_z=OSM_CITY_SPAWN_Z;
    g_vehicle_heading=OSM_CITY_SPAWN_YAW;
    g_position=0.0f;
    g_player_x=0.0f;
    g_camera_initialized=0;
}

static float track_length(void){return TRACK_SEGMENTS*SEG_LEN;}

static int seg_index_from_pos(float p)
{
    int i=(int)(p/SEG_LEN)%TRACK_SEGMENTS;
    if(i<0)i+=TRACK_SEGMENTS;
    return i;
}

static float lerpf(float a,float b,float t){return a+(b-a)*t;}


static float wrap_angle(float a)
{
    while(a>3.14159265f)a-=6.2831853f;
    while(a<-3.14159265f)a+=6.2831853f;
    return a;
}

static float approach_angle(float cur,float target,float step)
{
    float d=wrap_angle(target-cur);
    if(d>step)d=step;
    if(d<-step)d=-step;
    return wrap_angle(cur+d);
}

static void spring_scalar(
    float *value,float *velocity,float target,
    float hz,float damping,float dt)
{
    float omega=6.2831853f*hz;
    float accel=omega*omega*(target-*value)-2.0f*damping*omega*(*velocity);
    *velocity+=accel*dt;
    *value+=(*velocity)*dt;
}

static void spring_angle(
    float *value,float *velocity,float target,
    float hz,float damping,float dt)
{
    float omega=6.2831853f*hz;
    float error=wrap_angle(target-*value);
    float accel=omega*omega*error-2.0f*damping*omega*(*velocity);
    *velocity+=accel*dt;
    *value=wrap_angle(*value+(*velocity)*dt);
}

static void rotate_xz(float x,float z,float yaw,float *ox,float *oz)
{
    float cs=cosf(yaw),sn=sinf(yaw);
    *ox=x*cs+z*sn;
    *oz=-x*sn+z*cs;
}

static void build_world_track(void)
{
    float x=0.0f,z=0.0f,yaw=0.0f;
    int i;
    for(i=0;i<TRACK_SEGMENTS;++i){
        g_track_world[i].x=x;
        g_track_world[i].y=g_track[i].y;
        g_track_world[i].z=z;
        g_track_world[i].yaw=yaw;

        yaw=wrap_angle(yaw+g_track[i].curve*TRACK_CURVE_SCALE);
        x+=sinf(yaw)*SEG_LEN;
        z+=cosf(yaw)*SEG_LEN;
    }
    g_track_world[TRACK_SEGMENTS].x=x;
    g_track_world[TRACK_SEGMENTS].y=g_track[TRACK_SEGMENTS-1].y;
    g_track_world[TRACK_SEGMENTS].z=z;
    g_track_world[TRACK_SEGMENTS].yaw=yaw;
}

static void repeat_transform(float *x,float *z,float *yaw,int lap)
{
    float ex=g_track_world[TRACK_SEGMENTS].x;
    float ez=g_track_world[TRACK_SEGMENTS].z;
    float eyaw=g_track_world[TRACK_SEGMENTS].yaw;
    while(lap>0){
        float rx,rz;
        rotate_xz(*x,*z,eyaw,&rx,&rz);
        *x=ex+rx;*z=ez+rz;*yaw=wrap_angle(*yaw+eyaw);
        lap--;
    }
    while(lap<0){
        float dx=*x-ex,dz=*z-ez,rx,rz;
        rotate_xz(dx,dz,-eyaw,&rx,&rz);
        *x=rx;*z=rz;*yaw=wrap_angle(*yaw-eyaw);
        lap++;
    }
}

static void raw_track_pose(int raw,track_world_t *o)
{
    int lap=raw/TRACK_SEGMENTS;
    int idx=raw%TRACK_SEGMENTS;
    if(idx<0){idx+=TRACK_SEGMENTS;lap--;}
    *o=g_track_world[idx];
    repeat_transform(&o->x,&o->z,&o->yaw,lap);
}

static void track_pose_at(float pos,float lateral,track_world_t *o)
{
    float sf=pos/SEG_LEN;
    int raw=(int)floorf(sf);
    float t=sf-(float)raw;
    track_world_t a,b;
    float dyaw;
    raw_track_pose(raw,&a);
    raw_track_pose(raw+1,&b);
    dyaw=wrap_angle(b.yaw-a.yaw);

    o->x=a.x+(b.x-a.x)*t;
    o->y=a.y+(b.y-a.y)*t;
    o->z=a.z+(b.z-a.z)*t;
    o->yaw=wrap_angle(a.yaw+dyaw*t);

    if(lateral!=0.0f){
        o->x+=cosf(o->yaw)*lateral;
        o->z-=sinf(o->yaw)*lateral;
    }
}

static int project_world_point(
    float wx,float wy,float wz,
    float camx,float camy,float camz,float camyaw,
    sv3_t *o)
{
    static int basis_valid=0;
    static float basis_yaw=0.0f,basis_cs=1.0f,basis_sn=0.0f;
    float dx=wx-camx,dy=wy-camy,dz=wz-camz;
    float cx,cz,s;

    /*
     * Every world-space draw pass in a frame uses the same chase-camera yaw.
     * Computing sin/cos for every road/curb/prop vertex was pure duplicated
     * work on Cortex-A9. Cache the camera basis and refresh it only when the
     * yaw actually changes between frames.
     */
    if(!basis_valid||camyaw!=basis_yaw){
        basis_yaw=camyaw;
        basis_cs=cosf(camyaw);
        basis_sn=sinf(camyaw);
        basis_valid=1;
    }

    cx=dx*basis_cs-dz*basis_sn;
    cz=dx*basis_sn+dz*basis_cs;
    if(cz<45.0f){o->valid=0;return 0;}
    s=TRACK_FOCAL/cz;
    o->sx=RW*0.5f+cx*s;
    o->sy=TRACK_SCREEN_Y-dy*s;
    o->z=cz;o->valid=1;
    return 1;
}

static void get_player_world(track_world_t *car,float *road_yaw)
{
    if(g_osm_city_mode||g_vc_city_mode){
        car->x=g_world_x;
        car->y=g_world_y;
        car->z=g_world_z;
        car->yaw=g_vehicle_heading;
        if(road_yaw)*road_yaw=g_vehicle_heading;
        return;
    }

    {
        track_world_t center;
        track_pose_at(g_position,0.0f,&center);
        track_pose_at(g_position,g_player_x*ROAD_WIDTH,car);
        if(road_yaw)*road_yaw=center.yaw;
    }
}

static void reset_chase_camera(void)
{
    track_world_t car;
    float road_yaw;
    float speed_ratio=fabsf(g_speed)/MAX_SPEED;
    float distance=CHASE_NEAR_DISTANCE+(CHASE_FAR_DISTANCE-CHASE_NEAR_DISTANCE)*speed_ratio;
    float height=CHASE_NEAR_HEIGHT+(CHASE_FAR_HEIGHT-CHASE_NEAR_HEIGHT)*speed_ratio;

    get_player_world(&car,&road_yaw);
    (void)road_yaw;

    g_camera_arm_heading=g_vehicle_heading;
    g_camera_arm_heading_vel=0.0f;
    g_camera_heading=g_vehicle_heading;
    g_camera_heading_vel=0.0f;

    g_camera_distance=distance;
    g_camera_distance_vel=0.0f;
    g_camera_target_distance=distance;
    g_camera_height=height;
    g_camera_height_vel=0.0f;
    g_camera_target_height=height;

    g_camera_x=car.x-sinf(g_camera_arm_heading)*g_camera_distance;
    g_camera_z=car.z-cosf(g_camera_arm_heading)*g_camera_distance;
    g_camera_y=car.y+g_camera_height;
    g_camera_initialized=1;
}

static void update_chase_camera(float speed_ratio)
{
    const float dt=1.0f/60.0f;
    track_world_t car;
    float road_yaw;
    float distance,height;
    float speed_delta=fabsf(g_speed)-fabsf(g_prev_speed);
    float accel_push=0.0f;
    float look_x,look_z,desired_look;

    if(speed_ratio<0.0f)speed_ratio=0.0f;
    if(speed_ratio>1.0f)speed_ratio=1.0f;

    get_player_world(&car,&road_yaw);
    (void)road_yaw;

    if(!g_camera_initialized){
        reset_chase_camera();
        return;
    }

    /*
     * Spring arm rather than springing the whole world-space position.
     * This keeps the camera responsive at our large world-unit velocities
     * while preserving the elastic acceleration/braking feel.
     */
    if(g_speed>=0.0f){
        if(speed_delta>0.0f)
            accel_push=fminf(110.0f,speed_delta*115.0f);
        distance=CHASE_NEAR_DISTANCE+
                 (CHASE_FAR_DISTANCE-CHASE_NEAR_DISTANCE)*speed_ratio+
                 accel_push;
    }else{
        distance=CHASE_NEAR_DISTANCE+
                 (CHASE_REVERSE_DISTANCE-CHASE_NEAR_DISTANCE)*
                 fminf(1.0f,fabsf(g_speed)/REVERSE_SPEED);
    }

    height=CHASE_NEAR_HEIGHT+
           (CHASE_FAR_HEIGHT-CHASE_NEAR_HEIGHT)*speed_ratio;

    g_camera_target_distance=distance;
    g_camera_target_height=height;

    /* Position arm turns more lazily than the view: this produces a soft
       outward swing through corners instead of a rigid camera-car bar. */
    spring_angle(&g_camera_arm_heading,&g_camera_arm_heading_vel,
                 g_vehicle_heading,1.45f,0.78f,dt);

    spring_scalar(&g_camera_distance,&g_camera_distance_vel,
                  g_camera_target_distance,1.70f,0.80f,dt);
    spring_scalar(&g_camera_height,&g_camera_height_vel,
                  g_camera_target_height,1.55f,0.88f,dt);

    if(g_camera_distance<880.0f)g_camera_distance=880.0f;
    if(g_camera_distance>1660.0f)g_camera_distance=1660.0f;

    g_camera_x=car.x-sinf(g_camera_arm_heading)*g_camera_distance;
    g_camera_z=car.z-cosf(g_camera_arm_heading)*g_camera_distance;
    g_camera_y=car.y+g_camera_height;

    /*
     * View direction is a slightly faster spring aimed at a speed-dependent
     * look-ahead point. The arm can swing while the lens smoothly catches up.
     */
    look_x=car.x+sinf(g_vehicle_heading)*(150.0f+310.0f*speed_ratio);
    look_z=car.z+cosf(g_vehicle_heading)*(150.0f+310.0f*speed_ratio);
    desired_look=atan2f(look_x-g_camera_x,look_z-g_camera_z);

    spring_angle(&g_camera_heading,&g_camera_heading_vel,desired_look,
                 2.45f,0.86f,dt);
}

static void get_chase_camera(float *camx,float *camy,float *camz,float *camyaw)
{
    if(!g_camera_initialized)reset_chase_camera();
    *camx=g_camera_x;
    *camy=g_camera_y;
    *camz=g_camera_z;
    *camyaw=g_camera_heading;
}

static void get_player_camera_pose(
    float *ox,float *oy,float *oz,float *relative_yaw,
    float *screen_x,float *screen_y)
{
    track_world_t car;
    float road_yaw;
    float dx,dy,dz,cs,sn,cx,cz;
    float ref_z;

    get_player_world(&car,&road_yaw);
    (void)road_yaw;
    if(!g_camera_initialized)reset_chase_camera();

    dx=car.x-g_camera_x;
    dy=car.y-g_camera_y;
    dz=car.z-g_camera_z;
    cs=cosf(g_camera_heading);
    sn=sinf(g_camera_heading);
    cx=dx*cs-dz*sn;
    cz=dx*sn+dz*cs;

    if(cz<620.0f)cz=620.0f;
    if(cz>1900.0f)cz=1900.0f;

    /*
     * Keep the car renderer's established projection while letting the
     * spring-arm distance and angular swing affect size and screen position.
     */
    ref_z=1660.0f+(cz-CHASE_NEAR_DISTANCE)*0.55f;
    if(ref_z<1500.0f)ref_z=1500.0f;
    if(ref_z>1920.0f)ref_z=1920.0f;

    *oz=ref_z;
    *oy=-1515.0f-(ref_z-1660.0f)*0.30f;
    *ox=tanf(fmaxf(-0.22f,fminf(0.22f,atan2f(cx,cz))))*ref_z*0.55f;
    *relative_yaw=wrap_angle(g_vehicle_heading-g_camera_heading);

    if(screen_x)
        *screen_x=RW*0.5f+CAMERA_DEPTH/(*oz)*(*ox)*RW*0.5f;
    if(screen_y)
        *screen_y=RH*0.5f-CAMERA_DEPTH/(*oz)*(*oy)*RH*0.5f;

    (void)dy;
}

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


static const uint8_t g_far_mountain_ridge[32]={
    22,28,34,31,25,38,52,43,35,30,42,55,63,49,37,31,
    27,39,48,45,33,29,36,50,58,46,35,32,41,47,36,26
};
static const uint8_t g_near_hill_ridge[32]={
    10,13,17,20,15,12,24,31,25,18,14,19,27,34,28,17,
    12,16,23,29,21,15,18,26,32,24,16,14,20,25,18,12
};

static void draw_ridge_layer(
    const uint8_t *ridge,int count,int cell,int shift,
    int base_y,int amplitude,uint16_t color)
{
    int x,y;
    int period=count*cell;
    if(period<=0)return;
    shift%=period;if(shift<0)shift+=period;

    for(x=0;x<RW;++x){
        int u=x+shift;
        int i0=(u/cell)%count;
        int i1=(i0+1)%count;
        int frac=u%cell;
        int h=((int)ridge[i0]*(cell-frac)+(int)ridge[i1]*frac)/cell;
        int top=base_y-(h*amplitude)/32;
        if(top<0)top=0;
        if(base_y>=RH)base_y=RH-1;
        for(y=top;y<=base_y;++y)
            g_canvas[(size_t)y*RW+x]=color;
    }
}

static void draw_dynamic_sky(void)
{
    float camx,camy,camz,camyaw;
    int x,y,shift;
    const int sky_bottom=150;

    get_chase_camera(&camx,&camy,&camz,&camyaw);
    (void)camx;(void)camy;(void)camz;

    /*
     * Stage7.1 uses a real CC0 Poly Haven pure-sky HDRI, tone-mapped and baked
     * at build time into racer_sky RGB1555.  It rotates only with camera yaw,
     * so the sky is fixed to the world rather than scrolling with road travel.
     */
    shift=(int)(camyaw*(float)RACER_SKY_W/(2.0f*3.1415926f));
    shift%=RACER_SKY_W;if(shift<0)shift+=RACER_SKY_W;

    for(y=0;y<sky_bottom;++y){
        int sy=(y*RACER_SKY_H)/sky_bottom;
        if(sy>=RACER_SKY_H)sy=RACER_SKY_H-1;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        for(x=0;x<RW;++x){
            int sx=((x*RACER_SKY_W)/RW+shift)%RACER_SKY_W;
            dst[x]=racer_sky[sy*RACER_SKY_W+sx];
        }
    }

    /*
     * Two cheap parallax ridges close the empty horizon like classic PS1-era
     * racers: far blue mountains, then greener foothills.  They are screen-
     * space background geometry, so they do not inflate the world triangle
     * queue or the props profile.
     */
    {
        int yaw_shift=(int)(camyaw*86.0f);
        int travel_shift=(int)(g_position*0.00020f);
        draw_ridge_layer(g_far_mountain_ridge,32,28,yaw_shift+travel_shift,
                         130,40,pack1555(92,121,147));
        draw_ridge_layer(g_near_hill_ridge,32,24,yaw_shift/2+travel_shift*2,
                         136,31,pack1555(70,111,83));
    }

    /* Atmospheric horizon haze softens the seam into the ground plane. */
    for(y=122;y<128;++y){
        uint16_t haze=pack1555((unsigned)(143+(y-122)*5),
                               (unsigned)(170+(y-122)*5),
                               (unsigned)(175+(y-122)*4));
        int x0;
        for(x0=0;x0<RW;++x0){
            if(((x0+y)&3)==0)g_canvas[(size_t)y*RW+x0]=haze;
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


static void queue_flat_tri(
    int x0,int y0,int x1,int y1,int x2,int y2,float depth,uint16_t color,int *n)
{
    if(*n>=MAX_DRAW_TRIS)return;
    g_mesh_out[*n].depth=depth;
    g_mesh_out[*n].x0=x0;g_mesh_out[*n].y0=y0;
    g_mesh_out[*n].x1=x1;g_mesh_out[*n].y1=y1;
    g_mesh_out[*n].x2=x2;g_mesh_out[*n].y2=y2;
    g_mesh_out[*n].color=color;
    (*n)++;
}

static void queue_textured_tri(
    const sv3_t *a,const sv3_t *b,const sv3_t *d,
    float u0,float v0,float u1,float v1,float u2,float v2,
    float light,int *n)
{
    if(*n>=MAX_DRAW_TRIS)return;
    g_tex_out[*n].depth=(a->z+b->z+d->z)/3.0f;
    g_tex_out[*n].x0=(int)a->sx;g_tex_out[*n].y0=(int)a->sy;
    g_tex_out[*n].x1=(int)b->sx;g_tex_out[*n].y1=(int)b->sy;
    g_tex_out[*n].x2=(int)d->sx;g_tex_out[*n].y2=(int)d->sy;
    g_tex_out[*n].u0=u0;g_tex_out[*n].v0=v0;
    g_tex_out[*n].u1=u1;g_tex_out[*n].v1=v1;
    g_tex_out[*n].u2=u2;g_tex_out[*n].v2=v2;
    g_tex_out[*n].z0=a->z;g_tex_out[*n].z1=b->z;g_tex_out[*n].z2=d->z;
    g_tex_out[*n].light=light;
    (*n)++;
}

static void draw_true3d_track(void)
{
    track_world_t car;
    float road_yaw;
    float camyaw,camx,camy,camz;
    int base=(int)floorf(g_position/SEG_LEN);
    int k,ntex=0,nflat=0;
    int range_back,range_front;
    float road_rel;

    get_player_world(&car,&road_yaw);
    get_chase_camera(&camx,&camy,&camz,&camyaw);
    road_rel=wrap_angle(g_vehicle_heading-road_yaw);
    if(cosf(road_rel)>0.35f){range_back=24;range_front=110;}
    else if(cosf(road_rel)<-0.35f){range_back=110;range_front=24;}
    else{range_back=70;range_front=70;}

    /* ground plane: the lower half is deliberately calm so the textured road
       remains readable even when the car points across or backwards. */
    fill_rect(0,(int)TRACK_SCREEN_Y,RW,RH-(int)TRACK_SCREEN_Y,pack1555(58,124,65));

    for(k=-range_back;k<range_front;++k){
        int raw0=base+k,raw1=raw0+1;
        track_world_t c0,c1;
        sv3_t l0,r0,l1,r1;
        sv3_t sl0,sr0,sl1,sr1,ol0,or0,ol1,or1;
        float w=ROAD_WIDTH;
        float shoulder=w*1.55f;
        float curb1=w*1.10f;
        float rx0,rz0,rx1,rz1;
        /* Low-frequency asphalt: one texture cycle spans sixteen segments
           instead of eight, reducing visible grain and repetitive shimmer. */
        int tex_phase=raw0&15;
        float v0=(float)(tex_phase*8);
        float v1=(float)((tex_phase+1)*8);
        uint16_t curb=((raw0>>1)&1)?C_RED:C_WHITE;

        raw_track_pose(raw0,&c0);
        raw_track_pose(raw1,&c1);

        rx0=cosf(c0.yaw);rz0=-sinf(c0.yaw);
        rx1=cosf(c1.yaw);rz1=-sinf(c1.yaw);

        if(!project_world_point(c0.x-rx0*w,c0.y,c0.z-rz0*w,camx,camy,camz,camyaw,&l0))continue;
        if(!project_world_point(c0.x+rx0*w,c0.y,c0.z+rz0*w,camx,camy,camz,camyaw,&r0))continue;
        if(!project_world_point(c1.x-rx1*w,c1.y,c1.z-rz1*w,camx,camy,camz,camyaw,&l1))continue;
        if(!project_world_point(c1.x+rx1*w,c1.y,c1.z+rz1*w,camx,camy,camz,camyaw,&r1))continue;

        {
            float zavg=(l0.z+r0.z+l1.z+r1.z)*0.25f;
            if(zavg<10500.0f){
                queue_textured_tri(&l0,&r0,&r1,0,v0,TRACK_ASPHALT_W-1,v0,TRACK_ASPHALT_W-1,v1,0.86f,&ntex);
                queue_textured_tri(&l0,&r1,&l1,0,v0,TRACK_ASPHALT_W-1,v1,0,v1,0.86f,&ntex);
            }else{
                uint16_t farroad=((raw0>>2)&1)?C_ROAD1:C_ROAD2;
                queue_flat_tri((int)l0.sx,(int)l0.sy,(int)r0.sx,(int)r0.sy,(int)r1.sx,(int)r1.sy,
                               (l0.z+r0.z+r1.z)/3.0f,farroad,&nflat);
                queue_flat_tri((int)l0.sx,(int)l0.sy,(int)r1.sx,(int)r1.sy,(int)l1.sx,(int)l1.sy,
                               (l0.z+r1.z+l1.z)/3.0f,farroad,&nflat);
            }
        }

        /* raised curbs and sloped shoulder make the road an actual 3D ribbon. */
        if(project_world_point(c0.x-rx0*curb1,c0.y-18,c0.z-rz0*curb1,camx,camy,camz,camyaw,&sl0) &&
           project_world_point(c1.x-rx1*curb1,c1.y-18,c1.z-rz1*curb1,camx,camy,camz,camyaw,&sl1)){
            queue_flat_tri((int)sl0.sx,(int)sl0.sy,(int)l0.sx,(int)l0.sy,(int)l1.sx,(int)l1.sy,
                           (sl0.z+l0.z+l1.z)/3.0f,curb,&nflat);
            queue_flat_tri((int)sl0.sx,(int)sl0.sy,(int)l1.sx,(int)l1.sy,(int)sl1.sx,(int)sl1.sy,
                           (sl0.z+l1.z+sl1.z)/3.0f,curb,&nflat);
        }
        if(project_world_point(c0.x+rx0*curb1,c0.y-18,c0.z+rz0*curb1,camx,camy,camz,camyaw,&sr0) &&
           project_world_point(c1.x+rx1*curb1,c1.y-18,c1.z+rz1*curb1,camx,camy,camz,camyaw,&sr1)){
            queue_flat_tri((int)r0.sx,(int)r0.sy,(int)sr0.sx,(int)sr0.sy,(int)sr1.sx,(int)sr1.sy,
                           (r0.z+sr0.z+sr1.z)/3.0f,curb,&nflat);
            queue_flat_tri((int)r0.sx,(int)r0.sy,(int)sr1.sx,(int)sr1.sy,(int)r1.sx,(int)r1.sy,
                           (r0.z+sr1.z+r1.z)/3.0f,curb,&nflat);
        }

        /* sloped verge/embankment gives the ribbon thickness and a grounded edge */
        if(project_world_point(c0.x-rx0*shoulder,c0.y-95,c0.z-rz0*shoulder,camx,camy,camz,camyaw,&ol0) &&
           project_world_point(c1.x-rx1*shoulder,c1.y-95,c1.z-rz1*shoulder,camx,camy,camz,camyaw,&ol1) &&
           project_world_point(c0.x-rx0*curb1,c0.y-18,c0.z-rz0*curb1,camx,camy,camz,camyaw,&sl0) &&
           project_world_point(c1.x-rx1*curb1,c1.y-18,c1.z-rz1*curb1,camx,camy,camz,camyaw,&sl1)){
            uint16_t verge=((raw0>>2)&1)?pack1555(45,103,50):pack1555(39,91,43);
            queue_flat_tri((int)ol0.sx,(int)ol0.sy,(int)sl0.sx,(int)sl0.sy,(int)sl1.sx,(int)sl1.sy,
                           (ol0.z+sl0.z+sl1.z)/3.0f,verge,&nflat);
            queue_flat_tri((int)ol0.sx,(int)ol0.sy,(int)sl1.sx,(int)sl1.sy,(int)ol1.sx,(int)ol1.sy,
                           (ol0.z+sl1.z+ol1.z)/3.0f,verge,&nflat);
        }
        if(project_world_point(c0.x+rx0*shoulder,c0.y-95,c0.z+rz0*shoulder,camx,camy,camz,camyaw,&or0) &&
           project_world_point(c1.x+rx1*shoulder,c1.y-95,c1.z+rz1*shoulder,camx,camy,camz,camyaw,&or1) &&
           project_world_point(c0.x+rx0*curb1,c0.y-18,c0.z+rz0*curb1,camx,camy,camz,camyaw,&sr0) &&
           project_world_point(c1.x+rx1*curb1,c1.y-18,c1.z+rz1*curb1,camx,camy,camz,camyaw,&sr1)){
            uint16_t verge=((raw0>>2)&1)?pack1555(45,103,50):pack1555(39,91,43);
            queue_flat_tri((int)sr0.sx,(int)sr0.sy,(int)or0.sx,(int)or0.sy,(int)or1.sx,(int)or1.sy,
                           (sr0.z+or0.z+or1.z)/3.0f,verge,&nflat);
            queue_flat_tri((int)sr0.sx,(int)sr0.sy,(int)or1.sx,(int)or1.sy,(int)sr1.sx,(int)sr1.sy,
                           (sr0.z+or1.z+sr1.z)/3.0f,verge,&nflat);
        }

        /* lane centre dashes are real projected quads, not screen-space lines. */
        if((raw0&7)<4){
            float lw=38.0f;
            sv3_t a,b,d,e;
            if(project_world_point(c0.x-rx0*lw,c0.y+3,c0.z-rz0*lw,camx,camy,camz,camyaw,&a) &&
               project_world_point(c0.x+rx0*lw,c0.y+3,c0.z+rz0*lw,camx,camy,camz,camyaw,&b) &&
               project_world_point(c1.x+rx1*lw,c1.y+3,c1.z+rz1*lw,camx,camy,camz,camyaw,&d) &&
               project_world_point(c1.x-rx1*lw,c1.y+3,c1.z-rz1*lw,camx,camy,camz,camyaw,&e)){
                queue_flat_tri((int)a.sx,(int)a.sy,(int)b.sx,(int)b.sy,(int)d.sx,(int)d.sy,
                               (a.z+b.z+d.z)/3.0f,C_LANE,&nflat);
                queue_flat_tri((int)a.sx,(int)a.sy,(int)d.sx,(int)d.sy,(int)e.sx,(int)e.sy,
                               (a.z+d.z+e.z)/3.0f,C_LANE,&nflat);
            }
        }

    }

    qsort(g_tex_out,(size_t)ntex,sizeof(g_tex_out[0]),cmp_textri_far_first);
    for(k=0;k<ntex;++k)
        fill_tri_textured_perspective_wrap(
            g_tex_out[k].x0,g_tex_out[k].y0,g_tex_out[k].u0,g_tex_out[k].v0,g_tex_out[k].z0,
            g_tex_out[k].x1,g_tex_out[k].y1,g_tex_out[k].u1,g_tex_out[k].v1,g_tex_out[k].z1,
            g_tex_out[k].x2,g_tex_out[k].y2,g_tex_out[k].u2,g_tex_out[k].v2,g_tex_out[k].z2,
            g_tex_out[k].light,track_asphalt,TRACK_ASPHALT_W,TRACK_ASPHALT_H);

    qsort(g_mesh_out,(size_t)nflat,sizeof(g_mesh_out[0]),cmp_drawtri_far_first);
    for(k=0;k<nflat;++k)
        fill_tri2d(g_mesh_out[k].x0,g_mesh_out[k].y0,g_mesh_out[k].x1,g_mesh_out[k].y1,
                   g_mesh_out[k].x2,g_mesh_out[k].y2,g_mesh_out[k].color);
}


static void queue_world_box(
    float cx,float cy,float cz,float yaw,
    float w,float h,float d,
    uint16_t color,
    float camx,float camy,float camz,float camyaw,
    int *n)
{
    float hx=w*0.5f,hz=d*0.5f;
    v3f_t local[8]={
        {-hx,0,-hz},{hx,0,-hz},{hx,0,hz},{-hx,0,hz},
        {-hx,h,-hz},{hx,h,-hz},{hx,h,hz},{-hx,h,hz}
    };
    static const uint8_t faces[12][3]={
        {0,1,5},{0,5,4},{1,2,6},{1,6,5},
        {2,3,7},{2,7,6},{3,0,4},{3,4,7},
        {4,5,6},{4,6,7},{0,3,2},{0,2,1}
    };
    sv3_t p[8];
    int i;
    float cs=cosf(yaw),sn=sinf(yaw);

    for(i=0;i<8;++i){
        float wx=cx+local[i].x*cs+local[i].z*sn;
        float wz=cz-local[i].x*sn+local[i].z*cs;
        project_world_point(wx,cy+local[i].y,wz,camx,camy,camz,camyaw,&p[i]);
    }
    for(i=0;i<12;++i){
        int a=faces[i][0],b=faces[i][1],didx=faces[i][2];
        if(!p[a].valid||!p[b].valid||!p[didx].valid)continue;
        queue_flat_tri((int)p[a].sx,(int)p[a].sy,(int)p[b].sx,(int)p[b].sy,
                       (int)p[didx].sx,(int)p[didx].sy,
                       (p[a].z+p[b].z+p[didx].z)/3.0f,
                       shade1555(color,0.82f+0.12f*(float)(i&1)),n);
    }
}

static void world_offset_from_pose(
    const track_world_t *p,float lateral,float longitudinal,
    float *wx,float *wz)
{
    float rx=cosf(p->yaw),rz=-sinf(p->yaw);
    float fx=sinf(p->yaw),fz=cosf(p->yaw);
    *wx=p->x+rx*lateral+fx*longitudinal;
    *wz=p->z+rz*lateral+fz*longitudinal;
}

static void queue_house_lod(
    float x,float y,float z,float yaw,float scale,int variant,
    float camx,float camy,float camz,float camyaw,int *n)
{
    uint16_t wall,roof;
    float w=(760.0f+(variant&1)*120.0f)*scale;
    float h=(620.0f+((variant>>1)&1)*150.0f)*scale;
    float d=(720.0f+(variant&3)*55.0f)*scale;

    switch(variant&3){
        case 0: wall=pack1555(190,177,151);roof=pack1555(119,75,59);break;
        case 1: wall=pack1555(165,184,192);roof=pack1555(70,82,92);break;
        case 2: wall=pack1555(205,194,170);roof=pack1555(108,84,64);break;
        default:wall=pack1555(178,162,145);roof=pack1555(83,91,96);break;
    }

    queue_world_box(x,y,z,yaw,w,h,d,wall,camx,camy,camz,camyaw,n);
    queue_world_box(x,y+h,z,yaw,w*0.90f,h*0.22f,d*0.88f,
                    roof,camx,camy,camz,camyaw,n);
}

static void queue_tree_lod(
    float x,float y,float z,float scale,
    float camx,float camy,float camz,float camyaw,int *n)
{
    queue_world_box(x,y,z,0.0f,95.0f*scale,440.0f*scale,95.0f*scale,
                    pack1555(93,64,39),camx,camy,camz,camyaw,n);
    queue_world_box(x,y+330.0f*scale,z,0.0f,430.0f*scale,500.0f*scale,430.0f*scale,
                    pack1555(52,118,61),camx,camy,camz,camyaw,n);
}

static void queue_world_static_mesh(
    const v3f_t *verts,int vcount,const tri3d_t *tris,int tcount,
    const uint16_t *palette,int palette_count,
    float ox,float oy,float oz,float yaw,float scale,
    float camx,float camy,float camz,float camyaw,
    int *n)
{
    v3f_t *rv=g_mesh_rv;
    sv3_t *sv=g_mesh_sv;
    float cs=cosf(yaw),sn=sinf(yaw);
    int i;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_DRAW_TRIS||palette_count<1)return;
    if(*n>=MAX_DRAW_TRIS)return;

    for(i=0;i<vcount;++i){
        v3f_t p=verts[i],q;
        p.x*=scale;p.y*=scale;p.z*=scale;
        q.x=p.x*cs+p.z*sn;
        q.y=p.y;
        q.z=-p.x*sn+p.z*cs;
        rv[i]=q;
        project_world_point(ox+q.x,oy+q.y,oz+q.z,
                            camx,camy,camz,camyaw,&sv[i]);
    }

    for(i=0;i<tcount&&*n<MAX_VC_DRAW_TRIS;++i){
        const tri3d_t *t=&tris[i];
        int minx,maxx,miny,maxy;
        int area;
        v3f_t a,b,d;
        float ux,uy,uz,vx,vy,vz,nx,ny,nz,mag;
        float light=0.80f;
        uint16_t base;

        if(!sv[t->a].valid||!sv[t->b].valid||!sv[t->c].valid)continue;
        minx=(int)fminf(sv[t->a].sx,fminf(sv[t->b].sx,sv[t->c].sx));
        maxx=(int)fmaxf(sv[t->a].sx,fmaxf(sv[t->b].sx,sv[t->c].sx));
        miny=(int)fminf(sv[t->a].sy,fminf(sv[t->b].sy,sv[t->c].sy));
        maxy=(int)fmaxf(sv[t->a].sy,fmaxf(sv[t->b].sy,sv[t->c].sy));
        if(maxx<0||minx>=RW||maxy<0||miny>=RH)continue;

        area=((int)sv[t->b].sx-(int)sv[t->a].sx)*((int)sv[t->c].sy-(int)sv[t->a].sy)-
             ((int)sv[t->b].sy-(int)sv[t->a].sy)*((int)sv[t->c].sx-(int)sv[t->a].sx);
        if(area>-2&&area<2)continue;

        a=rv[t->a];b=rv[t->b];d=rv[t->c];
        ux=b.x-a.x;uy=b.y-a.y;uz=b.z-a.z;
        vx=d.x-a.x;vy=d.y-a.y;vz=d.z-a.z;
        nx=uy*vz-uz*vy;ny=uz*vx-ux*vz;nz=ux*vy-uy*vx;
        mag=sqrtf(nx*nx+ny*ny+nz*nz);
        if(mag>0.001f){
            nx/=mag;ny/=mag;nz/=mag;
            light=0.66f+0.34f*fabsf(nx*0.28f+ny*0.88f+nz*(-0.38f));
        }

        base=palette[(int)t->material%palette_count];
        queue_flat_tri((int)sv[t->a].sx,(int)sv[t->a].sy,
                       (int)sv[t->b].sx,(int)sv[t->b].sy,
                       (int)sv[t->c].sx,(int)sv[t->c].sy,
                       (sv[t->a].z+sv[t->b].z+sv[t->c].z)/3.0f,
                       shade1555(base,light),n);
    }
}

static void city_world_to_camera(
    float wx,float wy,float wz,
    float camx,float camy,float camz,float camyaw,
    v3f_t *o)
{
    float dx=wx-camx,dy=wy-camy,dz=wz-camz;
    float cs=cosf(camyaw),sn=sinf(camyaw);
    o->x=dx*cs-dz*sn;
    o->y=dy;
    o->z=dx*sn+dz*cs;
}

static int city_clip_near_triangle(
    const v3f_t in[3],v3f_t out[4])
{
    const float near_z=45.0f;
    v3f_t tmp[5];
    int outn=0;
    int i;

    for(i=0;i<3;++i){
        const v3f_t *a=&in[i];
        const v3f_t *b=&in[(i+1)%3];
        int ain=(a->z>=near_z);
        int bin=(b->z>=near_z);

        if(ain){
            tmp[outn++]=*a;
        }
        if(ain!=bin){
            float den=b->z-a->z;
            float t=(fabsf(den)>1.0e-8f)?((near_z-a->z)/den):0.0f;
            v3f_t q;
            if(t<0.0f)t=0.0f;if(t>1.0f)t=1.0f;
            q.x=a->x+(b->x-a->x)*t;
            q.y=a->y+(b->y-a->y)*t;
            q.z=near_z;
            tmp[outn++]=q;
        }
    }

    if(outn>4)outn=4;
    for(i=0;i<outn;++i)out[i]=tmp[i];
    return outn;
}

static void city_project_camera(const v3f_t *p,sv3_t *o)
{
    float s=TRACK_FOCAL/p->z;
    o->sx=RW*0.5f+p->x*s;
    o->sy=TRACK_SCREEN_Y-p->y*s;
    o->z=p->z;
    o->valid=1;
}

static void queue_world_static_mesh_z(
    const v3f_t *verts,int vcount,const tri3d_t *tris,int tcount,
    const uint16_t *palette,int palette_count,
    float ox,float oy,float oz,float yaw,float scale,
    float camx,float camy,float camz,float camyaw,
    int *n)
{
    v3f_t *rv=g_mesh_rv;
    v3f_t *cv=g_mesh_cam;
    float cs=cosf(yaw),sn=sinf(yaw);
    int i;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_DRAW_TRIS||palette_count<1)return;
    if(*n>=MAX_DRAW_TRIS)return;

    /*
     * Keep camera-space vertices for Stage7.7 near-plane clipping.
     * Stage7.6 projected first and discarded an entire triangle when ANY
     * vertex fell behind z=45, which created large holes in the 24m terrain
     * grid and made building walls pop in/out beside the chase camera.
     */
    for(i=0;i<vcount;++i){
        v3f_t p=verts[i],q;
        p.x*=scale;p.y*=scale;p.z*=scale;
        q.x=p.x*cs+p.z*sn;
        q.y=p.y;
        q.z=-p.x*sn+p.z*cs;
        rv[i]=q;
        city_world_to_camera(
            ox+q.x,oy+q.y,oz+q.z,
            camx,camy,camz,camyaw,&cv[i]);
    }

    for(i=0;i<tcount&&*n<MAX_VC_DRAW_TRIS;++i){
        const tri3d_t *t=&tris[i];
        v3f_t a,b,d;
        float ux,uy,uz,vx,vy,vz,nx,ny,nz,mag;
        float light=0.80f;
        uint16_t base;
        v3f_t in[3],poly[4];
        sv3_t sp[4];
        int pc,j;

        a=rv[t->a];b=rv[t->b];d=rv[t->c];
        ux=b.x-a.x;uy=b.y-a.y;uz=b.z-a.z;
        vx=d.x-a.x;vy=d.y-a.y;vz=d.z-a.z;
        nx=uy*vz-uz*vy;ny=uz*vx-ux*vz;nz=ux*vy-uy*vx;
        mag=sqrtf(nx*nx+ny*ny+nz*nz);
        if(mag>0.001f){
            nx/=mag;ny/=mag;nz/=mag;
            light=0.66f+0.34f*fabsf(nx*0.28f+ny*0.88f+nz*(-0.38f));
        }
        base=palette[(int)t->material%palette_count];

        in[0]=cv[t->a];in[1]=cv[t->b];in[2]=cv[t->c];
        pc=city_clip_near_triangle(in,poly);
        if(pc<3)continue;
        for(j=0;j<pc;++j)city_project_camera(&poly[j],&sp[j]);

        /* clipped polygon is convex; triangulate as a fan (3 -> 1 tri, 4 -> 2) */
        for(j=1;j+1<pc&&*n<MAX_VC_DRAW_TRIS;++j){
            citytri_t *o;
            int x0=(int)sp[0].sx,y0=(int)sp[0].sy;
            int x1=(int)sp[j].sx,y1=(int)sp[j].sy;
            int x2=(int)sp[j+1].sx,y2=(int)sp[j+1].sy;
            int minx=x0,maxx=x0,miny=y0,maxy=y0,area;

            if(x1<minx)minx=x1;if(x2<minx)minx=x2;
            if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
            if(y1<miny)miny=y1;if(y2<miny)miny=y2;
            if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
            if(maxx<0||minx>=RW||maxy<0||miny>=RH)continue;

            area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
            if(area>-2&&area<2)continue;

            o=&g_city_out[*n];
            o->x0=x0;o->y0=y0;o->z0=sp[0].z;
            o->x1=x1;o->y1=y1;o->z1=sp[j].z;
            o->x2=x2;o->y2=y2;o->z2=sp[j+1].z;
            o->color=shade1555(base,light);
            (*n)++;
        }
    }
}

typedef struct {
    v3f_t p;
    float u,v;
} vc_clip_v_t;

static int vc_clip_near_textured(const vc_clip_v_t in[3],vc_clip_v_t out[4])
{
    const float near_z=45.0f;
    vc_clip_v_t tmp[5];
    int outn=0,i;
    for(i=0;i<3;++i){
        const vc_clip_v_t *a=&in[i];
        const vc_clip_v_t *b=&in[(i+1)%3];
        int ain=(a->p.z>=near_z);
        int bin=(b->p.z>=near_z);
        if(ain)tmp[outn++]=*a;
        if(ain!=bin){
            float den=b->p.z-a->p.z;
            float t=(fabsf(den)>1.0e-8f)?((near_z-a->p.z)/den):0.0f;
            vc_clip_v_t q;
            if(t<0.0f)t=0.0f;if(t>1.0f)t=1.0f;
            q.p.x=a->p.x+(b->p.x-a->p.x)*t;
            q.p.y=a->p.y+(b->p.y-a->p.y)*t;
            q.p.z=near_z;
            q.u=a->u+(b->u-a->u)*t;
            q.v=a->v+(b->v-a->v)*t;
            tmp[outn++]=q;
        }
    }
    if(outn>4)outn=4;
    for(i=0;i<outn;++i)out[i]=tmp[i];
    return outn;
}

static void queue_vc_mesh_textured(
    const vc_vertex_t *verts,int vcount,const vc_tri_t *tris,int tcount,
    float scale,float camx,float camy,float camz,float camyaw,int *n)
{
    v3f_t *rv=g_mesh_rv;
    v3f_t *cv=g_mesh_cam;
    int i;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_VC_DRAW_TRIS)return;
    if(*n>=MAX_VC_DRAW_TRIS)return;

    for(i=0;i<vcount;++i){
        v3f_t q;
        q.x=verts[i].x*scale;
        q.y=verts[i].y*scale;
        q.z=verts[i].z*scale;
        rv[i]=q;
        g_vc_mesh_uv[i].u=verts[i].u;
        g_vc_mesh_uv[i].v=verts[i].v;
        city_world_to_camera(q.x,q.y,q.z,camx,camy,camz,camyaw,&cv[i]);
    }

    for(i=0;i<tcount&&*n<MAX_VC_DRAW_TRIS;++i){
        const vc_tri_t *t=&tris[i];
        v3f_t a,b,d;
        float ux,uy,uz,vx,vy,vz,nx,ny,nz,mag,light=0.80f;
        vc_clip_v_t in[3],poly[4];
        sv3_t sp[4];
        int pc,j;

        if(t->a>=vcount||t->b>=vcount||t->c>=vcount||
           t->material>=g_vc_map.material_count)continue;

        a=rv[t->a];b=rv[t->b];d=rv[t->c];
        ux=b.x-a.x;uy=b.y-a.y;uz=b.z-a.z;
        vx=d.x-a.x;vy=d.y-a.y;vz=d.z-a.z;
        nx=uy*vz-uz*vy;ny=uz*vx-ux*vz;nz=ux*vy-uy*vx;
        mag=sqrtf(nx*nx+ny*ny+nz*nz);
        if(mag>0.001f){
            nx/=mag;ny/=mag;nz/=mag;
            light=0.70f+0.30f*fabsf(nx*0.28f+ny*0.88f+nz*(-0.38f));
        }

        in[0].p=cv[t->a];in[0].u=g_vc_mesh_uv[t->a].u;in[0].v=g_vc_mesh_uv[t->a].v;
        in[1].p=cv[t->b];in[1].u=g_vc_mesh_uv[t->b].u;in[1].v=g_vc_mesh_uv[t->b].v;
        in[2].p=cv[t->c];in[2].u=g_vc_mesh_uv[t->c].u;in[2].v=g_vc_mesh_uv[t->c].v;
        pc=vc_clip_near_textured(in,poly);
        if(pc<3)continue;
        for(j=0;j<pc;++j)city_project_camera(&poly[j].p,&sp[j]);

        for(j=1;j+1<pc&&*n<MAX_VC_DRAW_TRIS;++j){
            vc_textri_t *o;
            int x0=(int)sp[0].sx,y0=(int)sp[0].sy;
            int x1=(int)sp[j].sx,y1=(int)sp[j].sy;
            int x2=(int)sp[j+1].sx,y2=(int)sp[j+1].sy;
            int minx=x0,maxx=x0,miny=y0,maxy=y0,area;
            if(x1<minx)minx=x1;if(x2<minx)minx=x2;
            if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
            if(y1<miny)miny=y1;if(y2<miny)miny=y2;
            if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
            if(maxx<0||minx>=RW||maxy<0||miny>=RH)continue;
            area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
            if(area>-2&&area<2)continue;

            o=&g_vc_tex_out[*n];
            o->x0=x0;o->y0=y0;o->z0=sp[0].z;o->u0=poly[0].u;o->v0=poly[0].v;
            o->x1=x1;o->y1=y1;o->z1=sp[j].z;o->u1=poly[j].u;o->v1=poly[j].v;
            o->x2=x2;o->y2=y2;o->z2=sp[j+1].z;o->u2=poly[j+1].u;o->v2=poly[j+1].v;
            o->light=light;
            o->material=t->material;
            (*n)++;
        }
    }
}


static void free_vc_map(void)
{
    free(g_vc_map.verts);
    free(g_vc_map.tris);
    free(g_vc_map.sectors);
    free(g_vc_map.materials);
    free(g_vc_map.atlas);
    memset(&g_vc_map,0,sizeof(g_vc_map));
    g_vc_city_mode=0;
}

static int vc_read_exact(FILE *fp,void *dst,size_t bytes)
{
    return bytes==0 || fread(dst,1,bytes,fp)==bytes;
}

static int load_vc_map_file(const char *path)
{
    FILE *fp;
    vcmap_header_t h;
    uint32_t i;
    size_t atlas_pixels;

    if(!path||!*path)return 0;
    fp=fopen(path,"rb");
    if(!fp)return 0;

    memset(&h,0,sizeof(h));
    if(sizeof(h)!=72 || !vc_read_exact(fp,&h,sizeof(h))){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP2 reject %s: short/ABI header\n",path);
        return -1;
    }
    if(memcmp(h.magic,"VCM2",4)!=0 || h.version!=2){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP2 reject %s: expected VCM2/version2\n",path);
        return -1;
    }
    atlas_pixels=(size_t)h.atlas_w*(size_t)h.atlas_h;
    if(!(h.world_scale>1.0f && h.world_scale<10000.0f) ||
       !(h.sector_m>1.0f && h.sector_m<10000.0f) ||
       h.vertex_count==0 || h.tri_count==0 || h.sector_count==0 ||
       h.vertex_count>1200000U || h.tri_count>2400000U ||
       h.sector_count>65535U || h.material_count==0 || h.material_count>255U ||
       h.atlas_w==0 || h.atlas_h==0 || h.atlas_w>2048U || h.atlas_h>2048U ||
       atlas_pixels>4194304U){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP2 reject %s: unsafe counts/range\n",path);
        return -1;
    }
    if(sizeof(vc_vertex_t)!=20 || sizeof(vc_tri_t)!=8 ||
       sizeof(vc_material_t)!=12 || sizeof(vc_sector_t)!=20){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP2 reject: unexpected local struct ABI v=%u t=%u m=%u s=%u\n",
                (unsigned)sizeof(vc_vertex_t),(unsigned)sizeof(vc_tri_t),
                (unsigned)sizeof(vc_material_t),(unsigned)sizeof(vc_sector_t));
        return -1;
    }

    free_vc_map();
    g_vc_map.verts=(vc_vertex_t*)calloc((size_t)h.vertex_count,sizeof(vc_vertex_t));
    g_vc_map.tris=(vc_tri_t*)calloc((size_t)h.tri_count,sizeof(vc_tri_t));
    g_vc_map.sectors=(vc_sector_t*)calloc((size_t)h.sector_count,sizeof(vc_sector_t));
    g_vc_map.materials=(vc_material_t*)calloc((size_t)h.material_count,sizeof(vc_material_t));
    g_vc_map.atlas=(uint16_t*)calloc(atlas_pixels,sizeof(uint16_t));
    if(!g_vc_map.verts||!g_vc_map.tris||!g_vc_map.sectors||
       !g_vc_map.materials||!g_vc_map.atlas){
        fclose(fp);free_vc_map();
        fprintf(stderr,"[racer] VCMAP2 reject %s: allocation failed\n",path);
        return -1;
    }

    if(!vc_read_exact(fp,g_vc_map.materials,(size_t)h.material_count*sizeof(vc_material_t)) ||
       !vc_read_exact(fp,g_vc_map.verts,(size_t)h.vertex_count*sizeof(vc_vertex_t)) ||
       !vc_read_exact(fp,g_vc_map.tris,(size_t)h.tri_count*sizeof(vc_tri_t)) ||
       !vc_read_exact(fp,g_vc_map.sectors,(size_t)h.sector_count*sizeof(vc_sector_t)) ||
       !vc_read_exact(fp,g_vc_map.atlas,atlas_pixels*sizeof(uint16_t))){
        fclose(fp);free_vc_map();
        fprintf(stderr,"[racer] VCMAP2 reject %s: truncated payload\n",path);
        return -1;
    }
    fclose(fp);

    for(i=0;i<h.material_count;++i){
        const vc_material_t *m=&g_vc_map.materials[i];
        if((m->flags&1U) &&
           ((unsigned)m->x+(unsigned)m->w>h.atlas_w ||
            (unsigned)m->y+(unsigned)m->h>h.atlas_h ||
            m->w==0 || m->h==0)){
            fprintf(stderr,"[racer] VCMAP2 reject %s: invalid material %u atlas rect\n",
                    path,(unsigned)i);
            free_vc_map();return -1;
        }
    }

    for(i=0;i<h.sector_count;++i){
        const vc_sector_t *s=&g_vc_map.sectors[i];
        uint32_t j;
        if(s->vertex_base>h.vertex_count || s->vertex_count>h.vertex_count-s->vertex_base ||
           s->tri_base>h.tri_count || s->tri_count>h.tri_count-s->tri_base ||
           s->vertex_count>65535U || s->vertex_count>MAX_MESH_VERTS){
            fprintf(stderr,"[racer] VCMAP2 reject %s: invalid sector %u ranges\n",path,(unsigned)i);
            free_vc_map();return -1;
        }
        for(j=0;j<s->tri_count;++j){
            const vc_tri_t *t=&g_vc_map.tris[s->tri_base+j];
            if(t->a>=s->vertex_count||t->b>=s->vertex_count||t->c>=s->vertex_count||
               t->material>=h.material_count){
                fprintf(stderr,"[racer] VCMAP2 reject %s: invalid triangle in sector %u\n",
                        path,(unsigned)i);
                free_vc_map();return -1;
            }
        }
    }

    g_vc_map.world_scale=h.world_scale;
    g_vc_map.sector_m=h.sector_m;
    g_vc_map.sector_world=h.sector_m*h.world_scale;
    g_vc_map.spawn_x=h.spawn_x*h.world_scale;
    g_vc_map.spawn_y=h.spawn_y*h.world_scale;
    g_vc_map.spawn_z=h.spawn_z*h.world_scale;
    g_vc_map.spawn_yaw=h.spawn_yaw;
    g_vc_map.min_x=h.min_x*h.world_scale;
    g_vc_map.max_x=h.max_x*h.world_scale;
    g_vc_map.min_z=h.min_z*h.world_scale;
    g_vc_map.max_z=h.max_z*h.world_scale;
    g_vc_map.vertex_count=h.vertex_count;
    g_vc_map.tri_count=h.tri_count;
    g_vc_map.sector_count=h.sector_count;
    g_vc_map.material_count=h.material_count;
    g_vc_map.atlas_w=h.atlas_w;
    g_vc_map.atlas_h=h.atlas_h;

    g_vc_city_mode=1;
    g_osm_city_mode=0;
    g_world_x=g_vc_map.spawn_x;
    g_world_y=g_vc_map.spawn_y;
    g_world_z=g_vc_map.spawn_z;
    g_vc_ground_y=g_world_y;
    g_vehicle_heading=g_vc_map.spawn_yaw;
    g_speed=0.0f;
    g_position=0.0f;
    g_player_x=0.0f;
    g_camera_initialized=0;

    fprintf(stderr,
        "[racer] VCMAP2 loaded path=%s vertices=%u triangles=%u sectors=%u materials=%u atlas=%ux%u scale=%.1f spawn=%.0f,%.0f,%.0f\n",
        path,(unsigned)h.vertex_count,(unsigned)h.tri_count,(unsigned)h.sector_count,
        (unsigned)h.material_count,(unsigned)h.atlas_w,(unsigned)h.atlas_h,
        h.world_scale,g_world_x,g_world_y,g_world_z);
    return 1;
}

static int try_load_vc_map(void)
{
    const char *env=getenv("RACER_VCMAP");
    int r;
    if(env&&*env){
        r=load_vc_map_file(env);
        if(r!=0)return r>0;
    }
    r=load_vc_map_file("/mnt/usb/H3531/APPS/racer/VCMAP.BIN");
    if(r!=0)return r>0;
    r=load_vc_map_file("VCMAP.BIN");
    if(r!=0)return r>0;
    return 0;
}

static int vc_point_in_tri_xz(
    float px,float pz,
    const vc_vertex_t *a,const vc_vertex_t *b,const vc_vertex_t *c,
    float *wa,float *wb,float *wc)
{
    float den=(b->z-c->z)*(a->x-c->x)+(c->x-b->x)*(a->z-c->z);
    float u,v,w;
    if(fabsf(den)<1.0e-6f)return 0;
    u=((b->z-c->z)*(px-c->x)+(c->x-b->x)*(pz-c->z))/den;
    v=((c->z-a->z)*(px-c->x)+(a->x-c->x)*(pz-c->z))/den;
    w=1.0f-u-v;
    if(u<-0.001f||v<-0.001f||w<-0.001f)return 0;
    if(wa)*wa=u;if(wb)*wb=v;if(wc)*wc=w;
    return 1;
}

static int vc_city_ground_height(float world_x,float world_z,float current_y,float *out_y)
{
    int psx,psz;
    uint32_t i,j;
    float ux,uz;
    float best=0.0f,best_delta=1.0e30f;
    int found=0;

    if(!g_vc_city_mode||g_vc_map.sector_world<=1.0f)return 0;
    ux=world_x/g_vc_map.world_scale;
    uz=world_z/g_vc_map.world_scale;
    psx=(int)floorf(world_x/g_vc_map.sector_world);
    psz=(int)floorf(world_z/g_vc_map.sector_world);

    for(i=0;i<g_vc_map.sector_count;++i){
        const vc_sector_t *s=&g_vc_map.sectors[i];
        if(abs((int)s->sx-psx)>1||abs((int)s->sz-psz)>1)continue;
        for(j=0;j<s->tri_count;++j){
            const vc_tri_t *t=&g_vc_map.tris[s->tri_base+j];
            const vc_vertex_t *a,*b,*c;
            float wa,wb,wc,y,delta;
            if(!(t->flags&1U))continue;
            a=&g_vc_map.verts[s->vertex_base+t->a];
            b=&g_vc_map.verts[s->vertex_base+t->b];
            c=&g_vc_map.verts[s->vertex_base+t->c];
            if(!vc_point_in_tri_xz(ux,uz,a,b,c,&wa,&wb,&wc))continue;
            y=(wa*a->y+wb*b->y+wc*c->y)*g_vc_map.world_scale;
            delta=fabsf(y-current_y);
            if(delta<best_delta && delta<1800.0f){
                best_delta=delta;best=y;found=1;
            }
        }
    }
    if(found&&out_y)*out_y=best;
    return found;
}


static float osm_city_height_at_world(float x,float z)
{
    float fx=(x-OSM_CITY_HEIGHT_MIN_X)/OSM_CITY_HEIGHT_STEP_X;
    float fz=(z-OSM_CITY_HEIGHT_MIN_Z)/OSM_CITY_HEIGHT_STEP_Z;
    int ix=(int)floorf(fx);
    int iz=(int)floorf(fz);
    float tx,tz;
    float a,b,c,d;

    if(ix<0){ix=0;fx=0.0f;}
    if(iz<0){iz=0;fz=0.0f;}
    if(ix>OSM_CITY_HEIGHT_NX-2){ix=OSM_CITY_HEIGHT_NX-2;fx=(float)(OSM_CITY_HEIGHT_NX-1);}
    if(iz>OSM_CITY_HEIGHT_NZ-2){iz=OSM_CITY_HEIGHT_NZ-2;fz=(float)(OSM_CITY_HEIGHT_NZ-1);}
    tx=fx-(float)ix;
    tz=fz-(float)iz;
    if(tx<0.0f)tx=0.0f;if(tx>1.0f)tx=1.0f;
    if(tz<0.0f)tz=0.0f;if(tz>1.0f)tz=1.0f;

    a=osm_city_height[iz*OSM_CITY_HEIGHT_NX+ix];
    b=osm_city_height[iz*OSM_CITY_HEIGHT_NX+ix+1];
    c=osm_city_height[(iz+1)*OSM_CITY_HEIGHT_NX+ix];
    d=osm_city_height[(iz+1)*OSM_CITY_HEIGHT_NX+ix+1];
    return (a+(b-a)*tx)+((c+(d-c)*tx)-(a+(b-a)*tx))*tz;
}


static float point_seg_dist2(float px,float pz,float ax,float az,float bx,float bz)
{
    float dx=bx-ax,dz=bz-az;
    float den=dx*dx+dz*dz;
    float t,qx,qz;
    if(den<=1.0e-8f){
        dx=px-ax;dz=pz-az;
        return dx*dx+dz*dz;
    }
    t=((px-ax)*dx+(pz-az)*dz)/den;
    if(t<0.0f)t=0.0f;if(t>1.0f)t=1.0f;
    qx=ax+t*dx;qz=az+t*dz;
    dx=px-qx;dz=pz-qz;
    return dx*dx+dz*dz;
}

static int osm_city_hits_building(float x,float z,float margin)
{
    int i;
    float m2=margin*margin;
    for(i=0;i<OSM_CITY_BUILDING_COUNT;++i){
        const osm_city_building_t *b=&osm_city_building[i];
        int j,inside=0;
        if(x<=b->minx-margin||x>=b->maxx+margin||
           z<=b->minz-margin||z>=b->maxz+margin)continue;

        for(j=0;j<(int)b->point_count;++j){
            int nj=(j+1)%(int)b->point_count;
            const osm_city_point_t *a=&osm_city_building_point[b->point_base+j];
            const osm_city_point_t *d=&osm_city_building_point[b->point_base+nj];

            if(((a->z>z)!=(d->z>z))){
                float den=d->z-a->z;
                float cross;
                if(fabsf(den)<1.0e-8f)den=(den<0.0f)?-1.0e-8f:1.0e-8f;
                cross=(d->x-a->x)*(z-a->z)/den+a->x;
                if(x<cross)inside=!inside;
            }
            if(point_seg_dist2(x,z,a->x,a->z,d->x,d->z)<=m2)return 1;
        }
        if(inside)return 1;
    }
    return 0;
}

static void draw_osm_city_world(void)
{
    float camx,camy,camz,camyaw;
    track_world_t car;
    int psx,psz;
    int i,n=0,k;
    const float sw=OSM_CITY_SECTOR_WORLD;

    get_chase_camera(&camx,&camy,&camz,&camyaw);
    get_player_world(&car,NULL);
    psx=(int)floorf(car.x/sw);
    psz=(int)floorf(car.z/sw);

    /*
     * Sector residency replaces runtime GLB parsing. A 7x7 maximum window
     * corresponds to ~336m at the current 48m sector size, but sectors well
     * behind the camera are rejected before any vertex transform.
     */
    for(i=0;i<OSM_CITY_SECTOR_COUNT && n<MAX_DRAW_TRIS;++i){
        const osm_city_sector_t *s=&osm_city_sector[i];
        int dx=(int)s->sx-psx;
        int dz=(int)s->sz-psz;
        float cx=((float)s->sx+0.5f)*sw;
        float cz=((float)s->sz+0.5f)*sw;
        float rx=cx-car.x;
        float rz=cz-car.z;
        float d2=rx*rx+rz*rz;

        /*
         * Stage7.7 residency is camera-direction independent. Stage7.6 could
         * suddenly instantiate a whole sector while the player rotated because
         * sectors behind the current camera heading were dropped wholesale.
         * Keep a stable neighborhood around the vehicle; near-plane clipping
         * and screen rejection decide what is actually visible.
         */
        if(dx<-3||dx>3||dz<-3||dz>3)continue;
        if(d2>sw*sw*19.0f)continue;

        queue_world_static_mesh_z(
            &osm_city_v[s->vertex_base],(int)s->vertex_count,
            &osm_city_t[s->tri_base],(int)s->tri_count,
            osm_city_mat,OSM_CITY_MATERIAL_COUNT,
            0.0f,0.0f,0.0f,0.0f,1.0f,
            camx,camy,camz,camyaw,&n);
    }

    memset(g_city_zbuf,0,sizeof(g_city_zbuf));
    for(k=0;k<n;++k)
        fill_tri2d_z(
            g_city_out[k].x0,g_city_out[k].y0,g_city_out[k].z0,
            g_city_out[k].x1,g_city_out[k].y1,g_city_out[k].z1,
            g_city_out[k].x2,g_city_out[k].y2,g_city_out[k].z2,
            g_city_out[k].color);
}


static void draw_vc_city_world(void)
{
    float camx,camy,camz,camyaw;
    track_world_t car;
    int psx,psz;
    uint32_t i;
    int n=0,k,visible_sectors=0;
    float sw=g_vc_map.sector_world;
    int cap_hit=0;

    if(!g_vc_city_mode||sw<=1.0f)return;
    get_chase_camera(&camx,&camy,&camz,&camyaw);
    get_player_world(&car,NULL);
    psx=(int)floorf(car.x/sw);
    psz=(int)floorf(car.z/sw);

    for(i=0;i<g_vc_map.sector_count && n<MAX_VC_DRAW_TRIS;++i){
        const vc_sector_t *s=&g_vc_map.sectors[i];
        int dx=(int)s->sx-psx;
        int dz=(int)s->sz-psz;
        float cx=((float)s->sx+0.5f)*sw;
        float cz=((float)s->sz+0.5f)*sw;
        float rx=cx-car.x,rz=cz-car.z;
        float d2=rx*rx+rz*rz;
        v3f_t sc;
        float sector_radius=sw*0.80f;
        float frustum_slope=((float)RW*0.5f)/TRACK_FOCAL;

        if(dx<-3||dx>3||dz<-3||dz>3)continue;
        if(d2>sw*sw*19.0f)continue;

        /*
         * Stage7.9 queued nearby sectors even when they were fully behind the
         * camera. That wasted the 8192-triangle budget, so whole buildings
         * disappeared as the camera turned. Cull only sectors safely outside a
         * generous horizontal frustum; the radius margin keeps edge-spanning
         * geometry alive.
         */
        city_world_to_camera(cx,car.y,cz,camx,camy,camz,camyaw,&sc);
        if(sc.z < -sector_radius)continue;
        if(sc.z > 1.0f &&
           fabsf(sc.x) > sc.z*(frustum_slope+0.35f)+sector_radius)
            continue;

        visible_sectors++;
        queue_vc_mesh_textured(
            &g_vc_map.verts[s->vertex_base],(int)s->vertex_count,
            &g_vc_map.tris[s->tri_base],(int)s->tri_count,
            g_vc_map.world_scale,
            camx,camy,camz,camyaw,&n);
    }

    if(n>=MAX_VC_DRAW_TRIS)cap_hit=1;
    g_vc_last_queued=n;
    g_vc_last_visible_sectors=visible_sectors;
    g_vc_last_cap_hit=cap_hit;

    memset(g_city_zbuf,0,sizeof(g_city_zbuf));
    if(g_vc_debug_flat){
        for(k=0;k<n;++k){
            const vc_textri_t *t=&g_vc_tex_out[k];
            const vc_material_t *m=&g_vc_map.materials[t->material];
            fill_tri2d_z(
                t->x0,t->y0,t->z0,
                t->x1,t->y1,t->z1,
                t->x2,t->y2,t->z2,
                g_fog_lut[vc_fog_level_for_z((t->z0+t->z1+t->z2)*(1.0f/3.0f))]
                         [shade1555(m->fallback,t->light)&0x7fffU]);
        }
    }else{
        for(k=0;k<n;++k)
            fill_tri_vc_textured_z(&g_vc_tex_out[k]);
    }
}

static void draw_world_billboard(
    float pos,float side,float w,float h,
    float camx,float camy,float camz,float camyaw)
{
    track_world_t p;
    float rx,rz;
    sv3_t a,b,d,e;
    track_pose_at(pos,side*ROAD_WIDTH*1.9f,&p);
    rx=cosf(p.yaw);rz=-sinf(p.yaw);

    if(!project_world_point(p.x-rx*w*0.5f,p.y+80,p.z-rz*w*0.5f,camx,camy,camz,camyaw,&a))return;
    if(!project_world_point(p.x+rx*w*0.5f,p.y+80,p.z+rz*w*0.5f,camx,camy,camz,camyaw,&b))return;
    if(!project_world_point(p.x+rx*w*0.5f,p.y+80+h,p.z+rz*w*0.5f,camx,camy,camz,camyaw,&d))return;
    if(!project_world_point(p.x-rx*w*0.5f,p.y+80+h,p.z-rz*w*0.5f,camx,camy,camz,camyaw,&e))return;

    fill_tri_textured((int)a.sx,(int)a.sy,0,RACER_BILLBOARD_H-1,
                      (int)b.sx,(int)b.sy,RACER_BILLBOARD_W-1,RACER_BILLBOARD_H-1,
                      (int)d.sx,(int)d.sy,RACER_BILLBOARD_W-1,0,
                      0.95f,racer_billboard,RACER_BILLBOARD_W,RACER_BILLBOARD_H);
    fill_tri_textured((int)a.sx,(int)a.sy,0,RACER_BILLBOARD_H-1,
                      (int)d.sx,(int)d.sy,RACER_BILLBOARD_W-1,0,
                      (int)e.sx,(int)e.sy,0,0,
                      0.95f,racer_billboard,RACER_BILLBOARD_W,RACER_BILLBOARD_H);
}

static void draw_true3d_props(void)
{
    float camx,camy,camz,camyaw;
    int base=(int)floorf(g_position/SEG_LEN);
    int k,n=0;
    int range_back=26,range_front=156;
    track_world_t car;
    float road_yaw;
    float rel;

    get_chase_camera(&camx,&camy,&camz,&camyaw);
    get_player_world(&car,&road_yaw);
    rel=wrap_angle(g_vehicle_heading-road_yaw);
    if(cosf(rel)<-0.35f){range_back=156;range_front=26;}
    else if(fabsf(cosf(rel))<=0.35f){range_back=72;range_front=72;}

    for(k=-range_back;k<range_front;++k){
        int raw=base+k;
        int idx=raw%TRACK_SEGMENTS;
        track_world_t p;
        unsigned flags;
        float side;
        int view_k;
        if(idx<0)idx+=TRACK_SEGMENTS;
        flags=g_track[idx].flags;
        raw_track_pose(raw,&p);

        /*
         * PS1-style asymmetric visibility window:
         * positive view_k is in front of the camera/vehicle heading, negative
         * is already passed.  We intentionally keep much more world ahead than
         * behind, mirroring old sector/paging renderers that prefetched upcoming
         * content and discarded traversed sectors quickly.
         */
        {
            float facing=cosf(rel);
            view_k=(facing<-0.35f)?-k:k;
            if(fabsf(facing)<=0.35f)view_k=(k<0)?-k:k;
        }

        /* guardrail sections: true thin boxes on fast/urban sections, sparse LOD */
        if((raw&15)==0 && (fabsf(g_track[idx].curve)>0.20f || (idx>320&&idx<570))){
            side=-1.0f;
            {
                track_world_t q=p;
                q.x+=cosf(p.yaw)*side*ROAD_WIDTH*1.34f;
                q.z-=sinf(p.yaw)*side*ROAD_WIDTH*1.34f;
                queue_world_box(q.x,q.y+35,q.z,p.yaw,35,180,SEG_LEN*5.5f,
                                pack1555(150,154,158),camx,camy,camz,camyaw,&n);
            }
            side=1.0f;
            {
                track_world_t q=p;
                q.x+=cosf(p.yaw)*side*ROAD_WIDTH*1.34f;
                q.z-=sinf(p.yaw)*side*ROAD_WIDTH*1.34f;
                queue_world_box(q.x,q.y+35,q.z,p.yaw,35,180,SEG_LEN*5.5f,
                                pack1555(150,154,158),camx,camy,camz,camyaw,&n);
            }
        }

        if((flags&TF_TREES) && view_k>-14 && view_k<138){
            float s;
            for(s=-1.0f;s<=1.0f;s+=2.0f){
                float qx,qz;
                float lateral=ROAD_WIDTH*(2.08f+0.18f*(float)(idx&3));
                float yaw=0.37f*(float)(idx%11)+s*0.21f;
                float scale=0.86f+0.06f*(float)(idx&3);
                world_offset_from_pose(&p,s*lateral,0.0f,&qx,&qz);

                if(view_k>-10 && view_k<58){
                    if((idx&2)==0)
                        queue_world_static_mesh(
                            env_tree_default_v,ENV_TREE_DEFAULT_VERTEX_COUNT,
                            env_tree_default_t,ENV_TREE_DEFAULT_TRIANGLE_COUNT,
                            env_tree_default_mat,ENV_TREE_DEFAULT_MATERIAL_COUNT,
                            qx,p.y,qz,yaw,scale,
                            camx,camy,camz,camyaw,&n);
                    else
                        queue_world_static_mesh(
                            env_tree_pine_v,ENV_TREE_PINE_VERTEX_COUNT,
                            env_tree_pine_t,ENV_TREE_PINE_TRIANGLE_COUNT,
                            env_tree_pine_mat,ENV_TREE_PINE_MATERIAL_COUNT,
                            qx,p.y,qz,yaw,scale,
                            camx,camy,camz,camyaw,&n);
                }else if((idx&1)==0){
                    queue_tree_lod(qx,p.y,qz,scale,
                                   camx,camy,camz,camyaw,&n);
                }
            }
        }

        if((flags&TF_CITY) && view_k>-18 && view_k<148){
            int j;
            static const float lateral_mul[6]={-2.45f,2.55f,-3.05f,3.12f,-2.68f,2.82f};
            static const float longitudinal[6]={-250.0f,-120.0f,170.0f,310.0f,520.0f,610.0f};

            /*
             * Three PS1-style LOD bands, asymmetric around the player:
             *
             *   FAR  : -18 .. +148 segments -- cheap 24-triangle silhouettes
             *   MID  : -14 ..  +58 segments -- authored lighter Kenney house
             *   NEAR :  -8 ..  +30 segments -- current full Suburban house
             *
             * The important detail is that transitions happen much earlier in
             * front of the player than behind.  A house becomes "real" while it
             * is still comfortably ahead, remains detailed as we pass it, and
             * only drops back after it is behind the camera.  No disk loading is
             * involved: all three levels are static/prefaulted in RAM.
             */
            for(j=0;j<6;++j){
                float qx,qz;
                float side=lateral_mul[j]*ROAD_WIDTH;
                float sc=0.72f+0.05f*(float)((idx+j)&3);
                float byaw=p.yaw+((side<0.0f)?3.1415926f:0.0f);
                world_offset_from_pose(&p,side,longitudinal[j],&qx,&qz);

                if(j==0){
                    /*
                     * All three LODs are now generated from the SAME
                     * building-type-a.glb.  This removes the Stage7.2 visual
                     * "house A turns into house B" discontinuity.
                     */
                    if(view_k>-12 && view_k<48){
                        queue_world_static_mesh(
                            env_house_suburban_v,ENV_HOUSE_SUBURBAN_VERTEX_COUNT,
                            env_house_suburban_t,ENV_HOUSE_SUBURBAN_TRIANGLE_COUNT,
                            env_house_suburban_mat,ENV_HOUSE_SUBURBAN_MATERIAL_COUNT,
                            qx,p.y,qz,byaw,0.82f,
                            camx,camy,camz,camyaw,&n);
                    }else if(view_k>-18 && view_k<88){
                        queue_world_static_mesh(
                            env_house_mid_v,ENV_HOUSE_MID_VERTEX_COUNT,
                            env_house_mid_t,ENV_HOUSE_MID_TRIANGLE_COUNT,
                            env_house_mid_mat,ENV_HOUSE_MID_MATERIAL_COUNT,
                            qx,p.y,qz,byaw,0.82f,
                            camx,camy,camz,camyaw,&n);
                    }else{
                        queue_world_static_mesh(
                            env_house_far_v,ENV_HOUSE_FAR_VERTEX_COUNT,
                            env_house_far_t,ENV_HOUSE_FAR_TRIANGLE_COUNT,
                            env_house_far_mat,ENV_HOUSE_FAR_MATERIAL_COUNT,
                            qx,p.y,qz,byaw,0.82f,
                            camx,camy,camz,camyaw,&n);
                    }
                }else if(j==1 && (idx&2)){
                    if(view_k>-12 && view_k<44){
                        queue_world_static_mesh(
                            env_building_commercial_v,ENV_BUILDING_COMMERCIAL_VERTEX_COUNT,
                            env_building_commercial_t,ENV_BUILDING_COMMERCIAL_TRIANGLE_COUNT,
                            env_building_commercial_mat,ENV_BUILDING_COMMERCIAL_MATERIAL_COUNT,
                            qx,p.y,qz,byaw,0.84f,
                            camx,camy,camz,camyaw,&n);
                    }else if(view_k>-18 && view_k<82){
                        queue_world_static_mesh(
                            env_building_commercial_mid_v,ENV_BUILDING_COMMERCIAL_MID_VERTEX_COUNT,
                            env_building_commercial_mid_t,ENV_BUILDING_COMMERCIAL_MID_TRIANGLE_COUNT,
                            env_building_commercial_mid_mat,ENV_BUILDING_COMMERCIAL_MID_MATERIAL_COUNT,
                            qx,p.y,qz,byaw,0.84f,
                            camx,camy,camz,camyaw,&n);
                    }else{
                        queue_world_static_mesh(
                            env_building_commercial_far_v,ENV_BUILDING_COMMERCIAL_FAR_VERTEX_COUNT,
                            env_building_commercial_far_t,ENV_BUILDING_COMMERCIAL_FAR_TRIANGLE_COUNT,
                            env_building_commercial_far_mat,ENV_BUILDING_COMMERCIAL_FAR_MATERIAL_COUNT,
                            qx,p.y,qz,byaw,0.84f,
                            camx,camy,camz,camyaw,&n);
                    }
                }else{
                    queue_house_lod(qx,p.y,qz,byaw,sc,idx+j,
                                    camx,camy,camz,camyaw,&n);
                }
            }

            /* Street furniture also pre-enters the frame and unloads behind. */
            if(view_k>-10 && view_k<72 && (idx&1)==0){
                float lx,lz;
                world_offset_from_pose(&p,-ROAD_WIDTH*1.55f,40.0f,&lx,&lz);
                queue_world_static_mesh(
                    env_street_light_v,ENV_STREET_LIGHT_VERTEX_COUNT,
                    env_street_light_t,ENV_STREET_LIGHT_TRIANGLE_COUNT,
                    env_street_light_mat,ENV_STREET_LIGHT_MATERIAL_COUNT,
                    lx,p.y,lz,p.yaw,0.90f,
                    camx,camy,camz,camyaw,&n);
            }
            if(view_k>-8 && view_k<58 && (idx%3)==0){
                float sx,sz;
                world_offset_from_pose(&p,ROAD_WIDTH*1.48f,-120.0f,&sx,&sz);
                queue_world_static_mesh(
                    env_warning_sign_v,ENV_WARNING_SIGN_VERTEX_COUNT,
                    env_warning_sign_t,ENV_WARNING_SIGN_TRIANGLE_COUNT,
                    env_warning_sign_mat,ENV_WARNING_SIGN_MATERIAL_COUNT,
                    sx,p.y,sz,p.yaw,0.82f,
                    camx,camy,camz,camyaw,&n);
            }
        }

        if((flags&(TF_GRANDSTAND_L|TF_GRANDSTAND_R)) && k>-20 && k<55){
            if(flags&TF_GRANDSTAND_L){
                track_world_t q=p;
                q.x-=cosf(p.yaw)*ROAD_WIDTH*2.1f;q.z+=sinf(p.yaw)*ROAD_WIDTH*2.1f;
                queue_world_box(q.x,q.y,q.z,p.yaw,1800,900,1300,pack1555(125,128,135),
                                camx,camy,camz,camyaw,&n);
            }
            if(flags&TF_GRANDSTAND_R){
                track_world_t q=p;
                q.x+=cosf(p.yaw)*ROAD_WIDTH*2.1f;q.z-=sinf(p.yaw)*ROAD_WIDTH*2.1f;
                queue_world_box(q.x,q.y,q.z,p.yaw,1800,900,1300,pack1555(125,128,135),
                                camx,camy,camz,camyaw,&n);
            }
        }

        if((flags&TF_BILLBOARD_L) && view_k>-10 && view_k<84)draw_world_billboard(raw*SEG_LEN,-1.0f,1300,730,camx,camy,camz,camyaw);
        if((flags&TF_BILLBOARD_R) && view_k>-10 && view_k<84)draw_world_billboard(raw*SEG_LEN,1.0f,1300,730,camx,camy,camz,camyaw);
    }

    qsort(g_mesh_out,(size_t)n,sizeof(g_mesh_out[0]),cmp_drawtri_far_first);
    for(k=0;k<n;++k)
        fill_tri2d(g_mesh_out[k].x0,g_mesh_out[k].y0,g_mesh_out[k].x1,g_mesh_out[k].y1,
                   g_mesh_out[k].x2,g_mesh_out[k].y2,g_mesh_out[k].color);
}

static void draw_player_shadow(void)
{
    float ox,oy,oz,relative_yaw,sx,sy;
    int row;
    float scale;
    int half;
    int cx,cy;
    uint16_t shadow=pack1555(20,28,22);

    get_player_camera_pose(&ox,&oy,&oz,&relative_yaw,&sx,&sy);
    (void)ox;(void)oy;(void)relative_yaw;

    scale=1660.0f/oz;
    half=(int)(66.0f*scale);
    if(half<48)half=48;
    if(half>74)half=74;
    cx=(int)sx;
    cy=(int)sy-7;

    for(row=0;row<14;++row){
        int hw=half-(row*row)/5;
        if(hw<10)hw=10;
        hline(cx-hw,cx+hw,cy+row,shadow);
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
    float ox,oy,oz,relative_yaw;

    get_player_camera_pose(&ox,&oy,&oz,&relative_yaw,NULL,NULL);

    render_sports_vehicle(
        ox,oy,oz,
        g_body_pitch,-relative_yaw,g_body_roll,
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
    float old_world_x,old_world_z;
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

    if(g_vc_city_mode){
        const float edge_margin=420.0f;
        float road_y;

        old_world_x=g_world_x;
        old_world_z=g_world_z;
        g_world_x+=sinf(g_vehicle_heading)*travel;
        g_world_z+=cosf(g_vehicle_heading)*travel;

        if(g_world_x>g_vc_map.max_x-edge_margin)g_world_x=g_vc_map.max_x-edge_margin;
        if(g_world_x<g_vc_map.min_x+edge_margin)g_world_x=g_vc_map.min_x+edge_margin;
        if(g_world_z>g_vc_map.max_z-edge_margin)g_world_z=g_vc_map.max_z-edge_margin;
        if(g_world_z<g_vc_map.min_z+edge_margin)g_world_z=g_vc_map.min_z+edge_margin;

        if(vc_city_ground_height(g_world_x,g_world_z,g_vc_ground_y,&road_y)){
            g_vc_ground_y=road_y;
            g_world_y+=((road_y+21.0f)-g_world_y)*0.35f;
        }

        g_position=0.0f;
        g_player_x=0.0f;
    }else if(g_osm_city_mode){
        const float edge_margin=420.0f;
        const float car_margin=180.0f;

        old_world_x=g_world_x;
        old_world_z=g_world_z;
        g_world_x+=sinf(g_vehicle_heading)*travel;
        g_world_z+=cosf(g_vehicle_heading)*travel;

        /* Collision remains intentionally cheap, but the visual buildings now
           use their true OSM footprints rather than these bounds. */
        if(osm_city_hits_building(g_world_x,g_world_z,car_margin)){
            g_world_x=old_world_x;
            g_world_z=old_world_z;
            g_speed*=-0.14f;
        }

        if(g_world_x>OSM_CITY_MAX_X-edge_margin)g_world_x=OSM_CITY_MAX_X-edge_margin;
        if(g_world_x<OSM_CITY_MIN_X+edge_margin)g_world_x=OSM_CITY_MIN_X+edge_margin;
        if(g_world_z>OSM_CITY_MAX_Z-edge_margin)g_world_z=OSM_CITY_MAX_Z-edge_margin;
        if(g_world_z<OSM_CITY_MIN_Z+edge_margin)g_world_z=OSM_CITY_MIN_Z+edge_margin;

        /* Follow the baked SRTM surface instead of an invisible flat plane. */
        g_world_y=osm_city_height_at_world(g_world_x,g_world_z)+21.0f;

        /* Legacy spline state is held neutral while the city world owns pose. */
        g_position=0.0f;
        g_player_x=0.0f;
    }else{
        track_world_t center;
        float rel;
        track_pose_at(g_position,0.0f,&center);
        rel=wrap_angle(g_vehicle_heading-center.yaw);
        longitudinal=travel*cosf(rel);
        lateral=travel*sinf(rel);

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

    /* Camera has its own damped position + angular spring. */
    update_chase_camera(abs_ratio);

    if(!g_osm_city_mode&&!g_vc_city_mode){
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
        update_traffic();
    }

    g_prev_speed=g_speed;
}

static uint32_t prefault_words(const uint16_t *p,size_t words)
{
    volatile uint32_t sum=0;
    size_t step=4096U/sizeof(uint16_t);
    size_t i;
    if(step<1)step=1;
    for(i=0;i<words;i+=step)sum+=p[i];
    if(words)sum+=p[words-1];
    return sum;
}

static void prefault_runtime_assets(void)
{
    uint32_t sum=0;
    int locked=0;

    /*
     * RACER.BIN runs directly from USB. Large const texture arrays are backed
     * by executable pages and an old Linux kernel may fault them in lazily.
     * Touch every 4 KiB page before gameplay so first-use texture access cannot
     * create a one-second USB demand-paging hitch.
     */
    sum^=prefault_words(sports_colormap,(size_t)SPORTS_COLORMAP_W*SPORTS_COLORMAP_H);
    sum^=prefault_words(track_asphalt,(size_t)TRACK_ASPHALT_W*TRACK_ASPHALT_H);
    sum^=prefault_words(kenney_colormap,(size_t)KENNEY_COLORMAP_W*KENNEY_COLORMAP_H);

#if defined(MCL_CURRENT)
    if(mlockall(MCL_CURRENT|MCL_FUTURE)==0)locked=1;
#endif
    fprintf(stderr,"[racer] assets prefaulted checksum=%08x mlockall=%s\n",
            (unsigned)sum,locked?"active":"unavailable");
}

static void render_frame(video_t *v,int idx)
{
    uint64_t t0,t1,begin=mono_ns();

    memcpy(v->canvas[idx],v->base,(size_t)RW*RH*2U);

    t0=mono_ns();
    draw_dynamic_sky();
    t1=mono_ns();
    g_prof.sky_ns+=t1-t0;

    t0=t1;
    if(g_vc_city_mode)draw_vc_city_world();
    else if(g_osm_city_mode)draw_osm_city_world();
    else draw_true3d_track();
    t1=mono_ns();
    g_prof.track_ns+=t1-t0;
    if(t1-t0>g_prof.max_track_ns)g_prof.max_track_ns=t1-t0;

    t0=t1;
    if(!g_osm_city_mode&&!g_vc_city_mode)draw_true3d_props();
    t1=mono_ns();
    g_prof.props_ns+=t1-t0;
    if(t1-t0>g_prof.max_props_ns)g_prof.max_props_ns=t1-t0;

    t0=t1;
    draw_player_shadow();
    t1=mono_ns();
    g_prof.shadow_ns+=t1-t0;

    t0=t1;
    draw_player_car3d();
    t1=mono_ns();
    g_prof.car_ns+=t1-t0;
    if(t1-t0>g_prof.max_car_ns)g_prof.max_car_ns=t1-t0;

    t0=t1;
    draw_hud();
    t1=mono_ns();
    g_prof.hud_ns+=t1-t0;

    g_prof.total_ns+=t1-begin;
    if(t1-begin>g_prof.max_total_ns)g_prof.max_total_ns=t1-begin;
    g_prof.frames++;
}

static int selftest(void)
{
    video_t v;
    unsigned i,changed=0;
    memset(&v,0,sizeof(v));
    init_colors();
    init_shade_lut();
    init_fog_lut();
    v.canvas[0]=(uint16_t*)calloc((size_t)RW*RH,sizeof(uint16_t));
    v.base=(uint16_t*)calloc((size_t)RW*RH,sizeof(uint16_t));
    if(!v.canvas[0]||!v.base)return 2;
    g_canvas=v.canvas[0];
    build_level();
    try_load_vc_map();
    reset_chase_camera();

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
    unsigned sim_ticks_window=0;
    unsigned last_presented=0;
    uint64_t acquire_ns_total=0,submit_ns_total=0;
    uint64_t acquire_ns_max=0,submit_ns_max=0;

    if(argc>1&&strcmp(argv[1],"--selftest")==0)return selftest();

    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGHUP,on_signal);
    init_colors();
    init_shade_lut();
    init_fog_lut();
    build_level();
    try_load_vc_map();
    reset_chase_camera();
    prefault_runtime_assets();

    if(video_open(&v)<0){video_close(&v);return 10;}
    probe_tde_backend();
    input_open(&in);
    pin_thread(0,"renderer");
    if(video_start(&v)<0){input_close(&in);video_close(&v);return 11;}

    {
        uint64_t last_sim=mono_ns();
        uint64_t accumulator=0;
        uint64_t next_frame=last_sim+FRAME_NS;
        perf=last_sim;
        memset(&g_prof,0,sizeof(g_prof));
        last_presented=v.presented;

        fprintf(stderr,"[racer] fixed simulation/present target=60Hz %s free-drive reverse sports-texture=%dx%d%s\n",
            g_vc_city_mode?"vcmap2-textured":"osm-terrain-city",
            SPORTS_COLORMAP_W,SPORTS_COLORMAP_H,
            g_vc_city_mode?" debug-toggle=T fog=30..78m far=92m":"");

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
                sim_ticks_window++;
            }

            {
                uint64_t q0=mono_ns(),q1,q2;
                video_acquire(&v,idx);
                q1=mono_ns();
                acquire_ns_total+=q1-q0;
                if(q1-q0>acquire_ns_max)acquire_ns_max=q1-q0;

                render_frame(&v,idx);

                q1=mono_ns();
                video_submit(&v,idx);
                q2=mono_ns();
                submit_ns_total+=q2-q1;
                if(q2-q1>submit_ns_max)submit_ns_max=q2-q1;
            }
            idx^=1;
            g_frame++;frames++;

            now=mono_ns();
            if(frames>=300){
                double sec=(double)(now-perf)/1000000000.0;
                double render_fps=sec>0.0?(double)frames/sec:0.0;
                unsigned presented_now;
                unsigned presented_delta;
                uint64_t present_total,present_max;
                unsigned present_count;
                double inv=(g_prof.frames>0)?1.0/(double)g_prof.frames:0.0;

                pthread_mutex_lock(&v.lock);
                presented_now=v.presented;
                present_total=v.present_ns_total;
                present_max=v.present_ns_max;
                present_count=v.present_profile_count;
                v.present_ns_total=0;
                v.present_ns_max=0;
                v.present_profile_count=0;
                pthread_mutex_unlock(&v.lock);

                presented_delta=presented_now-last_presented;

                fprintf(stderr,
                    "[racer] PERF stage7.13 render_fps=%.2f sim_hz=%.2f presented_fps=%.2f speed=%.1f world=%.0f,%.0f,%.0f sector=%d,%d input=%d rack=%.3f ack=%.3f/%.3f heading=%.3f cam=%.3f arm=%.3f camdist=%.0f targetdist=%.0f camh=%.0f slip=%.3f wheel=%.3f vcq=%d vcsec=%d vccap=%d vcmode=%s\n",
                    render_fps,
                    sec>0.0?(double)sim_ticks_window/sec:0.0,
                    sec>0.0?(double)presented_delta/sec:0.0,
                    g_speed,g_world_x,g_world_y,g_world_z,
                    (int)floorf(g_world_x/OSM_CITY_SECTOR_WORLD),
                    (int)floorf(g_world_z/OSM_CITY_SECTOR_WORLD),
                    in.steer,g_steer_angle,
                    g_steer_fl,g_steer_fr,g_vehicle_heading,g_camera_heading,g_camera_arm_heading,
                    g_camera_distance,g_camera_target_distance,g_camera_height,g_vehicle_slip,
                    g_wheel_spin,
                    g_vc_last_queued,g_vc_last_visible_sectors,g_vc_last_cap_hit,
                    g_vc_debug_flat?"flat":"textured");

                fprintf(stderr,
                    "[racer] PROFILE avg_ms total=%.2f sky=%.2f track=%.2f props=%.2f shadow=%.2f car=%.2f hud=%.2f acquire=%.2f submit=%.2f present=%.2f max_ms total=%.2f track=%.2f props=%.2f car=%.2f acquire=%.2f submit=%.2f present=%.2f\n",
                    (double)g_prof.total_ns*inv/1000000.0,
                    (double)g_prof.sky_ns*inv/1000000.0,
                    (double)g_prof.track_ns*inv/1000000.0,
                    (double)g_prof.props_ns*inv/1000000.0,
                    (double)g_prof.shadow_ns*inv/1000000.0,
                    (double)g_prof.car_ns*inv/1000000.0,
                    (double)g_prof.hud_ns*inv/1000000.0,
                    (double)acquire_ns_total/(double)frames/1000000.0,
                    (double)submit_ns_total/(double)frames/1000000.0,
                    present_count?(double)present_total/(double)present_count/1000000.0:0.0,
                    (double)g_prof.max_total_ns/1000000.0,
                    (double)g_prof.max_track_ns/1000000.0,
                    (double)g_prof.max_props_ns/1000000.0,
                    (double)g_prof.max_car_ns/1000000.0,
                    (double)acquire_ns_max/1000000.0,
                    (double)submit_ns_max/1000000.0,
                    (double)present_max/1000000.0);

                if(g_prof.max_total_ns>70000000ULL)
                    fprintf(stderr,"[racer] HITCH render_max_ms=%.2f (textures are baked; this is render workload, not disk texture streaming)\n",
                            (double)g_prof.max_total_ns/1000000.0);

                memset(&g_prof,0,sizeof(g_prof));
                acquire_ns_total=submit_ns_total=0;
                acquire_ns_max=submit_ns_max=0;
                sim_ticks_window=0;
                last_presented=presented_now;
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
