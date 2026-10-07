/*
 * Stayplaytion Racer Stage 8.3 - Dense VC vehicle + collision unstick
 *
 * Native Hi3531 hybrid pseudo-3D + true low-poly 3D arcade racer.
 * No SDL/OpenGL/X11 while native framebuffer lease is active.
 *
 * Visual architecture:
 *   - 640x360 internal A1R5G5B5 render target
 *   - Hi3531 TDE 2x present to 1280x720 HIFB with CPU fallback
 *   - CPU0 game/render, CPU1 framebuffer presenter
 *   - OutRun-style projected road + true low-poly 3D cars/buildings
 *   - build-time packed CC0 sky/billboard art from OpenMRac-data
 *
 * Hardware path:
 *   - TDE QuickResize handles full-frame 640x360 -> 1280x720 scaling
 *   - CPU fallback remains available if TDE or HIFB staging is unavailable
 * Future:
 *   - cached MMZ render surfaces and CPU1 scene-preparation pipeline
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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "racer_assets.h"
#include "h3531_tde_direct.h"
#include "h3531_mmz_direct.h"

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
#define VC_FOG_START_M 48.0f
#define VC_FAR_CLIP_M 112.0f
/* VFW pages are only transport/cache containers. Visual identity now comes
 * from VCOBJ instances, which activate individually inside a loaded page. */
#define VC_DETAIL_PREFETCH_M 48.0f
#define VC_DETAIL_EVICT_M 192.0f
#define VC_TRI_LOD_STEP_M 8.0f
#define VC_MODEL_FADE_M 20.0f
#define VC_OBJECT_FADE_NS 550000000ULL
#define VC_OBJECT_START_BUDGET 5
#define VC_PLAYER_SLIP_START_RAD 0.075f
#define VC_PLAYER_SLIP_FULL_RAD 0.235f
#define VC_PLAYER_REAR_GRIP_MIN 0.70f
#define VC_PLAYER_FRONT_GRIP_MIN 0.92f
#define VC_PLAYER_BALANCE_START_TICKS 12U
#define VC_PLAYER_BALANCE_RAMP_TICKS 18.0f
#define VC_PLAYER_COM_MAX_UP_M 0.10f
#define VC_SECTOR_SPAN 4
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
/*
 * reVC treats the gameplay FOV as a 4:3 horizontal FOV, then its enabled
 * ASPECT_RATIO_SCALE converts it HOR+ for the current aspect ratio:
 *
 *   base hFOV 70 deg @ 4:3 -> vFOV 55.413 deg
 *   same vFOV @ 16:9      -> hFOV 86.067 deg
 *   focal @ 640x360       -> 342.756 px
 *
 * The old 257 px focal produced roughly 102-degree hFOV and severe edge
 * stretching. Using the 4:3 70-degree focal directly (457 px) would instead
 * over-zoom widescreen. City and VCVEH share this reVC-style 16:9 projection.
 */
#define VC_FOCAL 342.756f
#define VC_SCREEN_Y ((float)RH*0.5f)

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
    int canvas_heap;
    int mmz_fd;
    int mmz_abi;
    int mmz_ready;
    int mmz_direct;
    uint32_t mmz_phys[2];
    void *mmz_virt[2];
    size_t mmz_bytes;
    unsigned mmz_flush_failures;
    int vblank_state;
    int tde_fd;
    int tde_ready;
    size_t tde_src_offset;
    uint8_t *tde_src_virt;
    uint32_t tde_src_phys;
    unsigned tde_failures;
    uint64_t tde_copy_ns_total;
    uint64_t tde_job_ns_total;
    uint64_t tde_job_ns_max;
    unsigned tde_profile_count;
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
    int gas,brake,handbrake;
    int steer,move_y;
    int steer_node;
    int start_down,select_down;
    int camera_cycle_pressed;
    int camera_cycle_prev;
    int camera_look_key;
    int camera_look_behind;
    int camera_side_left,camera_side_right;
    int camera_view_toggle_pressed;
    int camera_view_toggle_prev;
    int radio_cycle_pressed;
    int radio_cycle_prev;
    int camera_orbit_x,camera_orbit_y;
    int dev_lift,dev_lower;
    int dev_left,dev_right,dev_up,dev_down;
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
/* VCV1 and legacy VCM2 triangle. */
typedef struct { uint16_t a,b,c; uint8_t material,flags; } vc_tri_t;
/* VCM3 map triangle: widened material id, explicit pad, 10 bytes. */
typedef struct { uint16_t a,b,c,material; uint8_t flags,pad; } vc_map_tri_t;
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
    char magic[4];
    uint32_t version;
    uint32_t vertex_count,tri_count,material_count,atlas_w,atlas_h;
    float world_scale;
    float mass,traction_mult,traction_loss,traction_bias;
    float max_velocity_kmh,engine_accel_raw,brake_decel_raw,brake_bias;
    float steering_lock_deg;
    float com_x,com_y,com_z;
    float dim_x,dim_y,dim_z;
} vcveh_header_t;

typedef struct {
    char magic[4];
    uint32_t version;
    char name[16];
    float mass;
    float dim_x,dim_y,dim_z;
    float com_x,com_y,com_z;
    float traction_mult,traction_loss,traction_bias;
    float max_velocity_kmh,engine_accel_raw;
    float brake_decel_raw,brake_bias;
    float steering_lock_deg;
    float suspension_force,suspension_damping;
    float suspension_upper,suspension_lower;
    float suspension_bias,suspension_antidive;
    uint32_t gears,drive_type,engine_type,abs_enabled,flags;
} vchand_header_t;

typedef struct {
    float adhesive[6][6];
    int loaded;
} vc_surface_runtime_t;

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t sphere_count,box_count,tri_count,line_count;
    float bound_cx,bound_cy,bound_cz,bound_r;
    float box_min_x,box_min_y,box_min_z;
    float box_max_x,box_max_y,box_max_z;
} vcveh_col_ext_header_v1_t;

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t sphere_count,box_count,tri_count,line_count;
    float bound_cx,bound_cy,bound_cz,bound_r;
    float box_min_x,box_min_y,box_min_z;
    float box_max_x,box_max_y,box_max_z;
    float rest_height_world;
} vcveh_col_ext_header_v2_t;

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t sphere_count,box_count,tri_count,line_count;
    float bound_cx,bound_cy,bound_cz,bound_r;
    float box_min_x,box_min_y,box_min_z;
    float box_max_x,box_max_y,box_max_z;
    float rest_height_world;
    float suspension_force,suspension_damping;
    float suspension_upper,suspension_lower;
    float suspension_bias,suspension_antidive;
} vcveh_col_ext_header_v3_t;

typedef struct {
    float x,y,z,r;
    uint8_t surface,piece;
    uint16_t pad;
} vcveh_col_sphere_t;

typedef struct {
    float p0x,p0y,p0z,p1x,p1y,p1z;
    uint8_t part,pad1;
    uint16_t pad2;
} vcveh_col_line_t;

typedef struct {
    uint32_t vertex_count,tri_count,material_count,atlas_w,atlas_h;
    float world_scale;
    float wheelbase,track,wheel_radius;
    float dim_x,dim_y,dim_z;
    v3f_t centre_of_mass;
    float turn_mass_world;
    vc_vertex_t *verts;
    vc_tri_t *tris;
    vc_material_t *materials;
    uint16_t *atlas;
    uint8_t *vertex_part;
    v3f_t wheel_pivot[5];
    uint8_t wheel_present[5];

    /* Optional VCL1 trailer: original GTA CColModel body spheres. */
    uint32_t col_sphere_count,col_box_count,col_tri_count,col_line_count;
    vcveh_col_sphere_t *col_spheres;
    vcveh_col_line_t *col_lines;
    v3f_t col_bound_center,col_box_min,col_box_max;
    float col_bound_radius;
    float rest_height_world;
    float suspension_force,suspension_damping;
    float suspension_upper,suspension_lower;
    float suspension_bias,suspension_antidive;
    uint32_t native_col_version;
    int native_col_loaded;
    int loaded;
} vc_vehicle_runtime_t;

typedef struct {
    float cx,cy,cz,radius;
    float draw_world;
    float lod_world[3];
    uint32_t flags,model_id;
    uint8_t alpha;
    uint8_t active;
    uint8_t lod_count;
    uint8_t lod_selected;
    uint8_t camera_visible;
    uint8_t pad0;
} vc_stream_object_t;

typedef struct {
    float world_scale,sector_m,sector_world;
    float spawn_x,spawn_y,spawn_z,spawn_yaw;
    float min_x,max_x,min_z,max_z;
    uint32_t vertex_count,tri_count,sector_count,material_count;
    uint32_t atlas_w,atlas_h;
    vc_vertex_t *verts;
    vc_map_tri_t *tris;
    vc_sector_t *sectors;
    vc_material_t *materials;
    uint16_t *atlas;
    uint32_t *tex_offsets;
    size_t compact_texels;
    int compact_textures;
    uint32_t object_count;
    vc_stream_object_t *objects;
    uint16_t *tri_object;
    uint8_t *tri_lod;
    uint64_t object_last_ns;
    uint32_t object_last_frame;
    uint32_t object_visibility_frame;
} vc_runtime_map_t;

typedef struct {
    float ax,ay,az,bx,by,bz,cx,cy,cz;
    /*
     * VCC1: material=GTA surface, flags=old derived ground/solid flags.
     * VCC2: material=GTA surface, flags=GTA piece.
     */
    uint8_t material,flags;
    uint16_t pad;
} vc_col_tri_t;

typedef struct {
    float x,y,z,r;
    uint8_t surface,piece;
    uint16_t pad;
} vc_col_sphere_t;

typedef struct {
    int16_t sx,sz;
    uint32_t tri_base,tri_count;
} vc_col_sector_t;

typedef struct {
    int16_t sx,sz;
    uint32_t tri_base,tri_count;
    uint32_t sphere_base,sphere_count;
} vc_col_sector2_t;

typedef struct {
    float world_scale,sector_m,sector_world;
    uint32_t version,tri_count,sphere_count,sector_count;
    vc_col_tri_t *tris;
    vc_col_sphere_t *spheres;
    vc_col_sector_t *sectors;
    vc_col_sector2_t *sectors2;
    int loaded;
} vc_collision_runtime_t;

#define VC_WORLD_CACHE_RADIUS 2
#define VC_WORLD_CACHE_SIDE (VC_WORLD_CACHE_RADIUS*2+1)
#define VC_WORLD_CACHE_SLOTS (VC_WORLD_CACHE_SIDE*VC_WORLD_CACHE_SIDE)
#define VC_WORLD_PAGE_PATH_MAX 320

typedef struct {
    char magic[4];
    uint32_t version;
    float page_m,sector_m;
    float min_x,min_y,max_x,max_y;
    int32_t min_page_x,max_page_x,min_page_y,max_page_y;
    uint32_t page_count,reserved;
} vcworld_header_t;

typedef struct {
    int32_t page_x,page_y;
    uint32_t instances;
    uint32_t vcmap_bytes,vccol_bytes;
    uint32_t vertices,triangles,materials;
    uint32_t atlas_w,atlas_h;
} vcworld_entry_t;

typedef struct {
    int loaded;                 /* page residency: collision/base are available */
    int page_x,page_y;
    int entry_index;
    vc_runtime_map_t map;       /* streamed detail: ordinary buildings/props */
    vc_runtime_map_t base;      /* persistent roads/big buildings/GTA LODs */
    vc_collision_runtime_t collision;
    int base_loaded;
    int detail_state;           /* 0=pending, 1=loaded/fading, -1=no detail */
    uint64_t detail_loaded_ns;
} vc_world_page_t;

typedef struct {
    int loaded;
    float page_m,sector_m,world_scale,sector_world;
    float min_x,min_y,max_x,max_y;
    int min_page_x,max_page_x,min_page_y,max_page_y;
    int center_page_x,center_page_y;
    int split_layout;
    int stream_layout_version;
    uint32_t page_count;
    vcworld_entry_t *entries;
    vc_world_page_t pages[VC_WORLD_CACHE_SLOTS];
    char base_dir[VC_WORLD_PAGE_PATH_MAX];
} vc_world_runtime_t;

typedef struct {
    float nx,ny,nz;
    float depth;
    float px,py,pz;
    uint8_t surface;
    uint8_t piece;
    int hit;
} vc_body_contact_t;

typedef struct {
    int hit;
    float ratio;               /* reVC-normalized spring ratio: 0 compressed, 1 extended */
    v3f_t point;               /* world-space tyre contact */
    v3f_t normal;              /* world-space COL normal */
    v3f_t spring_dir;          /* p0 -> p1, world-space unit vector */
    uint8_t surface;
} vc_wheel_contact_t;

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
    uint16_t material;
    uint8_t page_slot;
    uint8_t pad;
    uint8_t fade;
    uint8_t reserved;
} vc_textri_t;


static volatile sig_atomic_t g_stop=0;
static int g_control_fd=-1;
static char g_control_path[128]="/var/racer-control";
static char g_control_buf[256];
static size_t g_control_len=0;
static uint16_t *g_canvas=NULL;
static track_seg_t g_track[TRACK_SEGMENTS];
static track_world_t g_track_world[TRACK_SEGMENTS+1];
static proj_t g_proj[DRAW_DISTANCE+1];
static traffic_t g_traffic[8];
static uint32_t g_frame=0;
static float g_position=0.0f;
static float g_speed=0.0f;
static float g_vehicle_vlong=0.0f;
static float g_vehicle_vlat=0.0f;
static float g_vehicle_yaw_rate=0.0f;
static float g_vehicle_steer_input=0.0f;
/* Raw pad steering, equivalent to CPad::GetSteeringLeftRight()/128 in reVC.
 * The filtered value above drives the rack; player two-wheel balance must use
 * the raw input exactly like Vice City does. */
static float g_vc_raw_steer_input=0.0f;
static float g_player_x=0.0f;
static float g_world_x=OSM_CITY_SPAWN_X;
static float g_world_y=OSM_CITY_SPAWN_Y;
static float g_world_z=OSM_CITY_SPAWN_Z;
static int g_osm_city_mode=1;
static int g_vc_city_mode=0;
static int g_vc_debug_flat=0;
static int g_vc_debug_affine=0;
static int g_vc_last_queued=0;
static int g_vc_last_visible_sectors=0;
static int g_vc_last_cap_hit=0;
static vc_runtime_map_t g_vc_map;
static vc_collision_runtime_t g_vc_collision;
static vc_world_runtime_t g_vc_world;
static int g_vc_world_mode=0;
static vc_vehicle_runtime_t g_vc_vehicle;
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
static float g_body_pitch_vel=0.0f;
static float g_body_roll_vel=0.0f;
static v3f_t g_vc_turn_world={0.0f,0.0f,0.0f};
static v3f_t g_vc_body_right={1.0f,0.0f,0.0f};
static v3f_t g_vc_body_up={0.0f,1.0f,0.0f};
static v3f_t g_vc_body_forward={0.0f,0.0f,1.0f};
static int g_vc_body_basis_valid=0;
static float g_vehicle_vy=0.0f;
static int g_vehicle_airborne=0;
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
static int g_camera_zoom_mode=1; /* 0 near, 1 mid, 2 far */
static int g_camera_look_behind=0;
static float g_camera_pitch=0.0f;
static float g_camera_pitch_vel=0.0f;
static float g_camera_orbit_yaw=0.0f;
static float g_camera_orbit_pitch=0.0f;
static unsigned g_camera_orbit_idle_ticks=0;
static int g_camera_orbit_input_x=0,g_camera_orbit_input_y=0;
static int g_camera_side_left=0,g_camera_side_right=0;
/*
 * Mirror Vice City's normal vehicle camera cycle. reVC cycles:
 * 1STPRS -> ZOOM_1 -> ZOOM_2 -> ZOOM_3 -> CINEMATIC and deliberately skips
 * TOPDOWN. Keep the same ordering in the lightweight H3531 camera.
 */
enum {
    VC_CAM_1STPRS=0,
    VC_CAM_ZOOM_1=1,
    VC_CAM_ZOOM_2=2,
    VC_CAM_ZOOM_3=3,
    VC_CAM_CINEMATIC=4,
    VC_CAM_COUNT=5
};
static int g_camera_cycle_mode=VC_CAM_ZOOM_2;
static unsigned g_vc_two_wheel_ticks=0;
static uint64_t g_vc_dyn_last_log_ns=0ULL;
static float g_vc_effective_com_y=0.0f;
static int g_dev_hover=0;
static float g_dev_hover_fwd=0.0f;
static float g_dev_hover_yaw=0.0f;
static float g_dev_hover_up=0.0f;
static int g_lap=1;

#define VC_PAGE_BASE_FLAG 0x80U

static const vc_runtime_map_t *vc_map_for_page_slot(uint8_t slot)
{
    unsigned idx;
    if(slot==0xffU)return &g_vc_map;
    if(!g_vc_world_mode)return &g_vc_map;
    idx=(unsigned)(slot&0x7fU);
    if(idx>=VC_WORLD_CACHE_SLOTS || !g_vc_world.pages[idx].loaded)return NULL;
    if(slot&VC_PAGE_BASE_FLAG)
        return g_vc_world.pages[idx].base_loaded?&g_vc_world.pages[idx].base:NULL;
    return g_vc_world.pages[idx].detail_state==1?&g_vc_world.pages[idx].map:NULL;
}

static float vc_runtime_world_scale(void)
{
    if(g_vc_world_mode&&g_vc_world.world_scale>1.0f)
        return g_vc_world.world_scale;
    return g_vc_map.world_scale>1.0f?g_vc_map.world_scale:240.0f;
}

static float vc_runtime_sector_world(void)
{
    if(g_vc_world_mode&&g_vc_world.sector_world>1.0f)
        return g_vc_world.sector_world;
    return g_vc_map.sector_world;
}

static float vc_runtime_min_x(void)
{
    return g_vc_world_mode?g_vc_world.min_x*vc_runtime_world_scale():g_vc_map.min_x;
}
static float vc_runtime_max_x(void)
{
    return g_vc_world_mode?g_vc_world.max_x*vc_runtime_world_scale():g_vc_map.max_x;
}
static float vc_runtime_min_z(void)
{
    return g_vc_world_mode?g_vc_world.min_y*vc_runtime_world_scale():g_vc_map.min_z;
}
static float vc_runtime_max_z(void)
{
    return g_vc_world_mode?g_vc_world.max_y*vc_runtime_world_scale():g_vc_map.max_z;
}

/*
 * Stage8.0 reduced reVC-style handling state.
 *
 * This is an independent lightweight implementation.  It keeps the useful
 * behavioural architecture observed in reVC without importing its source:
 * filtered/non-linear steering, signed pedal state, persistent linear/angular
 * velocity, and finite tyre adhesion with traction loss.
 */
typedef struct {
    float mass;
    float traction_mult;
    float traction_loss;
    float traction_bias;
    float brake_bias;
    float max_forward;
    float max_reverse;
    float engine_accel;
    float brake_decel;
    float steering_lock_rad;
    float rolling_drag;
    float aero_drag;
    float suspension_force;
    float suspension_damping;
    float suspension_upper;
    float suspension_lower;
    float suspension_bias;
    float suspension_antidive;
    float dim_x,dim_y,dim_z;
    v3f_t centre_of_mass;
    float turn_mass_world;
    uint8_t gears;
    uint8_t drive_type;
    uint8_t engine_type;
    uint8_t abs_enabled;
    uint32_t flags;
    int gta_profile_loaded;
    char profile_name[16];
} vc_handling_lite_t;

static vc_handling_lite_t g_vehicle_handling={
    .mass=1400.0f,
    .traction_mult=1.02f,.traction_loss=0.82f,.traction_bias=0.52f,
    .brake_bias=0.52f,
    .max_forward=90.0f,.max_reverse=36.0f,
    .engine_accel=0.45f,.brake_decel=0.65f,
    .steering_lock_rad=0.57596f,
    .rolling_drag=0.085f,.aero_drag=0.000018f,
    .suspension_force=1.40f,.suspension_damping=0.12f,
    .suspension_upper=0.28f,.suspension_lower=-0.12f,
    .suspension_bias=0.50f,.suspension_antidive=0.0f,
    .dim_x=1.8f,.dim_y=4.2f,.dim_z=1.35f,
    .centre_of_mass={0.0f,0.0f,-0.15f*240.0f},
    .turn_mass_world=1400.0f*250000.0f,
    .gears=5,.drive_type='R',.engine_type='P',.abs_enabled=0,.flags=0,
    .gta_profile_loaded=0,.profile_name="builtin"
};

enum {
    VC_WHEEL_NORMAL=0,
    VC_WHEEL_SPINNING=1,
    VC_WHEEL_SKIDDING=2,
    VC_WHEEL_FIXED=3
};

static vc_surface_runtime_t g_vc_surface;
static uint8_t g_vc_wheel_state[4]={0,0,0,0};
static float g_vc_wheel_speed[4]={0,0,0,0};
static float g_vc_wheel_fwd_speed[4]={0,0,0,0};
static float g_vc_wheel_side_speed[4]={0,0,0,0};
static float g_vc_wheel_adhesion[4]={0,0,0,0};
static float g_vc_wheel_force_fwd[4]={0,0,0,0};
static float g_vc_wheel_force_side[4]={0,0,0,0};
static float g_vc_wheel_turn_mass[4]={0,0,0,0};
static uint8_t g_vc_current_gear=1;

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

typedef struct {
    uint64_t zpass_pixels;
    uint64_t texture_samples;
    uint64_t correction_segments;
    uint64_t bbox_pixels;
    uint64_t span_pixels;
} vc_raster_stats_t;

typedef struct {
    uint64_t scan_ns;
    uint64_t queue_ns;
    uint64_t zclear_ns;
    uint64_t raster_ns;
    uint64_t max_scan_ns;
    uint64_t max_queue_ns;
    uint64_t max_raster_ns;
    uint64_t xformed_vertices;
    uint64_t tested_tris;
    uint64_t zpass_pixels;
    uint64_t texture_samples;
    uint64_t correction_segments;
    uint64_t bbox_pixels;
    uint64_t span_pixels;
    uint64_t raster_top_ns;
    uint64_t raster_bottom_ns;
    uint64_t split_rows;
    unsigned frames;
} vc_prof_t;

static vc_prof_t g_vc_prof;
static unsigned g_vc_frame_xformed_vertices=0;
static unsigned g_vc_frame_tested_tris=0;
static unsigned g_vc_frame_affine_tris=0;
static unsigned g_vc_frame_clip_fast=0;
static unsigned g_vc_frame_clip_partial=0;
static unsigned g_vc_frame_clip_reject=0;
static unsigned g_vc_frame_backface_reject=0;
static unsigned g_vc_frame_fogflat_tris=0;
static unsigned g_vc_frame_far_tiny_reject=0;
static unsigned g_vc_frame_lod_reject=0;
static unsigned g_vc_frame_lod_fade=0;
static unsigned g_vc_frame_objects_active=0;
static unsigned g_vc_frame_objects_fading=0;
static unsigned g_vc_frame_objects_started=0;
static unsigned g_vc_frame_object_lod[3]={0,0,0};
static unsigned g_vc_frame_object_lod_switches=0;
static unsigned g_vc_frame_object_frustum_reject=0;
static unsigned g_vc_object_start_budget_left=0;
static unsigned g_vc_deck_rejects_window=0;
static unsigned g_vc_collision_blocks_window=0;
static unsigned g_vc_collision_blocks_total=0;
static unsigned g_vc_body_floor_suppressed_window=0;
static unsigned g_vc_visual_ground_fallback_window=0;
static unsigned g_vc_visual_ground_fallback_total=0;
static float g_vc_last_col_depth=0.0f;
static float g_vc_last_col_nx=0.0f,g_vc_last_col_ny=0.0f,g_vc_last_col_nz=0.0f;
static float g_vc_last_col_vn=0.0f;
static uint8_t g_vc_wheel_surface[4]={0,0,0,0};
static vc_wheel_contact_t g_vc_wheel_contact[4];
static uint8_t g_vc_wheel_exact_mask=0;
static uint8_t g_vc_wheel_rescue_mask=0;
static float g_vc_wheel_timer[4]={0,0,0,0};
static int g_vc_front_support=0,g_vc_rear_support=0;
static int g_vc_left_support=0,g_vc_right_support=0;
static float g_vc_last_support_pitch=0.0f;
static float g_vc_last_support_roll=0.0f;
static uint8_t g_vc_wheel_contact_mask=0;
static uint8_t g_vc_wheel_latched_mask=0;
static uint8_t g_vc_last_body_surface=0;
static unsigned g_vcveh_last_draw_tris=0;
static unsigned g_vcveh_last_tiny_reject=0;
static unsigned g_vcveh_last_screen_reject=0;
static unsigned g_vcveh_zpass_pixels=0;
static unsigned g_vcveh_zblocked_pixels=0;
static int g_vc_backface_cull=1;

static void build_world_track(void);
static float clampf_local(float v,float lo,float hi);
static float active_vehicle_wheelbase(void);
static float active_vehicle_track(void);
static float active_vehicle_wheel_radius(void);
static float active_suspension_travel_world(void);
static void get_chase_camera(float *camx,float *camy,float *camz,float *camyaw);
static void vc_body_basis_from_euler(void);
static void vc_body_rotate_local(v3f_t in,v3f_t *out);
static float vc_v3_dot(v3f_t a,v3f_t b);
static v3f_t vc_v3_cross(v3f_t a,v3f_t b);
static int vc_v3_normalize(v3f_t *v);
static void vc_reset_turn_world(void);
static void vc_integrate_turn_world(void);



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
#define MAX_MESH_VERTS 12000
#define MAX_DRAW_TRIS 16000
#define MAX_VC_DRAW_TRIS 16384
static v3f_t g_mesh_rv[MAX_MESH_VERTS];
static v3f_t g_mesh_cam[MAX_MESH_VERTS];
static sv3_t g_mesh_sv[MAX_MESH_VERTS];
static drawtri_t g_mesh_out[MAX_DRAW_TRIS];
static textri_t g_tex_out[MAX_DRAW_TRIS];
static vc_textri_t g_vc_tex_out[MAX_VC_DRAW_TRIS];
static uint16_t g_vc_order[MAX_VC_DRAW_TRIS];

/* Dense local VC player vehicle scratch, sized from VCVEH.BIN at load time. */
static v3f_t *g_vcveh_rv=NULL;
static sv3_t *g_vcveh_sv=NULL;
static textri_t *g_vcveh_out=NULL;
static uint32_t g_vcveh_vertex_cap=0;
static uint32_t g_vcveh_tri_cap=0;

static citytri_t g_city_out[MAX_DRAW_TRIS];
static v2f_t g_vc_mesh_uv[MAX_MESH_VERTS];
static uint8_t g_vc_mesh_xformed[MAX_MESH_VERTS];
static uint8_t g_vc_mesh_outcode[MAX_MESH_VERTS];
static uint8_t g_vc_mesh_projected[MAX_MESH_VERTS];
static sv3_t g_vc_mesh_proj[MAX_MESH_VERTS];
static uint16_t g_city_zbuf[RW*RH];

typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t start_cv;
    pthread_cond_t done_cv;
    int ready;
    int stop;
    int pending;
    int tri_count;
    int split_y;
    uint64_t raster_ns;
    vc_raster_stats_t stats;
} vc_raster_worker_t;

static vc_raster_worker_t g_vc_raster_worker;
static int g_vc_raster_split_y=RH/2;
static uint64_t g_vc_raster_top_ema_ns=0;
static uint64_t g_vc_raster_bottom_ema_ns=0;
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
/*
 * Stage8.0: the VC textured hot path used to perform two random lookups into
 * the 512 KiB shade LUT and the 512 KiB fog LUT for every visible texel.
 * Both operations are separable by 5-bit RGB channel, so collapse them into a
 * tiny 8*8*3*32 byte table that stays cache-hot on Cortex-A9.
 */
static uint8_t g_vc_color_chan[8][8][3][32];

static uint16_t pack1555(unsigned r,unsigned g,unsigned b)
{
    if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;
    return (uint16_t)(0x8000U|((r>>3)<<10)|((g>>3)<<5)|(b>>3));
}

static uint16_t revc_fog_color_from_baked_sky(void)
{
    /*
     * reVC CTimeCycle derives fog RGB as:
     *   (SkyTop + 2*SkyBottom) / 3
     * Sample those two bands from our baked day panorama so the far city
     * dissolves into the actual horizon instead of a fixed blue wall.
     */
    uint64_t tr=0,tg=0,tb=0,br=0,bg=0,bb=0;
    unsigned tn=0,bn=0;
    int x,y;
    int top0=RACER_SKY_H/12,top1=RACER_SKY_H/4;
    int bot0=(RACER_SKY_H*3)/4,bot1=(RACER_SKY_H*15)/16;
    for(y=top0;y<top1;++y){
        for(x=0;x<RACER_SKY_W;x+=4){
            uint16_t c=racer_sky[(size_t)y*RACER_SKY_W+x];
            tr+=(c>>10)&31U;tg+=(c>>5)&31U;tb+=c&31U;tn++;
        }
    }
    for(y=bot0;y<bot1;++y){
        for(x=0;x<RACER_SKY_W;x+=4){
            uint16_t c=racer_sky[(size_t)y*RACER_SKY_W+x];
            br+=(c>>10)&31U;bg+=(c>>5)&31U;bb+=c&31U;bn++;
        }
    }
    if(!tn||!bn)return pack1555(132,181,210);
    {
        unsigned r=(unsigned)(((tr/tn)+2U*(br/bn))/3U);
        unsigned g=(unsigned)(((tg/tn)+2U*(bg/bn))/3U);
        unsigned b=(unsigned)(((tb/tn)+2U*(bb/bn))/3U);
        return (uint16_t)(0x8000U|(r<<10)|(g<<5)|b);
    }
}

static void init_colors(void)
{
    C_SKY=pack1555(126,190,236);
    C_VC_FOG=revc_fog_color_from_baked_sky();
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

static uint8_t vc_page_fade_for_slot(uint8_t page_slot)
{
    /*
     * No spatial/stipple fade for streamed pages. The 1-bit alpha framebuffer
     * turned that approximation into visible "sieve" buildings. Detail is
     * prefetched outside the visible radius instead.
     */
    (void)page_slot;
    return 255U;
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

static int video_mmz_alloc80(video_t *v,int idx)
{
    h3531_mmb80_t m;
    int r;
    memset(&m,0,sizeof(m));
    m.size=(uint32_t)v->mmz_bytes;
    m.w32_stuf=H3531_MMZ_PROT_FLAGS;
    snprintf(m.mmb_name,sizeof(m.mmb_name),"racer%d",idx);
    r=ioctl(v->mmz_fd,H3531_MMB80_ALLOC_V2,&m);
    if(r!=0){
        memset(&m,0,sizeof(m));
        m.size=(uint32_t)v->mmz_bytes;
        m.w32_stuf=H3531_MMZ_PROT_FLAGS;
        snprintf(m.mmb_name,sizeof(m.mmb_name),"racer%d",idx);
        r=ioctl(v->mmz_fd,H3531_MMB80_ALLOC,&m);
    }
    if(r!=0)return -1;
    m.w32_stuf=H3531_MMZ_PROT_FLAGS;
    if(ioctl(v->mmz_fd,H3531_MMB80_REMAP_CACHE,&m)!=0){
        ioctl(v->mmz_fd,H3531_MMB80_FREE,&m);
        return -1;
    }
    if(!m.phys_addr||!m.mapped){
        ioctl(v->mmz_fd,H3531_MMB80_UNMAP,&m);
        ioctl(v->mmz_fd,H3531_MMB80_FREE,&m);
        return -1;
    }
    v->mmz_phys[idx]=m.phys_addr;
    v->mmz_virt[idx]=(void*)(uintptr_t)m.mapped;
    return 0;
}

static int video_mmz_alloc96(video_t *v,int idx)
{
    h3531_mmb96_t m;
    int r;
    memset(&m,0,sizeof(m));
    m.size=(uint32_t)v->mmz_bytes;
    m.w32_stuf=H3531_MMZ_PROT_FLAGS;
    snprintf(m.mmb_name,sizeof(m.mmb_name),"racer%d",idx);
    r=ioctl(v->mmz_fd,H3531_MMB96_ALLOC_V2,&m);
    if(r!=0){
        memset(&m,0,sizeof(m));
        m.size=(uint32_t)v->mmz_bytes;
        m.w32_stuf=H3531_MMZ_PROT_FLAGS;
        snprintf(m.mmb_name,sizeof(m.mmb_name),"racer%d",idx);
        r=ioctl(v->mmz_fd,H3531_MMB96_ALLOC,&m);
    }
    if(r!=0)return -1;
    m.w32_stuf=H3531_MMZ_PROT_FLAGS;
    if(ioctl(v->mmz_fd,H3531_MMB96_REMAP_CACHE,&m)!=0){
        ioctl(v->mmz_fd,H3531_MMB96_FREE,&m);
        return -1;
    }
    if(!m.phys_addr||!m.mapped){
        ioctl(v->mmz_fd,H3531_MMB96_UNMAP,&m);
        ioctl(v->mmz_fd,H3531_MMB96_FREE,&m);
        return -1;
    }
    v->mmz_phys[idx]=m.phys_addr;
    v->mmz_virt[idx]=(void*)(uintptr_t)m.mapped;
    return 0;
}

static void video_mmz_free_slot(video_t *v,int idx)
{
    if(v->mmz_fd<0||!v->mmz_phys[idx])return;
    if(v->mmz_abi==80){
        h3531_mmb80_t m;
        memset(&m,0,sizeof(m));
        m.phys_addr=v->mmz_phys[idx];
        m.mapped=(uint32_t)(uintptr_t)v->mmz_virt[idx];
        if(m.mapped)ioctl(v->mmz_fd,H3531_MMB80_UNMAP,&m);
        ioctl(v->mmz_fd,H3531_MMB80_FREE,&m);
    }else if(v->mmz_abi==96){
        h3531_mmb96_t m;
        memset(&m,0,sizeof(m));
        m.phys_addr=v->mmz_phys[idx];
        m.mapped=(uint32_t)(uintptr_t)v->mmz_virt[idx];
        if(m.mapped)ioctl(v->mmz_fd,H3531_MMB96_UNMAP,&m);
        ioctl(v->mmz_fd,H3531_MMB96_FREE,&m);
    }
    v->mmz_phys[idx]=0;
    v->mmz_virt[idx]=NULL;
}

static void video_mmz_close(video_t *v)
{
    if(v->mmz_fd>=0){
        video_mmz_free_slot(v,0);
        video_mmz_free_slot(v,1);
        close(v->mmz_fd);
    }
    v->mmz_fd=-1;
    v->mmz_ready=0;
    v->mmz_direct=0;
}

static int video_mmz_init(video_t *v)
{
    const char *mode=getenv("RACER_MMZ");
    int ok0=-1,ok1=-1;
    size_t bytes=((size_t)RW*RH*2U+4095U)&~(size_t)4095U;

    v->mmz_fd=-1;
    v->mmz_abi=0;
    v->mmz_ready=0;
    v->mmz_direct=0;
    v->mmz_bytes=bytes;

    /*
     * Direct MMZ ioctl is deliberately opt-in on the original Hi3531 image.
     * The real board showed a startup hang when auto-probing the allocator,
     * which means a same-number ioctl with a different private ABI can block.
     * Keep the known-good heap -> HIFB-tail -> TDE path as the default until
     * this exact firmware ABI is proven.
     */
    if(!mode || (strcmp(mode,"1")&&strcmp(mode,"on")&&strcmp(mode,"direct"))){
        fprintf(stderr,
            "[racer] MMZ direct disabled by default; set RACER_MMZ=on for explicit probe\n");
        return 0;
    }
    if(bytes>0xffffffffU)return 0;

    v->mmz_fd=open("/dev/mmz_userdev",O_RDWR|O_SYNC);
    if(v->mmz_fd<0)return 0;

    /* Classic Hi3531 kernels commonly use the 80-byte ABI. Probe it first,
       then the newer 96-byte OSAL layout. */
    ok0=video_mmz_alloc80(v,0);
    if(ok0==0){
        v->mmz_abi=80;
        ok1=video_mmz_alloc80(v,1);
    }else{
        ok0=video_mmz_alloc96(v,0);
        if(ok0==0){
            v->mmz_abi=96;
            ok1=video_mmz_alloc96(v,1);
        }
    }

    if(ok0!=0||ok1!=0){
        video_mmz_close(v);
        fprintf(stderr,"[racer] MMZ alloc/remap unavailable; heap canvas fallback\n");
        return 0;
    }

    if((uintptr_t)v->mmz_virt[0]>0xffffffffULL ||
       (uintptr_t)v->mmz_virt[1]>0xffffffffULL){
        video_mmz_close(v);
        fprintf(stderr,"[racer] MMZ mapping outside 32-bit userspace; heap fallback\n");
        return 0;
    }

    memset(v->mmz_virt[0],0,bytes);
    memset(v->mmz_virt[1],0,bytes);
    v->mmz_ready=1;
    v->mmz_direct=1;
    fprintf(stderr,
        "[racer] MMZ ready cached-double-buffer abi=%d bytes=%lu "
        "phys=0x%08x/0x%08x\n",
        v->mmz_abi,(unsigned long)bytes,
        v->mmz_phys[0],v->mmz_phys[1]);
    return 1;
}

static int video_mmz_flush(video_t *v,int idx)
{
    h3531_mmz_dirty_t d;
    if(!v->mmz_ready||v->mmz_fd<0||idx<0||idx>1)return -1;
    memset(&d,0,sizeof(d));
    d.dirty_phys_start=v->mmz_phys[idx];
    d.dirty_virt_start=(uint32_t)(uintptr_t)v->mmz_virt[idx];
    d.dirty_size=(uint32_t)((size_t)RW*RH*2U);
    if(ioctl(v->mmz_fd,H3531_MMZ_FLUSH_DIRTY,&d)==0)return 0;

    /* Older Hi3531 MMZ modules expose only the whole-cache command. */
    if(ioctl(v->mmz_fd,H3531_MMZ_FLUSH_ALL,0)==0)return 0;
    v->mmz_flush_failures++;
    return -1;
}

static void probe_tde_backend(const video_t *v)
{
    const char *mmzdevs[]={"/dev/mmz_userdev","/dev/mmz"};
    int i,mmz_found=0;
    const char *mmz_path="-";
    unsigned long fb_phys=v?(unsigned long)v->fix.smem_start:0UL;
    unsigned long fb_len=v?(unsigned long)v->fix.smem_len:0UL;

    for(i=0;i<(int)(sizeof(mmzdevs)/sizeof(mmzdevs[0]));++i){
        if(access(mmzdevs[i],F_OK)==0){mmz_found=1;mmz_path=mmzdevs[i];break;}
    }
    fprintf(stderr,
        "[racer] TDE probe device=%s mmz=%s(%s) fbphys=0x%08lx fbbytes=%lu "
        "source=%s srcphys=0x%08x backend=%s\n",
        (v&&v->tde_fd>=0)?"yes":(access("/dev/hi_tde",F_OK)==0?"present":"no"),
        mmz_found?"yes":"no",mmz_path,
        fb_phys,fb_len,
        (v&&v->mmz_ready&&v->mmz_direct)?"mmz-cached":
            ((v&&v->tde_ready)?"hifb-tail":"heap"),
        (v&&v->mmz_ready&&v->mmz_direct)?v->mmz_phys[0]:(v?v->tde_src_phys:0U),
        (v&&v->tde_ready)?
            ((v->mmz_ready&&v->mmz_direct)?"mmz-direct+tde-quickresize":"tde-quickresize"):
            "cpu-exact2x");
}

static int video_tde_init(video_t *v)
{
    const size_t src_bytes=(size_t)RW*RH*2U;
    const size_t visible_bytes=(size_t)v->stride*OH;
    const char *mode=getenv("RACER_TDE");
    size_t off;
    uint64_t phys;

    v->tde_fd=-1;
    v->tde_ready=0;
    v->tde_src_offset=0;
    v->tde_src_virt=NULL;
    v->tde_src_phys=0;

    if(mode&&(!strcmp(mode,"0")||!strcmp(mode,"off")||!strcmp(mode,"cpu"))){
        fprintf(stderr,"[racer] TDE disabled by RACER_TDE=%s\n",mode);
        return 0;
    }

    /*
     * Keep the unused HIFB tail as a guaranteed physical staging fallback.
     * When cached MMZ double buffers are available, TDE reads the rendered
     * canvas directly and this tail receives no per-frame CPU copy.
     */
    off=(visible_bytes+63U)&~(size_t)63U;
    if(off+src_bytes>v->len){
        fprintf(stderr,
            "[racer] TDE unavailable: HIFB tail too small need=%lu have=%lu\n",
            (unsigned long)(off+src_bytes),(unsigned long)v->len);
        return 0;
    }

    phys=(uint64_t)(unsigned long)v->fix.smem_start+(uint64_t)off;
    if((uint64_t)(unsigned long)v->fix.smem_start>0xffffffffULL ||
       phys>0xffffffffULL || phys+src_bytes>0x100000000ULL){
        fprintf(stderr,"[racer] TDE unavailable: framebuffer physical address is not 32-bit\n");
        return 0;
    }

    v->tde_fd=open("/dev/hi_tde",O_RDWR);
    if(v->tde_fd<0){
        fprintf(stderr,"[racer] TDE open /dev/hi_tde failed: %s; CPU fallback\n",
                strerror(errno));
        return 0;
    }

    v->tde_src_offset=off;
    v->tde_src_virt=v->mem+off;
    v->tde_src_phys=(uint32_t)phys;
    v->tde_ready=1;

    fprintf(stderr,
        "[racer] TDE ready direct-ioctl ARGB1555 source=%s "
        "src=0x%08x+%ux%u dst=0x%08lx+%ux%u\n",
        (v->mmz_ready&&v->mmz_direct)?"MMZ-cached-direct":"HIFB-tail",
        (v->mmz_ready&&v->mmz_direct)?v->mmz_phys[0]:v->tde_src_phys,RW,RH,
        (unsigned long)v->fix.smem_start,OW,OH);
    return 1;
}

static int video_present_tde(video_t *v,int idx)
{
    const size_t src_bytes=(size_t)RW*RH*2U;
    const uint16_t *src=v->canvas[idx];
    uint32_t src_phys=v->tde_src_phys;
    h3531_tde_resize_cmd_t cmd;
    h3531_tde_end_cmd_t end;
    int32_t handle=-1;
    uint64_t c0,c1,j0,j1;

    if(!v->tde_ready||v->tde_fd<0)return -1;

    c0=mono_ns();
    if(v->mmz_ready&&v->mmz_direct&&src==(const uint16_t*)v->mmz_virt[idx]){
        if(video_mmz_flush(v,idx)==0){
            src_phys=v->mmz_phys[idx];
        }else{
            fprintf(stderr,
                "[racer] MMZ cache flush failed errno=%d(%s); "
                "falling back to HIFB staging\n",errno,strerror(errno));
            v->mmz_direct=0;
            memcpy(v->tde_src_virt,src,src_bytes);
#if defined(__arm__)
            __asm__ volatile("dmb" ::: "memory");
#endif
        }
    }else{
        if(!v->tde_src_virt)return -1;
        memcpy(v->tde_src_virt,src,src_bytes);
#if defined(__arm__)
        __asm__ volatile("dmb" ::: "memory");
#endif
    }
    c1=mono_ns();

    j0=mono_ns();
    if(ioctl(v->tde_fd,H3531_TDE_IOC_BEGIN_JOB,&handle)<0||handle<0)
        goto fail;

    memset(&cmd,0,sizeof(cmd));
    cmd.handle=handle;

    cmd.src.phy_addr=src_phys;
    cmd.src.color_fmt=H3531_TDE_COLOR_FMT_ARGB1555;
    cmd.src.height=RH;
    cmd.src.width=RW;
    cmd.src.stride=RW*2U;
    cmd.src.alpha_max_255=1;
    cmd.src.alpha_ext_1555=1;
    cmd.src.alpha0=255;
    cmd.src.alpha1=255;
    cmd.src_rect.x=0;
    cmd.src_rect.y=0;
    cmd.src_rect.width=RW;
    cmd.src_rect.height=RH;

    cmd.dst.phy_addr=(uint32_t)(unsigned long)v->fix.smem_start;
    cmd.dst.color_fmt=H3531_TDE_COLOR_FMT_ARGB1555;
    cmd.dst.height=OH;
    cmd.dst.width=OW;
    cmd.dst.stride=v->stride;
    cmd.dst.alpha_max_255=1;
    cmd.dst.alpha_ext_1555=1;
    cmd.dst.alpha0=255;
    cmd.dst.alpha1=255;
    cmd.dst_rect.x=0;
    cmd.dst_rect.y=0;
    cmd.dst_rect.width=OW;
    cmd.dst_rect.height=OH;

    if(ioctl(v->tde_fd,H3531_TDE_IOC_RESIZE,&cmd)<0)
        goto fail;

    memset(&end,0,sizeof(end));
    end.handle=handle;
    end.sync=1;
    end.block=1;
    end.timeout_10ms=100;
    if(ioctl(v->tde_fd,H3531_TDE_IOC_END_JOB,&end)<0)
        goto fail;

#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
    j1=mono_ns();
    v->tde_copy_ns_total+=c1-c0; /* copy=0-ish in MMZ mode; includes cache flush */
    v->tde_job_ns_total+=j1-j0;
    if(j1-j0>v->tde_job_ns_max)v->tde_job_ns_max=j1-j0;
    v->tde_profile_count++;
    return 0;

fail:
    j1=mono_ns();
    fprintf(stderr,
        "[racer] TDE present failed handle=%d errno=%d(%s) job_ms=%.3f; "
        "disabling hardware present\n",
        (int)handle,errno,strerror(errno),(double)(j1-j0)/1000000.0);
    v->tde_failures++;
    v->tde_ready=0;
    return -1;
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


static void init_vc_color_chan_lut(void)
{
    int shade,fog,c;
    for(shade=0;shade<8;++shade){
        for(fog=0;fog<8;++fog){
            for(c=0;c<32;++c){
                uint16_t sr=g_shade_lut[shade][(unsigned)c<<10];
                uint16_t sg=g_shade_lut[shade][(unsigned)c<<5];
                uint16_t sb=g_shade_lut[shade][(unsigned)c];
                uint16_t fr=g_fog_lut[fog][sr&0x7fffU];
                uint16_t fg=g_fog_lut[fog][sg&0x7fffU];
                uint16_t fb=g_fog_lut[fog][sb&0x7fffU];
                g_vc_color_chan[shade][fog][0][c]=(uint8_t)((fr>>10)&31U);
                g_vc_color_chan[shade][fog][1][c]=(uint8_t)((fg>>5)&31U);
                g_vc_color_chan[shade][fog][2][c]=(uint8_t)(fb&31U);
            }
        }
    }
}

static int vc_fog_level_for_z(float z)
{
    float s=vc_runtime_world_scale();
    float start=s*VC_FOG_START_M;
    float end=s*VC_FAR_CLIP_M;
    float t;
    int level;
    if(z<=start)return 0;
    if(z>=end)return 7;
    t=(z-start)/(end-start);
    /* smoothstep keeps the nearby city clear and makes the last third
     * disappear progressively, closer to reVC's timecycle-driven fog feel. */
    t=t*t*(3.0f-2.0f*t);
    level=(int)(t*7.0f+0.5f);
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

    /*
     * Normalize winding once per triangle. The previous hot loop selected
     * >=0 versus <=0 for every candidate pixel even though area sign is
     * invariant for the entire triangle.
     */
    if(area<0){
        e0dx=-e0dx;e0dy=-e0dy;
        e1dx=-e1dx;e1dy=-e1dy;
        e2dx=-e2dx;e2dy=-e2dy;
        row0=-row0;row1=-row1;row2=-row2;
    }

    for(y=miny;y<=maxy;++y){
        int32_t w0=row0,w1=row1,w2=row2;
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
            int inside=(w0>=0&&w1>=0&&w2>=0);
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

static inline int vc_edge_limit_span(
    int32_t row,int32_t dx,int *lo,int *hi)
{
    if(dx>0){
        if(row<0){
            int need=-row;
            int k=(need+dx-1)/dx;
            if(k>*hi)return 0;
            if(k>*lo)*lo=k;
        }
    }else if(dx<0){
        int ndx=-dx;
        if(row<0)return 0;
        {
            int k=row/ndx;
            if(k<*lo)return 0;
            if(k<*hi)*hi=k;
        }
    }else if(row<0){
        return 0;
    }
    return *lo<=*hi;
}

static int vc_selftest_span_triangle(
    int x0,int y0,int x1,int y1,int x2,int y2)
{
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int32_t area,e0dx,e0dy,e1dx,e1dy,e2dx,e2dy,row0,row1,row2;

    if(x1<minx)minx=x1;if(x2<minx)minx=x2;
    if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
    if(y1<miny)miny=y1;if(y2<miny)miny=y2;
    if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
    area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
    if(area==0)return 1;

    e0dx=-(y1-y0);e0dy=(x1-x0);
    e1dx=-(y2-y1);e1dy=(x2-x1);
    e2dx=-(y0-y2);e2dy=(x0-x2);
    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);
    if(area<0){
        e0dx=-e0dx;e0dy=-e0dy;
        e1dx=-e1dx;e1dy=-e1dy;
        e2dx=-e2dx;e2dy=-e2dy;
        row0=-row0;row1=-row1;row2=-row2;
    }

    for(y=miny;y<=maxy;++y){
        int lo=0,hi=maxx-minx;
        int have=
            vc_edge_limit_span(row0,e0dx,&lo,&hi) &&
            vc_edge_limit_span(row1,e1dx,&lo,&hi) &&
            vc_edge_limit_span(row2,e2dx,&lo,&hi);
        for(x=minx;x<=maxx;++x){
            int k=x-minx;
            int32_t w0=row0+e0dx*k;
            int32_t w1=row1+e1dx*k;
            int32_t w2=row2+e2dx*k;
            int old_inside=(w0>=0&&w1>=0&&w2>=0);
            int span_inside=have&&k>=lo&&k<=hi;
            if(old_inside!=span_inside)return 0;
        }
        row0+=e0dy;row1+=e1dy;row2+=e2dy;
    }
    return 1;
}

static int vc_selftest_scanline_spans(void)
{
    return
        vc_selftest_span_triangle(10,10,60,18,25,70) &&
        vc_selftest_span_triangle(25,70,60,18,10,10) &&
        vc_selftest_span_triangle(5,5,300,6,9,40) &&
        vc_selftest_span_triangle(320,40,25,41,310,220);
}

static void fill_tri_vc_textured_z_range(
    const vc_textri_t *t,int clip_y0,int clip_y1,vc_raster_stats_t *stats)
{
    const float DEPTH_SCALE=2949075.0f; /* 45 * 65535 */
    const float DEPTH_FX_SCALE=2949075.0f*256.0f;
    int x0=t->x0,y0=t->y0,x1=t->x1,y1=t->y1,x2=t->x2,y2=t->y2;
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int32_t area;
    int32_t e0dx,e0dy,e1dx,e1dy,e2dx,e2dy,row0,row1,row2;
    float inv_area,zavg;
    float q0,q1,q2,uq0,uq1,uq2,vq0,vq1,vq2;
    float dq_dx,dq_dy,duq_dx,duq_dy,dvq_dx,dvq_dy;
    float du_dx=0.0f,du_dy=0.0f,dv_dx=0.0f,dv_dy=0.0f;
    float row_q,row_uq,row_vq;
    int32_t depth_dx_fx,depth_dy_fx,row_depth_fx;
    int32_t row_aff_u_fx=0,row_aff_v_fx=0,aff_du_fx=0,aff_dv_fx=0;
    const vc_runtime_map_t *map=vc_map_for_page_slot(t->page_slot);
    const int affine=(g_vc_debug_affine || (t->pad&1U));
    const int fog_flat=((t->pad&2U)!=0U);
    const vc_material_t *mat;
    const uint16_t *atlas_base=NULL;
    unsigned atlas_stride=0U,tex_w=0U,tex_h=0U;
    int textured=0;
    int level=shade_level(t->light);
    int tri_fog,corr_block=8;
    uint16_t solid_color=0;
    const uint8_t (*vc_chan)[32]=NULL;

#define VC_RSTAT_INC(field) do { \
    if(stats)(stats)->field++; else g_vc_prof.field++; \
} while(0)

#define VC_RSTAT_ADD(field,value) do { \
    uint64_t vc_rstat_v=(uint64_t)(value); \
    if(stats)(stats)->field+=vc_rstat_v; else g_vc_prof.field+=vc_rstat_v; \
} while(0)

    if(!map||t->material>=map->material_count)return;
    mat=&map->materials[t->material];
    if(t->z0<=0.0f||t->z1<=0.0f||t->z2<=0.0f)return;

    textured=((mat->flags&1U) && mat->w>0 && mat->h>0 && map->atlas &&
              !(fog_flat && !(mat->flags&2U)));
    if(textured){
        tex_w=(unsigned)mat->w;
        tex_h=(unsigned)mat->h;
        if(map->compact_textures && map->tex_offsets &&
           map->tex_offsets[t->material]!=0xffffffffU){
            atlas_stride=tex_w;
            atlas_base=map->atlas+map->tex_offsets[t->material];
        }else{
            atlas_stride=map->atlas_w;
            atlas_base=map->atlas+
                (size_t)mat->y*atlas_stride+(size_t)mat->x;
        }
    }

    zavg=(t->z0+t->z1+t->z2)*(1.0f/3.0f);
    tri_fog=vc_fog_level_for_z(zavg);
    vc_chan=g_vc_color_chan[level][tri_fog];
    if(!textured){
        solid_color=shade1555(mat->fallback,t->light);
        if(tri_fog>0)solid_color=g_fog_lut[tri_fog][solid_color&0x7fffU];
    }

    /*
     * Adaptive perspective correction: nearby geometry retains the original
     * 8-pixel correction cadence, while distant geometry pays far fewer ARM
     * divisions. At 640x360 the visual difference is sub-pixel in the far
     * field.
     */
    {
        float unit=map->world_scale>1.0f?map->world_scale:240.0f;
        if(zavg>unit*55.0f)corr_block=32;
        else if(zavg>unit*30.0f)corr_block=20;
        else if(zavg>unit*16.0f)corr_block=12;
    }

    if(x1<minx)minx=x1;if(x2<minx)minx=x2;
    if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
    if(y1<miny)miny=y1;if(y2<miny)miny=y2;
    if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
    if(maxx<0||minx>=RW||maxy<clip_y0||miny>=clip_y1)return;
    if(minx<0)minx=0;if(maxx>=RW)maxx=RW-1;
    if(miny<clip_y0)miny=clip_y0;if(maxy>=clip_y1)maxy=clip_y1-1;
    if(miny>maxy)return;

    /*
     * queue_vc_mesh_textured() fully clips to the 640x360 camera frustum
     * before emitting vc_textri_t, so screen edge products are safely 32-bit.
     * Keeping int64 here made every covered pixel expensive on Cortex-A9.
     */
    area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
    if(area==0)return;
    if((area>-48&&area<48) && corr_block<16)corr_block=16;
    inv_area=1.0f/(float)area;

    q0=1.0f/t->z0;q1=1.0f/t->z1;q2=1.0f/t->z2;
    uq0=t->u0*q0;uq1=t->u1*q1;uq2=t->u2*q2;
    vq0=t->v0*q0;vq1=t->v1*q1;vq2=t->v2*q2;

#define VC_ATTR_GRAD(a0,a1,a2,dx,dy) do { \
    (dx)=(((a1)-(a0))*(float)(y2-y0)-((a2)-(a0))*(float)(y1-y0))*inv_area; \
    (dy)=(((a2)-(a0))*(float)(x1-x0)-((a1)-(a0))*(float)(x2-x0))*inv_area; \
} while(0)
    VC_ATTR_GRAD(q0,q1,q2,dq_dx,dq_dy);
    VC_ATTR_GRAD(uq0,uq1,uq2,duq_dx,duq_dy);
    VC_ATTR_GRAD(vq0,vq1,vq2,dvq_dx,dvq_dy);
    if(affine && textured){
        VC_ATTR_GRAD(t->u0,t->u1,t->u2,du_dx,du_dy);
        VC_ATTR_GRAD(t->v0,t->v1,t->v2,dv_dx,dv_dy);
    }
#undef VC_ATTR_GRAD

    row_q=q0+dq_dx*((float)minx-x0)+dq_dy*((float)miny-y0);
    row_uq=uq0+duq_dx*((float)minx-x0)+duq_dy*((float)miny-y0);
    row_vq=vq0+dvq_dx*((float)minx-x0)+dvq_dy*((float)miny-y0);

    /*
     * q=1/z is linear in screen space. Store q*DEPTH_SCALE directly as 24.8
     * fixed point so the hot loop no longer performs three float additions per
     * pixel. Perspective q/u/z/v/z are reconstructed only at correction
     * boundaries (8..32 pixels), while affine/fog-flat paths stay integer-only.
     */
    depth_dx_fx=(int32_t)(dq_dx*DEPTH_FX_SCALE);
    depth_dy_fx=(int32_t)(dq_dy*DEPTH_FX_SCALE);
    row_depth_fx=(int32_t)(row_q*DEPTH_FX_SCALE);

    if(affine && textured){
        float au=t->u0+du_dx*((float)minx-x0)+du_dy*((float)miny-y0);
        float av=t->v0+dv_dx*((float)minx-x0)+dv_dy*((float)miny-y0);
        row_aff_u_fx=(int32_t)(au*65536.0f);
        row_aff_v_fx=(int32_t)(av*65536.0f);
        aff_du_fx=(int32_t)(du_dx*65536.0f);
        aff_dv_fx=(int32_t)(dv_dx*65536.0f);
    }

    e0dx=-(y1-y0); e0dy=(x1-x0);
    e1dx=-(y2-y1); e1dy=(x2-x1);
    e2dx=-(y0-y2); e2dy=(x0-x2);
    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    /*
     * Convert either winding to positive half-space once. Then each scanline
     * intersects three linear edge inequalities into one exact inclusive span.
     * The old bounding-box loop tested every candidate pixel, including large
     * empty regions around thin/diagonal GTA triangles.
     */
    if(area<0){
        e0dx=-e0dx;e0dy=-e0dy;
        e1dx=-e1dx;e1dy=-e1dy;
        e2dx=-e2dx;e2dy=-e2dy;
        row0=-row0;row1=-row1;row2=-row2;
    }

    for(y=miny;y<=maxy;++y){
        int lo=0,hi=maxx-minx;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        uint16_t *zrow=g_city_zbuf+(size_t)y*RW;
        int corr_left=0;
        int32_t u_fx=0,v_fx=0,du_fx=0,dv_fx=0;

        VC_RSTAT_ADD(bbox_pixels,(uint64_t)(maxx-minx+1));

        if(vc_edge_limit_span(row0,e0dx,&lo,&hi) &&
           vc_edge_limit_span(row1,e1dx,&lo,&hi) &&
           vc_edge_limit_span(row2,e2dx,&lo,&hi)){
            int span_minx=minx+lo;
            int span_maxx=minx+hi;
            int32_t dfx=row_depth_fx+depth_dx_fx*lo;
            int32_t aff_u_fx=row_aff_u_fx+aff_du_fx*lo;
            int32_t aff_v_fx=row_aff_v_fx+aff_dv_fx*lo;

            VC_RSTAT_ADD(span_pixels,(uint64_t)(span_maxx-span_minx+1));

            for(x=span_minx;x<=span_maxx;++x){
                int di=dfx>>8;
                int zpass;
                if(di<1)di=1;if(di>65535)di=65535;
                zpass=((uint16_t)di>zrow[x]);

                if(zpass){
                    uint16_t out_color=0;
                    int opaque=1;
                    VC_RSTAT_INC(zpass_pixels);

                    if(textured){
                        unsigned fu,fv,tx,ty;
                        uint16_t tex;
                        VC_RSTAT_INC(texture_samples);

                        if(affine){
                            fu=(unsigned)aff_u_fx&0xffffU;
                            fv=(unsigned)aff_v_fx&0xffffU;
                        }else{
                            if(corr_left<=0){
                                float xo=(float)(x-minx);
                                float step=(float)corr_block;
                                float q=row_q+dq_dx*xo;
                                float uq=row_uq+duq_dx*xo;
                                float vq=row_vq+dvq_dx*xo;
                                float qn=q+dq_dx*step;
                                float invq=(fabsf(q)>1.0e-12f)?(1.0f/q):0.0f;
                                float invqn=(fabsf(qn)>1.0e-12f)?(1.0f/qn):invq;
                                float u0f=uq*invq;
                                float v0f=vq*invq;
                                float u1f=(uq+duq_dx*step)*invqn;
                                float v1f=(vq+dvq_dx*step)*invqn;
                                u_fx=(int32_t)(u0f*65536.0f);
                                v_fx=(int32_t)(v0f*65536.0f);
                                du_fx=(int32_t)((u1f-u0f)*(65536.0f/step));
                                dv_fx=(int32_t)((v1f-v0f)*(65536.0f/step));
                                corr_left=corr_block;
                                VC_RSTAT_INC(correction_segments);
                            }
                            fu=(unsigned)u_fx&0xffffU;
                            fv=(unsigned)v_fx&0xffffU;
                        }

                        tx=(fu*tex_w)>>16;
                        ty=(fv*tex_h)>>16;
                        if(tx>=tex_w)tx=tex_w-1;
                        if(ty>=tex_h)ty=tex_h-1;
                        tex=atlas_base[(size_t)ty*atlas_stride+tx];

                        if(tex&0x8000U){
                            unsigned rgb=(unsigned)(tex&0x7fffU);
                            out_color=(uint16_t)(
                                0x8000U |
                                ((uint16_t)vc_chan[0][(rgb>>10)&31U]<<10) |
                                ((uint16_t)vc_chan[1][(rgb>>5)&31U]<<5) |
                                (uint16_t)vc_chan[2][rgb&31U]);
                        }else opaque=0;
                    }else{
                        out_color=solid_color;
                    }

                    if(opaque){
                        if(t->fade<255U){
                            unsigned a=(unsigned)t->fade,ia=255U-a;
                            unsigned src=(unsigned)out_color&0x7fffU;
                            unsigned bg=(unsigned)dst[x]&0x7fffU;
                            unsigned sr=(src>>10)&31U,sg=(src>>5)&31U,sb=src&31U;
                            unsigned br=(bg>>10)&31U,bg5=(bg>>5)&31U,bb=bg&31U;
                            unsigned rr=(sr*a+br*ia+127U)/255U;
                            unsigned rg=(sg*a+bg5*ia+127U)/255U;
                            unsigned rb=(sb*a+bb*ia+127U)/255U;
                            out_color=(uint16_t)(0x8000U|(rr<<10)|(rg<<5)|rb);
                        }
                        zrow[x]=(uint16_t)di;
                        dst[x]=out_color;
                    }
                }

                if(textured && !affine && corr_left>0){
                    u_fx+=du_fx;
                    v_fx+=dv_fx;
                    corr_left--;
                }

                dfx+=depth_dx_fx;
                if(affine && textured){
                    aff_u_fx+=aff_du_fx;
                    aff_v_fx+=aff_dv_fx;
                }
            }
        }
        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_depth_fx+=depth_dy_fx;
        if(textured && !affine){
            row_q+=dq_dy;
            row_uq+=duq_dy;
            row_vq+=dvq_dy;
        }
        if(affine && textured){
            row_aff_u_fx+=(int32_t)(du_dy*65536.0f);
            row_aff_v_fx+=(int32_t)(dv_dy*65536.0f);
        }
    }
#undef VC_RSTAT_ADD
#undef VC_RSTAT_INC
}

static void fill_tri_vc_textured_z(const vc_textri_t *t)
{
    fill_tri_vc_textured_z_range(t,0,RH,NULL);
}

static void *vc_raster_worker_main(void *arg)
{
    vc_raster_worker_t *w=(vc_raster_worker_t*)arg;
    pin_thread(1,"vc-raster");
    for(;;){
        int n,k,split_y;
        uint64_t r0,r1;
        pthread_mutex_lock(&w->lock);
        while(w->pending!=1&&!w->stop)
            pthread_cond_wait(&w->start_cv,&w->lock);
        if(w->stop){
            pthread_mutex_unlock(&w->lock);
            break;
        }
        n=w->tri_count;
        split_y=w->split_y;
        w->pending=2; /* running */
        memset(&w->stats,0,sizeof(w->stats));
        pthread_mutex_unlock(&w->lock);

        r0=mono_ns();
        for(k=0;k<n;++k)
            fill_tri_vc_textured_z_range(
                &g_vc_tex_out[g_vc_order[k]],split_y,RH,&w->stats);
        r1=mono_ns();

        pthread_mutex_lock(&w->lock);
        w->raster_ns=r1-r0;
        w->pending=-1; /* completed, awaiting collector */
        pthread_cond_broadcast(&w->done_cv);
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}

static int vc_raster_worker_start(void)
{
    vc_raster_worker_t *w=&g_vc_raster_worker;
    const char *mode=getenv("RACER_DUALRASTER");
    if(mode&&(!strcmp(mode,"0")||!strcmp(mode,"off")||!strcmp(mode,"single"))){
        fprintf(stderr,"[racer] dual raster disabled by RACER_DUALRASTER=%s\n",mode);
        return 0;
    }
    memset(w,0,sizeof(*w));
    pthread_mutex_init(&w->lock,NULL);
    pthread_cond_init(&w->start_cv,NULL);
    pthread_cond_init(&w->done_cv,NULL);
    if(pthread_create(&w->thread,NULL,vc_raster_worker_main,w)!=0){
        pthread_cond_destroy(&w->start_cv);
        pthread_cond_destroy(&w->done_cv);
        pthread_mutex_destroy(&w->lock);
        memset(w,0,sizeof(*w));
        return 0;
    }
    w->ready=1;
    g_vc_raster_split_y=RH/2;
    g_vc_raster_top_ema_ns=0;
    g_vc_raster_bottom_ema_ns=0;
    fprintf(stderr,
        "[racer] dual-core city raster active adaptive-split start=%d range=112..280 state-machine=v4\n",
        g_vc_raster_split_y);
    return 1;
}

static void vc_raster_worker_submit(int n,int split_y)
{
    vc_raster_worker_t *w=&g_vc_raster_worker;
    if(!w->ready)return;
    if(split_y<1)split_y=1;
    if(split_y>=RH)split_y=RH-1;
    pthread_mutex_lock(&w->lock);
    while(w->pending!=0)
        pthread_cond_wait(&w->done_cv,&w->lock);
    w->tri_count=n;
    w->split_y=split_y;
    w->raster_ns=0;
    w->pending=1;
    pthread_cond_signal(&w->start_cv);
    pthread_mutex_unlock(&w->lock);
}

static vc_raster_stats_t vc_raster_worker_collect(uint64_t *raster_ns)
{
    vc_raster_worker_t *w=&g_vc_raster_worker;
    vc_raster_stats_t out={0};
    if(raster_ns)*raster_ns=0;
    if(!w->ready)return out;
    pthread_mutex_lock(&w->lock);
    while(w->pending!=-1)
        pthread_cond_wait(&w->done_cv,&w->lock);
    out=w->stats;
    if(raster_ns)*raster_ns=w->raster_ns;
    w->pending=0;
    pthread_cond_broadcast(&w->done_cv);
    pthread_mutex_unlock(&w->lock);
    return out;
}

static void vc_raster_rebalance(uint64_t top_ns,uint64_t bottom_ns)
{
    uint64_t top,bot,hi,lo;
    int step;

    if(!top_ns||!bottom_ns)return;
    if(!g_vc_raster_top_ema_ns){
        g_vc_raster_top_ema_ns=top_ns;
        g_vc_raster_bottom_ema_ns=bottom_ns;
    }else{
        /*
         * Track scene changes faster than v3. At ~10 fps, waiting eight frames
         * per four scanlines took seconds to recover from a camera turn.
         */
        g_vc_raster_top_ema_ns=(g_vc_raster_top_ema_ns*3ULL+top_ns)/4ULL;
        g_vc_raster_bottom_ema_ns=(g_vc_raster_bottom_ema_ns*3ULL+bottom_ns)/4ULL;
    }

    if((g_frame&1U)!=0U)return;
    top=g_vc_raster_top_ema_ns;
    bot=g_vc_raster_bottom_ema_ns;
    hi=top>bot?top:bot;
    lo=top>bot?bot:top;
    if(lo==0ULL)return;

    if(hi*100ULL>lo*180ULL)step=16;
    else if(hi*100ULL>lo*135ULL)step=8;
    else if(hi*100ULL>lo*110ULL)step=4;
    else return;

    if(bot>top){
        g_vc_raster_split_y+=step; /* CPU0 takes more rows */
        if(g_vc_raster_split_y>280)g_vc_raster_split_y=280;
    }else{
        g_vc_raster_split_y-=step; /* CPU1 takes more rows */
        if(g_vc_raster_split_y<112)g_vc_raster_split_y=112;
    }
}

static void vc_raster_worker_stop(void)
{
    vc_raster_worker_t *w=&g_vc_raster_worker;
    if(!w->ready)return;
    pthread_mutex_lock(&w->lock);
    while(w->pending==1||w->pending==2)
        pthread_cond_wait(&w->done_cv,&w->lock);
    w->stop=1;
    pthread_cond_signal(&w->start_cv);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->start_cv);
    pthread_cond_destroy(&w->done_cv);
    pthread_mutex_destroy(&w->lock);
    memset(w,0,sizeof(*w));
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


static void fill_tri_textured_z(
    int x0,int y0,float u0,float v0,float z0,
    int x1,int y1,float u1,float v1,float z1,
    int x2,int y2,float u2,float v2,float z2,
    float light,const uint16_t *texture,int tex_w,int tex_h)
{
    const float DEPTH_SCALE=2949075.0f; /* same 45*65535 convention as VC city */
    int minx=x0,maxx=x0,miny=y0,maxy=y0,x,y;
    int area;
    int e0dx,e0dy,e1dx,e1dy,e2dx,e2dy;
    int row0,row1,row2;
    float inv_area,du_dx,du_dy,dv_dx,dv_dy;
    float d0,d1,d2,ddx,ddy,rowd;
    int32_t du_dx_fx,du_dy_fx,dv_dx_fx,dv_dy_fx;
    int32_t row_u_fx,row_v_fx,ddx_fx,ddy_fx,row_d_fx;
    int level=shade_level(light);

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

    du_dx=((u1-u0)*(float)(y2-y0)-(u2-u0)*(float)(y1-y0))*inv_area;
    du_dy=((u2-u0)*(float)(x1-x0)-(u1-u0)*(float)(x2-x0))*inv_area;
    dv_dx=((v1-v0)*(float)(y2-y0)-(v2-v0)*(float)(y1-y0))*inv_area;
    dv_dy=((v2-v0)*(float)(x1-x0)-(v1-v0)*(float)(x2-x0))*inv_area;

    d0=DEPTH_SCALE/z0;d1=DEPTH_SCALE/z1;d2=DEPTH_SCALE/z2;
    if(d0>65535.0f)d0=65535.0f;
    if(d1>65535.0f)d1=65535.0f;
    if(d2>65535.0f)d2=65535.0f;
    ddx=((d1-d0)*(float)(y2-y0)-(d2-d0)*(float)(y1-y0))*inv_area;
    ddy=((d2-d0)*(float)(x1-x0)-(d1-d0)*(float)(x2-x0))*inv_area;
    rowd=d0+ddx*((float)minx-x0)+ddy*((float)miny-y0);

    du_dx_fx=(int32_t)(du_dx*65536.0f);
    du_dy_fx=(int32_t)(du_dy*65536.0f);
    dv_dx_fx=(int32_t)(dv_dx*65536.0f);
    dv_dy_fx=(int32_t)(dv_dy*65536.0f);
    row_u_fx=(int32_t)((u0+du_dx*((float)minx-x0)+du_dy*((float)miny-y0))*65536.0f);
    row_v_fx=(int32_t)((v0+dv_dx*((float)minx-x0)+dv_dy*((float)miny-y0))*65536.0f);
    ddx_fx=(int32_t)(ddx*256.0f);
    ddy_fx=(int32_t)(ddy*256.0f);
    row_d_fx=(int32_t)(rowd*256.0f);

    e0dx=-(y1-y0);e0dy=(x1-x0);
    e1dx=-(y2-y1);e1dy=(x2-x1);
    e2dx=-(y0-y2);e2dy=(x0-x2);
    row0=(x1-x0)*(miny-y0)-(y1-y0)*(minx-x0);
    row1=(x2-x1)*(miny-y1)-(y2-y1)*(minx-x1);
    row2=(x0-x2)*(miny-y2)-(y0-y2)*(minx-x2);

    for(y=miny;y<=maxy;++y){
        int w0=row0,w1=row1,w2=row2;
        int32_t uu=row_u_fx,vv=row_v_fx,dfx=row_d_fx;
        uint16_t *dst=g_canvas+(size_t)y*RW;
        uint16_t *zrow=g_city_zbuf+(size_t)y*RW;
        for(x=minx;x<=maxx;++x){
            int inside=(area>0)?(w0>=0&&w1>=0&&w2>=0):(w0<=0&&w1<=0&&w2<=0);
            if(inside){
                int di=dfx>>8;
                if(di<1)di=1;if(di>65535)di=65535;
                if((uint16_t)di>zrow[x]){
                    int tx=(uu+32768)>>16,ty=(vv+32768)>>16;
                    uint16_t tex;
                    if(tx<0)tx=0;if(tx>=tex_w)tx=tex_w-1;
                    if(ty<0)ty=0;if(ty>=tex_h)ty=tex_h-1;
                    tex=texture[ty*tex_w+tx];
                    /* Alpha-test before writing depth, matching VC material holes. */
                    if(tex&0x8000U){
                        zrow[x]=(uint16_t)di;
                        dst[x]=g_shade_lut[level][tex&0x7fffU];
                        g_vcveh_zpass_pixels++;
                    }
                }else{
                    g_vcveh_zblocked_pixels++;
                }
            }
            w0+=e0dx;w1+=e1dx;w2+=e2dx;
            uu+=du_dx_fx;vv+=dv_dx_fx;dfx+=ddx_fx;
        }
        row0+=e0dy;row1+=e1dy;row2+=e2dy;
        row_u_fx+=du_dy_fx;row_v_fx+=dv_dy_fx;row_d_fx+=ddy_fx;
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

/* VC player vehicle writes the shared city Z buffer, so front-to-back ordering
 * is cheaper than the painter order used by legacy non-Z vehicle paths. */
static int cmp_textri_near_first(const void *aa,const void *bb)
{
    const textri_t *a=(const textri_t*)aa,*b=(const textri_t*)bb;
    if(a->depth<b->depth)return -1;
    if(a->depth>b->depth)return 1;
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

static void vc_body_basis_from_euler(void)
{
    /*
     * Racer heading convention is world-forward=(+sin(h),0,+cos(h)).
     * make_rotxyz's positive Y rotation maps local +Z to (-sin(y),0,+cos(y)),
     * so the physical body matrix must use -heading.  The old +heading path
     * mirrored tyre/suspension axes after every non-zero turn.
     */
    rotxyz_t r=make_rotxyz(g_body_pitch,-g_vehicle_heading,g_body_roll);
    rotate_xyz_precomputed((v3f_t){1.0f,0.0f,0.0f},&r,&g_vc_body_right);
    rotate_xyz_precomputed((v3f_t){0.0f,1.0f,0.0f},&r,&g_vc_body_up);
    rotate_xyz_precomputed((v3f_t){0.0f,0.0f,1.0f},&r,&g_vc_body_forward);
    g_vc_body_basis_valid=1;
}

static void vc_body_rotate_local(v3f_t in,v3f_t *out)
{
    if(!g_vc_body_basis_valid)vc_body_basis_from_euler();
    out->x=
        g_vc_body_right.x*in.x+
        g_vc_body_up.x*in.y+
        g_vc_body_forward.x*in.z;
    out->y=
        g_vc_body_right.y*in.x+
        g_vc_body_up.y*in.y+
        g_vc_body_forward.y*in.z;
    out->z=
        g_vc_body_right.z*in.x+
        g_vc_body_up.z*in.y+
        g_vc_body_forward.z*in.z;
}

static void vc_vehicle_local_to_world(
    v3f_t local,float carx,float cary,float carz,v3f_t *world)
{
    v3f_t q;
    vc_body_rotate_local(local,&q);
    world->x=carx+q.x;
    world->y=cary+q.y;
    world->z=carz+q.z;
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


static float vcveh_wrap01(float v)
{
    v-=floorf(v);
    if(v<0.0f)v+=1.0f;
    return v;
}

static void render_vc_vehicle(
    float carx,float cary,float carz,
    float scale,
    float camx,float camy,float camz,float camyaw)
{
    v3f_t *rv=g_vcveh_rv;
    sv3_t *sv=g_vcveh_sv;
    textri_t *out=g_vcveh_out;
    float cam_cs=cosf(camyaw),cam_sn=sinf(camyaw);
    float cam_cp=cosf(g_camera_pitch),cam_sp=sinf(g_camera_pitch);
    rotxyz_t wheel_rot[5];
    v3f_t wheel_pivot[5];
    uint32_t i;
    int n=0;
    unsigned tiny_reject=0,screen_reject=0;

    g_vcveh_zpass_pixels=0;
    g_vcveh_zblocked_pixels=0;

    if(!g_vc_vehicle.loaded || !g_vc_vehicle.verts || !g_vc_vehicle.tris ||
       !g_vc_vehicle.materials || !g_vc_vehicle.atlas ||
       !rv || !sv || !out ||
       g_vc_vehicle.vertex_count>g_vcveh_vertex_cap ||
       g_vc_vehicle.tri_count>g_vcveh_tri_cap)
        return;

    /*
     * Camera pitch and wheel articulation are constant for the whole rendered
     * frame.  Stage8.9 used to rebuild their sin/cos state for every VCVEH
     * vertex, which meant thousands of libm calls per frame on Cortex-A9.
     * Precompute the four wheel rotations and scaled pivots once here.
     */
    memset(wheel_rot,0,sizeof(wheel_rot));
    memset(wheel_pivot,0,sizeof(wheel_pivot));
    wheel_rot[1]=make_rotxyz(-g_wheel_spin,-g_steer_fl,0.0f);
    wheel_rot[2]=make_rotxyz( g_wheel_spin,-g_steer_fr,0.0f);
    wheel_rot[3]=make_rotxyz(-g_wheel_spin,0.0f,0.0f);
    wheel_rot[4]=make_rotxyz( g_wheel_spin,0.0f,0.0f);
    for(i=1;i<=4U;++i){
        wheel_pivot[i]=g_vc_vehicle.wheel_pivot[i];
        wheel_pivot[i].x*=scale;
        wheel_pivot[i].y*=scale;
        wheel_pivot[i].z*=scale;
    }

    for(i=0;i<g_vc_vehicle.vertex_count;++i){
        v3f_t p,q;
        unsigned part=g_vc_vehicle.vertex_part?g_vc_vehicle.vertex_part[i]:0U;
        p.x=g_vc_vehicle.verts[i].x*scale;
        p.y=g_vc_vehicle.verts[i].y*scale;
        p.z=g_vc_vehicle.verts[i].z*scale;

        if(part>=1U && part<=4U && g_vc_vehicle.wheel_present[part]){
            const v3f_t pivot=wheel_pivot[part];
            v3f_t local,turned;
            local.x=p.x-pivot.x;local.y=p.y-pivot.y;local.z=p.z-pivot.z;
            rotate_xyz_precomputed(local,&wheel_rot[part],&turned);
            p.x=pivot.x+turned.x;p.y=pivot.y+turned.y;p.z=pivot.z+turned.z;
        }

        /*
         * Vehicle orientation is physical WORLD state. Never combine body
         * pitch/roll with camera-relative yaw: doing so made roll appear to
         * reverse when the chase camera moved to the opposite side.
         */
        {
            v3f_t world;
            vc_vehicle_local_to_world(p,carx,cary,carz,&world);
            q.x=world.x-carx;q.y=world.y-cary;q.z=world.z-carz;
            rv[i]=q;
            {
            float wx=world.x,wy=world.y,wz=world.z;
            float dx=wx-camx,dy=wy-camy,dz=wz-camz;
            float cx=dx*cam_cs-dz*cam_sn;
            float hz=dx*cam_sn+dz*cam_cs;
            float cy=dy*cam_cp-hz*cam_sp;
            float cz=dy*cam_sp+hz*cam_cp;
            if(cz<45.0f){
                sv[i].valid=0;
            }else{
                float ps=VC_FOCAL/cz;
                sv[i].sx=RW*0.5f+cx*ps;
                sv[i].sy=VC_SCREEN_Y-cy*ps;
                sv[i].z=cz;
                sv[i].valid=1;
            }
            }
        }
    }

    for(i=0;i<g_vc_vehicle.tri_count && (uint32_t)n<g_vcveh_tri_cap;++i){
        const vc_tri_t *t=&g_vc_vehicle.tris[i];
        const vc_material_t *m;
        v3f_t a,b,d;
        float ux,uy,uz,vx,vy,vz,nx,ny,nz,mag,light=0.76f;
        float uu[3],vv[3];
        uint16_t ids[3];

        if(t->a>=g_vc_vehicle.vertex_count ||
           t->b>=g_vc_vehicle.vertex_count ||
           t->c>=g_vc_vehicle.vertex_count ||
           t->material>=g_vc_vehicle.material_count)
            continue;
        if(!sv[t->a].valid||!sv[t->b].valid||!sv[t->c].valid)
            continue;
        {
            int x0=(int)sv[t->a].sx,y0=(int)sv[t->a].sy;
            int x1=(int)sv[t->b].sx,y1=(int)sv[t->b].sy;
            int x2=(int)sv[t->c].sx,y2=(int)sv[t->c].sy;
            int minx=x0,maxx=x0,miny=y0,maxy=y0;
            int area2;
            if(x1<minx)minx=x1;if(x2<minx)minx=x2;
            if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
            if(y1<miny)miny=y1;if(y2<miny)miny=y2;
            if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
            if(maxx<0||minx>=RW||maxy<0||miny>=RH){
                screen_reject++;
                continue;
            }
            area2=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
            /*
             * A 40k-triangle replacement car contains thousands of sub-pixel
             * faces at 640x360. They cost raster time but cannot contribute a
             * stable visible pixel. Keep >= ~1 pixel projected area.
             */
            if(area2>-2 && area2<2){
                tiny_reject++;
                continue;
            }
        }

        m=&g_vc_vehicle.materials[t->material];
        if(!(m->flags&1U) || m->w==0 || m->h==0)
            continue;

        a=rv[t->a];b=rv[t->b];d=rv[t->c];
        ux=b.x-a.x;uy=b.y-a.y;uz=b.z-a.z;
        vx=d.x-a.x;vy=d.y-a.y;vz=d.z-a.z;
        nx=uy*vz-uz*vy;ny=uz*vx-ux*vz;nz=ux*vy-uy*vx;
        mag=sqrtf(nx*nx+ny*ny+nz*nz);
        if(mag>0.001f){
            nx/=mag;ny/=mag;nz/=mag;
            light=0.50f+0.46f*fabsf(nx*0.24f+ny*0.84f+nz*(-0.42f));
        }

        ids[0]=t->a;ids[1]=t->b;ids[2]=t->c;
        {
            int k;
            for(k=0;k<3;++k){
                const vc_vertex_t *v=&g_vc_vehicle.verts[ids[k]];
                float fu=vcveh_wrap01(v->u);
                float fv=vcveh_wrap01(v->v);
                uu[k]=(float)m->x+fu*(float)(m->w>1?m->w-1:0);
                vv[k]=(float)m->y+fv*(float)(m->h>1?m->h-1:0);
            }
        }

        out[n].depth=(sv[t->a].z+sv[t->b].z+sv[t->c].z)*(1.0f/3.0f);
        out[n].z0=sv[t->a].z;out[n].z1=sv[t->b].z;out[n].z2=sv[t->c].z;
        out[n].x0=(int)sv[t->a].sx;out[n].y0=(int)sv[t->a].sy;
        out[n].x1=(int)sv[t->b].sx;out[n].y1=(int)sv[t->b].sy;
        out[n].x2=(int)sv[t->c].sx;out[n].y2=(int)sv[t->c].sy;
        out[n].u0=uu[0];out[n].v0=vv[0];
        out[n].u1=uu[1];out[n].v1=vv[1];
        out[n].u2=uu[2];out[n].v2=vv[2];
        out[n].light=light;
        n++;
    }

    g_vcveh_last_draw_tris=(unsigned)n;
    g_vcveh_last_tiny_reject=tiny_reject;
    g_vcveh_last_screen_reject=screen_reject;
    /* Alpha-tested VC materials write depth only for opaque texels.
     * Near-first therefore preserves holes while letting the Z buffer reject
     * hidden rear/body pixels before texture work. */
    qsort(out,(size_t)n,sizeof(out[0]),cmp_textri_near_first);
    for(i=0;i<(uint32_t)n;++i)
        fill_tri_textured_z(
            out[i].x0,out[i].y0,out[i].u0,out[i].v0,out[i].z0,
            out[i].x1,out[i].y1,out[i].u1,out[i].v1,out[i].z1,
            out[i].x2,out[i].y2,out[i].u2,out[i].v2,out[i].z2,
            out[i].light,g_vc_vehicle.atlas,
            (int)g_vc_vehicle.atlas_w,(int)g_vc_vehicle.atlas_h);
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
    v->fd=-1;v->tde_fd=-1;v->mmz_fd=-1;v->pending=-1;

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

    if(video_mmz_init(v)>0){
        v->canvas[0]=(uint16_t*)v->mmz_virt[0];
        v->canvas[1]=(uint16_t*)v->mmz_virt[1];
        v->canvas_heap=0;
    }else{
        v->canvas[0]=(uint16_t*)malloc((size_t)RW*RH*2U);
        v->canvas[1]=(uint16_t*)malloc((size_t)RW*RH*2U);
        v->canvas_heap=1;
    }
    v->base=(uint16_t*)malloc((size_t)RW*RH*2U);
    v->row2x=(uint16_t*)malloc((size_t)OW*2U);
    if(!v->canvas[0]||!v->canvas[1]||!v->base||!v->row2x)return -1;

    pthread_mutex_init(&v->lock,NULL);
    pthread_cond_init(&v->ready,NULL);
    pthread_cond_init(&v->free_cv,NULL);
    video_tde_init(v);
    g_canvas=v->canvas[0];
    build_base(v);

    fprintf(stderr,
        "[racer] HIFB ready 1280x720 <- 640x360 Stage8.9 revc-placement-camera-wheels "
        "vcm3-vcveh-col revc-lite-handling fastcam revc-fog112 alpha-test city-zbuffer fixed60 "
        "backend=%s\n",
        v->tde_ready?"tde-quickresize":"cpu-exact2x");
    return 0;
}

static void video_close(video_t *v)
{
    if(v->tde_fd>=0)close(v->tde_fd);
    if(v->mem)munmap(v->mem,v->len);
    if(v->fd>=0)close(v->fd);
    if(v->canvas_heap){free(v->canvas[0]);free(v->canvas[1]);}
    video_mmz_close(v);
    free(v->base);free(v->row2x);
    pthread_cond_destroy(&v->ready);pthread_cond_destroy(&v->free_cv);
    pthread_mutex_destroy(&v->lock);
    memset(v,0,sizeof(*v));v->fd=-1;g_canvas=NULL;
}

static void video_vblank_sync(video_t *v)
{
    if(v->vblank_state>=0){
        errno=0;
        if(ioctl(v->fd,H3531_FBIOGET_VBLANK_HIFB,0)==0){
            if(v->vblank_state==0)fprintf(stderr,"[racer] HIFB vblank 0x4664 active\n");
            v->vblank_state=1;
        }else v->vblank_state=-1;
    }
}

static void video_present_buffer_cpu(video_t *v,const uint16_t *src)
{
    int y,x;
    for(y=0;y<RH;++y){
        const uint16_t *s=src+(size_t)y*RW;
        uint16_t *r=v->row2x;
        uint8_t *d0=v->mem+(size_t)(y*2)*v->stride;
        uint8_t *d1=v->mem+(size_t)(y*2+1)*v->stride;
        {
            uint32_t *r32=(uint32_t*)r;
            for(x=0;x<RW;x+=4){
                uint32_t p0=s[x],p1=s[x+1],p2=s[x+2],p3=s[x+3];
                r32[x  ]=p0|(p0<<16);
                r32[x+1]=p1|(p1<<16);
                r32[x+2]=p2|(p2<<16);
                r32[x+3]=p3|(p3<<16);
            }
        }
        memcpy(d0,r,OW*2U);memcpy(d1,r,OW*2U);
    }
#if defined(__arm__)
    __asm__ volatile("dmb" ::: "memory");
#endif
}

static void video_present_buffer(video_t *v,int idx)
{
    const uint16_t *src=v->canvas[idx];
    video_vblank_sync(v);
    if(v->tde_ready && video_present_tde(v,idx)==0)
        return;
    video_present_buffer_cpu(v,src);
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
            video_present_buffer(v,idx);
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
    fprintf(stderr,"[racer] dual-core pipeline active presenter=%s\n",
            v->tde_ready?
                ((v->mmz_ready&&v->mmz_direct)?"cpu1-mmzflush+tde-scale":"cpu1-stage+tde-scale"):
                "cpu1-exact2x");
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
    int i,steer=0,move_y=0,pad_gas=0,pad_brake=0,pad_handbrake=0;
    int dev_lift=0,dev_lower=0,dev_left=0,dev_right=0,dev_up=0,dev_down=0;
    int cam_side_left=0,cam_side_right=0,cam_cycle_now=0,radio_cycle_now=0;
    int cam_orbit_x=0,cam_orbit_y=0;

    if(in->kfd>=0){
        struct input_event e;
        while(read(in->kfd,&e,sizeof(e))==(ssize_t)sizeof(e)){
            int d=e.value!=0;
            if(e.type!=EV_KEY)continue;
            if(e.code==KEY_LEFT)in->left=d;
            else if(e.code==KEY_RIGHT)in->right=d;
            else if(e.code==KEY_UP||e.code==KEY_W)in->key_gas=d;
            else if(e.code==KEY_DOWN||e.code==KEY_S)in->key_brake=d;
            else if(e.code==KEY_C && e.value==1)in->camera_cycle_pressed=1;
            else if(e.code==KEY_SPACE)in->handbrake=d;
            else if(e.code==KEY_R && e.value==1)in->radio_cycle_pressed=1;
            else if(e.code==KEY_V)in->camera_look_key=d;
            else if(e.code==KEY_T && e.value==1 && g_vc_city_mode){
                g_vc_debug_flat=!g_vc_debug_flat;
                fprintf(stderr,"[racer] VC render mode=%s\n",
                        g_vc_debug_flat?"flat":(g_vc_debug_affine?"affine-textured":"perspective-textured"));
            }
            else if(e.code==KEY_U && e.value==1 && g_vc_city_mode){
                g_vc_debug_affine=!g_vc_debug_affine;
                g_vc_debug_flat=0;
                fprintf(stderr,"[racer] VC render mode=%s\n",
                        g_vc_debug_affine?"affine-textured":"perspective-textured");
            }
            else if((e.code==KEY_ESC||e.code==KEY_F12)&&d)g_stop=1;
        }
    }

    {
        int look_back_now=in->camera_look_key;
        in->start_down=0;in->select_down=0;

        for(i=0;i<in->pad_count;++i){
            pad_node_t *p=&in->pads[i];
            struct input_event e;
            int twin_usb=strstr(p->name,"Twin USB Joystick")!=NULL;

            while(read(p->fd,&e,sizeof(e))==(ssize_t)sizeof(e)){
                if(e.type==EV_ABS&&e.code<=ABS_MAX&&p->have_abs[e.code])
                    p->axis[e.code]=scale_abs_centered(
                        &p->absinfo[e.code],p->center_raw[e.code],e.value);
                else if(e.type==EV_KEY&&e.code<=KEY_MAX){
                    p->key_down[e.code]=(uint8_t)(e.value!=0);
                    if(i==in->steer_node && e.value!=2 &&
                       e.code>=BTN_JOYSTICK && e.code<=BTN_BASE6)
                        fprintf(stderr,"[racer] PAD_KEY code=%u value=%d name=%s\n",
                            (unsigned)e.code,e.value,p->name);
                }
            }

            /* Left analogue stick: steering on ground, yaw+forward in hover. */
            if(i==in->steer_node&&p->sx_code>=0){
                steer=shape_axis(p->axis[p->sx_code]);
                if(p->sy_code>=0)move_y=shape_axis(p->axis[p->sy_code]);
            }

            if(p->key_down[BTN_START])in->start_down=1;
            if(p->key_down[BTN_SELECT])in->select_down=1;

            if(twin_usb){
                /*
                 * Twin USB Joystick button indices are the standard profile:
                 *   X=0 A=1 B=2 Y=3 L2=4 R2=5 L1=6 R1=7 ... R3=11.
                 * Linux generic joystick aliases map those to
                 * BTN_TRIGGER/THUMB/THUMB2/TOP/.../BASE6 respectively.
                 */
                if(p->key_down[BTN_THUMB])pad_gas=1;       /* A / button 1 */
                if(p->key_down[BTN_THUMB2])pad_brake=1;   /* B / button 2 */
                if(p->key_down[BTN_TRIGGER])pad_handbrake=1; /* X / button 0 */
                if(p->key_down[BTN_TOP])cam_cycle_now=1;  /* Y / button 3 */
                if(p->key_down[BTN_BASE6])radio_cycle_now=1; /* R3 / button 11 */
            }else{
                if(p->key_down[BTN_SOUTH])pad_gas=1;
                if(p->key_down[BTN_EAST])pad_brake=1;
                if(p->key_down[BTN_WEST])pad_handbrake=1;
                if(p->key_down[BTN_NORTH])cam_cycle_now=1;
                if(p->key_down[BTN_THUMBR])radio_cycle_now=1;
            }

            if(i==in->steer_node){
                /* User-requested GTA-like momentary look controls. */
                if(p->key_down[BTN_TL] || (twin_usb&&p->key_down[BTN_BASE]))
                    cam_side_left=1;
                if(p->key_down[BTN_TR] || (twin_usb&&p->key_down[BTN_BASE2]))
                    cam_side_right=1;

                /*
                 * D-pad/arrow camera orbit in normal driving. The controller
                 * wizard confirmed ABS_HAT0X=left/right and ABS_HAT0Y=up/down.
                 * In developer hover the same arrows retain transport duty.
                 */
                {
                    int hat_x=0,hat_y=0;
                    if(p->key_down[BTN_DPAD_LEFT]||p->key_down[KEY_LEFT])
                        hat_x=-32768;
                    else if(p->key_down[BTN_DPAD_RIGHT]||p->key_down[KEY_RIGHT])
                        hat_x=32767;
                    else if(p->have_abs[ABS_HAT0X])
                        hat_x=shape_axis(p->axis[ABS_HAT0X]);

                    if(p->key_down[BTN_DPAD_UP]||p->key_down[KEY_UP])
                        hat_y=-32768;
                    else if(p->key_down[BTN_DPAD_DOWN]||p->key_down[KEY_DOWN])
                        hat_y=32767;
                    else if(p->have_abs[ABS_HAT0Y])
                        hat_y=shape_axis(p->axis[ABS_HAT0Y]);

                    if(g_dev_hover){
                        if(hat_x<-9000)dev_left=1;
                        if(hat_x> 9000)dev_right=1;
                        if(hat_y<-9000)dev_up=1;
                        if(hat_y> 9000)dev_down=1;
                    }else{
                        cam_orbit_x=hat_x;
                        cam_orbit_y=hat_y;
                    }
                }

                /*
                 * Restore the original proven developer-flight controls:
                 * R2 enters/raises flight, L2 lowers/lands.  Do not reuse
                 * these shoulders for throttle/brake.
                 */
                if(p->key_down[BTN_TR2] ||
                   (twin_usb&&p->key_down[BTN_PINKIE]))
                    dev_lift=1;
                if(p->key_down[BTN_TL2] ||
                   (twin_usb&&p->key_down[BTN_TOP2]))
                    dev_lower=1;
            }
        }

        if(cam_cycle_now&&!in->camera_view_toggle_prev)
            in->camera_cycle_pressed=1;
        in->camera_view_toggle_prev=cam_cycle_now;
        if(radio_cycle_now&&!in->radio_cycle_prev)
            in->radio_cycle_pressed=1;
        in->radio_cycle_prev=radio_cycle_now;
        in->camera_look_behind=look_back_now;
        in->camera_side_left=cam_side_left;
        in->camera_side_right=cam_side_right;
        in->camera_orbit_x=cam_orbit_x;
        in->camera_orbit_y=cam_orbit_y;
    }

    /* Keyboard arrows remain keyboard driving controls. */
    if(in->left)steer=-32768;
    if(in->right)steer=32767;

    in->steer=steer;
    in->move_y=move_y;
    in->gas=in->key_gas||pad_gas;
    in->brake=in->key_brake||pad_brake;
    in->handbrake=in->handbrake||pad_handbrake;
    in->dev_lift=dev_lift;
    in->dev_lower=dev_lower;
    in->dev_left=dev_left;
    in->dev_right=dev_right;
    in->dev_up=dev_up;
    in->dev_down=dev_down;
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
    g_speed=0.0f;
    g_vehicle_vlong=0.0f;
    g_vehicle_vlat=0.0f;
    vc_reset_turn_world();
    g_vc_body_basis_valid=0;
    g_vehicle_steer_input=0.0f;
    g_vc_raw_steer_input=0.0f;
    g_vc_two_wheel_ticks=0;
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

static float active_vehicle_camera_length(void)
{
    if(g_vc_vehicle.loaded && g_vc_vehicle.native_col_loaded){
        float len=g_vc_vehicle.col_box_max.z-g_vc_vehicle.col_box_min.z;
        if(len>200.0f && len<3000.0f)return len;
    }
    return fmaxf(700.0f,active_vehicle_wheelbase()*1.55f);
}

static float active_vehicle_camera_height(void)
{
    if(g_vc_vehicle.loaded && g_vc_vehicle.native_col_loaded){
        float h=g_vc_vehicle.col_box_max.y-g_vc_vehicle.col_box_min.y;
        if(h>100.0f && h<1800.0f)return h;
    }
    return 420.0f;
}

static const char *camera_cycle_name(int mode)
{
    switch(mode){
    case VC_CAM_1STPRS:return "first-person";
    case VC_CAM_ZOOM_1:return "zoom-1-close";
    case VC_CAM_ZOOM_2:return "zoom-2-mid";
    case VC_CAM_ZOOM_3:return "zoom-3-far";
    default:return "cinematic";
    }
}

static void camera_cycle_zoom(void)
{
    g_camera_cycle_mode=(g_camera_cycle_mode+1)%VC_CAM_COUNT;
    if(g_camera_cycle_mode==VC_CAM_ZOOM_1)g_camera_zoom_mode=0;
    else if(g_camera_cycle_mode==VC_CAM_ZOOM_2)g_camera_zoom_mode=1;
    else if(g_camera_cycle_mode==VC_CAM_ZOOM_3)g_camera_zoom_mode=2;

    /*
     * reVC jump-cuts first-person/cinematic changes. Reset only those modes;
     * the three chase zooms retain spring interpolation.
     */
    if(g_camera_cycle_mode==VC_CAM_1STPRS ||
       g_camera_cycle_mode==VC_CAM_CINEMATIC)
        g_camera_initialized=0;

    g_camera_orbit_yaw=0.0f;
    g_camera_orbit_pitch=0.0f;
    g_camera_orbit_idle_ticks=0;
    fprintf(stderr,"[racer] camera mode=%s (%d) reVC-cycle topdown-skipped\n",
        camera_cycle_name(g_camera_cycle_mode),g_camera_cycle_mode);
}

static void reset_chase_camera(void)
{
    static const float zoom_dist[3]={930.0f,1180.0f,1480.0f};
    static const float zoom_height[3]={360.0f,455.0f,565.0f};
    track_world_t car;
    float road_yaw;
    float car_len=active_vehicle_camera_length();
    float car_h=active_vehicle_camera_height();
    float distance=zoom_dist[g_camera_zoom_mode]+car_len*0.12f;
    float height=zoom_height[g_camera_zoom_mode]+car_h*0.10f;
    float target_heading=g_vehicle_heading+(g_camera_look_behind?3.14159265f:0.0f);
    float look_y,horiz;

    get_player_world(&car,&road_yaw);
    (void)road_yaw;

    g_camera_arm_heading=wrap_angle(target_heading);
    g_camera_arm_heading_vel=0.0f;
    g_camera_heading=wrap_angle(target_heading);
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

    look_y=car.y+car_h*0.72f;
    horiz=hypotf(car.x-g_camera_x,car.z-g_camera_z);
    g_camera_pitch=atan2f(look_y-g_camera_y,fmaxf(1.0f,horiz));
    g_camera_pitch_vel=0.0f;
    g_camera_initialized=1;
}

static void racer_control_set_pose(float x,float y,float z,float yaw,int set_yaw)
{
    g_world_x=x;
    g_world_y=y;
    g_world_z=z;
    g_vc_ground_y=y;
    if(set_yaw)g_vehicle_heading=wrap_angle(yaw);

    g_speed=0.0f;
    g_prev_speed=0.0f;
    g_vehicle_vlong=0.0f;
    g_vehicle_vlat=0.0f;
    vc_reset_turn_world();
    g_vehicle_steer_input=0.0f;
    g_vc_raw_steer_input=0.0f;
    g_vc_two_wheel_ticks=0;
    g_body_pitch=0.0f;
    g_body_roll=0.0f;
    g_vc_body_basis_valid=0;
    g_vehicle_vy=0.0f;
    g_vehicle_airborne=0;
    g_camera_initialized=0;
    reset_chase_camera();

    fprintf(stderr,
        "[racer] CONTROL pose world=%.2f,%.2f,%.2f yaw=%.6f\n",
        g_world_x,g_world_y,g_world_z,g_vehicle_heading);
}

static void racer_control_exec(char *line)
{
    float a,b,c,d;
    char *p=line;
    while(*p==' '||*p=='\t')p++;
    if(!*p)return;

    if(!strcmp(p,"where")){
        fprintf(stderr,
            "[racer] CONTROL where world=%.2f,%.2f,%.2f yaw=%.6f gta=%.4f,%.4f,%.4f\n",
            g_world_x,g_world_y,g_world_z,g_vehicle_heading,
            g_world_x/vc_runtime_world_scale(),
            g_world_z/vc_runtime_world_scale(),
            g_world_y/vc_runtime_world_scale());
        return;
    }
    if(sscanf(p,"teleport %f %f %f %f",&a,&b,&c,&d)==4){
        racer_control_set_pose(a,b,c,d,1);
        return;
    }
    if(sscanf(p,"pos %f %f %f",&a,&b,&c)==3){
        racer_control_set_pose(a,b,c,g_vehicle_heading,0);
        return;
    }
    if(sscanf(p,"delta %f %f %f",&a,&b,&c)==3){
        racer_control_set_pose(g_world_x+a,g_world_y+b,g_world_z+c,g_vehicle_heading,0);
        return;
    }
    if(sscanf(p,"gta %f %f %f",&a,&b,&c)==3){
        float sc=vc_runtime_world_scale();
        racer_control_set_pose(a*sc,c*sc,b*sc,g_vehicle_heading,0);
        return;
    }
    if(sscanf(p,"yawdeg %f",&a)==1){
        racer_control_set_pose(g_world_x,g_world_y,g_world_z,
            a*(3.14159265358979323846f/180.0f),1);
        return;
    }
    if(sscanf(p,"yaw %f",&a)==1){
        racer_control_set_pose(g_world_x,g_world_y,g_world_z,a,1);
        return;
    }

    fprintf(stderr,
        "[racer] CONTROL unknown='%s' commands: where | pos X Y Z | "
        "delta DX DY DZ | teleport X Y Z YAW | gta X Y Z | yaw R | yawdeg D\n",p);
}

static void racer_control_open(void)
{
    const char *env=getenv("RACER_CONTROL_FIFO");
    struct stat st;
    if(g_control_fd>=0)return;
    if(env&&*env)snprintf(g_control_path,sizeof(g_control_path),"%s",env);

    if(mkfifo(g_control_path,0666)<0 && errno!=EEXIST){
        fprintf(stderr,"[racer] CONTROL fifo create failed path=%s errno=%d\n",
            g_control_path,errno);
        return;
    }
    if(stat(g_control_path,&st)<0 || !S_ISFIFO(st.st_mode)){
        fprintf(stderr,"[racer] CONTROL path is not fifo: %s\n",g_control_path);
        return;
    }

    g_control_fd=open(g_control_path,O_RDWR|O_NONBLOCK);
    if(g_control_fd<0){
        fprintf(stderr,"[racer] CONTROL fifo open failed path=%s errno=%d\n",
            g_control_path,errno);
        return;
    }
    fprintf(stderr,
        "[racer] CONTROL live fifo=%s commands=where,pos,delta,teleport,gta,yaw,yawdeg\n",
        g_control_path);
}

static void racer_control_poll(void)
{
    char tmp[128];
    ssize_t n;
    /* Retry periodically if the FIFO could not be created during startup. */
    if(g_control_fd<0){
        if((g_frame%120U)==0U)racer_control_open();
        if(g_control_fd<0)return;
    }

    while((n=read(g_control_fd,tmp,sizeof(tmp)))>0){
        ssize_t i;
        for(i=0;i<n;++i){
            char ch=tmp[i];
            if(ch=='\r')continue;
            if(ch=='\n'){
                g_control_buf[g_control_len]='\0';
                racer_control_exec(g_control_buf);
                g_control_len=0;
            }else if(g_control_len+1<sizeof(g_control_buf)){
                g_control_buf[g_control_len++]=ch;
            }else{
                g_control_len=0;
            }
        }
    }
}


static void update_chase_camera(float speed_ratio)
{
    /*
     * H3531 implementation of Vice City's normal vehicle camera language:
     * 1STPRS, three CAM_ON_A_STRING zooms, CINEMATIC. TOPDOWN is skipped just
     * like reVC's normal cycle. D-pad supplies the manual orbit requested for
     * this controller; L1/R1 remain momentary side/front look overrides.
     */
    static const float zoom_dist[3]={900.0f,1180.0f,1510.0f};
    static const float zoom_height[3]={330.0f,455.0f,570.0f};
    const float dt=1.0f/60.0f;
    const float halfpi=1.57079633f;
    track_world_t car;
    float road_yaw;
    float car_len=active_vehicle_camera_length();
    float car_h=active_vehicle_camera_height();
    float distance=1180.0f,height=455.0f,look_y;
    float look_x,look_z,desired_look,desired_pitch,horiz;
    float target_arm=g_vehicle_heading;
    float abs_v=sqrtf(g_vehicle_vlong*g_vehicle_vlong+
                      g_vehicle_vlat*g_vehicle_vlat);
    float rx=(float)g_camera_orbit_input_x/32767.0f;
    float ry=(float)g_camera_orbit_input_y/32767.0f;
    int fixed_view=0,first_person=0,cinematic=0;

    if(speed_ratio<0.0f)speed_ratio=0.0f;
    if(speed_ratio>1.0f)speed_ratio=1.0f;
    get_player_world(&car,&road_yaw);
    (void)road_yaw;

    if(!g_camera_initialized){
        reset_chase_camera();
        /* reset_chase_camera initializes from zoom mode; continue so special
         * first-person/cinematic mode gets its correct pose this same tick. */
    }

    if(g_camera_cycle_mode==VC_CAM_ZOOM_1)g_camera_zoom_mode=0;
    else if(g_camera_cycle_mode==VC_CAM_ZOOM_2)g_camera_zoom_mode=1;
    else if(g_camera_cycle_mode==VC_CAM_ZOOM_3)g_camera_zoom_mode=2;

    if(g_camera_cycle_mode>=VC_CAM_ZOOM_1 &&
       g_camera_cycle_mode<=VC_CAM_ZOOM_3){
        distance=zoom_dist[g_camera_zoom_mode]+car_len*0.12f+
                 90.0f*speed_ratio;
        height=zoom_height[g_camera_zoom_mode]+car_h*0.05f+
               24.0f*speed_ratio;
        if(g_speed<0.0f)distance+=60.0f;
    }else if(g_camera_cycle_mode==VC_CAM_1STPRS){
        first_person=1;
        fixed_view=1;
        /*
         * MODE_1STPERSON equivalent for the compact renderer: place the camera
         * just ahead of the vehicle centre and above the bonnet/driver line.
         * We do not render an interior, so this is deliberately a clean
         * bumper/bonnet first-person view rather than clipping through DFF.
         */
        target_arm=wrap_angle(g_vehicle_heading+3.14159265f);
        distance=clampf_local(car_len*0.38f,330.0f,620.0f);
        height=clampf_local(car_h*0.52f,210.0f,430.0f);
    }else{
        unsigned phase=(g_frame/180U)%4U;
        cinematic=1;
        fixed_view=1;
        /*
         * reVC cinematic mode delegates to the Obbe car-camera sequence
         * (wheel/side/front/fixed shots). We keep the same language with four
         * inexpensive car-relative shots suitable for the H3531 rasterizer.
         */
        if(phase==0U){
            target_arm=wrap_angle(g_vehicle_heading+2.38f);
            distance=760.0f+car_len*0.08f;
            height=210.0f+car_h*0.06f;       /* front-left/wheel */
        }else if(phase==1U){
            target_arm=wrap_angle(g_vehicle_heading-halfpi);
            distance=1080.0f+car_len*0.10f;
            height=330.0f+car_h*0.10f;       /* right side */
        }else if(phase==2U){
            target_arm=wrap_angle(g_vehicle_heading+3.14159265f);
            distance=1220.0f+car_len*0.10f;
            height=300.0f+car_h*0.10f;       /* front */
        }else{
            target_arm=wrap_angle(g_vehicle_heading+0.42f);
            distance=1450.0f+car_len*0.10f;
            height=590.0f+car_h*0.08f;       /* elevated rear 3/4 */
        }
    }

    /*
     * D-pad manual orbit. Leave first-person and cinematic deterministic; in
     * three normal chase modes arrows rotate exactly around the vehicle.
     */
    if(!first_person && !cinematic &&
       (fabsf(rx)>0.08f || fabsf(ry)>0.08f)){
        if(fabsf(rx)>0.08f)
            g_camera_orbit_yaw=wrap_angle(
                g_camera_orbit_yaw-rx*0.050f);
        if(fabsf(ry)>0.08f)
            g_camera_orbit_pitch=clampf_local(
                g_camera_orbit_pitch-ry*0.014f,-0.28f,0.20f);
        g_camera_orbit_idle_ticks=0;
    }else if(!first_person && !cinematic){
        if(g_camera_orbit_idle_ticks<600U)g_camera_orbit_idle_ticks++;
        if(g_camera_orbit_idle_ticks>75U){
            g_camera_orbit_yaw=approach_angle(
                g_camera_orbit_yaw,0.0f,0.012f);
            g_camera_orbit_pitch=approachf(
                g_camera_orbit_pitch,0.0f,0.006f);
        }
    }

    if(first_person){
        target_arm=wrap_angle(g_vehicle_heading+3.14159265f);
    }else if(!cinematic && fabsf(g_camera_orbit_yaw)>0.015f){
        target_arm=wrap_angle(g_vehicle_heading+g_camera_orbit_yaw);
    }else if(!cinematic && abs_v>2.0f){
        float sh=sinf(g_vehicle_heading),ch=cosf(g_vehicle_heading);
        float vx=sh*g_vehicle_vlong+ch*g_vehicle_vlat;
        float vz=ch*g_vehicle_vlong-sh*g_vehicle_vlat;
        float vel_heading=atan2f(vx,vz);
        float w=clampf_local((abs_v-2.0f)/38.0f,0.0f,1.0f)*0.72f;
        target_arm=wrap_angle(
            g_vehicle_heading+
            wrap_angle(vel_heading-g_vehicle_heading)*w);
    }

    /*
     * L1/R1 momentary views override every persistent mode, as requested:
     * R1 right side, L1 left side, both front.
     */
    if(g_camera_side_left && g_camera_side_right){
        target_arm=wrap_angle(g_vehicle_heading+3.14159265f);
        distance=1280.0f+car_len*0.10f;
        height=330.0f+car_h*0.12f;
        fixed_view=1;first_person=0;cinematic=0;
    }else if(g_camera_side_right){
        target_arm=wrap_angle(g_vehicle_heading-halfpi);
        distance=1180.0f+car_len*0.10f;
        height=350.0f+car_h*0.12f;
        fixed_view=1;first_person=0;cinematic=0;
    }else if(g_camera_side_left){
        target_arm=wrap_angle(g_vehicle_heading+halfpi);
        distance=1180.0f+car_len*0.10f;
        height=350.0f+car_h*0.12f;
        fixed_view=1;first_person=0;cinematic=0;
    }else if(g_camera_look_behind){
        target_arm=wrap_angle(g_vehicle_heading+3.14159265f);
        fixed_view=1;first_person=0;cinematic=0;
    }

    g_camera_target_distance=distance;
    g_camera_target_height=height;

    spring_angle(&g_camera_arm_heading,&g_camera_arm_heading_vel,
                 target_arm,fixed_view?5.4f:2.05f,0.86f,dt);
    spring_scalar(&g_camera_distance,&g_camera_distance_vel,
                  g_camera_target_distance,fixed_view?5.0f:2.10f,0.90f,dt);
    spring_scalar(&g_camera_height,&g_camera_height_vel,
                  g_camera_target_height,fixed_view?4.2f:1.85f,0.90f,dt);

    if(g_camera_distance<280.0f)g_camera_distance=280.0f;
    if(g_camera_distance>2050.0f)g_camera_distance=2050.0f;

    g_camera_x=car.x-sinf(g_camera_arm_heading)*g_camera_distance;
    g_camera_z=car.z-cosf(g_camera_arm_heading)*g_camera_distance;
    g_camera_y=car.y+g_camera_height;

    if(first_person){
        look_x=car.x+sinf(g_vehicle_heading)*(car_len*1.8f);
        look_z=car.z+cosf(g_vehicle_heading)*(car_len*1.8f);
        look_y=car.y+car_h*0.48f;
    }else if(fixed_view || fabsf(g_camera_orbit_yaw)>0.015f){
        look_x=car.x;
        look_z=car.z;
        look_y=car.y+car_h*(cinematic?0.46f:0.55f);
    }else{
        look_x=car.x+sinf(g_vehicle_heading)*(170.0f+240.0f*speed_ratio);
        look_z=car.z+cosf(g_vehicle_heading)*(170.0f+240.0f*speed_ratio);
        look_y=car.y+car_h*0.70f;
    }

    desired_look=atan2f(look_x-g_camera_x,look_z-g_camera_z);
    horiz=hypotf(look_x-g_camera_x,look_z-g_camera_z);
    desired_pitch=atan2f(look_y-g_camera_y,fmaxf(1.0f,horiz));
    if(!first_person && !cinematic)
        desired_pitch+=g_camera_orbit_pitch;
    desired_pitch=clampf_local(desired_pitch,-0.52f,0.28f);

    spring_angle(&g_camera_heading,&g_camera_heading_vel,desired_look,
                 fixed_view?5.5f:2.80f,0.90f,dt);
    spring_angle(&g_camera_pitch,&g_camera_pitch_vel,desired_pitch,
                 fixed_view?4.8f:2.60f,0.90f,dt);
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
    float ref_z,car_h;

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

    /*
     * reVC's car camera aims above the vehicle origin (roughly 0.8 of the
     * collision-box height).  The old Racer path ignored the real camera Y
     * entirely and forced the car origin to -1515, pushing a large part of the
     * player car below the 360-line framebuffer.  Keep the lightweight local
     * car renderer, but anchor it from the actual vehicle height so the whole
     * body and wheels stay in frame across zoom modes.
     */
    car_h=active_vehicle_camera_height();
    *oz=ref_z;
    *oy=-(car_h*0.72f+260.0f)-(ref_z-1660.0f)*0.08f;
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
     * Legacy procedural Racer scenery belongs only to the original track.
     * Never overlay the Vice City world with fake mountain ridges or the
     * old dotted horizon-haze stripe.
     */
    if(!g_vc_city_mode){
        int yaw_shift=(int)(camyaw*86.0f);
        int travel_shift=(int)(g_position*0.00020f);
        draw_ridge_layer(g_far_mountain_ridge,32,28,yaw_shift+travel_shift,
                         130,40,pack1555(92,121,147));
        draw_ridge_layer(g_near_hill_ridge,32,24,yaw_shift/2+travel_shift*2,
                         136,31,pack1555(70,111,83));

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

static inline void city_world_to_camera_csp(
    float wx,float wy,float wz,
    float camx,float camy,float camz,
    float cs,float sn,float cp,float sp,
    v3f_t *o)
{
    float dx=wx-camx,dy=wy-camy,dz=wz-camz;
    float hx=dx*cs-dz*sn;
    float hz=dx*sn+dz*cs;
    o->x=hx;
    if(g_vc_city_mode){
        o->y=dy*cp-hz*sp;
        o->z=dy*sp+hz*cp;
    }else{
        o->y=dy;
        o->z=hz;
    }
}

static inline void city_world_to_camera_cs(
    float wx,float wy,float wz,
    float camx,float camy,float camz,
    float cs,float sn,
    v3f_t *o)
{
    float cp=1.0f,sp=0.0f;
    if(g_vc_city_mode){
        cp=cosf(g_camera_pitch);
        sp=sinf(g_camera_pitch);
    }
    city_world_to_camera_csp(
        wx,wy,wz,camx,camy,camz,cs,sn,cp,sp,o);
}

static void city_world_to_camera(
    float wx,float wy,float wz,
    float camx,float camy,float camz,float camyaw,
    v3f_t *o)
{
    float cs=cosf(camyaw),sn=sinf(camyaw);
    city_world_to_camera_cs(wx,wy,wz,camx,camy,camz,cs,sn,o);
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
    float focal=g_vc_city_mode?VC_FOCAL:TRACK_FOCAL;
    float screen_y=g_vc_city_mode?VC_SCREEN_Y:TRACK_SCREEN_Y;
    float s=focal/p->z;
    o->sx=RW*0.5f+p->x*s;
    o->sy=screen_y-p->y*s;
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
            int minx=x0,maxx=x0,miny=y0,maxy=y0;
            int32_t area;

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

static float vc_clip_plane_eval(const vc_clip_v_t *v,int plane)
{
    const float margin=8.0f;
    const float near_z=45.0f;
    switch(plane){
    case 0: return v->p.z-near_z;
    case 1: return v->p.x+(((float)RW*0.5f+margin)/VC_FOCAL)*v->p.z;
    case 2: return ((((float)RW-1.0f)-(float)RW*0.5f+margin)/VC_FOCAL)*v->p.z-v->p.x;
    case 3: return ((VC_SCREEN_Y+margin)/VC_FOCAL)*v->p.z-v->p.y;
    default:return v->p.y+((((float)RH-1.0f)-VC_SCREEN_Y+margin)/VC_FOCAL)*v->p.z;
    }
}

/*
 * Full camera-frustum clip before projection. Near-only clipping let a large
 * world triangle reach z~=45 with an arbitrarily large X/Y, producing huge
 * projected coordinates and 32-bit edge overflows ("city-covering polygons").
 */
static int vc_clip_frustum_textured(const vc_clip_v_t in[3],vc_clip_v_t out[12])
{
    vc_clip_v_t a[12],b[12];
    int count=3,plane,i;
    a[0]=in[0];a[1]=in[1];a[2]=in[2];

    for(plane=0;plane<5 && count>=3;++plane){
        int n=0;
        for(i=0;i<count;++i){
            const vc_clip_v_t *p=&a[i];
            const vc_clip_v_t *q=&a[(i+1)%count];
            float ep=vc_clip_plane_eval(p,plane);
            float eq=vc_clip_plane_eval(q,plane);
            int pin=ep>=0.0f,qin=eq>=0.0f;

            if(pin && n<12)b[n++]=*p;
            if(pin!=qin && n<12){
                float den=ep-eq;
                float t=fabsf(den)>1.0e-10f?ep/den:0.0f;
                vc_clip_v_t x;
                t=clampf_local(t,0.0f,1.0f);
                x.p.x=p->p.x+(q->p.x-p->p.x)*t;
                x.p.y=p->p.y+(q->p.y-p->p.y)*t;
                x.p.z=p->p.z+(q->p.z-p->p.z)*t;
                x.u=p->u+(q->u-p->u)*t;
                x.v=p->v+(q->v-p->v)*t;
                b[n++]=x;
            }
        }
        count=n;
        for(i=0;i<count;++i)a[i]=b[i];
    }
    for(i=0;i<count;++i)out[i]=a[i];
    return count;
}

static unsigned vc_clip_outcode_textured(const vc_clip_v_t *v)
{
    unsigned mask=0U;
    int plane;
    for(plane=0;plane<5;++plane)
        if(vc_clip_plane_eval(v,plane)<0.0f)mask|=(1U<<(unsigned)plane);
    return mask;
}

static float vc_object_xz_distance(const vc_stream_object_t *o)
{
    float dx=o->cx-g_world_x,dz=o->cz-g_world_z;
    float ax=fabsf(dx),az=fabsf(dz);
    float nearv=fminf(ax,az),farv=fmaxf(ax,az);
    float d=farv+0.375f*nearv-o->radius;
    return d>0.0f?d:0.0f;
}

static float vc_object_center_xz_distance(const vc_stream_object_t *o)
{
    float dx=o->cx-g_world_x,dz=o->cz-g_world_z;
    float ax=fabsf(dx),az=fabsf(dz);
    float nearv=fminf(ax,az),farv=fmaxf(ax,az);
    return farv+0.375f*nearv;
}

static float vc_object_stream_limit(const vc_runtime_map_t *map,const vc_stream_object_t *o)
{
    float far=VC_FAR_CLIP_M*(map->world_scale>1.0f?map->world_scale:240.0f);
    if(o->draw_world>1.0f && o->draw_world<far)return o->draw_world;
    return far;
}

static uint8_t vc_object_lod_for_distance(const vc_stream_object_t *o,float d)
{
    unsigned i,count=o->lod_count?o->lod_count:1U;
    if(count>3U)count=3U;
    for(i=0;i<count;++i){
        float limit=o->lod_world[i];
        if(limit>1.0f && d<limit)return (uint8_t)i;
    }
    return (uint8_t)(count-1U);
}

static void vc_update_object_stream(vc_runtime_map_t *map)
{
    uint64_t now,dt;
    unsigned i,starts=0;
    int step;

    if(!map||!map->objects||!map->object_count)return;
    if(map->object_last_frame==g_frame)return;
    map->object_last_frame=g_frame;
    now=mono_ns();
    dt=map->object_last_ns?now-map->object_last_ns:0ULL;
    if(dt>100000000ULL)dt=100000000ULL;
    map->object_last_ns=now;
    step=(int)((dt*255ULL)/VC_OBJECT_FADE_NS);
    if(step<1)step=1;if(step>64)step=64;

    /*
     * reVC CSimpleModelInfo::GetAtomicFromDistance() chooses one geometry
     * atomic from the model's LOD distances. Distance controls the selected
     * atomic, not a per-triangle transparency ramp.
     */
    for(i=0;i<map->object_count;++i){
        vc_stream_object_t *o=&map->objects[i];
        float d,center_d,limit,hyst;
        int wanted;
        uint8_t lod;
        if(!o->active)continue;
        d=vc_object_xz_distance(o);
        center_d=vc_object_center_xz_distance(o);
        limit=vc_object_stream_limit(map,o);
        hyst=24.0f*(map->world_scale>1.0f?map->world_scale:240.0f);
        wanted=d<=limit+hyst;
        if(wanted){
            lod=vc_object_lod_for_distance(o,center_d);
            if(lod!=o->lod_selected){
                o->lod_selected=lod;
                g_vc_frame_object_lod_switches++;
            }
            if(o->flags&2U)o->alpha=255U;
            else if(o->alpha<255U){
                unsigned a=(unsigned)o->alpha+(unsigned)step;
                o->alpha=(uint8_t)(a>255U?255U:a);
            }
        }else{
            if(o->alpha>(uint8_t)step)o->alpha=(uint8_t)(o->alpha-step);
            else{o->alpha=0U;o->active=0U;}
        }
    }

    /*
     * Page data is already prefetched. Match reVC's STREAM_DISTANCE intent by
     * starting nearest inactive instances gradually, then keep them fully
     * resident while their selected atomic changes with distance.
     */
    while(starts<VC_OBJECT_START_BUDGET && g_vc_object_start_budget_left>0U){
        int best=-1;
        float bestd=1.0e30f;
        for(i=0;i<map->object_count;++i){
            vc_stream_object_t *o=&map->objects[i];
            float d,limit;
            if(o->active)continue;
            d=vc_object_xz_distance(o);
            limit=vc_object_stream_limit(map,o);
            if(d<=limit && d<bestd){best=(int)i;bestd=d;}
        }
        if(best<0)break;
        map->objects[best].active=1U;
        map->objects[best].lod_selected=
            vc_object_lod_for_distance(
                &map->objects[best],
                vc_object_center_xz_distance(&map->objects[best]));
        map->objects[best].alpha=
            (map->objects[best].flags&2U)?255U:1U;
        starts++;
        g_vc_object_start_budget_left--;
    }

    for(i=0;i<map->object_count;++i){
        const vc_stream_object_t *o=&map->objects[i];
        if(o->active){
            unsigned lod=o->lod_selected<3U?o->lod_selected:2U;
            g_vc_frame_objects_active++;
            g_vc_frame_object_lod[lod]++;
        }
        if(o->alpha>0U&&o->alpha<255U)g_vc_frame_objects_fading++;
    }
    g_vc_frame_objects_started+=starts;
}

static void vc_update_object_camera_visibility(
    vc_runtime_map_t *map,
    float camx,float camy,float camz,
    float cam_cs,float cam_sn,float cam_cp,float cam_sp)
{
    const float margin=8.0f;
    const float near_z=45.0f;
    const float sx_l=((float)RW*0.5f+margin)/VC_FOCAL;
    const float sx_r=(((float)RW-1.0f)-(float)RW*0.5f+margin)/VC_FOCAL;
    const float sy_t=(VC_SCREEN_Y+margin)/VC_FOCAL;
    const float sy_b=(((float)RH-1.0f)-VC_SCREEN_Y+margin)/VC_FOCAL;
    float far_world;
    unsigned i;

    if(!map||!map->objects||!map->object_count)return;
    if(map->object_visibility_frame==g_frame)return;
    map->object_visibility_frame=g_frame;
    far_world=(map->world_scale>1.0f?map->world_scale:240.0f)*VC_FAR_CLIP_M;

    for(i=0;i<map->object_count;++i){
        vc_stream_object_t *o=&map->objects[i];
        v3f_t p;
        float r=fmaxf(1.0f,o->radius);
        int visible=1;

        if(!o->active||o->alpha==0U){
            o->camera_visible=0U;
            continue;
        }

        city_world_to_camera_csp(
            o->cx,o->cy,o->cz,
            camx,camy,camz,cam_cs,cam_sn,cam_cp,cam_sp,&p);

        /*
         * Conservative sphere/frustum test.  r*(1+|slope|) intentionally
         * overestimates each plane-normal length, so this may keep a few
         * off-screen objects but cannot clip a visible GTA instance.
         */
        if(p.z+r<near_z || p.z-r>far_world)visible=0;
        else if(p.x+sx_l*p.z < -r*(1.0f+sx_l))visible=0;
        else if(sx_r*p.z-p.x < -r*(1.0f+sx_r))visible=0;
        else if(sy_t*p.z-p.y < -r*(1.0f+sy_t))visible=0;
        else if(p.y+sy_b*p.z < -r*(1.0f+sy_b))visible=0;

        o->camera_visible=(uint8_t)(visible?1U:0U);
        if(!visible)g_vc_frame_object_frustum_reject++;
    }
}

static int vc_triangle_backfacing(v3f_t a,v3f_t b,v3f_t c)
{
    /*
     * reVC renders roads/buildings with rwCULLMODECULLBACK, but VCM3 converts
     * GTA's Z-up coordinates to Racer Y-up as (x,y,z)->(x,z,y). Swapping one
     * axis pair has determinant -1, so the packed triangle winding is reversed
     * even though its vertex indices are preserved. Therefore the converted
     * GTA FRONT face has the opposite camera-space normal sign from a native
     * Racer mesh.
     *
     * Camera is at the origin looking +Z:
     *   converted GTA front face -> dot(normal,position) > 0  (keep)
     *   converted GTA back face  -> dot(normal,position) < 0  (cull)
     *
     * This sign is intentionally VCM3-specific; do not reuse it for native
     * Racer meshes unless their coordinate conversion has the same handedness.
     */
    v3f_t ab={b.x-a.x,b.y-a.y,b.z-a.z};
    v3f_t ac={c.x-a.x,c.y-a.y,c.z-a.z};
    v3f_t n={
        ab.y*ac.z-ab.z*ac.y,
        ab.z*ac.x-ab.x*ac.z,
        ab.x*ac.y-ab.y*ac.x
    };
    float d=n.x*a.x+n.y*a.y+n.z*a.z;
    return d<=0.0f;
}

static void queue_vc_mesh_textured(
    const vc_runtime_map_t *map,uint8_t page_slot,
    const vc_vertex_t *verts,int vcount,const vc_map_tri_t *tris,int tcount,
    float scale,float camx,float camy,float camz,
    float cam_cs,float cam_sn,float cam_cp,float cam_sp,int *n)
{
    v3f_t *rv=g_mesh_rv;
    v3f_t *cv=g_mesh_cam;
    int i;

    if(vcount>MAX_MESH_VERTS||tcount>MAX_VC_DRAW_TRIS)return;
    if(*n>=MAX_VC_DRAW_TRIS)return;

    /*
     * VCO2 can carry multiple atomics for the same GTA instance. Transform
     * vertices lazily only after object/LOD selection so inactive and distant
     * LOD tiers do not consume queue CPU.
     */
    memset(g_vc_mesh_xformed,0,(size_t)vcount);
    memset(g_vc_mesh_projected,0,(size_t)vcount);

    for(i=0;i<tcount&&*n<MAX_VC_DRAW_TRIS;++i){
        g_vc_frame_tested_tris++;
        const vc_map_tri_t *t=&tris[i];
        float light=0.70f+0.30f*((float)t->pad/255.0f);
        uint8_t object_fade=255U;
        vc_clip_v_t in[3],poly[12];
        sv3_t sp[12];
        uint16_t ids[3];
        int pc,j,k;

        if(t->a>=vcount||t->b>=vcount||t->c>=vcount||
           t->material>=map->material_count)continue;

        if(map->tri_object&&map->objects){
            ptrdiff_t ti=t-map->tris;
            if(ti>=0&&(uint32_t)ti<map->tri_count){
                uint16_t oid=map->tri_object[ti];
                if(oid<map->object_count){
                    const vc_stream_object_t *obj=&map->objects[oid];
                    object_fade=obj->alpha;
                    if(!obj->camera_visible)continue;
                    if(map->tri_lod && map->tri_lod[ti]!=obj->lod_selected){
                        g_vc_frame_lod_reject++;
                        continue;
                    }
                }
            }
            if(object_fade==0U)continue;
        }

        ids[0]=t->a;ids[1]=t->b;ids[2]=t->c;
        for(k=0;k<3;++k){
            uint16_t vi=ids[k];
            if(!g_vc_mesh_xformed[vi]){
                v3f_t q;
                q.x=verts[vi].x*scale;
                q.y=verts[vi].y*scale;
                q.z=verts[vi].z*scale;
                rv[vi]=q;
                g_vc_mesh_uv[vi].u=verts[vi].u;
                g_vc_mesh_uv[vi].v=verts[vi].v;
                city_world_to_camera_csp(
                    q.x,q.y,q.z,camx,camy,camz,
                    cam_cs,cam_sn,cam_cp,cam_sp,&cv[vi]);
                {
                    vc_clip_v_t cvonly;
                    cvonly.p=cv[vi];
                    cvonly.u=0.0f;cvonly.v=0.0f;
                    g_vc_mesh_outcode[vi]=(uint8_t)
                        vc_clip_outcode_textured(&cvonly);
                }
                g_vc_mesh_xformed[vi]=1U;
                g_vc_frame_xformed_vertices++;
            }
        }

        if(g_vc_backface_cull &&
           vc_triangle_backfacing(cv[t->a],cv[t->b],cv[t->c])){
            g_vc_frame_backface_reject++;
            continue;
        }

        in[0].p=cv[t->a];in[0].u=g_vc_mesh_uv[t->a].u;in[0].v=g_vc_mesh_uv[t->a].v;
        in[1].p=cv[t->b];in[1].u=g_vc_mesh_uv[t->b].u;in[1].v=g_vc_mesh_uv[t->b].v;
        in[2].p=cv[t->c];in[2].u=g_vc_mesh_uv[t->c].u;in[2].v=g_vc_mesh_uv[t->c].v;
        {
            unsigned oc0=g_vc_mesh_outcode[t->a];
            unsigned oc1=g_vc_mesh_outcode[t->b];
            unsigned oc2=g_vc_mesh_outcode[t->c];
            unsigned any=oc0|oc1|oc2;
            if((oc0&oc1&oc2)!=0U){
                g_vc_frame_clip_reject++;
                continue;
            }
            if(any==0U){
                uint16_t pvi[3]={t->a,t->b,t->c};
                poly[0]=in[0];poly[1]=in[1];poly[2]=in[2];
                pc=3;
                for(j=0;j<3;++j){
                    uint16_t vi=pvi[j];
                    if(!g_vc_mesh_projected[vi]){
                        city_project_camera(&cv[vi],&g_vc_mesh_proj[vi]);
                        g_vc_mesh_projected[vi]=1U;
                    }
                    sp[j]=g_vc_mesh_proj[vi];
                }
                g_vc_frame_clip_fast++;
            }else{
                pc=vc_clip_frustum_textured(in,poly);
                g_vc_frame_clip_partial++;
                if(pc<3){
                    g_vc_frame_clip_reject++;
                    continue;
                }
                for(j=0;j<pc;++j)city_project_camera(&poly[j].p,&sp[j]);
            }
        }

        for(j=1;j+1<pc&&*n<MAX_VC_DRAW_TRIS;++j){
            vc_textri_t *o;
            int x0=(int)sp[0].sx,y0=(int)sp[0].sy;
            int x1=(int)sp[j].sx,y1=(int)sp[j].sy;
            int x2=(int)sp[j+1].sx,y2=(int)sp[j+1].sy;
            int minx=x0,maxx=x0,miny=y0,maxy=y0;
            int32_t area;
            if(x1<minx)minx=x1;if(x2<minx)minx=x2;
            if(x1>maxx)maxx=x1;if(x2>maxx)maxx=x2;
            if(y1<miny)miny=y1;if(y2<miny)miny=y2;
            if(y1>maxy)maxy=y1;if(y2>maxy)maxy=y2;
            if(maxx<0||minx>=RW||maxy<0||miny>=RH)continue;
            area=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
            if(area>-2&&area<2)continue;
            {
                float avgz=(sp[0].z+sp[j].z+sp[j+1].z)*(1.0f/3.0f);
                float unit=map->world_scale>1.0f?map->world_scale:240.0f;
                const vc_material_t *qm=&map->materials[t->material];
                if(avgz>unit*72.0f && !(qm->flags&2U) &&
                   (maxx-minx)<=1 && (maxy-miny)<=1){
                    g_vc_frame_far_tiny_reject++;
                    continue;
                }
            }

            o=&g_vc_tex_out[*n];
            o->x0=x0;o->y0=y0;o->z0=sp[0].z;o->u0=poly[0].u;o->v0=poly[0].v;
            o->x1=x1;o->y1=y1;o->z1=sp[j].z;o->u1=poly[j].u;o->v1=poly[j].v;
            o->x2=x2;o->y2=y2;o->z2=sp[j+1].z;o->u2=poly[j+1].u;o->v2=poly[j+1].v;
            o->light=light;
            o->material=t->material;
            o->page_slot=page_slot;
            o->fade=object_fade;
            o->reserved=0U;
            /*
             * Keep world UV perspective-correct. Automatic affine/fog-flat
             * substitution caused visible texture swimming and made distant
             * facades look like they were changing texture rather than LOD.
             * RACER's explicit debug affine mode remains available.
             */
            o->pad=0U;
            (*n)++;
        }
    }
}


static void free_vc_map_struct(vc_runtime_map_t *m)
{
    if(!m)return;
    free(m->verts);
    free(m->tris);
    free(m->sectors);
    free(m->materials);
    free(m->atlas);
    free(m->tex_offsets);
    free(m->objects);
    free(m->tri_object);
    free(m->tri_lod);
    memset(m,0,sizeof(*m));
}

static void free_vc_map(void)
{
    free_vc_map_struct(&g_vc_map);
    if(!g_vc_world_mode)g_vc_city_mode=0;
}

static int vc_read_exact(FILE *fp,void *dst,size_t bytes)
{
    return bytes==0 || fread(dst,1,bytes,fp)==bytes;
}

static int vc_compact_map_textures(
    uint32_t material_count,uint32_t atlas_w,uint32_t atlas_h)
{
    uint32_t i;
    size_t total=0;
    uint16_t *packed;
    uint32_t *offs;

    if(!g_vc_map.atlas||!g_vc_map.materials||!material_count)return 0;
    offs=(uint32_t*)malloc((size_t)material_count*sizeof(uint32_t));
    if(!offs)return 0;
    for(i=0;i<material_count;++i)offs[i]=0xffffffffU;

    /* 64-byte starts keep each material friendly to the Cortex-A9 cache line. */
    for(i=0;i<material_count;++i){
        const vc_material_t *m=&g_vc_map.materials[i];
        if(!(m->flags&1U)||!m->w||!m->h)continue;
        total=(total+31U)&~(size_t)31U; /* 32 RGB1555 pixels = 64 bytes */
        if(total+(size_t)m->w*(size_t)m->h>8388608U){
            free(offs);return 0;
        }
        offs[i]=(uint32_t)total;
        total+=(size_t)m->w*(size_t)m->h;
    }
    if(!total){free(offs);return 0;}

    packed=(uint16_t*)malloc(total*sizeof(uint16_t));
    if(!packed){free(offs);return 0;}
    memset(packed,0,total*sizeof(uint16_t));

    for(i=0;i<material_count;++i){
        const vc_material_t *m=&g_vc_map.materials[i];
        uint16_t *dst;
        uint32_t y;
        if(offs[i]==0xffffffffU)continue;
        if((uint32_t)m->x+(uint32_t)m->w>atlas_w ||
           (uint32_t)m->y+(uint32_t)m->h>atlas_h){
            free(packed);free(offs);return 0;
        }
        dst=packed+offs[i];
        for(y=0;y<m->h;++y)
            memcpy(dst+(size_t)y*m->w,
                   g_vc_map.atlas+(size_t)(m->y+y)*atlas_w+m->x,
                   (size_t)m->w*sizeof(uint16_t));
    }

    free(g_vc_map.atlas);
    g_vc_map.atlas=packed;
    g_vc_map.tex_offsets=offs;
    g_vc_map.compact_texels=total;
    g_vc_map.compact_textures=1;
    return 1;
}

static int load_vc_map_file(const char *path)
{
    FILE *fp;
    vcmap_header_t h;
    uint32_t i;
    size_t atlas_pixels;
    int legacy_v2=0;

    if(!path||!*path)return 0;
    fp=fopen(path,"rb");
    if(!fp)return 0;

    memset(&h,0,sizeof(h));
    if(sizeof(h)!=72 || !vc_read_exact(fp,&h,sizeof(h))){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP reject %s: short/ABI header\n",path);
        return -1;
    }
    if(memcmp(h.magic,"VCM2",4)==0 && h.version==2)
        legacy_v2=1;
    else if(memcmp(h.magic,"VCM3",4)!=0 || h.version!=3){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP reject %s: expected VCM2/v2 or VCM3/v3\n",path);
        return -1;
    }

    atlas_pixels=(size_t)h.atlas_w*(size_t)h.atlas_h;
    if(!(h.world_scale>1.0f && h.world_scale<10000.0f) ||
       !(h.sector_m>1.0f && h.sector_m<10000.0f) ||
       h.vertex_count==0 || h.tri_count==0 || h.sector_count==0 ||
       h.vertex_count>1600000U || h.tri_count>3000000U ||
       h.sector_count>65535U || h.material_count==0 ||
       h.material_count>(legacy_v2?255U:1536U) ||
       h.atlas_w==0 || h.atlas_h==0 || h.atlas_w>2048U || h.atlas_h>2048U ||
       atlas_pixels>4194304U){
        fclose(fp);
        fprintf(stderr,"[racer] VCMAP reject %s: unsafe counts/range\n",path);
        return -1;
    }
    if(sizeof(vc_vertex_t)!=20 || sizeof(vc_tri_t)!=8 || sizeof(vc_map_tri_t)!=10 ||
       sizeof(vc_material_t)!=12 || sizeof(vc_sector_t)!=20){
        fclose(fp);
        fprintf(stderr,
            "[racer] VCMAP reject: unexpected ABI v=%u oldt=%u t=%u m=%u s=%u\n",
            (unsigned)sizeof(vc_vertex_t),(unsigned)sizeof(vc_tri_t),
            (unsigned)sizeof(vc_map_tri_t),(unsigned)sizeof(vc_material_t),
            (unsigned)sizeof(vc_sector_t));
        return -1;
    }

    free_vc_map();
    g_vc_map.verts=(vc_vertex_t*)calloc((size_t)h.vertex_count,sizeof(vc_vertex_t));
    g_vc_map.tris=(vc_map_tri_t*)calloc((size_t)h.tri_count,sizeof(vc_map_tri_t));
    g_vc_map.sectors=(vc_sector_t*)calloc((size_t)h.sector_count,sizeof(vc_sector_t));
    g_vc_map.materials=(vc_material_t*)calloc((size_t)h.material_count,sizeof(vc_material_t));
    g_vc_map.atlas=(uint16_t*)calloc(atlas_pixels,sizeof(uint16_t));
    if(!g_vc_map.verts||!g_vc_map.tris||!g_vc_map.sectors||
       !g_vc_map.materials||!g_vc_map.atlas){
        fclose(fp);free_vc_map();
        fprintf(stderr,"[racer] VCMAP reject %s: allocation failed\n",path);
        return -1;
    }

    if(!vc_read_exact(fp,g_vc_map.materials,(size_t)h.material_count*sizeof(vc_material_t)) ||
       !vc_read_exact(fp,g_vc_map.verts,(size_t)h.vertex_count*sizeof(vc_vertex_t))){
        fclose(fp);free_vc_map();
        fprintf(stderr,"[racer] VCMAP reject %s: truncated material/vertex payload\n",path);
        return -1;
    }

    if(legacy_v2){
        vc_tri_t *old=(vc_tri_t*)malloc((size_t)h.tri_count*sizeof(vc_tri_t));
        if(!old){
            fclose(fp);free_vc_map();return -1;
        }
        if(!vc_read_exact(fp,old,(size_t)h.tri_count*sizeof(vc_tri_t))){
            free(old);fclose(fp);free_vc_map();
            fprintf(stderr,"[racer] VCMAP2 reject %s: truncated triangle payload\n",path);
            return -1;
        }
        for(i=0;i<h.tri_count;++i){
            g_vc_map.tris[i].a=old[i].a;
            g_vc_map.tris[i].b=old[i].b;
            g_vc_map.tris[i].c=old[i].c;
            g_vc_map.tris[i].material=old[i].material;
            g_vc_map.tris[i].flags=old[i].flags;
            g_vc_map.tris[i].pad=0;
        }
        free(old);
    }else if(!vc_read_exact(fp,g_vc_map.tris,(size_t)h.tri_count*sizeof(vc_map_tri_t))){
        fclose(fp);free_vc_map();
        fprintf(stderr,"[racer] VCMAP3 reject %s: truncated triangle payload\n",path);
        return -1;
    }

    if(!vc_read_exact(fp,g_vc_map.sectors,(size_t)h.sector_count*sizeof(vc_sector_t)) ||
       !vc_read_exact(fp,g_vc_map.atlas,atlas_pixels*sizeof(uint16_t))){
        fclose(fp);free_vc_map();
        fprintf(stderr,"[racer] VCMAP reject %s: truncated sector/atlas payload\n",path);
        return -1;
    }
    fclose(fp);

    /*
     * City geometry is static. Convert positions to Racer world units once
     * instead of multiplying every visible vertex by world_scale every frame.
     * UVs remain untouched.
     */
    for(i=0;i<h.vertex_count;++i){
        g_vc_map.verts[i].x*=h.world_scale;
        g_vc_map.verts[i].y*=h.world_scale;
        g_vc_map.verts[i].z*=h.world_scale;
    }

    for(i=0;i<h.material_count;++i){
        const vc_material_t *m=&g_vc_map.materials[i];
        if((m->flags&1U) &&
           ((unsigned)m->x+(unsigned)m->w>h.atlas_w ||
            (unsigned)m->y+(unsigned)m->h>h.atlas_h ||
            m->w==0 || m->h==0)){
            fprintf(stderr,"[racer] VCMAP reject %s: invalid material %u atlas rect\n",
                    path,(unsigned)i);
            free_vc_map();return -1;
        }
    }

    for(i=0;i<h.sector_count;++i){
        const vc_sector_t *sec=&g_vc_map.sectors[i];
        uint32_t j;
        if(sec->vertex_base>h.vertex_count || sec->vertex_count>h.vertex_count-sec->vertex_base ||
           sec->tri_base>h.tri_count || sec->tri_count>h.tri_count-sec->tri_base ||
           sec->vertex_count>65535U || sec->vertex_count>MAX_MESH_VERTS){
            fprintf(stderr,"[racer] VCMAP reject %s: invalid sector %u ranges\n",path,(unsigned)i);
            free_vc_map();return -1;
        }
        for(j=0;j<sec->tri_count;++j){
            vc_map_tri_t *t=&g_vc_map.tris[sec->tri_base+j];
            if(t->a>=sec->vertex_count||t->b>=sec->vertex_count||t->c>=sec->vertex_count||
               t->material>=h.material_count){
                fprintf(stderr,"[racer] VCMAP reject %s: invalid triangle in sector %u\n",
                        path,(unsigned)i);
                free_vc_map();return -1;
            }
            {
                const vc_vertex_t *a=&g_vc_map.verts[sec->vertex_base+t->a];
                const vc_vertex_t *b=&g_vc_map.verts[sec->vertex_base+t->b];
                const vc_vertex_t *c=&g_vc_map.verts[sec->vertex_base+t->c];
                float ux=b->x-a->x,uy=b->y-a->y,uz=b->z-a->z;
                float vx=c->x-a->x,vy=c->y-a->y,vz=c->z-a->z;
                float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
                float mag=sqrtf(nx*nx+ny*ny+nz*nz);
                float dot=0.333333f;
                if(mag>0.001f)
                    dot=fabsf((nx*0.28f+ny*0.88f+nz*(-0.38f))/mag);
                if(dot<0.0f)dot=0.0f;if(dot>1.0f)dot=1.0f;
                t->pad=(uint8_t)(dot*255.0f+0.5f);
            }
        }
    }

    if(vc_compact_map_textures(h.material_count,h.atlas_w,h.atlas_h))
        fprintf(stderr,"[racer] VCMAP texture cache packed texels=%lu bytes=%lu\n",
                (unsigned long)g_vc_map.compact_texels,
                (unsigned long)(g_vc_map.compact_texels*sizeof(uint16_t)));

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
    g_vehicle_vlong=0.0f;
    g_vehicle_vlat=0.0f;
    g_vehicle_yaw_rate=0.0f;
    g_vehicle_steer_input=0.0f;
    g_position=0.0f;
    g_player_x=0.0f;
    g_camera_initialized=0;

    fprintf(stderr,
        "[racer] VCMAP%d loaded path=%s vertices=%u triangles=%u sectors=%u materials=%u atlas=%ux%u scale=%.1f spawn=%.0f,%.0f,%.0f\n",
        legacy_v2?2:3,path,(unsigned)h.vertex_count,(unsigned)h.tri_count,
        (unsigned)h.sector_count,(unsigned)h.material_count,
        (unsigned)h.atlas_w,(unsigned)h.atlas_h,
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



static void free_vc_collision_struct(vc_collision_runtime_t *c)
{
    if(!c)return;
    free(c->tris);
    free(c->spheres);
    free(c->sectors);
    free(c->sectors2);
    memset(c,0,sizeof(*c));
}

static void free_vc_collision(void)
{
    free_vc_collision_struct(&g_vc_collision);
}

static int load_vc_collision_file(const char *path)
{
    FILE *fp;
    char magic[4];
    uint32_t version,tri_count=0,sphere_count=0,sector_count=0,i;
    float world_scale,sector_m;

    if(!path||!*path)return 0;
    fp=fopen(path,"rb");
    if(!fp)return 0;

    if(!vc_read_exact(fp,magic,4) ||
       !vc_read_exact(fp,&version,4) ||
       !vc_read_exact(fp,&world_scale,4) ||
       !vc_read_exact(fp,&sector_m,4) ||
       !vc_read_exact(fp,&tri_count,4)){
        fclose(fp);return -1;
    }

    if(memcmp(magic,"VCC1",4)==0 && version==1){
        if(!vc_read_exact(fp,&sector_count,4)){
            fclose(fp);return -1;
        }
        if(!(world_scale>1.0f&&world_scale<10000.0f) ||
           !(sector_m>1.0f&&sector_m<10000.0f) ||
           tri_count==0 || tri_count>1000000U ||
           sector_count==0 || sector_count>65535U ||
           sizeof(vc_col_tri_t)!=40 || sizeof(vc_col_sector_t)!=12){
            fclose(fp);
            fprintf(stderr,"[racer] VCCOL1 reject %s: invalid header/ABI\n",path);
            return -1;
        }

        free_vc_collision();
        g_vc_collision.tris=(vc_col_tri_t*)calloc((size_t)tri_count,sizeof(vc_col_tri_t));
        g_vc_collision.sectors=(vc_col_sector_t*)calloc((size_t)sector_count,sizeof(vc_col_sector_t));
        if(!g_vc_collision.tris||!g_vc_collision.sectors){
            fclose(fp);free_vc_collision();return -1;
        }
        if(!vc_read_exact(fp,g_vc_collision.tris,(size_t)tri_count*sizeof(vc_col_tri_t)) ||
           !vc_read_exact(fp,g_vc_collision.sectors,(size_t)sector_count*sizeof(vc_col_sector_t))){
            fclose(fp);free_vc_collision();
            fprintf(stderr,"[racer] VCCOL1 reject %s: truncated payload\n",path);
            return -1;
        }
        fclose(fp);

        for(i=0;i<sector_count;++i){
            const vc_col_sector_t *sec=&g_vc_collision.sectors[i];
            if(sec->tri_base>tri_count || sec->tri_count>tri_count-sec->tri_base){
                fprintf(stderr,"[racer] VCCOL1 reject %s: bad sector %u\n",path,(unsigned)i);
                free_vc_collision();return -1;
            }
        }

        g_vc_collision.version=1;
        g_vc_collision.world_scale=world_scale;
        g_vc_collision.sector_m=sector_m;
        g_vc_collision.sector_world=world_scale*sector_m;
        g_vc_collision.tri_count=tri_count;
        g_vc_collision.sector_count=sector_count;
        g_vc_collision.loaded=1;
        fprintf(stderr,
            "[racer] VCCOL1 loaded path=%s triangles=%u sectors=%u scale=%.1f sector=%.1fm legacy-derived-flags\n",
            path,(unsigned)tri_count,(unsigned)sector_count,world_scale,sector_m);
        return 1;
    }

    if(memcmp(magic,"VCC2",4)==0 && version==2){
        if(!vc_read_exact(fp,&sphere_count,4) ||
           !vc_read_exact(fp,&sector_count,4)){
            fclose(fp);return -1;
        }
        if(!(world_scale>1.0f&&world_scale<10000.0f) ||
           !(sector_m>1.0f&&sector_m<10000.0f) ||
           tri_count>1000000U || sphere_count>1000000U ||
           ((tri_count||sphere_count) && sector_count==0) ||
           sector_count>65535U ||
           sizeof(vc_col_tri_t)!=40 ||
           sizeof(vc_col_sphere_t)!=20 ||
           sizeof(vc_col_sector2_t)!=20){
            fclose(fp);
            fprintf(stderr,"[racer] VCCOL2 reject %s: invalid header/ABI\n",path);
            return -1;
        }

        free_vc_collision();
        if(tri_count)
            g_vc_collision.tris=(vc_col_tri_t*)calloc((size_t)tri_count,sizeof(vc_col_tri_t));
        if(sphere_count)
            g_vc_collision.spheres=(vc_col_sphere_t*)calloc((size_t)sphere_count,sizeof(vc_col_sphere_t));
        if(sector_count)
            g_vc_collision.sectors2=(vc_col_sector2_t*)calloc((size_t)sector_count,sizeof(vc_col_sector2_t));
        if((tri_count&&!g_vc_collision.tris) ||
           (sphere_count&&!g_vc_collision.spheres) ||
           (sector_count&&!g_vc_collision.sectors2)){
            fclose(fp);free_vc_collision();return -1;
        }
        if((tri_count && !vc_read_exact(fp,g_vc_collision.tris,(size_t)tri_count*sizeof(vc_col_tri_t))) ||
           (sphere_count && !vc_read_exact(fp,g_vc_collision.spheres,(size_t)sphere_count*sizeof(vc_col_sphere_t))) ||
           (sector_count && !vc_read_exact(fp,g_vc_collision.sectors2,(size_t)sector_count*sizeof(vc_col_sector2_t)))){
            fclose(fp);free_vc_collision();
            fprintf(stderr,"[racer] VCCOL2 reject %s: truncated payload\n",path);
            return -1;
        }
        fclose(fp);

        for(i=0;i<sector_count;++i){
            const vc_col_sector2_t *sec=&g_vc_collision.sectors2[i];
            if(sec->tri_base>tri_count || sec->tri_count>tri_count-sec->tri_base ||
               sec->sphere_base>sphere_count || sec->sphere_count>sphere_count-sec->sphere_base){
                fprintf(stderr,"[racer] VCCOL2 reject %s: bad sector %u\n",path,(unsigned)i);
                free_vc_collision();return -1;
            }
        }

        g_vc_collision.version=2;
        g_vc_collision.world_scale=world_scale;
        g_vc_collision.sector_m=sector_m;
        g_vc_collision.sector_world=world_scale*sector_m;
        g_vc_collision.tri_count=tri_count;
        g_vc_collision.sphere_count=sphere_count;
        g_vc_collision.sector_count=sector_count;
        g_vc_collision.loaded=1;
        fprintf(stderr,
            "[racer] VCCOL2 loaded path=%s triangles=%u spheres=%u sectors=%u scale=%.1f sector=%.1fm gta-native-surfaces\n",
            path,(unsigned)tri_count,(unsigned)sphere_count,(unsigned)sector_count,
            world_scale,sector_m);
        return 1;
    }

    fclose(fp);
    fprintf(stderr,
        "[racer] VCCOL reject %s: unsupported magic/version %.4s/%u\n",
        path,magic,(unsigned)version);
    return -1;
}

static int try_load_vc_collision(void)
{
    const char *env=getenv("RACER_VCCOL");
    int r;
    if(env&&*env){
        r=load_vc_collision_file(env);
        if(r!=0)return r>0;
    }
    r=load_vc_collision_file("/mnt/usb/H3531/APPS/racer/VCCOL.BIN");
    if(r!=0)return r>0;
    r=load_vc_collision_file("VCCOL.BIN");
    if(r!=0)return r>0;
    fprintf(stderr,"[racer] VCCOL not found; visual-road fallback active\n");
    return 0;
}


typedef struct {
    char magic[4];
    uint32_t version,object_count,tri_count;
    float world_scale;
    uint32_t reserved;
} vcobj_header_t;

typedef struct {
    float cx,cy,cz,radius,draw_m;
    float lod_m[3];
    uint32_t flags,model_id;
} vcobj2_record_t;

static int load_vc_object_sidecar(const char *map_path,vc_runtime_map_t *map)
{
    char path[VC_WORLD_PAGE_PATH_MAX];
    const char *slash,*name;
    FILE *fp;
    vcobj_header_t h;
    uint32_t i;

    if(!map_path||!map||!map->tri_count)return 0;
    slash=strrchr(map_path,'/');
    if(!slash)slash=strrchr(map_path,'\\');
    name=slash?slash+1:map_path;
    if(strcmp(name,"VCMAP.BIN")!=0)return 0;

    if(slash){
        size_t n=(size_t)(slash-map_path+1);
        if(n+strlen("VCOBJ.BIN")+1>sizeof(path))return -1;
        memcpy(path,map_path,n);
        strcpy(path+n,"VCOBJ.BIN");
    }else{
        strcpy(path,"VCOBJ.BIN");
    }

    fp=fopen(path,"rb");
    if(!fp)return 0;
    memset(&h,0,sizeof(h));
    if(sizeof(h)!=24 || !vc_read_exact(fp,&h,sizeof(h)) ||
       memcmp(h.magic,"VCO2",4)!=0 || h.version!=2 ||
       h.tri_count!=map->tri_count || h.object_count==0 ||
       h.object_count>65535U ||
       !(h.world_scale>1.0f&&h.world_scale<10000.0f)){
        fclose(fp);
        fprintf(stderr,"[racer] VCOBJ reject path=%s VCO2 header/count mismatch\n",path);
        return -1;
    }

    map->objects=(vc_stream_object_t*)calloc(
        (size_t)h.object_count,sizeof(vc_stream_object_t));
    map->tri_object=(uint16_t*)malloc(
        (size_t)h.tri_count*sizeof(uint16_t));
    map->tri_lod=(uint8_t*)malloc((size_t)h.tri_count);
    if(!map->objects||!map->tri_object||!map->tri_lod){
        fclose(fp);
        free(map->objects);map->objects=NULL;
        free(map->tri_object);map->tri_object=NULL;
        free(map->tri_lod);map->tri_lod=NULL;
        return -1;
    }

    for(i=0;i<h.object_count;++i){
        vcobj2_record_t r;
        vc_stream_object_t *o=&map->objects[i];
        unsigned j,count=0;
        if(sizeof(r)!=40 || !vc_read_exact(fp,&r,sizeof(r))){
            fclose(fp);free(map->objects);map->objects=NULL;
            free(map->tri_object);map->tri_object=NULL;
            free(map->tri_lod);map->tri_lod=NULL;return -1;
        }
        o->cx=r.cx*h.world_scale;
        o->cy=r.cy*h.world_scale;
        o->cz=r.cz*h.world_scale;
        o->radius=r.radius*h.world_scale;
        o->draw_world=r.draw_m>0.0f?r.draw_m*h.world_scale:0.0f;
        for(j=0;j<3U;++j){
            o->lod_world[j]=r.lod_m[j]>0.0f?r.lod_m[j]*h.world_scale:0.0f;
            if(r.lod_m[j]>0.0f)count=j+1U;
        }
        if(count==0U){
            count=1U;
            o->lod_world[0]=o->draw_world;
        }
        o->lod_count=(uint8_t)count;
        o->lod_selected=0U;
        o->flags=r.flags;o->model_id=r.model_id;
        o->alpha=0;o->active=0;
    }
    if(!vc_read_exact(fp,map->tri_object,
                      (size_t)h.tri_count*sizeof(uint16_t)) ||
       !vc_read_exact(fp,map->tri_lod,(size_t)h.tri_count)){
        fclose(fp);free(map->objects);map->objects=NULL;
        free(map->tri_object);map->tri_object=NULL;
        free(map->tri_lod);map->tri_lod=NULL;return -1;
    }
    fclose(fp);
    for(i=0;i<h.tri_count;++i){
        uint16_t oid=map->tri_object[i];
        if(oid>=h.object_count ||
           map->tri_lod[i]>=map->objects[oid].lod_count){
            fprintf(stderr,
                "[racer] VCOBJ reject path=%s tri=%u object=%u/%u lod=%u/%u\n",
                path,(unsigned)i,(unsigned)oid,(unsigned)h.object_count,
                (unsigned)map->tri_lod[i],
                oid<h.object_count?(unsigned)map->objects[oid].lod_count:0U);
            free(map->objects);map->objects=NULL;
            free(map->tri_object);map->tri_object=NULL;
            free(map->tri_lod);map->tri_lod=NULL;
            return -1;
        }
    }
    map->object_count=h.object_count;
    map->object_last_ns=mono_ns();
    map->object_last_frame=0xffffffffU;
    map->object_visibility_frame=0xffffffffU;
    fprintf(stderr,
        "[racer] VCOBJ loaded path=%s objects=%u triangles=%u lod=object-tier-v2 fade=activation-only\n",
        path,(unsigned)h.object_count,(unsigned)h.tri_count);
    return 1;
}

static int load_vc_map_detached(const char *path,vc_runtime_map_t *out)
{
    vc_runtime_map_t saved_map=g_vc_map;
    int saved_city=g_vc_city_mode,saved_osm=g_osm_city_mode,saved_world_mode=g_vc_world_mode;
    float sx=g_world_x,sy=g_world_y,sz=g_world_z,sg=g_vc_ground_y;
    float sh=g_vehicle_heading,ss=g_speed,svl=g_vehicle_vlong,svt=g_vehicle_vlat;
    float syr=g_vehicle_yaw_rate,ssi=g_vehicle_steer_input,sp=g_position,sxp=g_player_x;
    int sci=g_camera_initialized;
    int r;

    memset(&g_vc_map,0,sizeof(g_vc_map));
    g_vc_world_mode=0;
    r=load_vc_map_file(path);
    if(r>0){
        int orc;
        *out=g_vc_map;
        memset(&g_vc_map,0,sizeof(g_vc_map));
        orc=load_vc_object_sidecar(path,out);
        if(orc<0){
            free_vc_map_struct(out);
            r=-1;
        }
    }else{
        free_vc_map_struct(&g_vc_map);
    }
    g_vc_map=saved_map;
    g_vc_city_mode=saved_city;g_osm_city_mode=saved_osm;g_vc_world_mode=saved_world_mode;
    g_world_x=sx;g_world_y=sy;g_world_z=sz;g_vc_ground_y=sg;
    g_vehicle_heading=sh;g_speed=ss;g_vehicle_vlong=svl;g_vehicle_vlat=svt;
    g_vehicle_yaw_rate=syr;g_vehicle_steer_input=ssi;g_position=sp;g_player_x=sxp;
    g_camera_initialized=sci;
    return r;
}

static int load_vc_collision_detached(const char *path,vc_collision_runtime_t *out)
{
    vc_collision_runtime_t saved=g_vc_collision;
    int r;
    memset(&g_vc_collision,0,sizeof(g_vc_collision));
    r=load_vc_collision_file(path);
    if(r>0){
        *out=g_vc_collision;
        memset(&g_vc_collision,0,sizeof(g_vc_collision));
    }else{
        free_vc_collision_struct(&g_vc_collision);
    }
    g_vc_collision=saved;
    return r;
}

static int vc_world_find_entry(int px,int py)
{
    uint32_t i;
    if(!g_vc_world.loaded||!g_vc_world.entries)return -1;
    for(i=0;i<g_vc_world.page_count;++i)
        if(g_vc_world.entries[i].page_x==px&&g_vc_world.entries[i].page_y==py)
            return (int)i;
    return -1;
}

static void vc_world_free_slot(int slot)
{
    vc_world_page_t *p;
    if(slot<0||slot>=VC_WORLD_CACHE_SLOTS)return;
    p=&g_vc_world.pages[slot];
    free_vc_map_struct(&p->map);
    free_vc_map_struct(&p->base);
    free_vc_collision_struct(&p->collision);
    memset(p,0,sizeof(*p));
    p->entry_index=-1;
}

static void free_vc_world(void)
{
    int i;
    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i)vc_world_free_slot(i);
    free(g_vc_world.entries);
    memset(&g_vc_world,0,sizeof(g_vc_world));
    g_vc_world_mode=0;
}

static int vc_world_load_slot(int slot,int px,int py,int load_detail)
{
    vc_world_page_t *p;
    const vc_runtime_map_t *scale_map=NULL;
    int ei;
    char map_path[VC_WORLD_PAGE_PATH_MAX];
    char base_path[VC_WORLD_PAGE_PATH_MAX];
    char col_path[VC_WORLD_PAGE_PATH_MAX];
    int mr=0,br=0,cr;

    if(slot<0||slot>=VC_WORLD_CACHE_SLOTS)return 0;
    ei=vc_world_find_entry(px,py);
    if(ei<0)return 0;
    p=&g_vc_world.pages[slot];
    vc_world_free_slot(slot);

    snprintf(map_path,sizeof(map_path),"%s/pages/P_%d_%d/VCMAP.BIN",
             g_vc_world.base_dir,px,py);
    snprintf(base_path,sizeof(base_path),"%s/pages/P_%d_%d/VCBASE.BIN",
             g_vc_world.base_dir,px,py);
    snprintf(col_path,sizeof(col_path),"%s/pages/P_%d_%d/VCCOL.BIN",
             g_vc_world.base_dir,px,py);

    cr=load_vc_collision_detached(col_path,&p->collision);
    if(cr<0){
        fprintf(stderr,"[racer] VFW page collision rejected page=%d,%d path=%s\n",px,py,col_path);
        vc_world_free_slot(slot);
        return 0;
    }
    if(cr==0){
        /*
         * VCC2 explicitly supports a valid zero-primitive file. A MISSING file
         * is therefore never an "empty collision page" in object-stream v3:
         * it means the local world pack/deploy is incomplete. Failing closed
         * here is safer than silently creating a hole under the vehicle.
         */
        fprintf(stderr,
            "[racer] VFW_FATAL page collision file missing page=%d,%d path=%s\n",
            px,py,col_path);
        vc_world_free_slot(slot);
        return 0;
    }

    br=load_vc_map_detached(base_path,&p->base);
    if(br<0){
        fprintf(stderr,"[racer] VFW base map rejected page=%d,%d path=%s\n",px,py,base_path);
        vc_world_free_slot(slot);
        return 0;
    }
    p->base_loaded=br>0;

    /*
     * New gta-base-detail packs bring collision + persistent world in first.
     * Old packs have no VCBASE.BIN, so keep their original synchronous map
     * behaviour and do not fade them.
     */
    if(!g_vc_world.split_layout || load_detail){
        mr=load_vc_map_detached(map_path,&p->map);
        if(mr<0 || (!p->base_loaded && mr<=0)){
            fprintf(stderr,"[racer] VFW detail map load failed page=%d,%d path=%s\n",px,py,map_path);
            vc_world_free_slot(slot);
            return 0;
        }
        if(mr>0){
            p->detail_state=1;
            p->detail_loaded_ns=p->base_loaded?mono_ns():0ULL;
        }else{
            p->detail_state=-1;
        }
    }else{
        p->detail_state=0;
        p->detail_loaded_ns=0ULL;
    }

    scale_map=p->base_loaded?&p->base:(p->detail_state==1?&p->map:NULL);
    if(fabsf(p->collision.sector_m-g_vc_world.sector_m)>0.01f ||
       (scale_map && fabsf(scale_map->sector_m-g_vc_world.sector_m)>0.01f) ||
       (scale_map && fabsf(scale_map->world_scale-p->collision.world_scale)>0.01f)){
        fprintf(stderr,"[racer] VFW page scale mismatch page=%d,%d\n",px,py);
        vc_world_free_slot(slot);
        return 0;
    }

    p->loaded=1;
    p->page_x=px;p->page_y=py;p->entry_index=ei;
    if(g_vc_world.world_scale<=1.0f){
        g_vc_world.world_scale=p->collision.world_scale;
        g_vc_world.sector_world=p->collision.sector_world;
    }
    fprintf(stderr,
        "[racer] VFW page resident slot=%d page=%d,%d base=%s detail=%s "
        "col=%u/%u base_t=%u detail_t=%u objects=%u\n",
        slot,px,py,p->base_loaded?"yes":"legacy-none",
        p->detail_state==1?"loaded":(p->detail_state==0?"pending":"none"),
        (unsigned)p->collision.tri_count,(unsigned)p->collision.sphere_count,
        (unsigned)(p->base_loaded?p->base.tri_count:0U),
        (unsigned)(p->detail_state==1?p->map.tri_count:0U),
        (unsigned)(p->detail_state==1?p->map.object_count:0U));
    return 1;
}

static int vc_world_load_detail_slot(int slot)
{
    vc_world_page_t *p;
    char map_path[VC_WORLD_PAGE_PATH_MAX];
    int mr;
    if(slot<0||slot>=VC_WORLD_CACHE_SLOTS)return 0;
    p=&g_vc_world.pages[slot];
    if(!p->loaded || p->detail_state!=0)return 1;

    snprintf(map_path,sizeof(map_path),"%s/pages/P_%d_%d/VCMAP.BIN",
             g_vc_world.base_dir,p->page_x,p->page_y);
    mr=load_vc_map_detached(map_path,&p->map);
    if(mr<=0){
        p->detail_state=-1;
        p->detail_loaded_ns=0ULL;
        fprintf(stderr,"[racer] VFW detail absent/rejected slot=%d page=%d,%d path=%s\n",
                slot,p->page_x,p->page_y,map_path);
        return mr==0;
    }
    if(fabsf(p->map.sector_m-g_vc_world.sector_m)>0.01f ||
       (p->base_loaded&&fabsf(p->map.world_scale-p->base.world_scale)>0.01f)){
        fprintf(stderr,"[racer] VFW detail scale mismatch slot=%d page=%d,%d\n",
                slot,p->page_x,p->page_y);
        free_vc_map_struct(&p->map);
        p->detail_state=-1;
        return 0;
    }
    p->detail_state=1;
    p->detail_loaded_ns=mono_ns();
    fprintf(stderr,
        "[racer] VFW detail streamed slot=%d page=%d,%d v=%u t=%u objects=%u fade=object-alpha prefetch=%.0fm\n",
        slot,p->page_x,p->page_y,
        (unsigned)p->map.vertex_count,(unsigned)p->map.tri_count,
        (unsigned)p->map.object_count,
        (double)(VC_FAR_CLIP_M+VC_DETAIL_PREFETCH_M));
    return 1;
}

static void vc_world_unload_detail_slot(int slot,const char *reason)
{
    vc_world_page_t *p;
    if(slot<0||slot>=VC_WORLD_CACHE_SLOTS)return;
    p=&g_vc_world.pages[slot];
    if(!p->loaded||p->detail_state!=1)return;
    fprintf(stderr,
        "[racer] VFW detail freed slot=%d page=%d,%d v=%u t=%u reason=%s\n",
        slot,p->page_x,p->page_y,
        (unsigned)p->map.vertex_count,(unsigned)p->map.tri_count,
        reason?reason:"far");
    free_vc_map_struct(&p->map);
    p->detail_state=0;
    p->detail_loaded_ns=0ULL;
}

static int vc_world_rebuild_collision(void)
{
    uint32_t tt=0,ss=0,cc=0;
    uint32_t tb=0,sb=0,cb=0;
    int i;
    float scale=0.0f,sector=0.0f;

    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
        vc_world_page_t *p=&g_vc_world.pages[i];
        if(!p->loaded||!p->collision.loaded)continue;
        if(p->collision.version!=2){
            fprintf(stderr,"[racer] VFW requires VCC2 pages; slot=%d version=%u\n",
                    i,(unsigned)p->collision.version);
            return 0;
        }
        tt+=p->collision.tri_count;
        ss+=p->collision.sphere_count;
        cc+=p->collision.sector_count;
        if(scale<=1.0f){scale=p->collision.world_scale;sector=p->collision.sector_m;}
    }

    free_vc_collision();
    if(tt)g_vc_collision.tris=(vc_col_tri_t*)malloc((size_t)tt*sizeof(vc_col_tri_t));
    if(ss)g_vc_collision.spheres=(vc_col_sphere_t*)malloc((size_t)ss*sizeof(vc_col_sphere_t));
    if(cc)g_vc_collision.sectors2=(vc_col_sector2_t*)malloc((size_t)cc*sizeof(vc_col_sector2_t));
    if((tt&&!g_vc_collision.tris)||(ss&&!g_vc_collision.spheres)||(cc&&!g_vc_collision.sectors2)){
        free_vc_collision();
        fprintf(stderr,"[racer] VFW collision merge allocation failed t=%u s=%u sectors=%u\n",
                (unsigned)tt,(unsigned)ss,(unsigned)cc);
        return 0;
    }

    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
        vc_world_page_t *p=&g_vc_world.pages[i];
        uint32_t j;
        if(!p->loaded||!p->collision.loaded)continue;
        if(p->collision.tri_count){
            memcpy(g_vc_collision.tris+tb,p->collision.tris,
                   (size_t)p->collision.tri_count*sizeof(vc_col_tri_t));
        }
        if(p->collision.sphere_count){
            memcpy(g_vc_collision.spheres+sb,p->collision.spheres,
                   (size_t)p->collision.sphere_count*sizeof(vc_col_sphere_t));
        }
        for(j=0;j<p->collision.sector_count;++j){
            vc_col_sector2_t q=p->collision.sectors2[j];
            q.tri_base+=tb;
            q.sphere_base+=sb;
            g_vc_collision.sectors2[cb++]=q;
        }
        tb+=p->collision.tri_count;
        sb+=p->collision.sphere_count;
    }

    g_vc_collision.version=2;
    g_vc_collision.world_scale=scale>1.0f?scale:240.0f;
    g_vc_collision.sector_m=sector>1.0f?sector:g_vc_world.sector_m;
    g_vc_collision.sector_world=g_vc_collision.world_scale*g_vc_collision.sector_m;
    g_vc_collision.tri_count=tt;
    g_vc_collision.sphere_count=ss;
    g_vc_collision.sector_count=cc;
    g_vc_collision.loaded=1;
    fprintf(stderr,
        "[racer] VFW collision window merged triangles=%u spheres=%u sectors=%u\n",
        (unsigned)tt,(unsigned)ss,(unsigned)cc);
    return 1;
}

static int vc_world_page_needed(int px,int py,const int *need_x,const int *need_y,int need_n)
{
    int n;
    for(n=0;n<need_n;++n)
        if(need_x[n]==px&&need_y[n]==py)return 1;
    return 0;
}

static int vc_world_refresh_cache(int center_px,int center_py)
{
    int need_x[VC_WORLD_CACHE_SLOTS],need_y[VC_WORLD_CACHE_SLOTS];
    int need_n=0,dx,dy,i,n,loaded=0;

    if(!g_vc_world.loaded)return 0;
    for(dy=-VC_WORLD_CACHE_RADIUS;dy<=VC_WORLD_CACHE_RADIUS;++dy)
    for(dx=-VC_WORLD_CACHE_RADIUS;dx<=VC_WORLD_CACHE_RADIUS;++dx){
        int px=center_px+dx,py=center_py+dy;
        if(vc_world_find_entry(px,py)<0)continue;
        need_x[need_n]=px;need_y[need_n]=py;need_n++;
    }

    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i)
        if(g_vc_world.pages[i].loaded)vc_world_free_slot(i);

    /*
     * Startup is intentionally base-first: collision and GTA persistent world
     * are ready for the bounded 5x5 local safety window, while only the centre
     * detail page is loaded immediately. Remaining small detail pages stream
     * nearest-first after gameplay starts.
     */
    for(n=0;n<need_n;++n){
        int load_detail=(need_x[n]==center_px&&need_y[n]==center_py);
        if(!vc_world_load_slot(n,need_x[n],need_y[n],load_detail))return 0;
    }

    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i)if(g_vc_world.pages[i].loaded)loaded++;
    if(!loaded)return 0;
    if(!vc_world_rebuild_collision())return 0;
    g_vc_world.center_page_x=center_px;
    g_vc_world.center_page_y=center_py;
    fprintf(stderr,
        "[racer] VFW startup window center=%d,%d resident=%d/%d radius=%d page=%.0fm detail=center-first\n",
        center_px,center_py,loaded,need_n,VC_WORLD_CACHE_RADIUS,(double)g_vc_world.page_m);
    return 1;
}

static int vc_world_stream_step(int center_px,int center_py)
{
    int need_x[VC_WORLD_CACHE_SLOTS],need_y[VC_WORLD_CACHE_SLOTS];
    int need_n=0,dx,dy,i,n;
    int missing_n=-1,missing_score=999,replace_slot=-1;
    int pending_slot=-1;
    float pending_d2=1.0e30f;
    int far_detail_slot=-1;
    float far_detail_d2=-1.0f;
    int obsolete_slot=-1;

    if(!g_vc_world.loaded)return 0;
    for(dy=-VC_WORLD_CACHE_RADIUS;dy<=VC_WORLD_CACHE_RADIUS;++dy)
    for(dx=-VC_WORLD_CACHE_RADIUS;dx<=VC_WORLD_CACHE_RADIUS;++dx){
        int px=center_px+dx,py=center_py+dy;
        if(vc_world_find_entry(px,py)<0)continue;
        need_x[need_n]=px;need_y[need_n]=py;need_n++;
    }

    /* Find the closest desired page that is not resident yet. */
    for(n=0;n<need_n;++n){
        int found=0;
        int sx=need_x[n]-center_px,sy=need_y[n]-center_py;
        int score=sx*sx+sy*sy;
        for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
            vc_world_page_t *p=&g_vc_world.pages[i];
            if(p->loaded&&p->page_x==need_x[n]&&p->page_y==need_y[n]){
                found=1;break;
            }
        }
        if(!found&&score<missing_score){missing_score=score;missing_n=n;}
    }

    if(missing_n>=0){
        /* Prefer an empty slot, otherwise replace only one obsolete page. */
        for(i=0;i<VC_WORLD_CACHE_SLOTS;++i)
            if(!g_vc_world.pages[i].loaded){replace_slot=i;break;}
        if(replace_slot<0){
            for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
                vc_world_page_t *p=&g_vc_world.pages[i];
                if(p->loaded&&!vc_world_page_needed(
                       p->page_x,p->page_y,need_x,need_y,need_n)){
                    replace_slot=i;break;
                }
            }
        }
        if(replace_slot>=0){
            vc_world_free_slot(replace_slot);
            if(!vc_world_load_slot(
                    replace_slot,need_x[missing_n],need_y[missing_n],0))
                return 0;
            if(!vc_world_rebuild_collision())return 0;
            fprintf(stderr,
                "[racer] VFW stream resident center=%d,%d slot=%d page=%d,%d budget=1 cache=%dx%d\n",
                center_px,center_py,replace_slot,
                need_x[missing_n],need_y[missing_n],
                VC_WORLD_CACHE_SIDE,VC_WORLD_CACHE_SIDE);
        }
        g_vc_world.center_page_x=center_px;
        g_vc_world.center_page_y=center_py;
        return 1;
    }

    /*
     * Detail residency is tighter than collision/base residency. Free at most
     * one far detail page per update. Keeping collision/base in the 5x5 safety
     * window avoids physics holes while visual memory follows the player.
     */
    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
        vc_world_page_t *p=&g_vc_world.pages[i];
        float scale=g_vc_world.world_scale>1.0f?g_vc_world.world_scale:240.0f;
        float gx=g_world_x/scale,gz=g_world_z/scale;
        float x0=(float)p->page_x*g_vc_world.page_m;
        float z0=(float)p->page_y*g_vc_world.page_m;
        float x1=x0+g_vc_world.page_m,z1=z0+g_vc_world.page_m;
        float ddx=0.0f,ddz=0.0f,d2;
        if(!p->loaded||p->detail_state!=1)continue;
        if(gx<x0)ddx=x0-gx;else if(gx>x1)ddx=gx-x1;
        if(gz<z0)ddz=z0-gz;else if(gz>z1)ddz=gz-z1;
        d2=ddx*ddx+ddz*ddz;
        if(d2>VC_DETAIL_EVICT_M*VC_DETAIL_EVICT_M && d2>far_detail_d2){
            far_detail_d2=d2;
            far_detail_slot=i;
        }
    }
    if(far_detail_slot>=0){
        vc_world_unload_detail_slot(far_detail_slot,"distance");
        g_vc_world.center_page_x=center_px;
        g_vc_world.center_page_y=center_py;
        return 1;
    }

    /* All required collision/base pages are resident; stream one nearby detail page. */
    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
        vc_world_page_t *p=&g_vc_world.pages[i];
        float scale=g_vc_world.world_scale>1.0f?g_vc_world.world_scale:240.0f;
        float gx=g_world_x/scale,gz=g_world_z/scale;
        float x0=(float)p->page_x*g_vc_world.page_m;
        float z0=(float)p->page_y*g_vc_world.page_m;
        float x1=x0+g_vc_world.page_m,z1=z0+g_vc_world.page_m;
        float ddx=0.0f,ddz=0.0f,d2;
        float request=VC_FAR_CLIP_M+VC_DETAIL_PREFETCH_M;
        if(!p->loaded||p->detail_state!=0)continue;
        if(!vc_world_page_needed(p->page_x,p->page_y,need_x,need_y,need_n))continue;
        if(gx<x0)ddx=x0-gx;else if(gx>x1)ddx=gx-x1;
        if(gz<z0)ddz=z0-gz;else if(gz>z1)ddz=gz-z1;
        d2=ddx*ddx+ddz*ddz;
        if(d2>request*request)continue;
        if(d2<pending_d2){pending_d2=d2;pending_slot=i;}
    }
    if(pending_slot>=0){
        if(!vc_world_load_detail_slot(pending_slot))return 0;
        g_vc_world.center_page_x=center_px;
        g_vc_world.center_page_y=center_py;
        return 1;
    }

    /* At world edges the bounded local target may contain fewer pages. */
    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
        vc_world_page_t *p=&g_vc_world.pages[i];
        if(p->loaded&&!vc_world_page_needed(p->page_x,p->page_y,need_x,need_y,need_n)){
            obsolete_slot=i;break;
        }
    }
    if(obsolete_slot>=0){
        vc_world_free_slot(obsolete_slot);
        if(!vc_world_rebuild_collision())return 0;
    }

    g_vc_world.center_page_x=center_px;
    g_vc_world.center_page_y=center_py;
    return 1;
}

static int load_vc_world_index_file(const char *path)
{
    FILE *fp;
    vcworld_header_t h;
    vcworld_entry_t *entries;
    char tmp[VC_WORLD_PAGE_PATH_MAX];
    char *slash;
    uint32_t i;

    fp=fopen(path,"rb");
    if(!fp)return 0;
    memset(&h,0,sizeof(h));
    if(sizeof(h)!=56 || sizeof(vcworld_entry_t)!=40 ||
       !vc_read_exact(fp,&h,sizeof(h))){
        fclose(fp);return -1;
    }
    if(memcmp(h.magic,"VFW1",4)!=0||h.version!=1||
       !(h.page_m>=64.0f&&h.page_m<=1024.0f)||
       !(h.sector_m>=4.0f&&h.sector_m<=128.0f)||
       h.page_count==0||h.page_count>4096U){
        fclose(fp);
        fprintf(stderr,"[racer] VFW reject %s: invalid header\n",path);
        return -1;
    }
    entries=(vcworld_entry_t*)calloc((size_t)h.page_count,sizeof(vcworld_entry_t));
    if(!entries){fclose(fp);return -1;}
    if(!vc_read_exact(fp,entries,(size_t)h.page_count*sizeof(vcworld_entry_t))){
        free(entries);fclose(fp);return -1;
    }
    fclose(fp);
    for(i=0;i<h.page_count;++i){
        if(entries[i].atlas_w>2048U||entries[i].atlas_h>2048U||
           entries[i].materials>1536U||entries[i].vertices>1600000U||
           entries[i].triangles>3000000U){
            fprintf(stderr,"[racer] VFW reject %s: unsafe page entry %u\n",
                    path,(unsigned)i);
            free(entries);return -1;
        }
    }

    free_vc_world();
    g_vc_world.loaded=1;
    g_vc_world.page_m=h.page_m;
    g_vc_world.sector_m=h.sector_m;
    g_vc_world.min_x=h.min_x;g_vc_world.min_y=h.min_y;
    g_vc_world.max_x=h.max_x;g_vc_world.max_y=h.max_y;
    g_vc_world.min_page_x=h.min_page_x;g_vc_world.max_page_x=h.max_page_x;
    g_vc_world.min_page_y=h.min_page_y;g_vc_world.max_page_y=h.max_page_y;
    g_vc_world.center_page_x=0x3fffffff;
    g_vc_world.center_page_y=0x3fffffff;
    g_vc_world.stream_layout_version=(int)h.reserved;
    if(g_vc_world.stream_layout_version!=4){
        fprintf(stderr,
            "[racer] VFW reject %s: object-LOD layout v4 required, found %u\n",
            path,(unsigned)h.reserved);
        free(entries);
        memset(&g_vc_world,0,sizeof(g_vc_world));
        return -1;
    }
    g_vc_world.split_layout=1;
    g_vc_world.page_count=h.page_count;
    g_vc_world.entries=entries;
    for(i=0;i<VC_WORLD_CACHE_SLOTS;++i)g_vc_world.pages[i].entry_index=-1;

    snprintf(tmp,sizeof(tmp),"%s",path);
    slash=strrchr(tmp,'/');
    if(!slash)slash=strrchr(tmp,'\\');
    if(slash)*slash='\0';else snprintf(tmp,sizeof(tmp),".");
    snprintf(g_vc_world.base_dir,sizeof(g_vc_world.base_dir),"%s",tmp);

    fprintf(stderr,
        "[racer] VFW1 index loaded path=%s pages=%u page=%.0f sector=%.0f bounds=%.0f,%.0f..%.0f,%.0f layout=object-lod-v%d cache=%dx%d\n",
        path,(unsigned)h.page_count,h.page_m,h.sector_m,
        h.min_x,h.min_y,h.max_x,h.max_y,
        g_vc_world.stream_layout_version,
        VC_WORLD_CACHE_SIDE,VC_WORLD_CACHE_SIDE);
    return 1;
}

static int try_load_vc_world(void)
{
    const char *env=getenv("RACER_VCWORLD");
    const char *candidates[3];
    int ci,r=0,had_legacy=g_vc_city_mode&&g_vc_map.world_scale>1.0f;
    float old_x=g_world_x,old_y=g_world_y,old_z=g_world_z;
    float legacy_scale=g_vc_map.world_scale;
    int px=3,py=-2;
    const char *start=getenv("RACER_VC_START_PAGE");

    candidates[0]=(env&&*env)?env:"";
    candidates[1]="/mnt/usb/H3531/APPS/racer/VCWORLD.BIN";
    candidates[2]="VCWORLD.BIN";
    for(ci=0;ci<3;++ci){
        if(!candidates[ci][0])continue;
        r=load_vc_world_index_file(candidates[ci]);
        if(r!=0)break;
    }
    if(r<0){
        fprintf(stderr,"[racer] VFW_FATAL index rejected; refusing global VCMAP fallback\n");
        return -1;
    }
    if(r==0)return 0;

    if(start&&*start){
        int a,b;
        if(sscanf(start,"%d,%d",&a,&b)==2){px=a;py=b;}
    }else if(had_legacy){
        px=(int)floorf((old_x/legacy_scale)/g_vc_world.page_m);
        py=(int)floorf((old_z/legacy_scale)/g_vc_world.page_m);
    }else if(vc_world_find_entry(px,py)<0){
        uint32_t i,best=0;
        for(i=1;i<g_vc_world.page_count;++i)
            if(g_vc_world.entries[i].instances>g_vc_world.entries[best].instances)best=i;
        px=g_vc_world.entries[best].page_x;py=g_vc_world.entries[best].page_y;
    }

    if(!vc_world_refresh_cache(px,py)){
        fprintf(stderr,
            "[racer] VFW_FATAL initial cache load failed center=%d,%d; "
            "refusing global VCMAP fallback\n",px,py);
        free_vc_world();
        return -1;
    }

    free_vc_map_struct(&g_vc_map);
    g_vc_world_mode=1;
    g_vc_city_mode=1;
    g_osm_city_mode=0;

    if(had_legacy){
        g_world_x=old_x;g_world_y=old_y;g_world_z=old_z;
    }else{
        int i;
        for(i=0;i<VC_WORLD_CACHE_SLOTS;++i){
            vc_world_page_t *p=&g_vc_world.pages[i];
            if(p->loaded&&p->page_x==px&&p->page_y==py){
                g_world_x=p->map.spawn_x;g_world_y=p->map.spawn_y;g_world_z=p->map.spawn_z;
                g_vehicle_heading=p->map.spawn_yaw;
                break;
            }
        }
    }
    g_vc_ground_y=g_world_y;
    g_speed=0.0f;g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;
    vc_reset_turn_world();g_vc_body_basis_valid=0;g_vehicle_steer_input=0.0f;
    g_vc_raw_steer_input=0.0f;
    g_vc_two_wheel_ticks=0;
    g_camera_initialized=0;
    fprintf(stderr,
        "[racer] VFW1 runtime active center=%d,%d world=%.0f,%.0f,%.0f scale=%.1f\n",
        px,py,g_world_x,g_world_y,g_world_z,g_vc_world.world_scale);
    return 1;
}

static int vc_world_stream_update(int force)
{
    int px,py;
    float scale;
    if(!g_vc_world_mode||!g_vc_world.loaded)return 0;
    scale=g_vc_world.world_scale>1.0f?g_vc_world.world_scale:240.0f;
    px=(int)floorf((g_world_x/scale)/g_vc_world.page_m);
    py=(int)floorf((g_world_z/scale)/g_vc_world.page_m);
    if(force)return vc_world_refresh_cache(px,py);
    return vc_world_stream_step(px,py);
}

static void vc_apply_handling_profile(
    const char *name,
    float mass,float dim_x,float dim_y,float dim_z,
    float com_x,float com_y,float com_z,
    float traction_mult,float traction_loss,float traction_bias,
    float max_velocity_kmh,float engine_accel_raw,
    float brake_decel_raw,float brake_bias,float steering_lock_deg,
    float suspension_force,float suspension_damping,
    float suspension_upper,float suspension_lower,
    float suspension_bias,float suspension_antidive,
    uint32_t gears,uint32_t drive_type,uint32_t engine_type,uint32_t abs_enabled,
    uint32_t flags)
{
    float scale=vc_runtime_world_scale();
    float area;
    float drive_div;

    if(scale<=1.0f)scale=240.0f;
    if(mass<50.0f)mass=1400.0f;
    if(dim_x<=0.1f)dim_x=1.8f;
    if(dim_y<=0.1f)dim_y=4.2f;
    if(dim_z<=0.1f)dim_z=1.35f;

    g_vehicle_handling.mass=mass;
    g_vehicle_handling.traction_mult=fmaxf(0.25f,traction_mult);
    g_vehicle_handling.traction_loss=fmaxf(0.20f,traction_loss);
    g_vehicle_handling.traction_bias=clampf_local(traction_bias,0.0f,1.0f);
    g_vehicle_handling.brake_bias=clampf_local(brake_bias,0.0f,1.0f);
    g_vehicle_handling.max_forward=fmaxf(12.0f,max_velocity_kmh*scale/216.0f);
    /* reVC uses -0.2 game-velocity units for ordinary car reverse. */
    g_vehicle_handling.max_reverse=0.20f*scale*(50.0f/60.0f);

    /*
     * reVC ConvertDataToGameUnits converts the source handling.cfg values to
     * a 50 Hz simulation.  Express the same acceleration per our 60 Hz tick.
     * F/R drive splits engine force over two driven wheels, 4WD over four.
     */
    drive_div=(drive_type=='4')?4.0f:2.0f;
    g_vehicle_handling.engine_accel=fmaxf(
        0.02f,engine_accel_raw*0.4f*scale/(60.0f*60.0f*drive_div));
    g_vehicle_handling.brake_decel=fmaxf(
        0.04f,brake_decel_raw*scale/(60.0f*60.0f));
    g_vehicle_handling.steering_lock_rad=clampf_local(
        steering_lock_deg*3.14159265f/180.0f,0.15f,0.95f);

    g_vehicle_handling.suspension_force=clampf_local(suspension_force,0.20f,4.0f);
    g_vehicle_handling.suspension_damping=clampf_local(suspension_damping,0.01f,1.0f);
    g_vehicle_handling.suspension_upper=suspension_upper;
    g_vehicle_handling.suspension_lower=suspension_lower;
    g_vehicle_handling.suspension_bias=clampf_local(suspension_bias,0.05f,0.95f);
    g_vehicle_handling.suspension_antidive=clampf_local(suspension_antidive,0.0f,2.0f);

    g_vehicle_handling.dim_x=dim_x;
    g_vehicle_handling.dim_y=dim_y;
    g_vehicle_handling.dim_z=dim_z;
    /* GTA X/right,Y/forward,Z/up -> Racer X/right,Y/up,Z/forward. */
    g_vehicle_handling.centre_of_mass=(v3f_t){
        com_x*scale,com_z*scale,com_y*scale
    };
    g_vehicle_handling.turn_mass_world=
        (dim_x*dim_x+dim_y*dim_y)*mass/12.0f*scale*scale;
    if(g_vehicle_handling.turn_mass_world<1.0f)
        g_vehicle_handling.turn_mass_world=mass*250000.0f;

    g_vehicle_handling.gears=(uint8_t)clampf_local((float)gears,1.0f,8.0f);
    g_vehicle_handling.drive_type=(uint8_t)drive_type;
    g_vehicle_handling.engine_type=(uint8_t)engine_type;
    g_vehicle_handling.abs_enabled=(uint8_t)(abs_enabled?1:0);
    g_vehicle_handling.flags=flags;
    g_vehicle_handling.gta_profile_loaded=1;
    snprintf(g_vehicle_handling.profile_name,sizeof(g_vehicle_handling.profile_name),
             "%s",(name&&*name)?name:"GTA");

    g_vehicle_handling.rolling_drag=0.075f;
    area=fabsf(dim_x*dim_z);
    g_vehicle_handling.aero_drag=clampf_local(
        0.000012f+area/fmaxf(mass,100.0f)*0.00075f,
        0.000012f,0.000055f);
}

static int load_vc_handling_file(const char *path)
{
    FILE *fp;
    vchand_header_t h;
    if(!path||!*path)return 0;
    fp=fopen(path,"rb");
    if(!fp)return 0;
    memset(&h,0,sizeof(h));
    if(sizeof(h)!=128 || !vc_read_exact(fp,&h,sizeof(h))){
        fclose(fp);
        fprintf(stderr,"[racer] VCHAND reject %s: short/ABI header size=%u\n",
                path,(unsigned)sizeof(h));
        return -1;
    }
    fclose(fp);
    if(memcmp(h.magic,"VCH2",4)!=0 || h.version!=2 ||
       !(h.mass>50.0f&&h.mass<20000.0f) ||
       !(h.dim_x>0.1f&&h.dim_x<20.0f) ||
       !(h.dim_y>0.1f&&h.dim_y<30.0f) ||
       !(h.dim_z>0.1f&&h.dim_z<10.0f) ||
       !(h.traction_mult>0.0f&&h.traction_mult<10.0f) ||
       !(h.suspension_force>0.0f&&h.suspension_force<20.0f)){
        fprintf(stderr,"[racer] VCHAND reject %s: expected VCH2/version2 valid profile\n",path);
        return -1;
    }
    h.name[15]='\0';
    vc_apply_handling_profile(
        h.name,h.mass,h.dim_x,h.dim_y,h.dim_z,
        h.com_x,h.com_y,h.com_z,
        h.traction_mult,h.traction_loss,h.traction_bias,
        h.max_velocity_kmh,h.engine_accel_raw,
        h.brake_decel_raw,h.brake_bias,h.steering_lock_deg,
        h.suspension_force,h.suspension_damping,
        h.suspension_upper,h.suspension_lower,
        h.suspension_bias,h.suspension_antidive,
        h.gears,h.drive_type,h.engine_type,h.abs_enabled,h.flags);
    g_vc_current_gear=1;
    memset(g_vc_wheel_state,0,sizeof(g_vc_wheel_state));
    memset(g_vc_wheel_speed,0,sizeof(g_vc_wheel_speed));
    fprintf(stderr,
        "[racer] VCHAND2 loaded path=%s profile=%s mass=%.0f drive=%c gears=%u flags=0x%08x "
        "traction=%.2f/%.2f/%.2f brakeBias=%.2f suspension=%.2f/%.2f %.2f..%.2f bias=%.2f "
        "dims=%.2f/%.2f/%.2f com=%.2f/%.2f/%.2f turnMass=%.0f\n",
        path,g_vehicle_handling.profile_name,g_vehicle_handling.mass,
        g_vehicle_handling.drive_type?g_vehicle_handling.drive_type:'?',
        (unsigned)g_vehicle_handling.gears,(unsigned)g_vehicle_handling.flags,
        g_vehicle_handling.traction_mult,g_vehicle_handling.traction_loss,
        g_vehicle_handling.traction_bias,g_vehicle_handling.brake_bias,
        g_vehicle_handling.suspension_force,g_vehicle_handling.suspension_damping,
        g_vehicle_handling.suspension_lower,g_vehicle_handling.suspension_upper,
        g_vehicle_handling.suspension_bias,
        g_vehicle_handling.dim_x,g_vehicle_handling.dim_y,g_vehicle_handling.dim_z,
        g_vehicle_handling.centre_of_mass.x,g_vehicle_handling.centre_of_mass.y,
        g_vehicle_handling.centre_of_mass.z,g_vehicle_handling.turn_mass_world);
    return 1;
}

static int try_load_vc_handling(void)
{
    const char *env=getenv("RACER_VCHAND");
    int r;
    if(env&&*env){
        r=load_vc_handling_file(env);
        if(r!=0)return r>0;
    }
    r=load_vc_handling_file("/mnt/usb/H3531/APPS/racer/VCHAND.BIN");
    if(r!=0)return r>0;
    r=load_vc_handling_file("VCHAND.BIN");
    if(r!=0)return r>0;
    fprintf(stderr,"[racer] VCHAND not found; using built-in handling defaults\n");
    return 0;
}

static int load_vc_surface_file(const char *path)
{
    FILE *fp;
    char magic[4];
    uint32_t version;
    float matrix[36];
    int i,j,k=0;

    if(!path||!*path)return 0;
    fp=fopen(path,"rb");
    if(!fp)return 0;
    if(!vc_read_exact(fp,magic,4) ||
       !vc_read_exact(fp,&version,sizeof(version)) ||
       !vc_read_exact(fp,matrix,sizeof(matrix))){
        fclose(fp);
        fprintf(stderr,"[racer] VCSURF reject %s: short file\n",path);
        return -1;
    }
    fclose(fp);
    if(memcmp(magic,"VCS1",4)!=0 || version!=1){
        fprintf(stderr,"[racer] VCSURF reject %s: expected VCS1/version1\n",path);
        return -1;
    }
    for(i=0;i<6;++i)for(j=0;j<6;++j){
        float v=matrix[k++];
        if(!isfinite(v) || v<0.0f || v>10.0f){
            fprintf(stderr,"[racer] VCSURF reject %s: invalid matrix value\n",path);
            return -1;
        }
        g_vc_surface.adhesive[i][j]=v;
    }
    g_vc_surface.loaded=1;
    fprintf(stderr,
        "[racer] VCSURF loaded path=%s rubber-road=%.3f rubber-loose=%.3f rubber-sand=%.3f rubber-wet=%.3f\n",
        path,
        g_vc_surface.adhesive[0][2],
        g_vc_surface.adhesive[0][3],
        g_vc_surface.adhesive[0][4],
        g_vc_surface.adhesive[0][5]);
    return 1;
}

static int try_load_vc_surface(void)
{
    const char *env=getenv("RACER_VCSURF");
    int r;
    memset(&g_vc_surface,0,sizeof(g_vc_surface));
    if(env&&*env){
        r=load_vc_surface_file(env);
        if(r!=0)return r>0;
    }
    r=load_vc_surface_file("/mnt/usb/H3531/APPS/racer/VCSURF.BIN");
    if(r!=0)return r>0;
    r=load_vc_surface_file("VCSURF.BIN");
    if(r!=0)return r>0;

    /* Conservative dry fallback matching Vice City's group ordering. */
    {
        static const float fallback[6]={
            1.00f,0.92f,1.00f,0.72f,0.58f,0.52f
        };
        int i,j;
        for(i=0;i<6;++i)for(j=0;j<6;++j)
            g_vc_surface.adhesive[i][j]=fminf(fallback[i],fallback[j]);
        g_vc_surface.loaded=0;
    }
    fprintf(stderr,"[racer] VCSURF not found; using conservative dry adhesion fallback\n");
    return 0;
}

static void free_vc_vehicle(void)
{
    free(g_vc_vehicle.verts);
    free(g_vc_vehicle.tris);
    free(g_vc_vehicle.materials);
    free(g_vc_vehicle.atlas);
    free(g_vc_vehicle.vertex_part);
    free(g_vc_vehicle.col_spheres);
    free(g_vc_vehicle.col_lines);
    free(g_vcveh_rv);
    free(g_vcveh_sv);
    free(g_vcveh_out);
    g_vcveh_rv=NULL;
    g_vcveh_sv=NULL;
    g_vcveh_out=NULL;
    g_vcveh_vertex_cap=0;
    g_vcveh_tri_cap=0;
    memset(&g_vc_vehicle,0,sizeof(g_vc_vehicle));
}

static int load_vc_vehicle_file(const char *path)
{
    FILE *fp;
    vcveh_header_t h;
    size_t atlas_pixels;
    uint32_t i;
    float scale,area;

    if(!path||!*path)return 0;
    fp=fopen(path,"rb");
    if(!fp)return 0;

    memset(&h,0,sizeof(h));
    if(sizeof(h)!=92 || !vc_read_exact(fp,&h,sizeof(h))){
        fclose(fp);
        fprintf(stderr,"[racer] VCVEH reject %s: short/ABI header size=%u\n",
                path,(unsigned)sizeof(h));
        return -1;
    }
    if(memcmp(h.magic,"VCV1",4)!=0 || h.version!=1){
        fclose(fp);
        fprintf(stderr,"[racer] VCVEH reject %s: expected VCV1/version1\n",path);
        return -1;
    }

    atlas_pixels=(size_t)h.atlas_w*(size_t)h.atlas_h;
    if(h.vertex_count==0 || h.tri_count==0 || h.material_count==0 ||
       h.vertex_count>65535U || h.tri_count>131072U ||
       h.material_count>255U || h.atlas_w==0 || h.atlas_h==0 ||
       h.atlas_w>1024U || h.atlas_h>1024U || atlas_pixels>1048576U ||
       !(h.world_scale>1.0f&&h.world_scale<10000.0f) ||
       !(h.mass>50.0f&&h.mass<20000.0f)){
        fclose(fp);
        fprintf(stderr,"[racer] VCVEH reject %s: unsafe counts/range v=%u t=%u m=%u atlas=%ux%u\n",
                path,(unsigned)h.vertex_count,(unsigned)h.tri_count,
                (unsigned)h.material_count,(unsigned)h.atlas_w,(unsigned)h.atlas_h);
        return -1;
    }

    free_vc_vehicle();
    g_vc_vehicle.verts=(vc_vertex_t*)calloc((size_t)h.vertex_count,sizeof(vc_vertex_t));
    g_vc_vehicle.tris=(vc_tri_t*)calloc((size_t)h.tri_count,sizeof(vc_tri_t));
    g_vc_vehicle.materials=(vc_material_t*)calloc((size_t)h.material_count,sizeof(vc_material_t));
    g_vc_vehicle.atlas=(uint16_t*)calloc(atlas_pixels,sizeof(uint16_t));
    g_vc_vehicle.vertex_part=(uint8_t*)calloc((size_t)h.vertex_count,sizeof(uint8_t));
    g_vcveh_rv=(v3f_t*)calloc((size_t)h.vertex_count,sizeof(v3f_t));
    g_vcveh_sv=(sv3_t*)calloc((size_t)h.vertex_count,sizeof(sv3_t));
    g_vcveh_out=(textri_t*)calloc((size_t)h.tri_count,sizeof(textri_t));
    g_vcveh_vertex_cap=h.vertex_count;
    g_vcveh_tri_cap=h.tri_count;
    if(!g_vc_vehicle.verts||!g_vc_vehicle.tris||
       !g_vc_vehicle.materials||!g_vc_vehicle.atlas||!g_vc_vehicle.vertex_part||
       !g_vcveh_rv||!g_vcveh_sv||!g_vcveh_out){
        fclose(fp);free_vc_vehicle();
        fprintf(stderr,"[racer] VCVEH reject %s: allocation failed\n",path);
        return -1;
    }

    if(!vc_read_exact(fp,g_vc_vehicle.materials,(size_t)h.material_count*sizeof(vc_material_t)) ||
       !vc_read_exact(fp,g_vc_vehicle.verts,(size_t)h.vertex_count*sizeof(vc_vertex_t)) ||
       !vc_read_exact(fp,g_vc_vehicle.tris,(size_t)h.tri_count*sizeof(vc_tri_t)) ||
       !vc_read_exact(fp,g_vc_vehicle.atlas,atlas_pixels*sizeof(uint16_t))){
        fclose(fp);free_vc_vehicle();
        fprintf(stderr,"[racer] VCVEH reject %s: truncated payload\n",path);
        return -1;
    }

    /*
     * Optional native collision trailer.
     * VCL1: GTA CColModel spheres + suspension lines.
     * VCL2: VCL1 plus the normal ride height calculated with the same
     *       SetupSuspensionLines formula used by reVC.
     */
    {
        long trailer_pos=ftell(fp);
        struct { char magic[4]; uint32_t version; } prefix;
        size_t got=fread(&prefix,1,sizeof(prefix),fp);
        if(got==sizeof(prefix)){
            uint32_t sphere_count=0,box_count=0,tri_count=0,line_count=0;
            float bound_cx=0.0f,bound_cy=0.0f,bound_cz=0.0f,bound_r=0.0f;
            float box_min_x=0.0f,box_min_y=0.0f,box_min_z=0.0f;
            float box_max_x=0.0f,box_max_y=0.0f,box_max_z=0.0f;
            float rest_height_world=21.0f;
            float suspension_force=1.0f,suspension_damping=0.10f;
            float suspension_upper=0.30f,suspension_lower=-0.10f;
            float suspension_bias=0.50f,suspension_antidive=0.0f;
            uint32_t native_version=0;

            if(fseek(fp,trailer_pos,SEEK_SET)!=0){
                fclose(fp);free_vc_vehicle();return -1;
            }

            if(memcmp(prefix.magic,"VCL1",4)==0 && prefix.version==1){
                vcveh_col_ext_header_v1_t ch;
                if(sizeof(ch)!=64 || !vc_read_exact(fp,&ch,sizeof(ch))){
                    fclose(fp);free_vc_vehicle();
                    fprintf(stderr,"[racer] VCVEH reject %s: malformed VCL1 header\n",path);
                    return -1;
                }
                sphere_count=ch.sphere_count;box_count=ch.box_count;
                tri_count=ch.tri_count;line_count=ch.line_count;
                bound_cx=ch.bound_cx;bound_cy=ch.bound_cy;bound_cz=ch.bound_cz;bound_r=ch.bound_r;
                box_min_x=ch.box_min_x;box_min_y=ch.box_min_y;box_min_z=ch.box_min_z;
                box_max_x=ch.box_max_x;box_max_y=ch.box_max_y;box_max_z=ch.box_max_z;
                native_version=1;
            }else if(memcmp(prefix.magic,"VCL2",4)==0 && prefix.version==2){
                vcveh_col_ext_header_v2_t ch;
                if(sizeof(ch)!=68 || !vc_read_exact(fp,&ch,sizeof(ch))){
                    fclose(fp);free_vc_vehicle();
                    fprintf(stderr,"[racer] VCVEH reject %s: malformed VCL2 header\n",path);
                    return -1;
                }
                sphere_count=ch.sphere_count;box_count=ch.box_count;
                tri_count=ch.tri_count;line_count=ch.line_count;
                bound_cx=ch.bound_cx;bound_cy=ch.bound_cy;bound_cz=ch.bound_cz;bound_r=ch.bound_r;
                box_min_x=ch.box_min_x;box_min_y=ch.box_min_y;box_min_z=ch.box_min_z;
                box_max_x=ch.box_max_x;box_max_y=ch.box_max_y;box_max_z=ch.box_max_z;
                rest_height_world=ch.rest_height_world;
                native_version=2;
            }else if(memcmp(prefix.magic,"VCL3",4)==0 && prefix.version==3){
                vcveh_col_ext_header_v3_t ch;
                if(sizeof(ch)!=92 || !vc_read_exact(fp,&ch,sizeof(ch))){
                    fclose(fp);free_vc_vehicle();
                    fprintf(stderr,"[racer] VCVEH reject %s: malformed VCL3 header\n",path);
                    return -1;
                }
                sphere_count=ch.sphere_count;box_count=ch.box_count;
                tri_count=ch.tri_count;line_count=ch.line_count;
                bound_cx=ch.bound_cx;bound_cy=ch.bound_cy;bound_cz=ch.bound_cz;bound_r=ch.bound_r;
                box_min_x=ch.box_min_x;box_min_y=ch.box_min_y;box_min_z=ch.box_min_z;
                box_max_x=ch.box_max_x;box_max_y=ch.box_max_y;box_max_z=ch.box_max_z;
                rest_height_world=ch.rest_height_world;
                suspension_force=ch.suspension_force;
                suspension_damping=ch.suspension_damping;
                suspension_upper=ch.suspension_upper;
                suspension_lower=ch.suspension_lower;
                suspension_bias=ch.suspension_bias;
                suspension_antidive=ch.suspension_antidive;
                native_version=3;
            }else{
                fprintf(stderr,
                    "[racer] VCVEH warning %s: unknown trailer %.4s/%u ignored\n",
                    path,prefix.magic,(unsigned)prefix.version);
            }

            if(native_version){
                if(sphere_count>128U || box_count>256U || tri_count>4096U ||
                   line_count>4U ||
                   !(bound_r>=0.0f && bound_r<100000.0f) ||
                   !(rest_height_world>0.0f && rest_height_world<2000.0f) ||
                   !isfinite(suspension_force)||!isfinite(suspension_damping)||
                   !isfinite(suspension_upper)||!isfinite(suspension_lower)||
                   !isfinite(suspension_bias)||!isfinite(suspension_antidive)){
                    fclose(fp);free_vc_vehicle();
                    fprintf(stderr,"[racer] VCVEH reject %s: unsafe VCL%u counts/range\n",
                            path,(unsigned)native_version);
                    return -1;
                }

                if(sphere_count){
                    g_vc_vehicle.col_spheres=(vcveh_col_sphere_t*)calloc(
                        (size_t)sphere_count,sizeof(vcveh_col_sphere_t));
                    if(!g_vc_vehicle.col_spheres ||
                       !vc_read_exact(fp,g_vc_vehicle.col_spheres,
                                      (size_t)sphere_count*sizeof(vcveh_col_sphere_t))){
                        fclose(fp);free_vc_vehicle();
                        fprintf(stderr,"[racer] VCVEH reject %s: truncated VCL%u spheres\n",
                                path,(unsigned)native_version);
                        return -1;
                    }
                    for(i=0;i<sphere_count;++i){
                        const vcveh_col_sphere_t *sp=&g_vc_vehicle.col_spheres[i];
                        if(!(sp->r>0.0f && sp->r<100000.0f) ||
                           !isfinite(sp->x)||!isfinite(sp->y)||!isfinite(sp->z)){
                            fclose(fp);free_vc_vehicle();
                            fprintf(stderr,"[racer] VCVEH reject %s: invalid VCL%u sphere %u\n",
                                    path,(unsigned)native_version,(unsigned)i);
                            return -1;
                        }
                    }
                }

                if(line_count){
                    g_vc_vehicle.col_lines=(vcveh_col_line_t*)calloc(
                        (size_t)line_count,sizeof(vcveh_col_line_t));
                    if(!g_vc_vehicle.col_lines ||
                       !vc_read_exact(fp,g_vc_vehicle.col_lines,
                                      (size_t)line_count*sizeof(vcveh_col_line_t))){
                        fclose(fp);free_vc_vehicle();
                        fprintf(stderr,"[racer] VCVEH reject %s: truncated VCL%u lines\n",
                                path,(unsigned)native_version);
                        return -1;
                    }
                    for(i=0;i<line_count;++i){
                        const vcveh_col_line_t *ln=&g_vc_vehicle.col_lines[i];
                        if(ln->part<1U||ln->part>4U ||
                           !isfinite(ln->p0x)||!isfinite(ln->p0y)||!isfinite(ln->p0z)||
                           !isfinite(ln->p1x)||!isfinite(ln->p1y)||!isfinite(ln->p1z)){
                            fclose(fp);free_vc_vehicle();
                            fprintf(stderr,"[racer] VCVEH reject %s: invalid VCL%u line %u\n",
                                    path,(unsigned)native_version,(unsigned)i);
                            return -1;
                        }
                    }
                }

                g_vc_vehicle.col_sphere_count=sphere_count;
                g_vc_vehicle.col_box_count=box_count;
                g_vc_vehicle.col_tri_count=tri_count;
                g_vc_vehicle.col_line_count=line_count;
                g_vc_vehicle.col_bound_center=(v3f_t){bound_cx,bound_cy,bound_cz};
                g_vc_vehicle.col_bound_radius=bound_r;
                g_vc_vehicle.col_box_min=(v3f_t){box_min_x,box_min_y,box_min_z};
                g_vc_vehicle.col_box_max=(v3f_t){box_max_x,box_max_y,box_max_z};
                g_vc_vehicle.rest_height_world=rest_height_world;
                g_vc_vehicle.suspension_force=suspension_force;
                g_vc_vehicle.suspension_damping=suspension_damping;
                g_vc_vehicle.suspension_upper=suspension_upper;
                g_vc_vehicle.suspension_lower=suspension_lower;
                g_vc_vehicle.suspension_bias=suspension_bias;
                g_vc_vehicle.suspension_antidive=suspension_antidive;
                g_vc_vehicle.native_col_version=native_version;
                g_vc_vehicle.native_col_loaded=1;
            }
        }else if(got!=0){
            fclose(fp);free_vc_vehicle();
            fprintf(stderr,"[racer] VCVEH reject %s: partial trailer prefix\n",path);
            return -1;
        }
    }
    for(i=0;i<h.material_count;++i){
        const vc_material_t *m=&g_vc_vehicle.materials[i];
        if(!(m->flags&1U) || m->w==0 || m->h==0 ||
           (uint32_t)m->x+(uint32_t)m->w>h.atlas_w ||
           (uint32_t)m->y+(uint32_t)m->h>h.atlas_h){
            fprintf(stderr,"[racer] VCVEH reject %s: invalid material %u\n",path,(unsigned)i);
            free_vc_vehicle();return -1;
        }
    }
    for(i=0;i<h.tri_count;++i){
        const vc_tri_t *t=&g_vc_vehicle.tris[i];
        unsigned part=(unsigned)t->flags;
        if(t->a>=h.vertex_count||t->b>=h.vertex_count||t->c>=h.vertex_count||
           t->material>=h.material_count || part>4U){
            fprintf(stderr,"[racer] VCVEH reject %s: invalid triangle %u\n",path,(unsigned)i);
            free_vc_vehicle();return -1;
        }
        if(part){
            const uint16_t ids[3]={t->a,t->b,t->c};
            int k;
            for(k=0;k<3;++k){
                uint16_t vi=ids[k];
                if(g_vc_vehicle.vertex_part[vi]==0U)
                    g_vc_vehicle.vertex_part[vi]=(uint8_t)part;
            }
        }
    }

    {
        float minx[5]={0},miny[5]={0},minz[5]={0};
        float maxx[5]={0},maxy[5]={0},maxz[5]={0};
        uint8_t seen[5]={0};
        for(i=0;i<h.vertex_count;++i){
            unsigned part=(unsigned)g_vc_vehicle.vertex_part[i];
            const vc_vertex_t *v;
            if(part<1U||part>4U)continue;
            v=&g_vc_vehicle.verts[i];
            if(!seen[part]){
                minx[part]=maxx[part]=v->x;
                miny[part]=maxy[part]=v->y;
                minz[part]=maxz[part]=v->z;
                seen[part]=1;
            }else{
                if(v->x<minx[part])minx[part]=v->x;
                if(v->x>maxx[part])maxx[part]=v->x;
                if(v->y<miny[part])miny[part]=v->y;
                if(v->y>maxy[part])maxy[part]=v->y;
                if(v->z<minz[part])minz[part]=v->z;
                if(v->z>maxz[part])maxz[part]=v->z;
            }
        }
        for(i=1;i<=4;++i){
            if(seen[i]){
                g_vc_vehicle.wheel_present[i]=1;
                g_vc_vehicle.wheel_pivot[i].x=(minx[i]+maxx[i])*0.5f;
                g_vc_vehicle.wheel_pivot[i].y=(miny[i]+maxy[i])*0.5f;
                g_vc_vehicle.wheel_pivot[i].z=(minz[i]+maxz[i])*0.5f;
            }
        }
    }

    g_vc_vehicle.vertex_count=h.vertex_count;
    g_vc_vehicle.tri_count=h.tri_count;
    g_vc_vehicle.material_count=h.material_count;
    g_vc_vehicle.atlas_w=h.atlas_w;
    g_vc_vehicle.atlas_h=h.atlas_h;
    g_vc_vehicle.world_scale=h.world_scale;
    g_vc_vehicle.dim_x=h.dim_x;
    g_vc_vehicle.dim_y=h.dim_y;
    g_vc_vehicle.dim_z=h.dim_z;
    g_vc_vehicle.centre_of_mass=(v3f_t){
        h.com_x*h.world_scale,
        h.com_z*h.world_scale,
        h.com_y*h.world_scale
    };
    /*
     * reVC HandlingMgr::ConvertDataToGameUnits:
     *   turnMass=(Dimension.x^2+Dimension.y^2)*mass/12.
     * Convert metres to Racer world units here so r x J / I produces
     * radians-per-tick body rotation in the same coordinate scale.
     */
    g_vc_vehicle.turn_mass_world=
        (h.dim_x*h.dim_x+h.dim_y*h.dim_y)*h.mass/12.0f*
        h.world_scale*h.world_scale;
    if(g_vc_vehicle.turn_mass_world<1.0f)
        g_vc_vehicle.turn_mass_world=h.mass*250000.0f;

    scale=h.world_scale;
    g_vc_vehicle.wheelbase=fabsf(h.dim_y)*scale*0.62f;
    g_vc_vehicle.track=fabsf(h.dim_x)*scale*0.82f;
    g_vc_vehicle.wheel_radius=fabsf(h.dim_z)*scale*0.18f;
    if(g_vc_vehicle.wheelbase<180.0f)g_vc_vehicle.wheelbase=SPORTS_VEHICLE_WHEELBASE;
    if(g_vc_vehicle.track<120.0f)g_vc_vehicle.track=SPORTS_VEHICLE_TRACK;
    if(g_vc_vehicle.wheel_radius<35.0f)g_vc_vehicle.wheel_radius=SPORTS_VEHICLE_WHEEL_RADIUS;

    /*
     * handling.cfg is authored for the original ~50 Hz simulation.  ReVC first
     * applies its handling conversion; here we adapt the same source quantities
     * to our 60 Hz, world_scale-sized simulation without sharing reVC code.
     */
    g_vehicle_handling.mass=h.mass;
    g_vehicle_handling.traction_mult=fmaxf(0.25f,h.traction_mult);
    g_vehicle_handling.traction_loss=fmaxf(0.20f,h.traction_loss);
    g_vehicle_handling.traction_bias=clampf_local(h.traction_bias,0.0f,1.0f);
    g_vehicle_handling.brake_bias=clampf_local(h.brake_bias,0.0f,1.0f);
    g_vehicle_handling.max_forward=fmaxf(12.0f,h.max_velocity_kmh*scale/216.0f);
    g_vehicle_handling.max_reverse=g_vehicle_handling.max_forward*0.38f;
    /*
     * reVC's handling conversion scales engine acceleration by 0.4/50^2
     * and brake deceleration by 1/50^2. Convert those per-original-tick
     * quantities into our world_scale units at 60 Hz.
     */
    g_vehicle_handling.engine_accel=fmaxf(
        0.08f,h.engine_accel_raw*0.4f*scale/(50.0f*60.0f));
    g_vehicle_handling.brake_decel=fmaxf(
        0.18f,h.brake_decel_raw*scale/(50.0f*60.0f));
    g_vehicle_handling.steering_lock_rad=clampf_local(
        h.steering_lock_deg*3.14159265f/180.0f,0.15f,0.95f);
    g_vehicle_handling.rolling_drag=0.075f;
    area=fabsf(h.dim_x*h.dim_z);
    g_vehicle_handling.aero_drag=clampf_local(
        0.000012f+area/fmaxf(h.mass,100.0f)*0.00075f,
        0.000012f,0.000055f);
    g_vehicle_handling.dim_x=h.dim_x;
    g_vehicle_handling.dim_y=h.dim_y;
    g_vehicle_handling.dim_z=h.dim_z;
    g_vehicle_handling.centre_of_mass=g_vc_vehicle.centre_of_mass;
    g_vehicle_handling.turn_mass_world=g_vc_vehicle.turn_mass_world;
    g_vehicle_handling.suspension_force=
        g_vc_vehicle.native_col_version>=3U?g_vc_vehicle.suspension_force:1.40f;
    g_vehicle_handling.suspension_damping=
        g_vc_vehicle.native_col_version>=3U?g_vc_vehicle.suspension_damping:0.12f;
    g_vehicle_handling.suspension_upper=
        g_vc_vehicle.native_col_version>=3U?g_vc_vehicle.suspension_upper:0.28f;
    g_vehicle_handling.suspension_lower=
        g_vc_vehicle.native_col_version>=3U?g_vc_vehicle.suspension_lower:-0.12f;
    g_vehicle_handling.suspension_bias=
        g_vc_vehicle.native_col_version>=3U?g_vc_vehicle.suspension_bias:0.50f;
    g_vehicle_handling.suspension_antidive=
        g_vc_vehicle.native_col_version>=3U?g_vc_vehicle.suspension_antidive:0.0f;
    g_vehicle_handling.drive_type='R';
    g_vehicle_handling.gears=5;
    g_vehicle_handling.engine_type='P';
    g_vehicle_handling.abs_enabled=0;
    g_vehicle_handling.flags=0;
    g_vehicle_handling.gta_profile_loaded=1;
    snprintf(g_vehicle_handling.profile_name,sizeof(g_vehicle_handling.profile_name),"VCVEH");

    g_vc_vehicle.loaded=1;
    if(g_vc_city_mode && g_vc_vehicle.native_col_version>=2U)
        g_world_y=g_vc_ground_y+g_vc_vehicle.rest_height_world;

    fprintf(stderr,
        "[racer] VCVEH loaded path=%s vertices=%u triangles=%u materials=%u atlas=%ux%u "
        "mass=%.0f vmax=%.1f engine=%.3f brake=%.3f traction=%.2f/%.2f steer=%.1fdeg "
        "wheelbase=%.0f track=%.0f radius=%.0f wheels=%u%u%u%u "
        "nativecol=%s colSpheres=%u colBoxes=%u colTris=%u susLines=%u ride=%.1f scratch=%luKiB\n",
        path,(unsigned)h.vertex_count,(unsigned)h.tri_count,(unsigned)h.material_count,
        (unsigned)h.atlas_w,(unsigned)h.atlas_h,
        g_vehicle_handling.mass,g_vehicle_handling.max_forward,
        g_vehicle_handling.engine_accel,g_vehicle_handling.brake_decel,
        g_vehicle_handling.traction_mult,g_vehicle_handling.traction_loss,
        g_vehicle_handling.steering_lock_rad*180.0f/3.14159265f,
        g_vc_vehicle.wheelbase,g_vc_vehicle.track,g_vc_vehicle.wheel_radius,
        (unsigned)g_vc_vehicle.wheel_present[1],
        (unsigned)g_vc_vehicle.wheel_present[2],
        (unsigned)g_vc_vehicle.wheel_present[3],
        (unsigned)g_vc_vehicle.wheel_present[4],
        g_vc_vehicle.native_col_version==3U?"VCL3":
            (g_vc_vehicle.native_col_version==2U?"VCL2":
            (g_vc_vehicle.native_col_version==1U?"VCL1":"fallback")),
        (unsigned)g_vc_vehicle.col_sphere_count,
        (unsigned)g_vc_vehicle.col_box_count,
        (unsigned)g_vc_vehicle.col_tri_count,
        (unsigned)g_vc_vehicle.col_line_count,
        g_vc_vehicle.rest_height_world,
        (unsigned long)(((size_t)h.vertex_count*(sizeof(v3f_t)+sizeof(sv3_t))+
                         (size_t)h.tri_count*sizeof(textri_t))/1024U));
    return 1;
}

static int try_load_vc_vehicle(void)
{
    const char *env=getenv("RACER_VCVEH");
    int r;
    if(env&&*env){
        r=load_vc_vehicle_file(env);
        if(r!=0)return r>0;
    }
    r=load_vc_vehicle_file("/mnt/usb/H3531/APPS/racer/VCVEH.BIN");
    if(r!=0)return r>0;
    r=load_vc_vehicle_file("VCVEH.BIN");
    if(r!=0)return r>0;
    fprintf(stderr,"[racer] VCVEH not found; using built-in downloaded Rally sports car\n");
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

static int vc_col_point_in_tri_xz(
    float px,float pz,const vc_col_tri_t *t,float *wa,float *wb,float *wc)
{
    float den=(t->bz-t->cz)*(t->ax-t->cx)+(t->cx-t->bx)*(t->az-t->cz);
    float u,v,w;
    if(fabsf(den)<1.0e-7f)return 0;
    u=((t->bz-t->cz)*(px-t->cx)+(t->cx-t->bx)*(pz-t->cz))/den;
    v=((t->cz-t->az)*(px-t->cx)+(t->ax-t->cx)*(pz-t->cz))/den;
    w=1.0f-u-v;
    if(u<-0.001f||v<-0.001f||w<-0.001f)return 0;
    if(wa)*wa=u;if(wb)*wb=v;if(wc)*wc=w;
    return 1;
}

/*
 * Ground-only fallback from the exact VCMAP triangles that are rendered.
 * Some modded VC installs contain visible DFF road geometry without a matching
 * COL primitive. Never let suspension / landing pass through that visible
 * opaque horizontal surface just because VCCOL is sparse there.
 */
static int vc_visual_vertical_contact(
    float world_x,float world_z,float top_y,float bottom_y,float *out_y)
{
    if(g_vc_world_mode)return 0;
    int psx,psz,found=0;
    uint32_t i,j;
    float best=-1.0e30f;
    float scale=vc_runtime_world_scale();
    float min_area2=0.10f*scale*scale;

    if(!g_vc_city_mode || !g_vc_map.sectors || g_vc_map.sector_world<=1.0f)
        return 0;
    if(bottom_y>top_y){float t=bottom_y;bottom_y=top_y;top_y=t;}

    psx=(int)floorf(world_x/g_vc_map.sector_world);
    psz=(int)floorf(world_z/g_vc_map.sector_world);

    for(i=0;i<g_vc_map.sector_count;++i){
        const vc_sector_t *sec=&g_vc_map.sectors[i];
        if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;

        for(j=0;j<sec->tri_count;++j){
            const vc_map_tri_t *t=&g_vc_map.tris[sec->tri_base+j];
            const vc_vertex_t *a=&g_vc_map.verts[sec->vertex_base+t->a];
            const vc_vertex_t *b=&g_vc_map.verts[sec->vertex_base+t->b];
            const vc_vertex_t *c=&g_vc_map.verts[sec->vertex_base+t->c];
            float ux=b->x-a->x,uy=b->y-a->y,uz=b->z-a->z;
            float vx=c->x-a->x,vy=c->y-a->y,vz=c->z-a->z;
            float nx=uy*vz-uz*vy;
            float ny=uz*vx-ux*vz;
            float nz=ux*vy-uy*vx;
            float nlen=sqrtf(nx*nx+ny*ny+nz*nz);
            float area2_xz=fabsf(ux*vz-uz*vx);
            float wa,wb,wc,y;

            if(nlen<=1.0e-6f || fabsf(ny)/nlen<0.58f)continue;
            if(area2_xz<min_area2)continue;
            if(t->material<g_vc_map.material_count &&
               (g_vc_map.materials[t->material].flags&2U))
                continue;
            if(!vc_point_in_tri_xz(world_x,world_z,a,b,c,&wa,&wb,&wc))
                continue;

            y=wa*a->y+wb*b->y+wc*c->y;
            if(y<=top_y+0.5f && y>=bottom_y-0.5f && (!found||y>best)){
                best=y;found=1;
            }
        }
    }

    if(found){
        if(out_y)*out_y=best;
        g_vc_visual_ground_fallback_window++;
        g_vc_visual_ground_fallback_total++;
    }
    return found;
}

static int vc_collision_ground_contact(
    float world_x,float world_z,float current_y,
    float *out_y,uint8_t *out_surface)
{
    float scale,ux,uz,cy,best=0.0f,best_score=1.0e30f;
    uint8_t best_surface=0;
    int psx,psz,found=0;
    uint32_t i,j;

    if(!g_vc_collision.loaded)return 0;
    scale=g_vc_collision.world_scale;
    ux=world_x/scale;uz=world_z/scale;cy=current_y/scale;
    psx=(int)floorf(ux/g_vc_collision.sector_m);
    psz=(int)floorf(uz/g_vc_collision.sector_m);

    if(g_vc_collision.version==1){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector_t *sec=&g_vc_collision.sectors[i];
            if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
                float wa,wb,wc,y,delta;
                if(!(t->flags&1U))continue;
                if(!vc_col_point_in_tri_xz(ux,uz,t,&wa,&wb,&wc))continue;
                y=wa*t->ay+wb*t->by+wc*t->cy;
                if(y>cy+1.25f || y<cy-6.0f)continue;
                delta=fabsf(y-cy);
                if(delta<best_score){
                    best_score=delta;best=y;best_surface=t->material;found=1;
                }
            }
        }
    }else if(g_vc_collision.version==2){
        /*
         * Vice City style vertical contact: test the actual COL primitives.
         * No slope/material guess decides whether a triangle is ground.
         */
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector2_t *sec=&g_vc_collision.sectors2[i];
            if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
                float wa,wb,wc,y,delta;
                if(!vc_col_point_in_tri_xz(ux,uz,t,&wa,&wb,&wc))continue;
                y=wa*t->ay+wb*t->by+wc*t->cy;
                if(y>cy+1.25f || y<cy-6.0f)continue;
                delta=fabsf(y-cy);
                if(delta<best_score){
                    best_score=delta;best=y;best_surface=t->material;found=1;
                }
            }
            for(j=0;j<sec->sphere_count;++j){
                const vc_col_sphere_t *sp=&g_vc_collision.spheres[sec->sphere_base+j];
                float dx=ux-sp->x,dz=uz-sp->z,h2=sp->r*sp->r-dx*dx-dz*dz;
                float root,y0,y1,delta;
                if(h2<0.0f)continue;
                root=sqrtf(h2);
                y0=sp->y-root;
                y1=sp->y+root;
                if(y0<=cy+1.25f && y0>=cy-6.0f){
                    delta=fabsf(y0-cy);
                    if(delta<best_score){
                        best_score=delta;best=y0;best_surface=sp->surface;found=1;
                    }
                }
                if(y1<=cy+1.25f && y1>=cy-6.0f){
                    delta=fabsf(y1-cy);
                    if(delta<best_score){
                        best_score=delta;best=y1;best_surface=sp->surface;found=1;
                    }
                }
            }
        }
    }

    if(found){
        if(out_y)*out_y=best*scale;
        if(out_surface)*out_surface=best_surface;
        return 1;
    }

    {
        float vy=0.0f;
        if(vc_visual_vertical_contact(
            world_x,world_z,
            current_y+1.25f*scale,
            current_y-6.0f*scale,
            &vy)){
            if(out_y)*out_y=vy;
            if(out_surface)*out_surface=254U;
            return 1;
        }
    }
    return 0;
}

static int vc_collision_ground_height(
    float world_x,float world_z,float current_y,float *out_y)
{
    return vc_collision_ground_contact(world_x,world_z,current_y,out_y,NULL);
}

/*
 * Spawn-specific vertical query.
 *
 * Normal suspension contact searches near the current body height, which is
 * correct once the vehicle is already driving. It is wrong for startup if the
 * stored spawn happens to be below the road. This query ignores current Y and
 * scans the whole local VCCOL column from above, preferring genuine road
 * surfaces (Vice City surface ids 0/1) and sufficiently horizontal triangles.
 */
static int vc_collision_spawn_surface(
    float world_x,float world_z,float *out_y,uint8_t *out_surface)
{
    float scale,ux,uz,best_road=-1.0e30f,best_any=-1.0e30f;
    uint8_t best_road_surface=0,best_any_surface=0;
    int psx,psz,found_road=0,found_any=0;
    uint32_t i,j;

    if(!g_vc_collision.loaded)return 0;
    scale=g_vc_collision.world_scale;
    ux=world_x/scale;uz=world_z/scale;
    psx=(int)floorf(ux/g_vc_collision.sector_m);
    psz=(int)floorf(uz/g_vc_collision.sector_m);

    if(g_vc_collision.version==1){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector_t *sec=&g_vc_collision.sectors[i];
            if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
                float wa,wb,wc,y;
                float abx=t->bx-t->ax,aby=t->by-t->ay,abz=t->bz-t->az;
                float acx=t->cx-t->ax,acy=t->cy-t->ay,acz=t->cz-t->az;
                float nx=aby*acz-abz*acy;
                float ny=abz*acx-abx*acz;
                float nz=abx*acy-aby*acx;
                float nlen=sqrtf(nx*nx+ny*ny+nz*nz);
                float up=nlen>1.0e-6f?fabsf(ny)/nlen:0.0f;
                if(!(t->flags&1U) || up<0.65f)continue;
                if(!vc_col_point_in_tri_xz(ux,uz,t,&wa,&wb,&wc))continue;
                y=wa*t->ay+wb*t->by+wc*t->cy;
                if(!found_any || y>best_any){
                    best_any=y;best_any_surface=t->material;found_any=1;
                }
                if((t->material==0U||t->material==1U||t->material==5U) &&
                   (!found_road || y>best_road)){
                    best_road=y;best_road_surface=t->material;found_road=1;
                }
            }
        }
    }else if(g_vc_collision.version==2){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector2_t *sec=&g_vc_collision.sectors2[i];
            if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
                float wa,wb,wc,y;
                float abx=t->bx-t->ax,aby=t->by-t->ay,abz=t->bz-t->az;
                float acx=t->cx-t->ax,acy=t->cy-t->ay,acz=t->cz-t->az;
                float nx=aby*acz-abz*acy;
                float ny=abz*acx-abx*acz;
                float nz=abx*acy-aby*acx;
                float nlen=sqrtf(nx*nx+ny*ny+nz*nz);
                float up=nlen>1.0e-6f?fabsf(ny)/nlen:0.0f;
                if(up<0.65f)continue;
                if(!vc_col_point_in_tri_xz(ux,uz,t,&wa,&wb,&wc))continue;
                y=wa*t->ay+wb*t->by+wc*t->cy;
                if(!found_any || y>best_any){
                    best_any=y;best_any_surface=t->material;found_any=1;
                }
                if((t->material==0U||t->material==1U||t->material==5U) &&
                   (!found_road || y>best_road)){
                    best_road=y;best_road_surface=t->material;found_road=1;
                }
            }

            /* Spheres are useful as generic fallback, but never preferred as a
             * road spawn because poles/planters/props also use sphere COL. */
            for(j=0;j<sec->sphere_count;++j){
                const vc_col_sphere_t *sp=&g_vc_collision.spheres[sec->sphere_base+j];
                float dx=ux-sp->x,dz=uz-sp->z,h2=sp->r*sp->r-dx*dx-dz*dz;
                float y;
                if(h2<0.0f)continue;
                y=sp->y+sqrtf(h2);
                if(!found_any || y>best_any){
                    best_any=y;best_any_surface=sp->surface;found_any=1;
                }
            }
        }
    }

    if(found_road){
        if(out_y)*out_y=best_road*scale;
        if(out_surface)*out_surface=best_road_surface;
        return 2;
    }
    if(found_any){
        if(out_y)*out_y=best_any*scale;
        if(out_surface)*out_surface=best_any_surface;
        return 1;
    }
    return 0;
}

static int vc_collision_vertical_contact_native(
    float world_x,float world_z,float top_y,float bottom_y,
    float *out_y,uint8_t *out_surface)
{
    float scale,ux,uz,top,bottom,best=-1.0e30f;
    uint8_t best_surface=0;
    int psx,psz,found=0;
    uint32_t i,j;

    if(!g_vc_collision.loaded)return 0;
    if(bottom_y>top_y){float t=bottom_y;bottom_y=top_y;top_y=t;}

    scale=g_vc_collision.world_scale;
    ux=world_x/scale;uz=world_z/scale;
    top=top_y/scale;bottom=bottom_y/scale;
    psx=(int)floorf(ux/g_vc_collision.sector_m);
    psz=(int)floorf(uz/g_vc_collision.sector_m);

    if(g_vc_collision.version==1){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector_t *sec=&g_vc_collision.sectors[i];
            if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
                float wa,wb,wc,y;
                if(!(t->flags&1U))continue;
                if(!vc_col_point_in_tri_xz(ux,uz,t,&wa,&wb,&wc))continue;
                y=wa*t->ay+wb*t->by+wc*t->cy;
                if(y<=top+0.02f && y>=bottom-0.02f && (!found||y>best)){
                    best=y;best_surface=t->material;found=1;
                }
            }
        }
    }else if(g_vc_collision.version==2){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector2_t *sec=&g_vc_collision.sectors2[i];
            if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
                float wa,wb,wc,y;
                if(!vc_col_point_in_tri_xz(ux,uz,t,&wa,&wb,&wc))continue;
                y=wa*t->ay+wb*t->by+wc*t->cy;
                if(y<=top+0.02f && y>=bottom-0.02f && (!found||y>best)){
                    best=y;best_surface=t->material;found=1;
                }
            }
            for(j=0;j<sec->sphere_count;++j){
                const vc_col_sphere_t *sp=&g_vc_collision.spheres[sec->sphere_base+j];
                float dx=ux-sp->x,dz=uz-sp->z,h2=sp->r*sp->r-dx*dx-dz*dz;
                float y;
                if(h2<0.0f)continue;
                y=sp->y+sqrtf(h2);
                if(y<=top+0.02f && y>=bottom-0.02f && (!found||y>best)){
                    best=y;best_surface=sp->surface;found=1;
                }
            }
        }
    }

    if(found){
        if(out_y)*out_y=best*scale;
        if(out_surface)*out_surface=best_surface;
        return 1;
    }
    return 0;
}

/* Developer/escape helper only. Normal driving uses native COL directly. */
static int vc_collision_vertical_contact(
    float world_x,float world_z,float top_y,float bottom_y,
    float *out_y,uint8_t *out_surface)
{
    if(vc_collision_vertical_contact_native(
        world_x,world_z,top_y,bottom_y,out_y,out_surface))
        return 1;

    {
        float vy=0.0f;
        if(vc_visual_vertical_contact(world_x,world_z,top_y,bottom_y,&vy)){
            if(out_y)*out_y=vy;
            if(out_surface)*out_surface=254U;
            return 1;
        }
    }
    return 0;
}

static int vc_segment_triangle_hit(
    v3f_t a,v3f_t b,const vc_col_tri_t *tri,float *out_t,v3f_t *out_n)
{
    v3f_t d={b.x-a.x,b.y-a.y,b.z-a.z};
    v3f_t e1={tri->bx-tri->ax,tri->by-tri->ay,tri->bz-tri->az};
    v3f_t e2={tri->cx-tri->ax,tri->cy-tri->ay,tri->cz-tri->az};
    v3f_t p={
        d.y*e2.z-d.z*e2.y,
        d.z*e2.x-d.x*e2.z,
        d.x*e2.y-d.y*e2.x
    };
    float det=e1.x*p.x+e1.y*p.y+e1.z*p.z;
    float inv,u,v,t;
    v3f_t tv,q,n;
    float nl,nd;

    if(fabsf(det)<1.0e-7f)return 0;
    inv=1.0f/det;
    tv=(v3f_t){a.x-tri->ax,a.y-tri->ay,a.z-tri->az};
    u=(tv.x*p.x+tv.y*p.y+tv.z*p.z)*inv;
    if(u<-0.0005f||u>1.0005f)return 0;

    q=(v3f_t){
        tv.y*e1.z-tv.z*e1.y,
        tv.z*e1.x-tv.x*e1.z,
        tv.x*e1.y-tv.y*e1.x
    };
    v=(d.x*q.x+d.y*q.y+d.z*q.z)*inv;
    if(v<-0.0005f||u+v>1.0005f)return 0;
    t=(e2.x*q.x+e2.y*q.y+e2.z*q.z)*inv;
    if(t<-0.0005f||t>1.0005f)return 0;

    n=(v3f_t){
        e1.y*e2.z-e1.z*e2.y,
        e1.z*e2.x-e1.x*e2.z,
        e1.x*e2.y-e1.y*e2.x
    };
    nl=sqrtf(n.x*n.x+n.y*n.y+n.z*n.z);
    if(nl<1.0e-7f)return 0;
    n.x/=nl;n.y/=nl;n.z/=nl;

    /* reVC spring force must oppose p0->p1. Keep COL winding irrelevant. */
    nd=n.x*d.x+n.y*d.y+n.z*d.z;
    if(nd>0.0f){n.x=-n.x;n.y=-n.y;n.z=-n.z;}

    if(out_t)*out_t=clampf_local(t,0.0f,1.0f);
    if(out_n)*out_n=n;
    return 1;
}

static int vc_segment_sphere_hit(
    v3f_t a,v3f_t b,const vc_col_sphere_t *sp,float *out_t,v3f_t *out_n)
{
    v3f_t d={b.x-a.x,b.y-a.y,b.z-a.z};
    v3f_t m={a.x-sp->x,a.y-sp->y,a.z-sp->z};
    float aa=d.x*d.x+d.y*d.y+d.z*d.z;
    float bb=2.0f*(m.x*d.x+m.y*d.y+m.z*d.z);
    float cc=m.x*m.x+m.y*m.y+m.z*m.z-sp->r*sp->r;
    float disc,t0,t1,t;
    v3f_t n;
    float nl,nd;

    if(aa<1.0e-10f)return 0;
    disc=bb*bb-4.0f*aa*cc;
    if(disc<0.0f)return 0;
    disc=sqrtf(disc);
    t0=(-bb-disc)/(2.0f*aa);
    t1=(-bb+disc)/(2.0f*aa);
    t=(t0>=0.0f&&t0<=1.0f)?t0:((t1>=0.0f&&t1<=1.0f)?t1:-1.0f);
    if(t<0.0f)return 0;

    n=(v3f_t){
        a.x+d.x*t-sp->x,
        a.y+d.y*t-sp->y,
        a.z+d.z*t-sp->z
    };
    nl=sqrtf(n.x*n.x+n.y*n.y+n.z*n.z);
    if(nl<1.0e-7f)return 0;
    n.x/=nl;n.y/=nl;n.z/=nl;
    nd=n.x*d.x+n.y*d.y+n.z*d.z;
    if(nd>0.0f){n.x=-n.x;n.y=-n.y;n.z=-n.z;}

    if(out_t)*out_t=t;
    if(out_n)*out_n=n;
    return 1;
}

/*
 * reVC-style suspension query: intersect the actual transformed suspension
 * segment with GTA COL, rather than sampling a vertical height at its midpoint.
 * VCMAP is deliberately NOT a fallback here; normal driving must use native COL.
 */
static int vc_collision_suspension_segment(
    v3f_t world_p0,v3f_t world_p1,vc_wheel_contact_t *out)
{
    float scale,best_t=2.0f,line_len,spring_len,wheel_fraction,ratio;
    v3f_t a,b,d,best_n={0,1,0},spring_dir;
    uint8_t best_surface=0;
    int sx0,sx1,sz0,sz1,found=0;
    uint32_t i,j;

    if(!g_vc_collision.loaded || !out)return 0;
    memset(out,0,sizeof(*out));

    scale=g_vc_collision.world_scale;
    a=(v3f_t){world_p0.x/scale,world_p0.y/scale,world_p0.z/scale};
    b=(v3f_t){world_p1.x/scale,world_p1.y/scale,world_p1.z/scale};
    d=(v3f_t){b.x-a.x,b.y-a.y,b.z-a.z};

    sx0=(int)floorf(fminf(a.x,b.x)/g_vc_collision.sector_m)-1;
    sx1=(int)floorf(fmaxf(a.x,b.x)/g_vc_collision.sector_m)+1;
    sz0=(int)floorf(fminf(a.z,b.z)/g_vc_collision.sector_m)-1;
    sz1=(int)floorf(fmaxf(a.z,b.z)/g_vc_collision.sector_m)+1;

    if(g_vc_collision.version==1){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector_t *sec=&g_vc_collision.sectors[i];
            if(sec->sx<sx0||sec->sx>sx1||sec->sz<sz0||sec->sz>sz1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *tri=&g_vc_collision.tris[sec->tri_base+j];
                float t;v3f_t n;
                if(!(tri->flags&1U))continue;
                if(vc_segment_triangle_hit(a,b,tri,&t,&n) && t<best_t){
                    best_t=t;best_n=n;best_surface=tri->material;found=1;
                }
            }
        }
    }else if(g_vc_collision.version==2){
        for(i=0;i<g_vc_collision.sector_count;++i){
            const vc_col_sector2_t *sec=&g_vc_collision.sectors2[i];
            if(sec->sx<sx0||sec->sx>sx1||sec->sz<sz0||sec->sz>sz1)continue;
            for(j=0;j<sec->tri_count;++j){
                const vc_col_tri_t *tri=&g_vc_collision.tris[sec->tri_base+j];
                float t;v3f_t n;
                if(vc_segment_triangle_hit(a,b,tri,&t,&n) && t<best_t){
                    best_t=t;best_n=n;best_surface=tri->material;found=1;
                }
            }
            for(j=0;j<sec->sphere_count;++j){
                const vc_col_sphere_t *sp=&g_vc_collision.spheres[sec->sphere_base+j];
                float t;v3f_t n;
                if(vc_segment_sphere_hit(a,b,sp,&t,&n) && t<best_t){
                    best_t=t;best_n=n;best_surface=sp->surface;found=1;
                }
            }
        }
    }
    if(!found)return 0;

    spring_dir=(v3f_t){
        world_p1.x-world_p0.x,
        world_p1.y-world_p0.y,
        world_p1.z-world_p0.z
    };
    line_len=sqrtf(
        spring_dir.x*spring_dir.x+
        spring_dir.y*spring_dir.y+
        spring_dir.z*spring_dir.z);
    if(line_len<1.0e-5f)return 0;
    spring_dir.x/=line_len;spring_dir.y/=line_len;spring_dir.z/=line_len;

    /*
     * reVC ProcessControl rescales ProcessColModels' line ratio to remove the
     * tyre-radius part of SetupSuspensionLines.
     */
    spring_len=fabsf(g_vc_vehicle.suspension_upper-g_vc_vehicle.suspension_lower)*scale;
    wheel_fraction=1.0f-clampf_local(spring_len/line_len,0.05f,1.0f);
    wheel_fraction=clampf_local(wheel_fraction,0.0f,0.95f);
    ratio=(best_t-wheel_fraction)/fmaxf(0.05f,1.0f-wheel_fraction);
    ratio=clampf_local(ratio,0.0f,1.0f);

    out->hit=1;
    out->ratio=ratio;
    out->point=(v3f_t){
        (a.x+d.x*best_t)*scale,
        (a.y+d.y*best_t)*scale,
        (a.z+d.z*best_t)*scale
    };
    out->normal=best_n;
    out->spring_dir=spring_dir;
    out->surface=best_surface;
    return 1;
}

/*
 * A thin native-COL tolerance envelope around a real suspension line.
 *
 * reVC can process collision before/after several rigid-body integration steps;
 * Racer's compact 60 Hz loop can move a wheel line completely across a thin
 * road triangle between two samples. This rescue extends the SAME GTA
 * suspension line only by a fraction of the tyre radius. It never queries
 * VCMAP and therefore cannot create the old surface=254 pseudo-road.
 */
static int vc_collision_suspension_rescue(
    v3f_t world_p0,v3f_t world_p1,vc_wheel_contact_t *out)
{
    v3f_t d={
        world_p1.x-world_p0.x,
        world_p1.y-world_p0.y,
        world_p1.z-world_p0.z
    };
    float len2=d.x*d.x+d.y*d.y+d.z*d.z;
    float len,inv,pad,raw_t,scale,spring_len,wheel_fraction,ratio;
    v3f_t dir,a,b;
    vc_wheel_contact_t tmp;

    if(len2<1.0e-6f || !out)return 0;
    len=sqrtf(len2);inv=1.0f/len;
    dir=(v3f_t){d.x*inv,d.y*inv,d.z*inv};
    pad=clampf_local(active_vehicle_wheel_radius()*0.32f,8.0f,len*0.32f);
    a=(v3f_t){
        world_p0.x-dir.x*pad,
        world_p0.y-dir.y*pad,
        world_p0.z-dir.z*pad
    };
    b=(v3f_t){
        world_p1.x+dir.x*pad,
        world_p1.y+dir.y*pad,
        world_p1.z+dir.z*pad
    };

    if(!vc_collision_suspension_segment(a,b,&tmp))return 0;

    raw_t=((tmp.point.x-world_p0.x)*d.x+
           (tmp.point.y-world_p0.y)*d.y+
           (tmp.point.z-world_p0.z)*d.z)/len2;

    scale=g_vc_collision.world_scale;
    spring_len=fabsf(g_vc_vehicle.suspension_upper-
                     g_vc_vehicle.suspension_lower)*scale;
    wheel_fraction=1.0f-clampf_local(spring_len/len,0.05f,1.0f);
    wheel_fraction=clampf_local(wheel_fraction,0.0f,0.95f);
    ratio=(raw_t-wheel_fraction)/fmaxf(0.05f,1.0f-wheel_fraction);

    /*
     * If the surface lies just beyond the nominal p1 after one integration
     * step, keep a small real spring compression so gravity cannot tunnel the
     * chassis through the road before the next exact line hit.
     */
    if(raw_t>1.0f)ratio=fminf(ratio,0.94f);
    ratio=clampf_local(ratio,0.0f,0.94f);

    *out=tmp;
    out->ratio=ratio;
    out->spring_dir=dir;
    return 1;
}

static int vc_collision_four_contacts(
    float world_x,float world_z,float heading,float current_ground,
    float wheelbase,float track,
    float *out_ground,float *out_pitch,float *out_roll)
{
    float y[4]={0,0,0,0};
    float wx[4]={0,0,0,0},wz[4]={0,0,0,0};
    int ok[4]={0,0,0,0};
    vc_wheel_contact_t prev_contact[4];
    uint8_t prev_surface[4];
    int i,count=0;
    float sum=0.0f;
    float pitch_span=fmaxf(160.0f,wheelbase*0.84f);
    float roll_span=fmaxf(110.0f,track*0.86f);

    memcpy(prev_contact,g_vc_wheel_contact,sizeof(prev_contact));
    memcpy(prev_surface,g_vc_wheel_surface,sizeof(prev_surface));
    memset(g_vc_wheel_contact,0,sizeof(g_vc_wheel_contact));
    g_vc_wheel_contact_mask=0;
    g_vc_wheel_latched_mask=0;
    g_vc_wheel_exact_mask=0;
    g_vc_wheel_rescue_mask=0;
    g_vc_front_support=g_vc_rear_support=0;
    g_vc_left_support=g_vc_right_support=0;

    if(g_vc_vehicle.native_col_loaded &&
       g_vc_vehicle.col_line_count>=4U &&
       g_vc_vehicle.col_lines){
        if(!g_vc_body_basis_valid)vc_body_basis_from_euler();

        for(i=0;i<(int)g_vc_vehicle.col_line_count;++i){
            const vcveh_col_line_t *ln=&g_vc_vehicle.col_lines[i];
            int idx=(int)ln->part-1;
            v3f_t p0={ln->p0x,ln->p0y,ln->p0z};
            v3f_t p1={ln->p1x,ln->p1y,ln->p1z};
            v3f_t q0,q1,w0,w1;

            if(idx<0||idx>3)continue;
            vc_body_rotate_local(p0,&q0);
            vc_body_rotate_local(p1,&q1);
            w0=(v3f_t){world_x+q0.x,g_world_y+q0.y,world_z+q0.z};
            w1=(v3f_t){world_x+q1.x,g_world_y+q1.y,world_z+q1.z};

            if(vc_collision_suspension_segment(w0,w1,&g_vc_wheel_contact[idx])){
                vc_wheel_contact_t *c=&g_vc_wheel_contact[idx];
                ok[idx]=1;
                y[idx]=c->point.y;wx[idx]=c->point.x;wz[idx]=c->point.z;
                sum+=y[idx];count++;
                g_vc_wheel_surface[idx]=c->surface;
                g_vc_wheel_contact_mask|=(uint8_t)(1U<<idx);
                g_vc_wheel_exact_mask|=(uint8_t)(1U<<idx);
                g_vc_wheel_timer[idx]=4.0f;
            }else if(vc_collision_suspension_rescue(
                         w0,w1,&g_vc_wheel_contact[idx])){
                vc_wheel_contact_t *c=&g_vc_wheel_contact[idx];
                ok[idx]=1;
                y[idx]=c->point.y;wx[idx]=c->point.x;wz[idx]=c->point.z;
                sum+=y[idx];count++;
                g_vc_wheel_surface[idx]=c->surface;
                g_vc_wheel_contact_mask|=(uint8_t)(1U<<idx);
                g_vc_wheel_rescue_mask|=(uint8_t)(1U<<idx);
                g_vc_wheel_timer[idx]=4.0f;
            }else{
                g_vc_wheel_timer[idx]=fmaxf(0.0f,g_vc_wheel_timer[idx]-1.0f);
                if(g_vc_wheel_timer[idx]>0.0f && prev_contact[idx].hit){
                    v3f_t sd={w1.x-w0.x,w1.y-w0.y,w1.z-w0.z};
                    float sm=sqrtf(sd.x*sd.x+sd.y*sd.y+sd.z*sd.z);
                    g_vc_wheel_contact[idx]=prev_contact[idx];
                    g_vc_wheel_contact[idx].hit=1;
                    g_vc_wheel_contact[idx].ratio=1.0f; /* no spring force while latched */
                    g_vc_wheel_contact[idx].point=w1;
                    if(sm>1.0e-5f){
                        g_vc_wheel_contact[idx].spring_dir=(v3f_t){
                            sd.x/sm,sd.y/sm,sd.z/sm
                        };
                    }
                    g_vc_wheel_surface[idx]=prev_surface[idx];
                    g_vc_wheel_latched_mask|=(uint8_t)(1U<<idx);
                }else{
                    g_vc_wheel_surface[idx]=0;
                }
            }
        }

        if(ok[0]&&ok[1]&&ok[2]&&ok[3]){
            float frontx=(wx[0]+wx[1])*0.5f,frontz=(wz[0]+wz[1])*0.5f;
            float rearx=(wx[2]+wx[3])*0.5f,rearz=(wz[2]+wz[3])*0.5f;
            float dx=frontx-rearx,dz=frontz-rearz;
            float lx=(wx[0]+wx[2])*0.5f,lz=(wz[0]+wz[2])*0.5f;
            float rx=(wx[1]+wx[3])*0.5f,rz=(wz[1]+wz[3])*0.5f;
            pitch_span=fmaxf(80.0f,sqrtf(dx*dx+dz*dz));
            dx=lx-rx;dz=lz-rz;
            roll_span=fmaxf(60.0f,sqrtf(dx*dx+dz*dz));
        }
    }else{
        const v3f_t pivots[4]={
            sports_wheel_fl_pivot,sports_wheel_fr_pivot,
            sports_wheel_rl_pivot,sports_wheel_rr_pivot
        };
        float scale=vc_runtime_world_scale();
        float upper=g_vehicle_handling.suspension_upper*scale;
        float lower=g_vehicle_handling.suspension_lower*scale;
        float tyre=active_vehicle_wheel_radius();
        if(!g_vc_body_basis_valid)vc_body_basis_from_euler();

        /*
         * The Rally sports model is visual-only, but its four wheel pivots are
         * real geometry. Build the same p0/p1 suspension line shape that
         * CAutomobile::SetupSuspensionLines creates for a GTA vehicle:
         *   p0 = wheel + upper limit
         *   p1 = wheel + lower limit - tyre radius.
         * This feeds the exact same native-COL contact/spring path as VCVEH.
         */
        for(i=0;i<4;++i){
            v3f_t p0=pivots[i],p1=pivots[i],q0,q1,w0,w1;
            p0.y+=upper;
            p1.y+=lower-tyre;
            vc_body_rotate_local(p0,&q0);
            vc_body_rotate_local(p1,&q1);
            w0=(v3f_t){world_x+q0.x,g_world_y+q0.y,world_z+q0.z};
            w1=(v3f_t){world_x+q1.x,g_world_y+q1.y,world_z+q1.z};

            if(vc_collision_suspension_segment(w0,w1,&g_vc_wheel_contact[i]) ||
               vc_collision_suspension_rescue(w0,w1,&g_vc_wheel_contact[i])){
                vc_wheel_contact_t *c=&g_vc_wheel_contact[i];
                ok[i]=1;
                y[i]=c->point.y;wx[i]=c->point.x;wz[i]=c->point.z;
                sum+=y[i];count++;
                g_vc_wheel_surface[i]=c->surface;
                g_vc_wheel_contact_mask|=(uint8_t)(1U<<i);
                if(c->ratio<0.9999f)g_vc_wheel_timer[i]=4.0f;
            }else{
                g_vc_wheel_timer[i]=fmaxf(0.0f,g_vc_wheel_timer[i]-1.0f);
                if(g_vc_wheel_timer[i]>0.0f && prev_contact[i].hit){
                    v3f_t sd={w1.x-w0.x,w1.y-w0.y,w1.z-w0.z};
                    float sm=sqrtf(sd.x*sd.x+sd.y*sd.y+sd.z*sd.z);
                    g_vc_wheel_contact[i]=prev_contact[i];
                    g_vc_wheel_contact[i].hit=1;
                    g_vc_wheel_contact[i].ratio=1.0f;
                    g_vc_wheel_contact[i].point=w1;
                    if(sm>1.0e-5f){
                        g_vc_wheel_contact[i].spring_dir=(v3f_t){
                            sd.x/sm,sd.y/sm,sd.z/sm
                        };
                    }
                    g_vc_wheel_surface[i]=prev_surface[i];
                    g_vc_wheel_latched_mask|=(uint8_t)(1U<<i);
                }else{
                    g_vc_wheel_surface[i]=0;
                }
            }
        }

        if(ok[0]&&ok[1]&&ok[2]&&ok[3]){
            float frontx=(wx[0]+wx[1])*0.5f,frontz=(wz[0]+wz[1])*0.5f;
            float rearx=(wx[2]+wx[3])*0.5f,rearz=(wz[2]+wz[3])*0.5f;
            float dx=frontx-rearx,dz=frontz-rearz;
            float lx=(wx[0]+wx[2])*0.5f,lz=(wz[0]+wz[2])*0.5f;
            float rx=(wx[1]+wx[3])*0.5f,rz=(wz[1]+wz[3])*0.5f;
            pitch_span=fmaxf(80.0f,sqrtf(dx*dx+dz*dz));
            dx=lx-rx;dz=lz-rz;
            roll_span=fmaxf(60.0f,sqrtf(dx*dx+dz*dz));
        }
    }

    /*
     * Stacked-deck/seam coherence.
     *
     * VFW collision can legitimately contain a road below a bridge or another
     * deck within the same X/Z footprint. A single suspension line choosing a
     * different deck than the other wheels creates an enormous pitch/roll
     * lever and is the main cause of the car standing on its nose/tail.
     *
     * Keep a coherent cluster around the previous ground when at least two
     * wheels agree with it; otherwise choose the densest current contact
     * cluster (important after jumps/teleports). Small curbs and ramps remain
     * inside the tolerance and are not flattened.
     */
    if(count>=2){
        float tol=fmaxf(
            active_suspension_travel_world()*1.35f,
            active_vehicle_wheel_radius()*1.15f);
        float ref=current_ground;
        int near_prev=0,best_i=-1,best_n=-1;

        for(i=0;i<4;++i)
            if(ok[i] && fabsf(y[i]-current_ground)<=tol)near_prev++;

        if(near_prev<2){
            int a,b;
            for(a=0;a<4;++a)if(ok[a]){
                int n=0;
                for(b=0;b<4;++b)
                    if(ok[b] && fabsf(y[b]-y[a])<=tol)n++;
                if(n>best_n){best_n=n;best_i=a;}
            }
            if(best_i>=0)ref=y[best_i];
        }

        if(near_prev>=2 || best_n>=2){
            int rejected=0;
            for(i=0;i<4;++i){
                if(!ok[i] || fabsf(y[i]-ref)<=tol)continue;
                ok[i]=0;
                g_vc_wheel_contact[i].hit=0;
                g_vc_wheel_surface[i]=0;
                g_vc_wheel_contact_mask&=(uint8_t)~(1U<<i);
                g_vc_wheel_exact_mask&=(uint8_t)~(1U<<i);
                g_vc_wheel_rescue_mask&=(uint8_t)~(1U<<i);
                g_vc_wheel_timer[i]=fmaxf(0.0f,g_vc_wheel_timer[i]-1.0f);
                rejected++;
            }
            if(rejected){
                g_vc_deck_rejects_window+=(unsigned)rejected;
                sum=0.0f;count=0;
                for(i=0;i<4;++i)if(ok[i]){sum+=y[i];count++;}
            }
        }
    }

    if(count<1)return 0;

    if(out_ground)*out_ground=sum/(float)count;
    {
        float front=0.0f,rear=0.0f,left=0.0f,right=0.0f;
        int nf=0,nr=0,nl=0,nrr=0;
        if(ok[0]){front+=y[0];nf++;left+=y[0];nl++;}
        if(ok[1]){front+=y[1];nf++;right+=y[1];nrr++;}
        if(ok[2]){rear+=y[2];nr++;left+=y[2];nl++;}
        if(ok[3]){rear+=y[3];nr++;right+=y[3];nrr++;}

        g_vc_front_support=nf;
        g_vc_rear_support=nr;
        g_vc_left_support=nl;
        g_vc_right_support=nrr;

        if(nf&&nr){
            float p=atan2f(front/(float)nf-rear/(float)nr,pitch_span);
            g_vc_last_support_pitch=p;
            if(out_pitch)*out_pitch=p;
        }else if(out_pitch)*out_pitch=g_vc_last_support_pitch;

        if(nl&&nrr){
            float r=atan2f(left/(float)nl-right/(float)nrr,roll_span);
            g_vc_last_support_roll=r;
            if(out_roll)*out_roll=r;
        }else if(out_roll)*out_roll=g_vc_last_support_roll;
    }
    return count;
}

static float vc_col_point_seg_dist2(
    float px,float pz,float ax,float az,float bx,float bz)
{
    float dx=bx-ax,dz=bz-az,den=dx*dx+dz*dz,t,qx,qz;
    if(den<=1.0e-9f){
        dx=px-ax;dz=pz-az;return dx*dx+dz*dz;
    }
    t=((px-ax)*dx+(pz-az)*dz)/den;
    if(t<0.0f)t=0.0f;if(t>1.0f)t=1.0f;
    qx=ax+t*dx;qz=az+t*dz;
    dx=px-qx;dz=pz-qz;
    return dx*dx+dz*dz;
}

static int vc_collision_hits_solid(float world_x,float world_y,float world_z,float radius_world)
{
    float scale,px,pz,py,r,r2;
    int psx,psz;
    uint32_t i,j;
    if(!g_vc_collision.loaded || g_vc_collision.version!=1)return 0;

    scale=g_vc_collision.world_scale;
    px=world_x/scale;pz=world_z/scale;py=world_y/scale;
    r=fmaxf(0.25f,radius_world/scale);r2=r*r;
    psx=(int)floorf(px/g_vc_collision.sector_m);
    psz=(int)floorf(pz/g_vc_collision.sector_m);

    for(i=0;i<g_vc_collision.sector_count;++i){
        const vc_col_sector_t *sec=&g_vc_collision.sectors[i];
        if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
        for(j=0;j<sec->tri_count;++j){
            const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
            float miny,maxy,d2;
            if((t->flags&3U)!=2U)continue;
            miny=fminf(t->ay,fminf(t->by,t->cy));
            maxy=fmaxf(t->ay,fmaxf(t->by,t->cy));
            if(py<miny-0.25f||py>maxy+1.5f)continue;
            d2=vc_col_point_seg_dist2(px,pz,t->ax,t->az,t->bx,t->bz);
            if(vc_col_point_seg_dist2(px,pz,t->bx,t->bz,t->cx,t->cz)<d2)
                d2=vc_col_point_seg_dist2(px,pz,t->bx,t->bz,t->cx,t->cz);
            if(vc_col_point_seg_dist2(px,pz,t->cx,t->cz,t->ax,t->az)<d2)
                d2=vc_col_point_seg_dist2(px,pz,t->cx,t->cz,t->ax,t->az);
            if(d2<=r2){g_vc_last_body_surface=t->material;return 1;}
        }
    }
    return 0;
}

static float vc_point_seg3_dist2(
    float px,float py,float pz,
    float ax,float ay,float az,float bx,float by,float bz)
{
    float dx=bx-ax,dy=by-ay,dz=bz-az;
    float den=dx*dx+dy*dy+dz*dz;
    float t,qx,qy,qz;
    if(den<=1.0e-12f){
        dx=px-ax;dy=py-ay;dz=pz-az;
        return dx*dx+dy*dy+dz*dz;
    }
    t=((px-ax)*dx+(py-ay)*dy+(pz-az)*dz)/den;
    if(t<0.0f)t=0.0f;if(t>1.0f)t=1.0f;
    qx=ax+t*dx;qy=ay+t*dy;qz=az+t*dz;
    dx=px-qx;dy=py-qy;dz=pz-qz;
    return dx*dx+dy*dy+dz*dz;
}

static float vc_point_tri_closest(
    float px,float py,float pz,const vc_col_tri_t *t,
    float *out_x,float *out_y,float *out_z)
{
    float abx=t->bx-t->ax,aby=t->by-t->ay,abz=t->bz-t->az;
    float acx=t->cx-t->ax,acy=t->cy-t->ay,acz=t->cz-t->az;
    float apx=px-t->ax,apy=py-t->ay,apz=pz-t->az;
    float d1=abx*apx+aby*apy+abz*apz;
    float d2=acx*apx+acy*apy+acz*apz;
    float bpx,bpy,bpz,d3,d4,vc;
    float cpx,cpy,cpz,d5,d6,vb,va,den,v,w,qx,qy,qz,dx,dy,dz;

    if(d1<=0.0f && d2<=0.0f){ qx=t->ax;qy=t->ay;qz=t->az;goto done; }
    bpx=px-t->bx;bpy=py-t->by;bpz=pz-t->bz;
    d3=abx*bpx+aby*bpy+abz*bpz;
    d4=acx*bpx+acy*bpy+acz*bpz;
    if(d3>=0.0f && d4<=d3){ qx=t->bx;qy=t->by;qz=t->bz;goto done; }

    vc=d1*d4-d3*d2;
    if(vc<=0.0f && d1>=0.0f && d3<=0.0f){
        den=d1-d3;v=fabsf(den)>1.0e-12f?d1/den:0.0f;
        qx=t->ax+v*abx;qy=t->ay+v*aby;qz=t->az+v*abz;goto done;
    }

    cpx=px-t->cx;cpy=py-t->cy;cpz=pz-t->cz;
    d5=abx*cpx+aby*cpy+abz*cpz;
    d6=acx*cpx+acy*cpy+acz*cpz;
    if(d6>=0.0f && d5<=d6){ qx=t->cx;qy=t->cy;qz=t->cz;goto done; }

    vb=d5*d2-d1*d6;
    if(vb<=0.0f && d2>=0.0f && d6<=0.0f){
        den=d2-d6;w=fabsf(den)>1.0e-12f?d2/den:0.0f;
        qx=t->ax+w*acx;qy=t->ay+w*acy;qz=t->az+w*acz;goto done;
    }

    va=d3*d6-d5*d4;
    if(va<=0.0f && (d4-d3)>=0.0f && (d5-d6)>=0.0f){
        float bcx=t->cx-t->bx,bcy=t->cy-t->by,bcz=t->cz-t->bz;
        den=(d4-d3)+(d5-d6);
        w=fabsf(den)>1.0e-12f?(d4-d3)/den:0.0f;
        qx=t->bx+w*bcx;qy=t->by+w*bcy;qz=t->bz+w*bcz;goto done;
    }

    den=va+vb+vc;
    if(fabsf(den)<=1.0e-12f){
        float da=(px-t->ax)*(px-t->ax)+(py-t->ay)*(py-t->ay)+(pz-t->az)*(pz-t->az);
        float db=(px-t->bx)*(px-t->bx)+(py-t->by)*(py-t->by)+(pz-t->bz)*(pz-t->bz);
        float dc=(px-t->cx)*(px-t->cx)+(py-t->cy)*(py-t->cy)+(pz-t->cz)*(pz-t->cz);
        if(da<=db && da<=dc){qx=t->ax;qy=t->ay;qz=t->az;}
        else if(db<=dc){qx=t->bx;qy=t->by;qz=t->bz;}
        else{qx=t->cx;qy=t->cy;qz=t->cz;}
        goto done;
    }

    den=1.0f/den;v=vb*den;w=vc*den;
    qx=t->ax+abx*v+acx*w;
    qy=t->ay+aby*v+acy*w;
    qz=t->az+abz*v+acz*w;

done:
    if(out_x)*out_x=qx;if(out_y)*out_y=qy;if(out_z)*out_z=qz;
    dx=px-qx;dy=py-qy;dz=pz-qz;
    return dx*dx+dy*dy+dz*dz;
}

/* Squared distance from a point to one GTA COL triangle in 3D. */
static float vc_point_tri_dist2(float px,float py,float pz,const vc_col_tri_t *t)
{
    float abx=t->bx-t->ax,aby=t->by-t->ay,abz=t->bz-t->az;
    float acx=t->cx-t->ax,acy=t->cy-t->ay,acz=t->cz-t->az;
    float apx=px-t->ax,apy=py-t->ay,apz=pz-t->az;
    float d1=abx*apx+aby*apy+abz*apz;
    float d2=acx*apx+acy*apy+acz*apz;
    float bpx,bpy,bpz,d3,d4,vc;
    float cpx,cpy,cpz,d5,d6,vb,va,den,v,w,qx,qy,qz,dx,dy,dz;

    if(d1<=0.0f && d2<=0.0f)
        return apx*apx+apy*apy+apz*apz;

    bpx=px-t->bx;bpy=py-t->by;bpz=pz-t->bz;
    d3=abx*bpx+aby*bpy+abz*bpz;
    d4=acx*bpx+acy*bpy+acz*bpz;
    if(d3>=0.0f && d4<=d3)
        return bpx*bpx+bpy*bpy+bpz*bpz;

    vc=d1*d4-d3*d2;
    if(vc<=0.0f && d1>=0.0f && d3<=0.0f){
        den=d1-d3;
        v=fabsf(den)>1.0e-12f?d1/den:0.0f;
        qx=t->ax+v*abx;qy=t->ay+v*aby;qz=t->az+v*abz;
        dx=px-qx;dy=py-qy;dz=pz-qz;
        return dx*dx+dy*dy+dz*dz;
    }

    cpx=px-t->cx;cpy=py-t->cy;cpz=pz-t->cz;
    d5=abx*cpx+aby*cpy+abz*cpz;
    d6=acx*cpx+acy*cpy+acz*cpz;
    if(d6>=0.0f && d5<=d6)
        return cpx*cpx+cpy*cpy+cpz*cpz;

    vb=d5*d2-d1*d6;
    if(vb<=0.0f && d2>=0.0f && d6<=0.0f){
        den=d2-d6;
        w=fabsf(den)>1.0e-12f?d2/den:0.0f;
        qx=t->ax+w*acx;qy=t->ay+w*acy;qz=t->az+w*acz;
        dx=px-qx;dy=py-qy;dz=pz-qz;
        return dx*dx+dy*dy+dz*dz;
    }

    va=d3*d6-d5*d4;
    if(va<=0.0f && (d4-d3)>=0.0f && (d5-d6)>=0.0f){
        float bcx=t->cx-t->bx,bcy=t->cy-t->by,bcz=t->cz-t->bz;
        den=(d4-d3)+(d5-d6);
        w=fabsf(den)>1.0e-12f?(d4-d3)/den:0.0f;
        qx=t->bx+w*bcx;qy=t->by+w*bcy;qz=t->bz+w*bcz;
        dx=px-qx;dy=py-qy;dz=pz-qz;
        return dx*dx+dy*dy+dz*dz;
    }

    den=va+vb+vc;
    if(fabsf(den)<=1.0e-12f){
        float e0=vc_point_seg3_dist2(px,py,pz,t->ax,t->ay,t->az,t->bx,t->by,t->bz);
        float e1=vc_point_seg3_dist2(px,py,pz,t->bx,t->by,t->bz,t->cx,t->cy,t->cz);
        float e2=vc_point_seg3_dist2(px,py,pz,t->cx,t->cy,t->cz,t->ax,t->ay,t->az);
        return fminf(e0,fminf(e1,e2));
    }
    den=1.0f/den;
    v=vb*den;w=vc*den;
    qx=t->ax+abx*v+acx*w;
    qy=t->ay+aby*v+acy*w;
    qz=t->az+abz*v+acz*w;
    dx=px-qx;dy=py-qy;dz=pz-qz;
    return dx*dx+dy*dy+dz*dz;
}

static int vc_collision_body_sphere_contact(
    float world_x,float world_y,float world_z,float radius_world,
    vc_body_contact_t *out)
{
    float scale,px,py,pz,r,r2,best_depth=0.0f;
    int psx,psz,found=0;
    uint32_t i,j;
    vc_body_contact_t best={0};

    if(!g_vc_collision.loaded || g_vc_collision.version!=2)return 0;
    scale=g_vc_collision.world_scale;
    px=world_x/scale;py=world_y/scale;pz=world_z/scale;
    r=fmaxf(0.08f,radius_world/scale);r2=r*r;
    psx=(int)floorf(px/g_vc_collision.sector_m);
    psz=(int)floorf(pz/g_vc_collision.sector_m);

    for(i=0;i<g_vc_collision.sector_count;++i){
        const vc_col_sector2_t *sec=&g_vc_collision.sectors2[i];
        if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;

        for(j=0;j<sec->tri_count;++j){
            const vc_col_tri_t *t=&g_vc_collision.tris[sec->tri_base+j];
            float qx,qy,qz,d2=vc_point_tri_closest(px,py,pz,t,&qx,&qy,&qz);
            if(d2<r2){
                float d=sqrtf(fmaxf(d2,0.0f));
                float nx,ny,nz,depth=r-d;
                if(d>1.0e-6f){
                    nx=(px-qx)/d;ny=(py-qy)/d;nz=(pz-qz)/d;
                }else{
                    float abx=t->bx-t->ax,aby=t->by-t->ay,abz=t->bz-t->az;
                    float acx=t->cx-t->ax,acy=t->cy-t->ay,acz=t->cz-t->az;
                    float cx=aby*acz-abz*acy;
                    float cy=abz*acx-abx*acz;
                    float cz=abx*acy-aby*acx;
                    float len=sqrtf(cx*cx+cy*cy+cz*cz);
                    float mx=(t->ax+t->bx+t->cx)/3.0f;
                    float my=(t->ay+t->by+t->cy)/3.0f;
                    float mz=(t->az+t->bz+t->cz)/3.0f;
                    if(len>1.0e-6f){nx=cx/len;ny=cy/len;nz=cz/len;}
                    else{nx=1.0f;ny=0.0f;nz=0.0f;}
                    if(nx*(px-mx)+ny*(py-my)+nz*(pz-mz)<0.0f){
                        nx=-nx;ny=-ny;nz=-nz;
                    }
                }
                if(depth>best_depth){
                    best_depth=depth;found=1;
                    best.hit=1;best.nx=nx;best.ny=ny;best.nz=nz;
                    best.depth=depth*scale;
                    best.px=qx*scale;best.py=qy*scale;best.pz=qz*scale;
                    best.surface=t->material;best.piece=t->flags;
                }
            }
        }

        for(j=0;j<sec->sphere_count;++j){
            const vc_col_sphere_t *sp=&g_vc_collision.spheres[sec->sphere_base+j];
            float dx=px-sp->x,dy=py-sp->y,dz=pz-sp->z;
            float rr=r+sp->r,d2=dx*dx+dy*dy+dz*dz;
            if(d2<rr*rr){
                float d=sqrtf(fmaxf(d2,0.0f));
                float depth=rr-d;
                float nx,ny,nz;
                if(d>1.0e-6f){nx=dx/d;ny=dy/d;nz=dz/d;}
                else{nx=1.0f;ny=0.0f;nz=0.0f;}
                if(depth>best_depth){
                    best_depth=depth;found=1;
                    best.hit=1;best.nx=nx;best.ny=ny;best.nz=nz;
                    best.depth=depth*scale;
                    best.px=(sp->x+nx*sp->r)*scale;
                    best.py=(sp->y+ny*sp->r)*scale;
                    best.pz=(sp->z+nz*sp->r)*scale;
                    best.surface=sp->surface;best.piece=sp->piece;
                }
            }
        }
    }

    if(found){
        g_vc_last_body_surface=best.surface;
        if(out)*out=best;
    }
    return found;
}

static int vc_collision_body_sphere_hits(
    float world_x,float world_y,float world_z,float radius_world)
{
    return vc_collision_body_sphere_contact(
        world_x,world_y,world_z,radius_world,NULL);
}

static int vc_collision_vehicle_body_contact(
    float world_x,float world_y,float world_z,float heading,
    float wheelbase,float track,float wheel_radius,
    vc_body_contact_t *out)
{
    float sh,ch,half,radius,height,best_depth=0.0f;
    int k,found=0;
    static const float pos[3]={-0.31f,0.0f,0.31f};
    vc_body_contact_t best={0};

    if(!g_vc_collision.loaded)return 0;
    if(g_vc_collision.version==1){
        if(vc_collision_hits_solid(world_x,world_y,world_z,track*0.43f)){
            best.hit=1;best.nx=-sinf(heading);best.ny=0.0f;best.nz=-cosf(heading);
            best.depth=fmaxf(4.0f,track*0.04f);
            best.px=world_x;best.py=world_y;best.pz=world_z;
            if(out)*out=best;
            return 1;
        }
        return 0;
    }

    if(g_vc_vehicle.native_col_loaded &&
       g_vc_vehicle.col_sphere_count &&
       g_vc_vehicle.col_spheres){
        uint32_t i;
        (void)heading;
        if(!g_vc_body_basis_valid)vc_body_basis_from_euler();
        for(i=0;i<g_vc_vehicle.col_sphere_count;++i){
            const vcveh_col_sphere_t *sp=&g_vc_vehicle.col_spheres[i];
            v3f_t local={sp->x,sp->y,sp->z},q;
            vc_body_contact_t c={0};
            vc_body_rotate_local(local,&q);
            if(vc_collision_body_sphere_contact(
                world_x+q.x,world_y+q.y,world_z+q.z,sp->r,&c) &&
               (!found || c.depth>best_depth)){
                best=c;best_depth=c.depth;found=1;
            }
        }
        if(found && out)*out=best;
        return found;
    }

    sh=sinf(heading);ch=cosf(heading);
    half=fmaxf(80.0f,wheelbase);
    radius=fmaxf(38.0f,track*0.22f);
    height=fmaxf(radius*1.20f,wheel_radius*1.08f);
    for(k=0;k<3;++k){
        float off=half*pos[k];
        vc_body_contact_t c={0};
        if(vc_collision_body_sphere_contact(
            world_x+sh*off,world_y+height,world_z+ch*off,radius,&c) &&
           (!found || c.depth>best_depth)){
            best=c;best_depth=c.depth;found=1;
        }
    }
    if(found && out)*out=best;
    return found;
}

static int vc_collision_vehicle_body_hits(
    float world_x,float world_y,float world_z,float heading,
    float wheelbase,float track,float wheel_radius)
{
    return vc_collision_vehicle_body_contact(
        world_x,world_y,world_z,heading,wheelbase,track,wheel_radius,NULL);
}

static int vc_visual_top_height(float world_x,float world_z,float *out_y)
{
    if(g_vc_world_mode)return 0;
    int psx,psz,found=0;
    uint32_t i,j;
    float best=-1.0e30f;

    if(!g_vc_city_mode||g_vc_map.sector_world<=1.0f)return 0;
    psx=(int)floorf(world_x/g_vc_map.sector_world);
    psz=(int)floorf(world_z/g_vc_map.sector_world);

    for(i=0;i<g_vc_map.sector_count;++i){
        const vc_sector_t *sec=&g_vc_map.sectors[i];
        if(abs((int)sec->sx-psx)>1||abs((int)sec->sz-psz)>1)continue;
        for(j=0;j<sec->tri_count;++j){
            const vc_map_tri_t *t=&g_vc_map.tris[sec->tri_base+j];
            const vc_vertex_t *a=&g_vc_map.verts[sec->vertex_base+t->a];
            const vc_vertex_t *b=&g_vc_map.verts[sec->vertex_base+t->b];
            const vc_vertex_t *c=&g_vc_map.verts[sec->vertex_base+t->c];
            float ux=b->x-a->x,uy=b->y-a->y,uz=b->z-a->z;
            float vx=c->x-a->x,vy=c->y-a->y,vz=c->z-a->z;
            float nx=uy*vz-uz*vy;
            float ny=uz*vx-ux*vz;
            float nz=ux*vy-uy*vx;
            float nlen=sqrtf(nx*nx+ny*ny+nz*nz);
            float wa,wb,wc,y;
            if(nlen<=1.0e-6f || fabsf(ny)/nlen<0.62f)continue;
            if(!vc_point_in_tri_xz(world_x,world_z,a,b,c,&wa,&wb,&wc))continue;
            y=wa*a->y+wb*b->y+wc*c->y;
            if(!found || y>best){best=y;found=1;}
        }
    }
    if(found&&out_y)*out_y=best;
    return found;
}

static int vc_city_ground_height(float world_x,float world_z,float current_y,float *out_y)
{
    int psx,psz;
    if(vc_collision_ground_height(world_x,world_z,current_y,out_y))
        return 1;
    if(g_vc_world_mode)return 0;
    uint32_t i,j;
    float ux,uz;
    float best=0.0f,best_delta=1.0e30f;
    int found=0;

    if(!g_vc_city_mode||g_vc_map.sector_world<=1.0f)return 0;
    ux=world_x;
    uz=world_z;
    psx=(int)floorf(world_x/g_vc_map.sector_world);
    psz=(int)floorf(world_z/g_vc_map.sector_world);

    for(i=0;i<g_vc_map.sector_count;++i){
        const vc_sector_t *s=&g_vc_map.sectors[i];
        if(abs((int)s->sx-psx)>1||abs((int)s->sz-psz)>1)continue;
        for(j=0;j<s->tri_count;++j){
            const vc_map_tri_t *t=&g_vc_map.tris[s->tri_base+j];
            const vc_vertex_t *a,*b,*c;
            float wa,wb,wc,y,delta;
            if(!(t->flags&1U))continue;
            a=&g_vc_map.verts[s->vertex_base+t->a];
            b=&g_vc_map.verts[s->vertex_base+t->b];
            c=&g_vc_map.verts[s->vertex_base+t->c];
            if(!vc_point_in_tri_xz(ux,uz,a,b,c,&wa,&wb,&wc))continue;
            y=wa*a->y+wb*b->y+wc*c->y;
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
        if(dx<-VC_SECTOR_SPAN||dx>VC_SECTOR_SPAN||
           dz<-VC_SECTOR_SPAN||dz>VC_SECTOR_SPAN)continue;
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
    enum { MAX_VC_VISIBLE_SECTORS=1024 };
    float camx,camy,camz,camyaw,cam_cs,cam_sn,cam_cp,cam_sp;
    track_world_t car;
    int psx,psz;
    uint32_t vis_idx[MAX_VC_VISIBLE_SECTORS];
    uint8_t vis_slot[MAX_VC_VISIBLE_SECTORS];
    float vis_d2[MAX_VC_VISIBLE_SECTORS];
    int vis_count=0;
    int n=0,k,cap_hit=0;
    float sw=vc_runtime_sector_world();
    float far_world;
    uint64_t p0,p1,p2,p3,p4;
    int slot_begin=0,slot_end=1,slot;

    if(!g_vc_city_mode||sw<=1.0f)return;
    get_chase_camera(&camx,&camy,&camz,&camyaw);
    cam_cs=cosf(camyaw);
    cam_sn=sinf(camyaw);
    cam_cp=cosf(g_camera_pitch);
    cam_sp=sinf(g_camera_pitch);
    g_vc_frame_xformed_vertices=0;
    g_vc_frame_tested_tris=0;
    g_vc_frame_affine_tris=0;
    g_vc_frame_clip_fast=0;
    g_vc_frame_clip_partial=0;
    g_vc_frame_clip_reject=0;
    g_vc_frame_backface_reject=0;
    g_vc_frame_fogflat_tris=0;
    g_vc_frame_far_tiny_reject=0;
    g_vc_frame_lod_reject=0;
    g_vc_frame_lod_fade=0;
    g_vc_frame_objects_active=0;
    g_vc_frame_objects_fading=0;
    g_vc_frame_objects_started=0;
    g_vc_frame_object_lod[0]=0;
    g_vc_frame_object_lod[1]=0;
    g_vc_frame_object_lod[2]=0;
    g_vc_frame_object_lod_switches=0;
    g_vc_frame_object_frustum_reject=0;
    g_vc_object_start_budget_left=VC_OBJECT_START_BUDGET;
    p0=mono_ns();
    get_player_world(&car,NULL);
    psx=(int)floorf(car.x/sw);
    psz=(int)floorf(car.z/sw);
    far_world=vc_runtime_world_scale()*VC_FAR_CLIP_M;

    if(g_vc_world_mode){
        slot_begin=0;slot_end=VC_WORLD_CACHE_SLOTS;
    }

    /*
     * Build one distance-sorted visible-sector list across the whole active
     * VFW window.  Each queued triangle keeps its source page slot so raster
     * sampling can use that page's compact atlas/material table.
     */
    for(slot=slot_begin;slot<slot_end;++slot){
        int layer_begin=0,layer_end=1,layer;
        if(g_vc_world_mode)layer_end=2;

        for(layer=layer_begin;layer<layer_end;++layer){
            const vc_runtime_map_t *map;
            uint8_t page_slot;
            uint32_t i;

            if(g_vc_world_mode){
                vc_world_page_t *p=&g_vc_world.pages[slot];
                if(!p->loaded)continue;
                if(layer==0){
                    if(!p->base_loaded)continue;
                    map=&p->base;
                    page_slot=(uint8_t)((unsigned)slot|VC_PAGE_BASE_FLAG);
                }else{
                    if(p->detail_state!=1)continue;
                    vc_update_object_stream(&p->map);
                    vc_update_object_camera_visibility(
                        &p->map,camx,camy,camz,
                        cam_cs,cam_sn,cam_cp,cam_sp);
                    map=&p->map;
                    page_slot=(uint8_t)slot;
                }
            }else{
                vc_update_object_stream(&g_vc_map);
                vc_update_object_camera_visibility(
                    &g_vc_map,camx,camy,camz,
                    cam_cs,cam_sn,cam_cp,cam_sp);
                map=&g_vc_map;
                page_slot=0xffU;
            }
            if(!map->sectors||map->sector_world<=1.0f)continue;

            for(i=0;i<map->sector_count;++i){
                const vc_sector_t *sec=&map->sectors[i];
                int dx=(int)sec->sx-psx;
                int dz=(int)sec->sz-psz;
                float cx=((float)sec->sx+0.5f)*sw;
                float cz=((float)sec->sz+0.5f)*sw;
                float rx=cx-car.x,rz=cz-car.z;
                float d2=rx*rx+rz*rz;
                v3f_t sc;
                float sector_radius=sw*0.80f;
                float frustum_slope=((float)RW*0.5f)/VC_FOCAL;
                float maxd=far_world+sector_radius;
                int pos;

                if(dx<-6||dx>6||dz<-6||dz>6)continue;
                if(d2>maxd*maxd)continue;

                city_world_to_camera_csp(
                    cx,car.y,cz,camx,camy,camz,
                    cam_cs,cam_sn,cam_cp,cam_sp,&sc);
                if(sc.z < -sector_radius)continue;
                if(sc.z > 1.0f &&
                   fabsf(sc.x) > sc.z*(frustum_slope+0.30f)+sector_radius)
                    continue;

                if(vis_count>=MAX_VC_VISIBLE_SECTORS)continue;
                pos=vis_count;
                while(pos>0 && vis_d2[pos-1]>d2){
                    vis_d2[pos]=vis_d2[pos-1];
                    vis_idx[pos]=vis_idx[pos-1];
                    vis_slot[pos]=vis_slot[pos-1];
                    --pos;
                }
                vis_d2[pos]=d2;
                vis_idx[pos]=i;
                vis_slot[pos]=page_slot;
                vis_count++;
            }
        }
    }

    p1=mono_ns();
    for(k=0;k<vis_count && n<MAX_VC_DRAW_TRIS;++k){
        const vc_runtime_map_t *map=vc_map_for_page_slot(vis_slot[k]);
        const vc_sector_t *sec;
        if(!map||vis_idx[k]>=map->sector_count)continue;
        sec=&map->sectors[vis_idx[k]];
        queue_vc_mesh_textured(
            map,vis_slot[k],
            &map->verts[sec->vertex_base],(int)sec->vertex_count,
            &map->tris[sec->tri_base],(int)sec->tri_count,
            1.0f,
            camx,camy,camz,cam_cs,cam_sn,cam_cp,cam_sp,&n);
    }
    p2=mono_ns();

    if(n>=MAX_VC_DRAW_TRIS)cap_hit=1;
    g_vc_last_queued=n;
    g_vc_last_visible_sectors=vis_count;
    g_vc_last_cap_hit=cap_hit;

    memset(g_city_zbuf,0,sizeof(g_city_zbuf));
    p3=mono_ns();
    {
        enum { VC_DEPTH_BINS=64 };
        unsigned opaque_count[VC_DEPTH_BINS]={0};
        unsigned fade_count[VC_DEPTH_BINS]={0};
        unsigned opaque_off[VC_DEPTH_BINS],fade_off[VC_DEPTH_BINS];
        unsigned opaque_cur[VC_DEPTH_BINS],fade_cur[VC_DEPTH_BINS];
        unsigned opaque_total=0,pos;
        float inv_far=(far_world>1.0f)?((float)VC_DEPTH_BINS/far_world):0.0f;
        int b;

        for(k=0;k<n;++k){
            const vc_textri_t *t=&g_vc_tex_out[k];
            float z=(t->z0+t->z1+t->z2)*(1.0f/3.0f);
            b=(int)(z*inv_far);
            if(b<0)b=0;if(b>=VC_DEPTH_BINS)b=VC_DEPTH_BINS-1;
            if(t->fade<255U)fade_count[b]++;else opaque_count[b]++;
        }

        pos=0;
        for(b=0;b<VC_DEPTH_BINS;++b){
            opaque_off[b]=opaque_cur[b]=pos;
            pos+=opaque_count[b];
        }
        opaque_total=pos;
        for(b=VC_DEPTH_BINS-1;b>=0;--b){
            fade_off[b]=fade_cur[b]=pos;
            pos+=fade_count[b];
        }

        for(k=0;k<n;++k){
            const vc_textri_t *t=&g_vc_tex_out[k];
            float z=(t->z0+t->z1+t->z2)*(1.0f/3.0f);
            b=(int)(z*inv_far);
            if(b<0)b=0;if(b>=VC_DEPTH_BINS)b=VC_DEPTH_BINS-1;
            if(t->fade<255U)g_vc_order[fade_cur[b]++]=(uint16_t)k;
            else g_vc_order[opaque_cur[b]++]=(uint16_t)k;
        }
        (void)opaque_total;

        if(g_vc_debug_flat){
            for(k=0;k<n;++k){
                const vc_textri_t *t=&g_vc_tex_out[g_vc_order[k]];
                const vc_runtime_map_t *map=vc_map_for_page_slot(t->page_slot);
                const vc_material_t *m;
                if(!map||t->material>=map->material_count)continue;
                m=&map->materials[t->material];
                fill_tri2d_z(
                    t->x0,t->y0,t->z0,
                    t->x1,t->y1,t->z1,
                    t->x2,t->y2,t->z2,
                    g_fog_lut[vc_fog_level_for_z((t->z0+t->z1+t->z2)*(1.0f/3.0f))]
                             [shade1555(m->fallback,t->light)&0x7fffU]);
            }
        }else{
            vc_raster_stats_t top={0},bottom={0};
            uint64_t top_ns=0,bottom_ns=0;
            int split_y=RH;
            if(g_vc_raster_worker.ready){
                uint64_t rt0,rt1;
                split_y=g_vc_raster_split_y;
                vc_raster_worker_submit(n,split_y);
                rt0=mono_ns();
                for(k=0;k<n;++k)
                    fill_tri_vc_textured_z_range(
                        &g_vc_tex_out[g_vc_order[k]],0,split_y,&top);
                rt1=mono_ns();
                top_ns=rt1-rt0;
                bottom=vc_raster_worker_collect(&bottom_ns);
                vc_raster_rebalance(top_ns,bottom_ns);
            }else{
                uint64_t rt0=mono_ns(),rt1;
                for(k=0;k<n;++k)
                    fill_tri_vc_textured_z_range(
                        &g_vc_tex_out[g_vc_order[k]],0,RH,&top);
                rt1=mono_ns();
                top_ns=rt1-rt0;
            }
            g_vc_prof.zpass_pixels+=top.zpass_pixels+bottom.zpass_pixels;
            g_vc_prof.texture_samples+=top.texture_samples+bottom.texture_samples;
            g_vc_prof.correction_segments+=
                top.correction_segments+bottom.correction_segments;
            g_vc_prof.bbox_pixels+=top.bbox_pixels+bottom.bbox_pixels;
            g_vc_prof.span_pixels+=top.span_pixels+bottom.span_pixels;
            g_vc_prof.raster_top_ns+=top_ns;
            g_vc_prof.raster_bottom_ns+=bottom_ns;
            g_vc_prof.split_rows+=(uint64_t)split_y;
        }
    }
    p4=mono_ns();

    g_vc_prof.scan_ns+=p1-p0;
    g_vc_prof.queue_ns+=p2-p1;
    g_vc_prof.zclear_ns+=p3-p2;
    g_vc_prof.raster_ns+=p4-p3;
    if(p1-p0>g_vc_prof.max_scan_ns)g_vc_prof.max_scan_ns=p1-p0;
    if(p2-p1>g_vc_prof.max_queue_ns)g_vc_prof.max_queue_ns=p2-p1;
    if(p4-p3>g_vc_prof.max_raster_ns)g_vc_prof.max_raster_ns=p4-p3;
    g_vc_prof.xformed_vertices+=g_vc_frame_xformed_vertices;
    g_vc_prof.tested_tris+=g_vc_frame_tested_tris;
    g_vc_prof.frames++;
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
    if(g_vc_vehicle.loaded && g_vc_city_mode){
        float camx,camy,camz,camyaw;
        get_chase_camera(&camx,&camy,&camz,&camyaw);
        render_vc_vehicle(
            g_world_x,g_world_y,g_world_z,
            1.0f,
            camx,camy,camz,camyaw);
    }else{
        float camx=0.0f,camy=0.0f;
        float ox,oy,oz,relative_yaw;
        get_player_camera_pose(&ox,&oy,&oz,&relative_yaw,NULL,NULL);
        if(g_vc_vehicle.loaded){
            /* Legacy non-VC path retained for compatibility. */
            render_vc_vehicle(
                g_world_x,g_world_y,g_world_z,
                1.0f,
                g_camera_x,g_camera_y,g_camera_z,g_camera_heading);
        }else{
            render_sports_vehicle(
                ox,oy,oz,
                g_body_pitch,-relative_yaw,g_body_roll,
                g_steer_fl,g_steer_fr,g_wheel_spin,
                1.08f,camx,camy);
        }
    }
}

static void draw_hud(void)
{
    float vmax=g_vehicle_handling.max_forward>1.0f?g_vehicle_handling.max_forward:MAX_SPEED;
    int bar=(int)(fabsf(g_speed)/vmax*120.0f);
    if(bar>120)bar=120;
    int i;
    fill_rect(16,14,130,15,pack1555(10,17,22));
    for(i=0;i<bar;++i)putpx(20+i,20,pack1555(45+(unsigned)i,190,85));
    hline(20,140,18,C_WHITE);
    hline(20,140,25,C_WHITE);
    /* reverse indicator */
    if(g_speed<0.0f)fill_rect(20,30,18,4,C_RED);

    /* Old procedural-track aiming marker is not part of Vice City. */
    if(!g_vc_city_mode){
        hline(RW/2-8,RW/2+8,HORIZON+14,pack1555(235,235,210));
        putpx(RW/2,HORIZON+13,C_WHITE);
        putpx(RW/2,HORIZON+15,C_WHITE);
    }

    /* lap/finish indicator without text rendering */
    fill_rect(RW-52,14,36,8,pack1555(20,24,28));
    fill_rect(RW-48,17,g_lap==1?10:20,2,C_WHITE);
}

/* ---------- game ---------- */

static float active_vehicle_wheelbase(void)
{
    return g_vc_vehicle.loaded?g_vc_vehicle.wheelbase:SPORTS_VEHICLE_WHEELBASE;
}

static float active_vehicle_track(void)
{
    return g_vc_vehicle.loaded?g_vc_vehicle.track:SPORTS_VEHICLE_TRACK;
}

static float active_vehicle_wheel_radius(void)
{
    return g_vc_vehicle.loaded?g_vc_vehicle.wheel_radius:SPORTS_VEHICLE_WHEEL_RADIUS;
}

static float active_vehicle_ride_height(void)
{
    float scale=vc_runtime_world_scale();
    float force=clampf_local(g_vehicle_handling.suspension_force,0.20f,4.0f);
    if(g_vc_vehicle.loaded && g_vc_vehicle.native_col_version>=2U &&
       g_vc_vehicle.rest_height_world>1.0f)
        return g_vc_vehicle.rest_height_world;
    if(scale>1.0f){
        float spring=fabsf(
            g_vehicle_handling.suspension_upper-
            g_vehicle_handling.suspension_lower)*scale;
        float p0y=sports_wheel_fl_pivot.y+
            g_vehicle_handling.suspension_upper*scale;
        float ride=spring*(1.0f-1.0f/(4.0f*force))-
            p0y+active_vehicle_wheel_radius();
        return clampf_local(ride,20.0f,600.0f);
    }
    return 21.0f;
}

static float active_suspension_force(void)
{
    return clampf_local(g_vehicle_handling.suspension_force,0.20f,4.0f);
}

static float active_suspension_damping(void)
{
    return clampf_local(g_vehicle_handling.suspension_damping,0.01f,1.0f);
}

static float active_suspension_antidive(void)
{
    return clampf_local(g_vehicle_handling.suspension_antidive,0.0f,2.0f);
}

static float active_suspension_travel_world(void)
{
    float scale=vc_runtime_world_scale();
    float travel=fabsf(
        g_vehicle_handling.suspension_upper-
        g_vehicle_handling.suspension_lower)*scale;
    if(travel>8.0f)
        return clampf_local(travel,8.0f,active_vehicle_wheelbase()*0.55f);
    return fmaxf(24.0f,active_vehicle_wheel_radius()*0.85f);
}

static int vc_prime_upright_wheel_contacts(float ground_hint)
{
    float gy=ground_hint,pitch=0.0f,roll=0.0f;
    int contacts;

    g_body_pitch=0.0f;g_body_roll=0.0f;
    vc_reset_turn_world();g_vc_body_basis_valid=0;
    memset(g_vc_wheel_timer,0,sizeof(g_vc_wheel_timer));
    memset(g_vc_wheel_contact,0,sizeof(g_vc_wheel_contact));
    g_vc_wheel_contact_mask=0;
    g_vc_wheel_latched_mask=0;
    g_vc_wheel_exact_mask=0;
    g_vc_wheel_rescue_mask=0;

    contacts=vc_collision_four_contacts(
        g_world_x,g_world_z,g_vehicle_heading,ground_hint,
        active_vehicle_wheelbase(),active_vehicle_track(),
        &gy,&pitch,&roll);
    if(contacts>0)g_vc_ground_y=gy;
    return contacts;
}

static int vc_spawn_pose_is_clear(float x,float z,float probe_ground,float *out_ground)
{
    float gy=0.0f,pitch=0.0f,roll=0.0f;
    float save_x=g_world_x,save_y=g_world_y,save_z=g_world_z,save_ground=g_vc_ground_y;
    float y,visual_y=0.0f;
    uint8_t surface=0;
    int surface_kind,contacts,blocked,have_visual;

    (void)probe_ground;
    if(!g_vc_city_mode || !g_vc_collision.loaded)
        return 0;

    surface_kind=vc_collision_spawn_surface(x,z,&gy,&surface);
    if(!surface_kind)
        return 0;

    /*
     * Do not accept a collision road hidden below the rendered city. A spawn
     * under a mansion/terrain slab was the reason the car appeared below the
     * textures even though VCCOL itself had a valid horizontal face.
     */
    have_visual=vc_visual_top_height(x,z,&visual_y);
    if(have_visual && visual_y>gy+2.5f*vc_runtime_world_scale())
        return 0;

    y=gy+active_vehicle_ride_height();
    g_world_x=x;g_world_y=y;g_world_z=z;g_vc_ground_y=gy;

    contacts=vc_collision_four_contacts(
        x,z,g_vehicle_heading,gy,
        active_vehicle_wheelbase(),active_vehicle_track(),
        &gy,&pitch,&roll);
    blocked=vc_collision_vehicle_body_hits(
        x,y,z,g_vehicle_heading,
        active_vehicle_wheelbase(),active_vehicle_track(),
        active_vehicle_wheel_radius());

    g_world_x=save_x;g_world_y=save_y;g_world_z=save_z;g_vc_ground_y=save_ground;

    /*
     * Stock Oceanic must start from a real suspension-supported pose. Accepting
     * only a centre vertical road hit can place the body above a seam with one
     * wheel contact, which immediately feeds a huge roll/pitch transient.
     */
    if(blocked)
        return 0;
    if(contacts<3)
        return 0;

    if(out_ground)*out_ground=gy;
    return 1;
}

static void vc_relocate_to_safe_spawn(void)
{
    float base_x,base_z,probe_ground,scale,step;
    int ring,slot;
    const int slots=24;

    if(!g_vc_city_mode || !g_vc_collision.loaded || !g_vc_vehicle.loaded)
        return;

    base_x=g_world_x;base_z=g_world_z;probe_ground=g_vc_ground_y;
    scale=vc_runtime_world_scale();
    step=5.0f*scale;

    /* Search up to 150 m around the imported hint. Starfish's geometric
     * centre is inside the mansion district; a real public road can easily be
     * farther than the old 30 m safety radius. */
    for(ring=0;ring<=30;++ring){
        int count=ring==0?1:slots;
        for(slot=0;slot<count;++slot){
            float a=ring==0?0.0f:(6.2831853f*(float)slot/(float)slots);
            float x=base_x+(float)ring*step*cosf(a);
            float z=base_z+(float)ring*step*sinf(a);
            float gy;

            if(x<vc_runtime_min_x()+2.0f*scale || x>vc_runtime_max_x()-2.0f*scale ||
               z<vc_runtime_min_z()+2.0f*scale || z>vc_runtime_max_z()-2.0f*scale)
                continue;

            if(vc_spawn_pose_is_clear(x,z,probe_ground,&gy)){
                g_world_x=x;g_world_z=z;g_vc_ground_y=gy;
                g_world_y=gy+active_vehicle_ride_height();
                if(!g_vc_world_mode){
                    g_vc_map.spawn_x=x;g_vc_map.spawn_y=g_world_y;g_vc_map.spawn_z=z;
                }
                g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
                g_vehicle_yaw_rate=0.0f;g_vehicle_airborne=0;
                g_body_pitch=0.0f;g_body_roll=0.0f;
                vc_reset_turn_world();g_vc_body_basis_valid=0;
                memset(g_vc_wheel_timer,0,sizeof(g_vc_wheel_timer));
                memset(g_vc_wheel_contact,0,sizeof(g_vc_wheel_contact));
                {
                    int primed=vc_prime_upright_wheel_contacts(g_vc_ground_y);
                    fprintf(stderr,
                        "[racer] VC_SAFE_SPAWN ring=%d world=%.1f,%.1f,%.1f contacts=%d mask=0x%x\n",
                        ring,g_world_x,g_world_y,g_world_z,
                        primed,(unsigned)g_vc_wheel_contact_mask);
                }
                return;
            }
        }
    }

    {
        float gy=0.0f;
        uint8_t surface=0;
        int kind=vc_collision_spawn_surface(base_x,base_z,&gy,&surface);
        if(kind){
            g_world_x=base_x;g_world_z=base_z;g_vc_ground_y=gy;
            g_world_y=gy+active_vehicle_ride_height();
            g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
            g_vehicle_yaw_rate=0.0f;g_vehicle_airborne=0;
            g_body_pitch=0.0f;g_body_roll=0.0f;
            vc_reset_turn_world();g_vc_body_basis_valid=0;
            memset(g_vc_wheel_timer,0,sizeof(g_vc_wheel_timer));
            memset(g_vc_wheel_contact,0,sizeof(g_vc_wheel_contact));
            {
                int primed=vc_prime_upright_wheel_contacts(g_vc_ground_y);
                g_vehicle_airborne=primed>0?0:1;
                fprintf(stderr,
                    "[racer] VC_SAFE_SPAWN forced-vertical world=%.1f,%.1f,%.1f surface=%u kind=%s contacts=%d mask=0x%x\n",
                    g_world_x,g_world_y,g_world_z,(unsigned)surface,
                    kind==2?"road":"generic",primed,
                    (unsigned)g_vc_wheel_contact_mask);
            }
            return;
        }
    }

    fprintf(stderr,
        "[racer] VC_SAFE_SPAWN warning no VCCOL surface within %.1fm of imported spawn\n",
        30.0f*step/scale);
}

static void update_ackermann(float steer)
{
    float wb=active_vehicle_wheelbase();
    float tw=active_vehicle_track();
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

static float clampf_local(float v,float lo,float hi)
{
    if(v<lo)return lo;
    if(v>hi)return hi;
    return v;
}

static float approach_zero(float v,float amount)
{
    if(v>0.0f){v-=amount;if(v<0.0f)v=0.0f;}
    else if(v<0.0f){v+=amount;if(v>0.0f)v=0.0f;}
    return v;
}

static int dev_hover_update(input_t *in)
{
    static int r2_prev=0;
    static uint64_t last_r2_tap_ns=0;
    const uint64_t double_tap_ns=450000000ULL;
    uint64_t now=mono_ns();
    int r2_rise=in->dev_lift && !r2_prev;
    float scale,sh,ch;
    float target_fwd=0.0f,target_yaw=0.0f,target_up=0.0f;
    float max_fwd,max_up,max_yaw,accel_h,accel_v,accel_yaw;
    float dx,dz,next_y;

    r2_prev=in->dev_lift;

    if(!g_vc_city_mode)
        return 0;

    scale=vc_runtime_world_scale();

    max_fwd=8.0f*scale/60.0f;
    max_up=5.0f*scale/60.0f;
    max_yaw=(75.0f*(3.14159265358979323846f/180.0f))/60.0f;
    accel_h=30.0f*scale/(60.0f*60.0f);
    accel_v=24.0f*scale/(60.0f*60.0f);
    accel_yaw=(280.0f*(3.14159265358979323846f/180.0f))/(60.0f*60.0f);

    if(r2_rise && !g_dev_hover){
        g_dev_hover=1;
        last_r2_tap_ns=0; /* entry press is not part of the exit double-tap */
        g_dev_hover_fwd=0.0f;g_dev_hover_yaw=0.0f;g_dev_hover_up=0.0f;
        g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
        vc_reset_turn_world();g_vehicle_airborne=1;
        g_body_pitch=0.0f;g_body_roll=0.0f;
        g_vc_body_basis_valid=0;
        fprintf(stderr,
            "[racer] DEV_HOVER enter world=%.1f,%.1f,%.1f "
            "R2=enter/up double-R2=exit L2=land dpad=forward/turn smooth=v4\n",
            g_world_x,g_world_y,g_world_z);
    }

    if(!g_dev_hover)
        return 0;

    /*
     * Double-R2 exits developer flight. If a road is already close beneath
     * the car, latch directly onto it; otherwise release into normal airborne
     * physics and let the swept landing solver catch the next VCCOL surface.
     */
    if(r2_rise){
        if(last_r2_tap_ns && now-last_r2_tap_ns<=double_tap_ns){
            float ride=active_vehicle_ride_height();
            float road_y=0.0f;
            uint8_t road_surface=0;
            int kind=vc_collision_spawn_surface(
                g_world_x,g_world_z,&road_y,&road_surface);
            if(!kind){
                float visual_y=0.0f;
                if(vc_visual_top_height(g_world_x,g_world_z,&visual_y)){
                    road_y=visual_y;
                    road_surface=254U;
                    kind=3; /* rendered VCMAP fallback */
                }
            }
            float bottom=g_world_y-ride;
            float gap=bottom-road_y;

            g_dev_hover=0;
            g_dev_hover_fwd=0.0f;g_dev_hover_yaw=0.0f;g_dev_hover_up=0.0f;
            g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;
            vc_reset_turn_world();
            g_vc_body_basis_valid=0;
            last_r2_tap_ns=0;

            if((kind==2||kind==3) && gap>=-8.0f*scale && gap<=8.0f*scale){
                g_vc_ground_y=road_y;
                g_world_y=road_y+ride;
                g_vehicle_vy=0.0f;
                g_vehicle_airborne=0;
                g_body_pitch=0.0f;g_body_roll=0.0f;
                vc_reset_turn_world();g_vc_body_basis_valid=0;
                memset(g_vc_wheel_timer,0,sizeof(g_vc_wheel_timer));
                memset(g_vc_wheel_contact,0,sizeof(g_vc_wheel_contact));
                g_vc_wheel_contact_mask=0;
                g_vc_wheel_latched_mask=0;
                {
                    int primed=vc_prime_upright_wheel_contacts(road_y);
                    g_vehicle_airborne=primed>0?0:1;
                    g_vc_last_body_surface=road_surface;
                    fprintf(stderr,
                        "[racer] DEV_HOVER exit double-r2 mode=snap-road "
                        "world=%.1f,%.1f,%.1f gap=%.2fm surface=%u source=%s contacts=%d mask=0x%x\n",
                        g_world_x,g_world_y,g_world_z,gap/scale,
                        (unsigned)road_surface,kind==3?"vcmap":"vccol",
                        primed,(unsigned)g_vc_wheel_contact_mask);
                }
            }else{
                g_vehicle_vy=0.0f;
                g_vehicle_airborne=1;
                fprintf(stderr,
                    "[racer] DEV_HOVER exit double-r2 mode=fall "
                    "world=%.1f,%.1f,%.1f road_gap=%.2fm kind=%d\n",
                    g_world_x,g_world_y,g_world_z,
                    kind?gap/scale:-999.0f,kind);
            }
            return 0;
        }
        last_r2_tap_ns=now;
    }else if(last_r2_tap_ns && now-last_r2_tap_ns>double_tap_ns){
        last_r2_tap_ns=0;
    }

    if(in->select_down && !in->start_down){
        g_dev_hover=0;
        g_dev_hover_fwd=0.0f;g_dev_hover_yaw=0.0f;g_dev_hover_up=0.0f;
        g_vehicle_airborne=1;
        g_vehicle_vy=0.0f;
        last_r2_tap_ns=0;
        fprintf(stderr,
            "[racer] DEV_HOVER cancel world=%.1f,%.1f,%.1f reason=select\n",
            g_world_x,g_world_y,g_world_z);
        return 0;
    }

    g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
    vc_reset_turn_world();g_speed=0.0f;g_vehicle_slip=0.0f;
    g_vehicle_airborne=1;

    if(in->dev_up)target_fwd+=max_fwd;
    if(in->dev_down)target_fwd-=max_fwd;

    /* Left/right now rotate the whole vehicle instead of strafing it. */
    if(in->dev_left)target_yaw-=max_yaw;
    if(in->dev_right)target_yaw+=max_yaw;

    if(in->dev_lower)target_up=-max_up;
    else if(in->dev_lift)target_up=max_up;

    g_dev_hover_fwd=approachf(g_dev_hover_fwd,target_fwd,accel_h);
    g_dev_hover_yaw=approachf(g_dev_hover_yaw,target_yaw,accel_yaw);
    g_dev_hover_up=approachf(g_dev_hover_up,target_up,accel_v);

    g_vehicle_heading=wrap_angle(g_vehicle_heading+g_dev_hover_yaw);
    g_vc_body_basis_valid=0;
    g_steer_visual=approachf(
        g_steer_visual,
        in->dev_left?-1.0f:(in->dev_right?1.0f:0.0f),
        0.12f);
    g_steer_angle=g_steer_visual*g_vehicle_handling.steering_lock_rad;
    update_ackermann(g_steer_angle);

    sh=sinf(g_vehicle_heading);ch=cosf(g_vehicle_heading);
    dx=sh*g_dev_hover_fwd;
    dz=ch*g_dev_hover_fwd;
    g_world_x+=dx;g_world_z+=dz;

    {
        float margin=1.5f*scale;
        if(g_world_x<vc_runtime_min_x()+margin)g_world_x=vc_runtime_min_x()+margin;
        if(g_world_x>vc_runtime_max_x()-margin)g_world_x=vc_runtime_max_x()-margin;
        if(g_world_z<vc_runtime_min_z()+margin)g_world_z=vc_runtime_min_z()+margin;
        if(g_world_z>vc_runtime_max_z()-margin)g_world_z=vc_runtime_max_z()-margin;
    }

    next_y=g_world_y+g_dev_hover_up;
    if(g_dev_hover_up<0.0f){
        float ride=active_vehicle_ride_height();
        float current_bottom=g_world_y-ride;
        float target_bottom=next_y-ride;
        float hit_y=0.0f;
        uint8_t hit_surface=0;

        if(g_vc_collision.loaded &&
           vc_collision_vertical_contact(
               g_world_x,g_world_z,current_bottom,target_bottom,
               &hit_y,&hit_surface)){
            g_vc_ground_y=hit_y;
            g_world_y=hit_y+ride;
            g_vehicle_airborne=0;
            g_dev_hover=0;
            g_dev_hover_fwd=0.0f;g_dev_hover_yaw=0.0f;g_dev_hover_up=0.0f;
            g_vehicle_vlong=0.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
            g_vehicle_yaw_rate=0.0f;
            g_body_pitch=0.0f;g_body_roll=0.0f;
            vc_reset_turn_world();g_vc_body_basis_valid=0;
            memset(g_vc_wheel_timer,0,sizeof(g_vc_wheel_timer));
            memset(g_vc_wheel_contact,0,sizeof(g_vc_wheel_contact));
            g_vc_wheel_contact_mask=0;
            g_vc_wheel_latched_mask=0;
            {
                int primed=vc_prime_upright_wheel_contacts(hit_y);
                g_vehicle_airborne=primed>0?0:1;
                last_r2_tap_ns=0;
                g_vc_last_body_surface=hit_surface;
                fprintf(stderr,
                    "[racer] DEV_HOVER land world=%.1f,%.1f,%.1f surface=%u contacts=%d mask=0x%x\n",
                    g_world_x,g_world_y,g_world_z,(unsigned)hit_surface,
                    primed,(unsigned)g_vc_wheel_contact_mask);
            }
            return 1;
        }
    }
    g_world_y=next_y;

    g_position=0.0f;
    g_player_x=0.0f;
    return 1;
}

static float vc_player_stability_com_y(float source_y,float scale)
{
    float max_up;
    if(scale<=1.0f)scale=240.0f;
    max_up=VC_PLAYER_COM_MAX_UP_M*scale;
    return source_y>max_up?max_up:source_y;
}

static float vc_player_balance_com_y(float base_y,float candidate_y)
{
    /*
     * reVC lets steering move the COM both down and up while balancing on two
     * wheels. In Racer's compact solver the upward branch can amplify the
     * rollover instead of merely letting the player balance it. Preserve the
     * helpful downward branch, but never let the aid raise COM above the
     * already-stabilised player base height.
     */
    return candidate_y>base_y?base_y:candidate_y;
}

static v3f_t vc_effective_centre_of_mass(void)
{
    v3f_t com=g_vehicle_handling.centre_of_mass;
    float top;
    float base_y=vc_player_stability_com_y(
        g_vehicle_handling.centre_of_mass.y,vc_runtime_world_scale());

    /*
     * handling.cfg is kept unmodified. Oceanic's stock +0.4 m COM is valid
     * for the full RenderWare/reVC physics stack, but this compact solver has
     * less tyre/suspension compliance and develops excessive rollover torque
     * with the same local offset. Cap only positive player COM height here.
     * Zero/negative stock COM values remain untouched.
     */
    com.y=base_y;

    /*
     * Stock Vice City player-car balance aid from CAutomobile::ProcessControl.
     * After ~0.5 s without all four wheels down, steering toward the loaded
     * side moves CentreOfMass.z downward. Racer's vertical axis is Y.
     * This is not a generic anti-roll clamp: it only acts in the same sustained
     * two/three-wheel condition as GTA and keeps the original 0.3 multiplier.
     */
    if(g_vc_two_wheel_ticks>VC_PLAYER_BALANCE_START_TICKS &&
       g_vc_body_up.y>0.0f){
        float tweak=clampf_local(
            ((float)g_vc_two_wheel_ticks-(float)VC_PLAYER_BALANCE_START_TICKS)/
            VC_PLAYER_BALANCE_RAMP_TICKS,0.0f,2.0f);
        if(g_vc_body_right.y<=0.0f)tweak=-tweak;

        if(g_vc_vehicle.loaded && g_vc_vehicle.native_col_loaded)
            top=fmaxf(1.0f,g_vc_vehicle.col_box_max.y);
        else
            top=fmaxf(1.0f,
                fabsf(g_vehicle_handling.dim_z)*vc_runtime_world_scale()*0.5f);

        com.y=vc_player_balance_com_y(
            base_y,
            base_y+
            clampf_local(g_vc_raw_steer_input,-1.0f,1.0f)*
            0.30f*tweak*top);
    }
    g_vc_effective_com_y=com.y;
    return com;
}

static void vc_apply_world_dv_turn_at_point(
    v3f_t linear_dv,v3f_t turn_dv,v3f_t point,float heading,
    float *vx,float *vy,float *vz)
{
    v3f_t com_local=vc_effective_centre_of_mass(),com_rot;
    float mass=fmaxf(1.0f,g_vehicle_handling.mass);
    float turn_mass=fmaxf(1.0f,g_vehicle_handling.turn_mass_world);
    v3f_t r,j;
    float tx,ty,tz;
    (void)heading;

    vc_body_rotate_local(com_local,&com_rot);
    r=(v3f_t){
        point.x-(g_world_x+com_rot.x),
        point.y-(g_world_y+com_rot.y),
        point.z-(g_world_z+com_rot.z)
    };

    *vx+=linear_dv.x;*vy+=linear_dv.y;*vz+=linear_dv.z;

    j=(v3f_t){turn_dv.x*mass,turn_dv.y*mass,turn_dv.z*mass};
    tx=r.y*j.z-r.z*j.y;
    ty=r.z*j.x-r.x*j.z;
    tz=r.x*j.y-r.y*j.x;

    /*
     * reVC keeps one world-space m_vecTurnSpeed.  Do not project the torque
     * into independent Euler pitch/roll channels: that projection changes as
     * the car tilts and was feeding nose/tail oscillation back into itself.
     */
    g_vc_turn_world.x+=tx/turn_mass;
    g_vc_turn_world.y+=ty/turn_mass;
    g_vc_turn_world.z+=tz/turn_mass;

    /* Legacy fields remain diagnostics/compatibility only in Vice City mode. */
    g_vehicle_yaw_rate=g_vc_turn_world.y;
    g_body_pitch_vel=
        vc_v3_dot(g_vc_turn_world,g_vc_body_right);
    g_body_roll_vel=
        vc_v3_dot(g_vc_turn_world,g_vc_body_forward);
}

static void vc_apply_world_dv_at_point(
    v3f_t dv,v3f_t point,float heading,
    float *vx,float *vy,float *vz)
{
    vc_apply_world_dv_turn_at_point(dv,dv,point,heading,vx,vy,vz);
}

static void vc_apply_revc_suspension(float heading)
{
    float sh=sinf(heading),ch=cosf(heading);
    float vx=sh*g_vehicle_vlong+ch*g_vehicle_vlat;
    float vy=g_vehicle_vy;
    float vz=ch*g_vehicle_vlong-sh*g_vehicle_vlat;
    float scale=vc_runtime_world_scale();
    float gravity=scale*9.81f/(60.0f*60.0f);
    float force=active_suspension_force();
    float damping=active_suspension_damping();
    int i;

    for(i=0;i<4;++i){
        vc_wheel_contact_t *c=&g_vc_wheel_contact[i];
        float compression,bias,spring_dv;
        v3f_t dv;

        if(!c->hit || c->ratio>=1.0f)continue;
        compression=1.0f-c->ratio;
        bias=(i<2)?
            clampf_local(g_vehicle_handling.suspension_bias,0.05f,0.95f):
            1.0f-clampf_local(g_vehicle_handling.suspension_bias,0.05f,0.95f);

        /*
         * reVC ApplySpringCollisionAlt:
         * gravity * springForce * compression * bias * 2.
         * Mass cancels when converting impulse back to delta-velocity.
         */
        spring_dv=gravity*force*compression*bias*2.0f;
        dv=(v3f_t){
            c->normal.x*spring_dv,
            c->normal.y*spring_dv,
            c->normal.z*spring_dv
        };
        vc_apply_world_dv_at_point(dv,c->point,heading,&vx,&vy,&vz);

        /*
         * reVC ApplySpringDampening uses velocity at the actual contact point.
         * Include pitch/yaw/roll angular velocity so a single compressed wheel
         * damps chassis rotation instead of forcing a target body angle.
         */
        {
            float wx=g_vc_turn_world.x;
            float wy=g_vc_turn_world.y;
            float wz=g_vc_turn_world.z;
            v3f_t r={
                c->point.x-g_world_x,
                c->point.y-g_world_y,
                c->point.z-g_world_z
            };
            v3f_t pv={
                vx + wy*r.z - wz*r.y,
                vy + wz*r.x - wx*r.z,
                vz + wx*r.y - wy*r.x
            };
            v3f_t damp_dir=c->spring_dir;
            float speed_b,damp_dv;

            /*
             * reVC Automobile: once a wheel is on a floor-like surface the
             * damping direction becomes -contactNormal, not the tilted
             * suspension line. This prevents pitch/roll feedback on seams.
             */
            if(c->normal.y>0.35f){
                damp_dir.x=-c->normal.x;
                damp_dir.y=-c->normal.y;
                damp_dir.z=-c->normal.z;
            }
            speed_b=
                pv.x*damp_dir.x+
                pv.y*damp_dir.y+
                pv.z*damp_dir.z;

            /*
             * reVC ApplySpringDampening:
             *   -damping * (speedA + speedB)/2 * step * 0.53
             * Automobile passes GetSpeed(contactPoint) as speedA and
             * ApplySpringDampening immediately samples the same point as
             * speedB, so the average is speed itself -- no extra 0.5 factor.
             */
            damp_dv=-damping*speed_b*(0.53f*(50.0f/60.0f));

            /*
             * Port reVC's turn-mass limiter. Without it, one sharply
             * compressed wheel can inject enough angular impulse to stand the
             * car on an axle even though linear suspension looks reasonable.
             */
            if(fabsf(speed_b)>1.0e-5f){
                float mass=fmaxf(1.0f,g_vehicle_handling.mass);
                float turn_mass=fmaxf(1.0f,g_vehicle_handling.turn_mass_world);
                float r2=r.x*r.x+r.y*r.y+r.z*r.z;
                float a=turn_mass/((r2+1.0f)*2.0f*mass);
                float b=fabsf(damp_dv/speed_b);
                if(a<1.0f && a<b && b>1.0e-8f)damp_dv*=a/b;
            }
            damp_dv=clampf_local(damp_dv,-gravity*3.0f,gravity*3.0f);
            dv=(v3f_t){
                damp_dir.x*damp_dv,
                damp_dir.y*damp_dv,
                damp_dir.z*damp_dv
            };
            vc_apply_world_dv_at_point(dv,c->point,heading,&vx,&vy,&vz);
        }
    }

    g_vehicle_vlong=sh*vx+ch*vz;
    g_vehicle_vlat =ch*vx-sh*vz;
    g_vehicle_vy=vy;
}

static int vc_bitcount4(uint8_t m)
{
    int n=0;
    m&=0x0fU;
    if(m&1U)n++;if(m&2U)n++;if(m&4U)n++;if(m&8U)n++;
    return n;
}

static uint8_t vc_wheel_timer_mask(void)
{
    uint8_t m=0;
    int i;
    for(i=0;i<4;++i)if(g_vc_wheel_timer[i]>0.0f)m|=(uint8_t)(1U<<i);
    return m;
}

static uint8_t active_drive_wheel_mask(void)
{
    switch(g_vehicle_handling.drive_type){
    case 'F': case 'f': return 0x03U;
    case '4': return 0x0fU;
    case 'R': case 'r':
    default: return 0x0cU;
    }
}

static float vc_drive_support_factor(void)
{
    uint8_t want=active_drive_wheel_mask();
    int total=vc_bitcount4(want);
    int hit=vc_bitcount4((uint8_t)(g_vc_wheel_contact_mask&want));
    if(!g_vc_city_mode)return 1.0f;
    return total?((float)hit/(float)total):0.0f;
}

static float vc_brake_support_factor(void)
{
    float front=clampf_local(g_vehicle_handling.brake_bias,0.0f,1.0f);
    float rear=1.0f-front;
    float f=0.0f;
    if(g_vc_wheel_contact_mask&0x01U)f+=front*0.5f;
    if(g_vc_wheel_contact_mask&0x02U)f+=front*0.5f;
    if(g_vc_wheel_contact_mask&0x04U)f+=rear*0.5f;
    if(g_vc_wheel_contact_mask&0x08U)f+=rear*0.5f;
    return g_vc_city_mode?clampf_local(f,0.0f,1.0f):1.0f;
}

static float vc_lateral_support_factor(void)
{
    float front=clampf_local(g_vehicle_handling.traction_bias,0.0f,1.0f);
    float rear=1.0f-front;
    float f=0.0f;
    if(g_vc_wheel_contact_mask&0x01U)f+=front*0.5f;
    if(g_vc_wheel_contact_mask&0x02U)f+=front*0.5f;
    if(g_vc_wheel_contact_mask&0x04U)f+=rear*0.5f;
    if(g_vc_wheel_contact_mask&0x08U)f+=rear*0.5f;
    return g_vc_city_mode?clampf_local(f,0.0f,1.0f):1.0f;
}

static float vc_v3_dot(v3f_t a,v3f_t b)
{
    return a.x*b.x+a.y*b.y+a.z*b.z;
}

static v3f_t vc_v3_cross(v3f_t a,v3f_t b)
{
    return (v3f_t){
        a.y*b.z-a.z*b.y,
        a.z*b.x-a.x*b.z,
        a.x*b.y-a.y*b.x
    };
}

static int vc_v3_normalize(v3f_t *v)
{
    float m2=vc_v3_dot(*v,*v);
    float inv;
    if(m2<1.0e-12f)return 0;
    inv=1.0f/sqrtf(m2);
    v->x*=inv;v->y*=inv;v->z*=inv;
    return 1;
}

static void vc_reset_turn_world(void)
{
    g_vc_turn_world=(v3f_t){0.0f,0.0f,0.0f};
    g_vehicle_yaw_rate=0.0f;
    g_body_pitch_vel=0.0f;
    g_body_roll_vel=0.0f;
}

static void vc_integrate_turn_world(void)
{
    v3f_t dr,du,df;
    v3f_t world_up={0.0f,1.0f,0.0f};
    v3f_t ref_right;
    float proj,hlen,roll_sin,roll_cos;
    float drag=powf(0.99f,50.0f/60.0f);

    if(!g_vc_body_basis_valid)vc_body_basis_from_euler();

    /*
     * reVC CPhysical::ApplyTurnSpeed:
     *   axis += turnSpeed x axis
     * for right/forward/up.  Normalise afterward because this compact engine
     * does not have RenderWare's later matrix orthonormalisation pass.
     */
    dr=vc_v3_cross(g_vc_turn_world,g_vc_body_right);
    du=vc_v3_cross(g_vc_turn_world,g_vc_body_up);
    df=vc_v3_cross(g_vc_turn_world,g_vc_body_forward);
    g_vc_body_right.x+=dr.x;g_vc_body_right.y+=dr.y;g_vc_body_right.z+=dr.z;
    g_vc_body_up.x+=du.x;g_vc_body_up.y+=du.y;g_vc_body_up.z+=du.z;
    g_vc_body_forward.x+=df.x;g_vc_body_forward.y+=df.y;g_vc_body_forward.z+=df.z;

    if(!vc_v3_normalize(&g_vc_body_right))
        g_vc_body_right=(v3f_t){1.0f,0.0f,0.0f};

    proj=vc_v3_dot(g_vc_body_up,g_vc_body_right);
    g_vc_body_up.x-=g_vc_body_right.x*proj;
    g_vc_body_up.y-=g_vc_body_right.y*proj;
    g_vc_body_up.z-=g_vc_body_right.z*proj;
    if(!vc_v3_normalize(&g_vc_body_up))
        g_vc_body_up=(v3f_t){0.0f,1.0f,0.0f};

    g_vc_body_forward=vc_v3_cross(g_vc_body_right,g_vc_body_up);
    if(!vc_v3_normalize(&g_vc_body_forward))
        g_vc_body_forward=(v3f_t){0.0f,0.0f,1.0f};
    g_vc_body_up=vc_v3_cross(g_vc_body_forward,g_vc_body_right);
    vc_v3_normalize(&g_vc_body_up);

    /*
     * Euler values are now output-only compatibility values for rendering,
     * camera and logs. Heading comes from the forward projection, so it
     * remains continuous through the full 360 degrees.
     */
    hlen=sqrtf(
        g_vc_body_forward.x*g_vc_body_forward.x+
        g_vc_body_forward.z*g_vc_body_forward.z);
    if(hlen>1.0e-5f)
        g_vehicle_heading=wrap_angle(
            atan2f(g_vc_body_forward.x,g_vc_body_forward.z));
    g_body_pitch=atan2f(-g_vc_body_forward.y,fmaxf(hlen,1.0e-6f));

    ref_right=vc_v3_cross(world_up,g_vc_body_forward);
    if(!vc_v3_normalize(&ref_right))
        ref_right=g_vc_body_right;
    roll_sin=vc_v3_dot(
        vc_v3_cross(ref_right,g_vc_body_right),
        g_vc_body_forward);
    roll_cos=vc_v3_dot(ref_right,g_vc_body_right);
    g_body_roll=atan2f(roll_sin,roll_cos);

    /* Air resistance damps the complete turn vector, not separate axes. */
    g_vc_turn_world.x*=drag;
    g_vc_turn_world.y*=drag;
    g_vc_turn_world.z*=drag;

    g_vehicle_yaw_rate=g_vc_turn_world.y;
    g_body_pitch_vel=vc_v3_dot(g_vc_turn_world,g_vc_body_right);
    g_body_roll_vel=vc_v3_dot(g_vc_turn_world,g_vc_body_forward);
}

enum {
    VC_ADH_RUBBER=0,
    VC_ADH_HARD=1,
    VC_ADH_ROAD=2,
    VC_ADH_LOOSE=3,
    VC_ADH_SAND=4,
    VC_ADH_WET=5
};

static int vc_surface_adhesion_group(uint8_t surface)
{
    switch(surface){
    case 0:  /* DEFAULT */
    case 1:  /* TARMAC */
    case 5:  /* PAVEMENT */
    case 20: /* WOOD_CRATES */
    case 21: /* WOOD_BENCH */
    case 22: /* WOOD_SOLID */
    case 34: /* CONCRETE_BEACH */
        return VC_ADH_ROAD;
    case 2:  /* GRASS */
    case 3:  /* GRAVEL */
    case 25: /* HEDGE */
    case 26: /* STEEP_CLIFF */
    case 30: /* CARDBOARDBOX */
        return VC_ADH_LOOSE;
    case 18: /* SAND */
    case 33: /* SAND_BEACH */
        return VC_ADH_SAND;
    case 19: /* WATER */
        return VC_ADH_WET;
    case 17: /* PED */
    case 23: /* RUBBER */
    case 29: /* WHEELBASE */
        return VC_ADH_RUBBER;
    case 4:  /* MUD_DRY */
    case 6:  /* CAR */
    case 7:  /* GLASS */
    case 8:  /* TRANSPARENT_CLOTH */
    case 9:  /* GARAGE_DOOR */
    case 10: /* CAR_PANEL */
    case 11: /* THICK_METAL_PLATE */
    case 12: /* SCAFFOLD_POLE */
    case 13: /* LAMP_POST */
    case 14: /* FIRE_HYDRANT */
    case 15: /* GIRDER */
    case 16: /* METAL_CHAIN_FENCE */
    case 24: /* PLASTIC */
    case 27: /* CONTAINER */
    case 28: /* NEWS_VENDOR */
    case 31: /* TRANSPARENT_STONE */
    case 32: /* METAL_GATE */
        return VC_ADH_HARD;
    default:
        return VC_ADH_ROAD;
    }
}

static float vc_surface_adhesive_limit(uint8_t surface)
{
    int g=vc_surface_adhesion_group(surface);
    float v=g_vc_surface.adhesive[VC_ADH_RUBBER][g];
    if(!(v>0.01f&&v<10.0f))v=1.0f;
    return v;
}

static float vc_revc_transmission_thrust(float gas)
{
    vc_handling_lite_t *h=&g_vehicle_handling;
    int gears=(int)h->gears;
    float v=g_vehicle_vlong;
    float maxv=fmaxf(1.0f,h->max_forward);
    float per,target,accel;
    int gear;

    if(fabsf(gas)<1.0e-5f)return 0.0f;
    if(gas<0.0f){
        g_vc_current_gear=0;
        target=-fmaxf(1.0f,h->max_reverse);
        accel=(target-v)*h->engine_accel/fmaxf(1.0f,fabsf(target));
        return fabsf(gas)*accel;
    }

    if(gears<1)gears=1;
    if(gears>8)gears=8;
    gear=(int)g_vc_current_gear;
    if(gear<1)gear=1;
    if(gear>gears)gear=gears;
    per=maxv/(float)gears;

    if(gear<gears){
        float up=(float)(gear-1)*per+per*0.6667f;
        if(v>up)gear++;
    }
    if(gear>1){
        float down=(float)(gear-2)*per+per*0.42f;
        if(v<down)gear--;
    }
    g_vc_current_gear=(uint8_t)gear;

    if(gears==1){
        target=maxv;
    }else{
        float f=1.0f-(float)(gear-1)/(float)(gears-1);
        float speed_mul=3.0f*f*f+1.0f;
        target=(float)gear*per*speed_mul;
    }
    if(target<1.0f)target=maxv;
    accel=(target-v)*h->engine_accel/fmaxf(1.0f,fabsf(target));
    if(v>(float)gear*per && gear>=gears)return 0.0f;
    return gas*accel;
}

static int vc_revc_wheel_basis(int i,float heading,v3f_t *fwd,v3f_t *right)
{
    vc_wheel_contact_t *c=&g_vc_wheel_contact[i];
    v3f_t local_fwd={0.0f,0.0f,1.0f};
    v3f_t base_fwd;
    float d,steer=0.0f,cs,sn;
    v3f_t oldf,oldr;
    (void)heading;

    vc_body_rotate_local(local_fwd,&base_fwd);
    d=vc_v3_dot(base_fwd,c->normal);
    base_fwd.x-=c->normal.x*d;
    base_fwd.y-=c->normal.y*d;
    base_fwd.z-=c->normal.z*d;
    if(!vc_v3_normalize(&base_fwd))return 0;

    *right=vc_v3_cross(c->normal,base_fwd);
    if(!vc_v3_normalize(right))return 0;
    *fwd=base_fwd;

    /*
     * reVC rotates both front tyre bases by the same m_fSteerAngle.
     * Keep Ackermann only for wheel rendering; physics uses the GTA rack angle.
     */
    if(i==0 || i==1)steer=g_steer_angle;
    if(i<2 && fabsf(steer)>1.0e-5f){
        cs=cosf(steer);sn=sinf(steer);
        oldf=*fwd;oldr=*right;
        /* Positive Racer steer turns toward +local-right. */
        fwd->x=cs*oldf.x+sn*oldr.x;
        fwd->y=cs*oldf.y+sn*oldr.y;
        fwd->z=cs*oldf.z+sn*oldr.z;
        right->x=-sn*oldf.x+cs*oldr.x;
        right->y=-sn*oldf.y+cs*oldr.y;
        right->z=-sn*oldf.z+cs*oldr.z;
        vc_v3_normalize(fwd);
        vc_v3_normalize(right);
    }
    return 1;
}

static v3f_t vc_revc_contact_speed(v3f_t point,float heading,float vx,float vy,float vz)
{
    float wx=g_vc_turn_world.x;
    float wy=g_vc_turn_world.y;
    float wz=g_vc_turn_world.z;
    v3f_t r={point.x-g_world_x,point.y-g_world_y,point.z-g_world_z};
    (void)heading;
    return (v3f_t){
        vx+wy*r.z-wz*r.y,
        vy+wz*r.x-wx*r.z,
        vz+wx*r.y-wy*r.x
    };
}

static float vc_player_lateral_grip_scale(int wheel)
{
    float steer=fabsf(g_vc_raw_steer_input);
    float slip=fabsf(g_vehicle_slip);
    float t,min_grip;

    if(steer<0.18f || slip<=VC_PLAYER_SLIP_START_RAD)return 1.0f;
    t=(slip-VC_PLAYER_SLIP_START_RAD)/
      (VC_PLAYER_SLIP_FULL_RAD-VC_PLAYER_SLIP_START_RAD);
    t=clampf_local(t,0.0f,1.0f);
    t*=clampf_local((steer-0.18f)/0.82f,0.0f,1.0f);
    min_grip=(wheel>=2)?VC_PLAYER_REAR_GRIP_MIN:VC_PLAYER_FRONT_GRIP_MIN;
    return 1.0f-(1.0f-min_grip)*t;
}

static float vc_revc_effective_turn_mass(v3f_t point,v3f_t direction)
{
    float mass=fmaxf(1.0f,g_vehicle_handling.mass);
    float turn_mass=fmaxf(1.0f,g_vehicle_handling.turn_mass_world);
    v3f_t pos={
        point.x-g_world_x,
        point.y-g_world_y,
        point.z-g_world_z
    };
    v3f_t cross;
    float den;

    /*
     * Direct port of reVC CPhysical::GetMass(pos,dir):
     *   1 / ( |pos x dir|^2 / turnMass + 1 / mass )
     *
     * ProcessWheel uses this effective mass for ApplyTurnForce only.
     * ApplyMoveForce still receives the full vehicle mass.  Without this
     * reduction, a lateral tyre impulse at the wheel lever arm can roll/pitch
     * the compact Racer chassis far more aggressively than Vice City.
     */
    if(!vc_v3_normalize(&direction))return mass;
    cross=vc_v3_cross(pos,direction);
    den=vc_v3_dot(cross,cross)/turn_mass+1.0f/mass;
    if(!(den>1.0e-12f))return mass;
    return clampf_local(1.0f/den,1.0f,mass);
}

static void vc_revc_process_wheel(
    int i,int wheels_on_ground,float thrust,float brake,float adhesion,
    v3f_t fwd,v3f_t right,v3f_t contact_speed,float heading,
    float *vx,float *vy,float *vz)
{
    vc_handling_lite_t *h=&g_vehicle_handling;
    vc_wheel_contact_t *c=&g_vc_wheel_contact[i];
    int was_skidding=(g_vc_wheel_state[i]!=VC_WHEEL_NORMAL);
    int braking=brake>1.0e-6f;
    int driving=fabsf(thrust)>1.0e-6f;
    float contact_fwd=vc_v3_dot(contact_speed,fwd);
    float contact_side=vc_v3_dot(contact_speed,right);
    float ff=0.0f,rf=0.0f,speed2,limit;
    v3f_t linear_dv,turn_dv;

    if(wheels_on_ground<1)wheels_on_ground=1;
    g_vc_wheel_state[i]=VC_WHEEL_NORMAL;
    if(was_skidding)adhesion*=h->traction_loss;
    adhesion=fmaxf(0.001f,adhesion);

    if(fabsf(contact_side)>1.0e-6f){
        rf=-contact_side/(float)wheels_on_ground;
        /*
         * Keep propulsion/braking on the stock VC adhesion circle. Only the
         * lateral tyre force falls after the peak slip angle, so the rear can
         * rotate into a controllable slide instead of losing engine/brake
         * authority together with side grip.
         */
        rf*=vc_player_lateral_grip_scale(i);
    }

    if(braking)thrust=0.0f;
    driving=fabsf(thrust)>1.0e-6f;
    if(driving){
        ff=thrust;
        rf=clampf_local(rf,-adhesion,adhesion);
    }else if(fabsf(contact_fwd)>1.0e-6f){
        float effective_brake=brake;
        ff=-contact_fwd/(float)wheels_on_ground;
        if(!braking)
            effective_brake=fmaxf(effective_brake,h->rolling_drag/(float)wheels_on_ground);

        if(effective_brake>adhesion){
            float fixed_threshold=0.005f*vc_runtime_world_scale()*(50.0f/60.0f);
            if(fabsf(contact_fwd)>fixed_threshold)
                g_vc_wheel_state[i]=VC_WHEEL_FIXED;
        }else{
            ff=clampf_local(ff,-effective_brake,effective_brake);
        }
    }

    speed2=ff*ff+rf*rf;
    if(speed2>adhesion*adhesion){
        if(g_vc_wheel_state[i]!=VC_WHEEL_FIXED){
            float spin_threshold=0.20f*vc_runtime_world_scale()*(50.0f/60.0f);
            if(driving && fabsf(contact_fwd)<spin_threshold)
                g_vc_wheel_state[i]=VC_WHEEL_SPINNING;
            else
                g_vc_wheel_state[i]=VC_WHEEL_SKIDDING;
        }
        limit=adhesion*(was_skidding?1.0f:h->traction_loss)/sqrtf(speed2);
        ff*=limit;rf*=limit;
    }

    linear_dv=(v3f_t){
        fwd.x*ff+right.x*rf,
        fwd.y*ff+right.y*rf,
        fwd.z*ff+right.z*rf
    };
    turn_dv=linear_dv;
    if(h->suspension_antidive>0.0f){
        float anti=braking?h->suspension_antidive:
                   (driving?0.5f*h->suspension_antidive:0.0f);
        turn_dv.x-=anti*ff*fwd.x;
        turn_dv.y-=anti*ff*fwd.y;
        turn_dv.z-=anti*ff*fwd.z;
    }

    if(ff!=0.0f || rf!=0.0f){
        float turn_speed=sqrtf(vc_v3_dot(turn_dv,turn_dv));
        if(turn_speed>1.0e-8f){
            v3f_t turn_dir={
                turn_dv.x/turn_speed,
                turn_dv.y/turn_speed,
                turn_dv.z/turn_speed
            };
            float eff_mass=vc_revc_effective_turn_mass(c->point,turn_dir);
            float turn_scale=eff_mass/fmaxf(1.0f,h->mass);
            g_vc_wheel_turn_mass[i]=eff_mass;
            turn_dv.x*=turn_scale;
            turn_dv.y*=turn_scale;
            turn_dv.z*=turn_scale;
        }
        vc_apply_world_dv_turn_at_point(
            linear_dv,turn_dv,c->point,heading,vx,vy,vz);
    }

    g_vc_wheel_fwd_speed[i]=contact_fwd;
    g_vc_wheel_side_speed[i]=contact_side;
    g_vc_wheel_adhesion[i]=adhesion;
    g_vc_wheel_force_fwd[i]=ff;
    g_vc_wheel_force_side[i]=rf;
    if(ff==0.0f && rf==0.0f)g_vc_wheel_turn_mass[i]=0.0f;
    g_vc_wheel_speed[i]=contact_fwd/fmaxf(1.0f,active_vehicle_wheel_radius());
}

static void vc_apply_revc_wheel_forces(float throttle,float brake,float heading)
{
    vc_handling_lite_t *h=&g_vehicle_handling;
    float sh=sinf(heading),ch=cosf(heading);
    float vx=sh*g_vehicle_vlong+ch*g_vehicle_vlat;
    float vy=g_vehicle_vy;
    float vz=ch*g_vehicle_vlong-sh*g_vehicle_vlat;
    float scale=vc_runtime_world_scale();
    float base_traction=0.004f*scale*(50.0f/60.0f)*h->traction_mult/4.0f;
    float thrust=vc_revc_transmission_thrust(throttle);
    float brake_base=brake*h->brake_decel;
    float brake_front=2.0f*h->brake_bias;
    /* Match reVC/VC source literally; this asymmetry is intentional. */
    float brake_rear=2.0f-h->brake_bias;
    float traction_front=2.0f*h->traction_bias;
    float traction_rear=2.0f-traction_front;
    uint8_t drive_mask=active_drive_wheel_mask();
    uint8_t ground_mask=vc_wheel_timer_mask();
    int wheels_on_ground=vc_bitcount4(ground_mask);
    int order[4]={0,1,2,3};
    v3f_t wfwd[4],wright[4],contact_speed[4];
    uint8_t basis_ok[4]={0,0,0,0};
    int i,k;

    if(wheels_on_ground<1){
        memset(g_vc_wheel_force_fwd,0,sizeof(g_vc_wheel_force_fwd));
        memset(g_vc_wheel_force_side,0,sizeof(g_vc_wheel_force_side));
        return;
    }

    /*
     * reVC samples GetSpeed(contactPoint) for every wheel after spring
     * damping and before ProcessWheel mutates chassis velocity. Preserve that
     * snapshot so left/right processing order cannot steer the car.
     */
    for(i=0;i<4;++i){
        if(!(ground_mask&(1U<<i)) || !g_vc_wheel_contact[i].hit)continue;
        if(!vc_revc_wheel_basis(i,heading,&wfwd[i],&wright[i]))continue;
        contact_speed[i]=vc_revc_contact_speed(
            g_vc_wheel_contact[i].point,heading,vx,vy,vz);
        basis_ok[i]=1;
    }

    if(h->flags&0x200000U){ /* HANDLING_REARWHEEL_1ST */
        order[0]=2;order[1]=3;order[2]=0;order[3]=1;
    }

    for(k=0;k<4;++k){
        i=order[k];
        {
            vc_wheel_contact_t *c=&g_vc_wheel_contact[i];
            float wheel_thrust=0.0f,wheel_brake,wheel_adhesion,bias;
            if(!(ground_mask&(1U<<i)) || !c->hit || !basis_ok[i]){
                g_vc_wheel_state[i]=VC_WHEEL_NORMAL;
                g_vc_wheel_fwd_speed[i]=0.0f;
                g_vc_wheel_side_speed[i]=0.0f;
                g_vc_wheel_adhesion[i]=0.0f;
                g_vc_wheel_force_fwd[i]=0.0f;
                g_vc_wheel_force_side[i]=0.0f;
                g_vc_wheel_turn_mass[i]=0.0f;
                g_vc_wheel_speed[i]*=0.95f;
                continue;
            }
            if(drive_mask&(1U<<i))wheel_thrust=thrust;
            wheel_brake=brake_base*(i<2?brake_front:brake_rear);
            bias=(i<2)?traction_front:traction_rear;
            wheel_adhesion=
                base_traction*vc_surface_adhesive_limit(c->surface)*bias;
            vc_revc_process_wheel(
                i,wheels_on_ground,wheel_thrust,wheel_brake,wheel_adhesion,
                wfwd[i],wright[i],contact_speed[i],heading,&vx,&vy,&vz);
        }
    }

    g_vehicle_vlong=sh*vx+ch*vz;
    g_vehicle_vlat=ch*vx-sh*vz;
    g_vehicle_vy=vy;
}

static int vc_body_contact_is_suspension_floor(
    const vc_body_contact_t *col,float vn,float wheel_r,float scale)
{
    float escape_depth;
    int ground_support;
    if(!col)return 0;
    ground_support=vc_bitcount4(vc_wheel_timer_mask());
    escape_depth=fmaxf(wheel_r*0.60f,0.15f*scale);
    return
        col->ny>0.65f &&
        ground_support>=2 &&
        fabsf(g_body_pitch)<0.60f &&
        fabsf(g_body_roll)<0.60f &&
        col->depth<escape_depth &&
        vn>-0.12f*scale;
}

static void vc_log_player_dynamics_event(void)
{
    uint64_t now;
    unsigned mask;
    int contacts;
    float slip=fabsf(g_vehicle_slip);
    float roll=fabsf(g_body_roll);
    float steer=fabsf(g_vc_raw_steer_input);

    if(!g_vc_city_mode)return;
    mask=vc_wheel_timer_mask();
    contacts=vc_bitcount4(mask);
    if(!((slip>=0.055f &&
          (fabsf(g_vehicle_vlong)>=5.0f || fabsf(g_vehicle_vlat)>=5.0f)) ||
         roll>=0.10f || g_vc_two_wheel_ticks>0U || contacts<4 ||
         (steer>=0.60f && fabsf(g_vehicle_vlong)>=25.0f)))
        return;

    now=mono_ns();
    if(g_vc_dyn_last_log_ns && now-g_vc_dyn_last_log_ns<180000000ULL)return;
    g_vc_dyn_last_log_ns=now;

    fprintf(stderr,
        "[racer] VC_DYN speed=%.2f vlat=%.2f steer=%.3f slip=%.3f yaw=%.5f "
        "body=%.3f/%.3f bodyv=%.5f/%.5f twheel=%u contact=0x%x "
        "com=%.1f/%.1f state=%u/%u/%u/%u adh=%.3f/%.3f/%.3f/%.3f "
        "forceSide=%.3f/%.3f/%.3f/%.3f forceFwd=%.3f/%.3f/%.3f/%.3f\n",
        g_vehicle_vlong,g_vehicle_vlat,g_vc_raw_steer_input,g_vehicle_slip,
        g_vehicle_yaw_rate,g_body_pitch,g_body_roll,
        g_body_pitch_vel,g_body_roll_vel,(unsigned)g_vc_two_wheel_ticks,mask,
        g_vehicle_handling.centre_of_mass.y,g_vc_effective_com_y,
        (unsigned)g_vc_wheel_state[0],(unsigned)g_vc_wheel_state[1],
        (unsigned)g_vc_wheel_state[2],(unsigned)g_vc_wheel_state[3],
        g_vc_wheel_adhesion[0],g_vc_wheel_adhesion[1],
        g_vc_wheel_adhesion[2],g_vc_wheel_adhesion[3],
        g_vc_wheel_force_side[0],g_vc_wheel_force_side[1],
        g_vc_wheel_force_side[2],g_vc_wheel_force_side[3],
        g_vc_wheel_force_fwd[0],g_vc_wheel_force_fwd[1],
        g_vc_wheel_force_fwd[2],g_vc_wheel_force_fwd[3]);
}

static void game_update(input_t *in)
{
    vc_handling_lite_t *h=&g_vehicle_handling;
    float raw_steer=(float)in->steer/32767.0f;
    float steer_shaped;
    float pedal=(in->gas?1.0f:0.0f)-(in->brake?1.0f:0.0f);
    float throttle=0.0f,brake=0.0f;
    float previous=g_vehicle_vlong;
    float abs_speed,limit,engine_factor;
    float drive_support=vc_drive_support_factor();
    float brake_support=vc_brake_support_factor();
    float lateral_support=vc_lateral_support_factor();
    float wb=active_vehicle_wheelbase();
    float wheel_r=active_vehicle_wheel_radius();
    float target_yaw,yaw_response,yaw_delta;
    float cs,sn,new_long,new_lat;
    float slip_ratio,lateral_grip,lateral_kill;
    float travel_fwd,travel_side;
    float old_world_x,old_world_y,old_world_z,old_ground_y;
    float longitudinal,lateral;
    float accel,abs_ratio;
    float surface_pitch=0.0f,surface_roll=0.0f;

    if(dev_hover_update(in)){
        /* Hover is still a world-space vehicle move, so the reVC chase camera
         * must run every simulation tick just like normal driving. */
        update_chase_camera(0.0f);
        return;
    }

    /*
     * reVC keeps two steering values with deliberately different semantics:
     * ProcessControlInputs filters/inverts the steering for the rack, while
     * the player two-wheel balance aid reads CPad::GetSteeringLeftRight()
     * directly.  Preserve the raw pad value separately before filtering.
     */
    g_vc_raw_steer_input=clampf_local(raw_steer,-1.0f,1.0f);

    /* reVC-like input shaping: smooth first, then signed square. */
    g_vehicle_steer_input+=(raw_steer-g_vehicle_steer_input)*(0.20f*(50.0f/60.0f));
    g_vehicle_steer_input=clampf_local(g_vehicle_steer_input,-1.0f,1.0f);
    steer_shaped=(g_vehicle_steer_input<0.0f)
        ?-(g_vehicle_steer_input*g_vehicle_steer_input)
        :(g_vehicle_steer_input*g_vehicle_steer_input);
    g_steer_angle=steer_shaped*h->steering_lock_rad;
    update_ackermann(g_steer_angle);

    /* Opposite pedal brakes first; reverse only engages around standstill. */
    if(fabsf(g_vehicle_vlong)<0.75f)
        throttle=pedal;
    else if(pedal!=0.0f && g_vehicle_vlong*pedal<0.0f)
        brake=fabsf(pedal);
    else
        throttle=pedal;

    if(wb<100.0f)wb=600.0f;
    if(wheel_r<10.0f)wheel_r=110.0f;

    if(!g_vc_city_mode){
        /*
         * Legacy track/OSM controller. Vice City mode no longer uses this
         * bicycle-yaw/lateral-kill approximation; its heading and velocity are
         * produced by the four ProcessWheel-style contact forces below.
         */
        abs_speed=fabsf(g_vehicle_vlong);
        limit=throttle<0.0f?h->max_reverse:h->max_forward;
        if(limit<1.0f)limit=1.0f;
        engine_factor=1.0f-clampf_local(abs_speed/limit,0.0f,1.0f)*0.78f;
        if(throttle!=0.0f && drive_support>0.0f)
            g_vehicle_vlong+=throttle*h->engine_accel*engine_factor*drive_support;

        if(brake>0.0f && brake_support>0.0f)
            g_vehicle_vlong=approach_zero(
                g_vehicle_vlong,h->brake_decel*brake*brake_support);

        if(throttle==0.0f && brake==0.0f)
            g_vehicle_vlong=approach_zero(g_vehicle_vlong,h->rolling_drag);
        if(g_vehicle_vlong!=0.0f){
            float aero=h->aero_drag*g_vehicle_vlong*g_vehicle_vlong;
            if(g_vehicle_vlong>0.0f)g_vehicle_vlong-=aero;
            else g_vehicle_vlong+=aero;
        }
        g_vehicle_vlong=clampf_local(
            g_vehicle_vlong,-h->max_reverse,h->max_forward);

        target_yaw=(fabsf(g_vehicle_vlong)>0.20f &&
                    (g_vc_wheel_contact_mask&0x03U))
            ?(g_vehicle_vlong/wb)*tanf(g_steer_angle):0.0f;
        yaw_response=0.13f+0.11f*clampf_local(h->traction_mult,0.4f,1.4f);
        g_vehicle_yaw_rate+=(target_yaw-g_vehicle_yaw_rate)*yaw_response;
        if(fabsf(g_vehicle_steer_input)<0.01f)
            g_vehicle_yaw_rate*=0.94f;

        yaw_delta=g_vehicle_yaw_rate;
        cs=cosf(yaw_delta);sn=sinf(yaw_delta);
        new_long=g_vehicle_vlong*cs+g_vehicle_vlat*sn;
        new_lat=-g_vehicle_vlong*sn+g_vehicle_vlat*cs;
        g_vehicle_vlong=new_long;
        g_vehicle_vlat=new_lat;

        slip_ratio=fabsf(g_vehicle_vlat)/(fabsf(g_vehicle_vlong)+2.0f);
        lateral_grip=(1.25f+0.038f*fabsf(g_vehicle_vlong))*
            h->traction_mult*lateral_support;
        if(slip_ratio>0.105f)
            lateral_grip*=h->traction_loss;
        lateral_kill=clampf_local(g_vehicle_vlat,-lateral_grip,lateral_grip);
        g_vehicle_vlat-=lateral_kill;
        if(slip_ratio>0.16f)
            g_vehicle_yaw_rate*=0.985f+
                0.010f*clampf_local(h->traction_loss,0.0f,1.0f);

        g_vehicle_heading+=yaw_delta;
        while(g_vehicle_heading>3.14159265f)g_vehicle_heading-=6.2831853f;
        while(g_vehicle_heading<-3.14159265f)g_vehicle_heading+=6.2831853f;
    }

    travel_fwd=g_vehicle_vlong;
    travel_side=g_vehicle_vlat;
    g_speed=g_vehicle_vlong;
    g_vehicle_slip=atan2f(g_vehicle_vlat,fabsf(g_vehicle_vlong)+1.0f);

    if(g_vc_city_mode){
        const float edge_margin=420.0f;
        /* Refresh diagnostic/player-balance COM every simulation tick. */
        (void)vc_effective_centre_of_mass();
        float road_y;
        float sh=sinf(g_vehicle_heading),ch=cosf(g_vehicle_heading);

        old_world_x=g_world_x;
        old_world_y=g_world_y;
        old_world_z=g_world_z;
        old_ground_y=g_vc_ground_y;
        g_world_x+=sh*travel_fwd+ch*travel_side;
        g_world_z+=ch*travel_fwd-sh*travel_side;

        if(g_world_x>vc_runtime_max_x()-edge_margin)g_world_x=vc_runtime_max_x()-edge_margin;
        if(g_world_x<vc_runtime_min_x()+edge_margin)g_world_x=vc_runtime_min_x()+edge_margin;
        if(g_world_z>vc_runtime_max_z()-edge_margin)g_world_z=vc_runtime_max_z()-edge_margin;
        if(g_world_z<vc_runtime_min_z()+edge_margin)g_world_z=vc_runtime_min_z()+edge_margin;

        /*
         * reVC suspension architecture:
         *  - gravity always acts on the chassis;
         *  - each transformed suspension line collides with native GTA COL;
         *  - each compressed spring applies a force at its own contact point;
         *  - pitch/roll arise from r x J, never from a target road angle.
         */
        {
            float gravity=vc_runtime_world_scale()*9.81f/(60.0f*60.0f);
            int contacts;

            g_vehicle_vy-=gravity;
            contacts=(g_vc_collision.loaded)?
                vc_collision_four_contacts(
                    g_world_x,g_world_z,g_vehicle_heading,g_vc_ground_y,
                    wb,active_vehicle_track(),
                    &road_y,&surface_pitch,&surface_roll):0;

            if(contacts>0){
                g_vc_ground_y=road_y;
                vc_apply_revc_suspension(g_vehicle_heading);
            }

            /*
             * Exact Vice City PlayerInfo rule:
             *   if (car->m_nWheelsOnGround < 3)
             *       m_nTimeNotFullyOnGround += timestep;
             *   else
             *       m_nTimeNotFullyOnGround = 0;
             *
             * CAutomobile computes m_nWheelsOnGround from m_aWheelTimer>0,
             * not only from this frame's exact spring hits.  Our previous
             * 2/3-exact-contact rule was wrong in both directions: it started
             * the aid with three wheels and disabled it after the car fell to
             * one or zero wheels.  That is exactly the phase where the hardware
             * log shows the Oceanic continuing from ~49 degrees to upside down.
             */
            {
                int gta_wheels_on_ground=
                    vc_bitcount4(vc_wheel_timer_mask());
                if(gta_wheels_on_ground<3){
                    if(g_vc_two_wheel_ticks<180U)g_vc_two_wheel_ticks++;
                }else{
                    g_vc_two_wheel_ticks=0;
                }
            }

            /*
             * reVC counts m_aWheelTimer, not only this frame's spring hits,
             * when deciding whether tyres can still transmit force. A missed
             * thin COL triangle therefore gets up to four ticks of tyre
             * continuity, but no spring force until a real line hit returns.
             */
            if(vc_wheel_timer_mask()!=0){
                g_vehicle_airborne=0;
                vc_apply_revc_wheel_forces(
                    throttle,brake,g_vehicle_heading);
            }else{
                g_vehicle_airborne=1;
                memset(g_vc_wheel_force_fwd,0,sizeof(g_vc_wheel_force_fwd));
                memset(g_vc_wheel_force_side,0,sizeof(g_vc_wheel_force_side));
            }

            /* Air resistance remains a chassis force; rolling resistance is
             * handled by ProcessWheel when no pedal is pressed. */
            if(g_vehicle_vlong!=0.0f){
                float aero=h->aero_drag*g_vehicle_vlong*g_vehicle_vlong;
                if(g_vehicle_vlong>0.0f)g_vehicle_vlong=fmaxf(0.0f,g_vehicle_vlong-aero);
                else g_vehicle_vlong=fminf(0.0f,g_vehicle_vlong+aero);
            }
            g_vehicle_vlong=clampf_local(
                g_vehicle_vlong,-h->max_reverse,h->max_forward);

            g_world_y+=g_vehicle_vy;

            if(g_vehicle_airborne && g_vc_collision.loaded && g_vehicle_vy<0.0f){
                /*
                 * Catastrophic/high-speed landing guard. Unlike the developer
                 * hover helper this is native VCCOL-only: surface 254 can no
                 * longer hold the car during ordinary driving.
                 */
                float ride=active_vehicle_ride_height();
                float hit_y=0.0f;
                uint8_t hit_surface=0;
                float prev_bottom=old_world_y-ride;
                float new_bottom=g_world_y-ride;
                if(vc_collision_vertical_contact_native(
                    g_world_x,g_world_z,prev_bottom,new_bottom,
                    &hit_y,&hit_surface)){
                    g_vc_ground_y=hit_y;
                    g_world_y=hit_y+ride;
                    g_vehicle_vy*=-0.05f;
                    if(fabsf(g_vehicle_vy)<2.0f)g_vehicle_vy=0.0f;
                    g_vehicle_airborne=0;
                    g_vc_last_body_surface=hit_surface;
                }
            }
        }

        {
            vc_body_contact_t col={0};
            if(vc_collision_vehicle_body_contact(
                g_world_x,g_world_y,g_world_z,g_vehicle_heading,
                wb,active_vehicle_track(),wheel_r,&col)){
                float vx=sh*g_vehicle_vlong+ch*g_vehicle_vlat;
                float vz=ch*g_vehicle_vlong-sh*g_vehicle_vlat;
                float vy=g_vehicle_vy;
                v3f_t com_rot;
                v3f_t r,n,omega,point_v,rxn;
                float mass=fmaxf(1.0f,g_vehicle_handling.mass);
                float turn_mass=fmaxf(1.0f,g_vehicle_handling.turn_mass_world);
                float vn;
                float scale=vc_runtime_world_scale();

                vc_body_rotate_local(
                    vc_effective_centre_of_mass(),&com_rot);
                r=(v3f_t){
                    col.px-(g_world_x+com_rot.x),
                    col.py-(g_world_y+com_rot.y),
                    col.pz-(g_world_z+com_rot.z)
                };
                n=(v3f_t){col.nx,col.ny,col.nz};
                /*
                 * Full CPhysical::GetSpeed(point): include pitch and roll in
                 * the contact velocity. The previous yaw-only approximation
                 * could see a nose/sill impact as nearly stationary while the
                 * body was rotating hard into the road.
                 */
                omega=g_vc_turn_world;
                point_v=(v3f_t){
                    vx + omega.y*r.z - omega.z*r.y,
                    vy + omega.z*r.x - omega.x*r.z,
                    vz + omega.x*r.y - omega.y*r.x
                };
                vn=point_v.x*n.x+point_v.y*n.y+point_v.z*n.z;
                float slop=0.0125f*scale;
                float correction=fmaxf(0.0f,col.depth-slop);
                float restitution=fabsf(vn)>8.0f?0.10f:0.0f;
                int suspension_floor=
                    vc_body_contact_is_suspension_floor(
                        &col,vn,wheel_r,scale);

                g_vc_collision_blocks_window++;
                g_vc_collision_blocks_total++;
                g_vc_last_col_depth=col.depth;
                g_vc_last_col_nx=col.nx;g_vc_last_col_ny=col.ny;g_vc_last_col_nz=col.nz;
                g_vc_last_col_vn=vn;

                /*
                 * The GTA vehicle body COL and suspension lines overlap the
                 * road slightly at normal ride height. reVC resolves the
                 * chassis through its full CPhysical collision pipeline; our
                 * older compact response performed an unconditional positional
                 * push every tick. That double-supported the car (springs +
                 * body sphere) and ratcheted it upward until suspension lines
                 * lost the road. While >=2 real suspension lines carry an
                 * upright car, a shallow upward-facing body contact is therefore
                 * diagnostic only. Walls, roofs, deep bottom-outs and hard
                 * landings still use normal body collision response.
                 */
                if(suspension_floor){
                    g_vc_body_floor_suppressed_window++;
                }else if(correction>0.0f){
                    float push=correction*1.05f;
                    g_world_x+=col.nx*push;
                    g_world_y+=col.ny*push;
                    g_world_z+=col.nz*push;
                }

                if(vn<0.0f && !suspension_floor){
                    float rxn2,inv_eff,impulse,com_dv;
                    v3f_t body_dv;
                    v3f_t body_point={col.px,col.py,col.pz};

                    /*
                     * reVC static-world collision uses GetMass(point, normal),
                     * i.e. the effective mass at the contact point. With a
                     * scalar turn inertia our equivalent is:
                     *   1/Meff = 1/M + |r x n|^2 / I
                     * Ignoring the rotational term gave an off-centre nose or
                     * sill hit the full translational impulse AND full torque,
                     * injecting enough energy to stand the car on an axle.
                     */
                    rxn=vc_v3_cross(r,n);
                    rxn2=vc_v3_dot(rxn,rxn);
                    inv_eff=1.0f/mass + rxn2/turn_mass;
                    if(inv_eff<1.0e-9f)inv_eff=1.0f/mass;
                    impulse=-(1.0f+restitution)*vn/inv_eff;
                    com_dv=impulse/mass;
                    body_dv=(v3f_t){
                        n.x*com_dv,n.y*com_dv,n.z*com_dv
                    };

                    /*
                     * reVC reduces vertical force on non-floor vehicle/static
                     * contacts. Racer's vertical axis is Y (GTA's Z).
                     */
                    if(n.y<0.70f)body_dv.y*=0.30f;

                    vc_apply_world_dv_at_point(
                        body_dv,body_point,g_vehicle_heading,
                        &vx,&vy,&vz);

                    /* reVC makes normal upright vehicle/building contacts
                     * effectively frictionless. Only floor-like body contacts
                     * get a small capped tangential correction here. */
                    if(col.ny>0.65f){
                        float nd=vx*col.nx+vy*col.ny+vz*col.nz;
                        float tx=vx-col.nx*nd,ty=vy-col.ny*nd,tz=vz-col.nz*nd;
                        float tm=sqrtf(tx*tx+ty*ty+tz*tz);
                        float max_fric=fabsf(com_dv)*0.12f;
                        if(tm>1.0e-5f && max_fric>0.0f){
                            float cut=fminf(tm,max_fric)/tm;
                            vx-=tx*cut;vy-=ty*cut;vz-=tz*cut;
                        }
                    }
                }

                g_vehicle_vlong=sh*vx+ch*vz;
                g_vehicle_vlat=ch*vx-sh*vz;
                g_vehicle_vy=vy;
                g_speed=g_vehicle_vlong;

                /* A second penetration-only correction handles a corner or
                 * pole touching another native body sphere after the first
                 * separation. It intentionally adds no velocity damping. */
                {
                    vc_body_contact_t c2={0};
                    if(vc_collision_vehicle_body_contact(
                        g_world_x,g_world_y,g_world_z,g_vehicle_heading,
                        wb,active_vehicle_track(),wheel_r,&c2)){
                        float c2corr=fmaxf(0.0f,c2.depth-slop);
                        int c2_floor=
                            c2.ny>0.65f &&
                            vc_bitcount4(vc_wheel_timer_mask())>=2 &&
                            fabsf(g_body_pitch)<0.60f &&
                            fabsf(g_body_roll)<0.60f &&
                            c2.depth<fmaxf(wheel_r*0.60f,0.15f*scale);
                        if(c2corr>0.0f && !c2_floor){
                            float push2=c2corr*0.80f;
                            g_world_x+=c2.nx*push2;
                            g_world_y+=c2.ny*push2;
                            g_world_z+=c2.nz*push2;
                        }
                    }
                }
            }
        }

        /*
         * reVC-style ApplyTurnSpeed: all wheel, spring and body impulses have
         * now accumulated into one world-space turn vector. Rotate the whole
         * body basis once, then re-express the unchanged world linear velocity
         * in the new vehicle heading.
         */
        {
            float oldh=g_vehicle_heading;
            float osh=sinf(oldh),och=cosf(oldh);
            float vx=osh*g_vehicle_vlong+och*g_vehicle_vlat;
            float vz=och*g_vehicle_vlong-osh*g_vehicle_vlat;
            float nsh,nch;

            vc_integrate_turn_world();
            nsh=sinf(g_vehicle_heading);nch=cosf(g_vehicle_heading);
            g_vehicle_vlong=nsh*vx+nch*vz;
            g_vehicle_vlat =nch*vx-nsh*vz;
        }
        g_speed=g_vehicle_vlong;
        g_vehicle_slip=atan2f(
            g_vehicle_vlat,fabsf(g_vehicle_vlong)+1.0f);
        vc_log_player_dynamics_event();

        g_position=0.0f;
        g_player_x=0.0f;
    }else if(g_osm_city_mode){
        const float edge_margin=420.0f;
        const float car_margin=180.0f;
        float sh=sinf(g_vehicle_heading),ch=cosf(g_vehicle_heading);

        old_world_x=g_world_x;
        old_world_z=g_world_z;
        g_world_x+=sh*travel_fwd+ch*travel_side;
        g_world_z+=ch*travel_fwd-sh*travel_side;

        if(osm_city_hits_building(g_world_x,g_world_z,car_margin)){
            g_world_x=old_world_x;
            g_world_z=old_world_z;
            g_vehicle_vlong*=-0.14f;
            g_vehicle_vlat*=0.25f;
            g_vehicle_yaw_rate*=0.65f;
            g_speed=g_vehicle_vlong;
        }

        if(g_world_x>OSM_CITY_MAX_X-edge_margin)g_world_x=OSM_CITY_MAX_X-edge_margin;
        if(g_world_x<OSM_CITY_MIN_X+edge_margin)g_world_x=OSM_CITY_MIN_X+edge_margin;
        if(g_world_z>OSM_CITY_MAX_Z-edge_margin)g_world_z=OSM_CITY_MAX_Z-edge_margin;
        if(g_world_z<OSM_CITY_MIN_Z+edge_margin)g_world_z=OSM_CITY_MIN_Z+edge_margin;

        g_world_y=osm_city_height_at_world(g_world_x,g_world_z)+21.0f;
        g_position=0.0f;
        g_player_x=0.0f;
    }else{
        track_world_t center;
        float rel;
        track_pose_at(g_position,0.0f,&center);
        rel=wrap_angle(g_vehicle_heading-center.yaw);
        longitudinal=travel_fwd*cosf(rel)-travel_side*sinf(rel);
        lateral=travel_fwd*sinf(rel)+travel_side*cosf(rel);
        g_position+=longitudinal;
        g_player_x+=lateral/ROAD_WIDTH;

        if(g_player_x>8.0f)g_player_x=8.0f;
        if(g_player_x<-8.0f)g_player_x=-8.0f;

        if(fabsf(g_player_x)>1.05f){
            float drag=OFFROAD_DECEL*0.18f;
            g_vehicle_vlong=approach_zero(g_vehicle_vlong,drag);
            g_vehicle_vlat*=0.94f;
            g_speed=g_vehicle_vlong;
        }
    }

    accel=g_vehicle_vlong-previous;
    abs_ratio=fabsf(g_vehicle_vlong)/(h->max_forward>1.0f?h->max_forward:90.0f);
    if(!(g_vc_city_mode && g_vc_vehicle.loaded &&
         g_vc_vehicle.native_col_loaded && g_vc_vehicle.col_line_count>=4U)){
        g_body_pitch+=(clampf_local(
            surface_pitch-accel*0.0085f,-0.30f,0.30f)-g_body_pitch)*0.16f;
        g_body_roll+=(clampf_local(
            surface_roll-g_steer_angle*abs_ratio*0.24f-g_vehicle_slip*0.52f,
            -0.30f,0.30f)-g_body_roll)*0.14f;
    }

    g_wheel_spin+=g_vehicle_vlong/wheel_r;
    while(g_wheel_spin>6.2831853f)g_wheel_spin-=6.2831853f;
    while(g_wheel_spin<-6.2831853f)g_wheel_spin+=6.2831853f;

    g_steer_visual+=(raw_steer-g_steer_visual)*0.13f;
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
    if(g_vc_vehicle.loaded && g_vc_vehicle.atlas)
        sum^=prefault_words(g_vc_vehicle.atlas,
            (size_t)g_vc_vehicle.atlas_w*(size_t)g_vc_vehicle.atlas_h);

#if defined(MCL_CURRENT)
    if(mlockall(MCL_CURRENT|MCL_FUTURE)==0)locked=1;
#endif
    fprintf(stderr,"[racer] assets prefaulted checksum=%08x mlockall=%s\n",
            (unsigned)sum,locked?"active":"unavailable");
    fprintf(stderr,
        "[racer] VC player dynamics lateralRearMin=%.2f lateralFrontMin=%.2f slip=%.3f..%.3frad balanceAid=%uticks ramp=%.0f comMaxUp=%.2fm balance=down-only dynlog=event\n",
        VC_PLAYER_REAR_GRIP_MIN,VC_PLAYER_FRONT_GRIP_MIN,
        VC_PLAYER_SLIP_START_RAD,VC_PLAYER_SLIP_FULL_RAD,
        (unsigned)VC_PLAYER_BALANCE_START_TICKS,
        VC_PLAYER_BALANCE_RAMP_TICKS,VC_PLAYER_COM_MAX_UP_M);
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
    if(!g_vc_city_mode)draw_player_shadow();
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
    init_vc_color_chan_lut();
    v.canvas[0]=(uint16_t*)calloc((size_t)RW*RH,sizeof(uint16_t));
    v.base=(uint16_t*)calloc((size_t)RW*RH,sizeof(uint16_t));
    if(!v.canvas[0]||!v.base)return 2;
    g_canvas=v.canvas[0];
    build_level();
    try_load_vc_map();
    try_load_vc_collision();
    if(getenv("RACER_SELFTEST_VFW"))try_load_vc_world();
    try_load_vc_vehicle();
    if(getenv("RACER_SELFTEST_VCHAND"))try_load_vc_handling();
    if(getenv("RACER_SELFTEST_VCSURF"))try_load_vc_surface();
    reset_chase_camera();

    if(g_vc_collision.version==2 &&
       g_vc_vehicle.native_col_loaded &&
       g_vc_vehicle.col_line_count>=4U){
        float gy=0.0f,gp=0.0f,gr=0.0f;
        int contacts;
        float saved_world_y=g_world_y;
        g_world_y=0.0f;
        contacts=vc_collision_four_contacts(
            0.0f,0.0f,0.0f,0.0f,500.0f,260.0f,&gy,&gp,&gr);
        g_world_y=saved_world_y;
        if(contacts!=4 || g_vc_wheel_contact_mask!=0x0fU){
            fprintf(stderr,
                "RACER_SELFTEST_FAIL suspension contacts=%d mask=0x%x ground=%.2f\n",
                contacts,(unsigned)g_vc_wheel_contact_mask,gy);
            return 4;
        }
        fprintf(stderr,
            "RACER_SELFTEST_SUSPENSION_OK contacts=%d mask=0x%x surface=%u/%u/%u/%u\n",
            contacts,(unsigned)g_vc_wheel_contact_mask,
            (unsigned)g_vc_wheel_surface[0],(unsigned)g_vc_wheel_surface[1],
            (unsigned)g_vc_wheel_surface[2],(unsigned)g_vc_wheel_surface[3]);

        /*
         * Synthetic VCCOL2 fixture has a second road deck at y=2 game units.
         * A vertical line confined around that deck must select the bridge,
         * never the road underneath at y=0.
         */
        {
            float by=0.0f;
            uint8_t bs=0;
            int bridge=vc_collision_vertical_contact(
                0.0f,0.0f,
                3.0f*g_vc_collision.world_scale,
                1.0f*g_vc_collision.world_scale,
                &by,&bs);
            if(!bridge || fabsf(by-2.0f*g_vc_collision.world_scale)>1.0f || bs!=7U){
                fprintf(stderr,
                    "RACER_SELFTEST_FAIL bridge contact=%d y=%.2f surface=%u\n",
                    bridge,by,(unsigned)bs);
                return 6;
            }
            fprintf(stderr,
                "RACER_SELFTEST_BRIDGE_OK y=%.1f surface=%u\n",
                by,(unsigned)bs);
        }

        /*
         * Regress the real-device tunnelling case: the nominal line ends just
         * above a thin native road, so exact contact must miss while the narrow
         * native-COL envelope catches it. No VCMAP fallback is involved.
         */
        {
            float sc=g_vc_collision.world_scale;
            v3f_t p0={0.0f,0.30f*sc,0.0f};
            v3f_t p1={0.0f,0.05f*sc,0.0f};
            vc_wheel_contact_t exact={0},rescue={0};
            int eh=vc_collision_suspension_segment(p0,p1,&exact);
            int rh=vc_collision_suspension_rescue(p0,p1,&rescue);
            if(eh || !rh || rescue.surface!=1U || rescue.ratio>=1.0f){
                fprintf(stderr,
                    "RACER_SELFTEST_FAIL suspension-rescue exact=%d rescue=%d surface=%u ratio=%.3f\n",
                    eh,rh,(unsigned)rescue.surface,rescue.ratio);
                return 7;
            }
            fprintf(stderr,
                "RACER_SELFTEST_SUSPENSION_RESCUE_OK exact=%d rescue=%d surface=%u ratio=%.3f\n",
                eh,rh,(unsigned)rescue.surface,rescue.ratio);
        }

        /*
         * Regression for the downloaded Rally sports-car path.  VCVEH.BIN is
         * intentionally absent in normal VFW sports mode, so the four mesh
         * wheel pivots themselves must produce real suspension contacts.
         */
        {
            int saved_loaded=g_vc_vehicle.loaded;
            int saved_native_loaded=g_vc_vehicle.native_col_loaded;
            uint32_t saved_native_version=g_vc_vehicle.native_col_version;
            uint32_t saved_lines=g_vc_vehicle.col_line_count;
            float saved_y=g_world_y;
            float saved_pitch=g_body_pitch,saved_roll=g_body_roll;
            float gy=0.0f,gp=0.0f,gr=0.0f;
            int contacts,i,all_hit=1;

            g_vc_vehicle.loaded=0;
            g_vc_vehicle.native_col_loaded=0;
            g_vc_vehicle.native_col_version=0;
            g_vc_vehicle.col_line_count=0;
            g_body_pitch=0.0f;g_body_roll=0.0f;
            g_world_y=active_vehicle_ride_height();
            contacts=vc_collision_four_contacts(
                0.0f,0.0f,0.0f,0.0f,
                active_vehicle_wheelbase(),active_vehicle_track(),
                &gy,&gp,&gr);
            for(i=0;i<4;++i)if(!g_vc_wheel_contact[i].hit)all_hit=0;

            g_vc_vehicle.loaded=saved_loaded;
            g_vc_vehicle.native_col_loaded=saved_native_loaded;
            g_vc_vehicle.native_col_version=saved_native_version;
            g_vc_vehicle.col_line_count=saved_lines;
            g_world_y=saved_y;g_body_pitch=saved_pitch;g_body_roll=saved_roll;

            if(contacts!=4 || g_vc_wheel_contact_mask!=0x0fU || !all_hit){
                fprintf(stderr,
                    "RACER_SELFTEST_FAIL builtin-sports-suspension contacts=%d mask=0x%x all=%d\n",
                    contacts,(unsigned)g_vc_wheel_contact_mask,all_hit);
                return 8;
            }
            fprintf(stderr,
                "RACER_SELFTEST_BUILTIN_SPORTS_SUSPENSION_OK contacts=%d mask=0x%x\n",
                contacts,(unsigned)g_vc_wheel_contact_mask);
        }

        /*
         * Regression for reVC-style tyre control itself.  This intentionally
         * bypasses the old bicycle controller: rear-drive thrust must enter
         * through RL/RR only, and front steering at speed must create yaw by
         * contact-point tyre forces.
         */
        {
            vc_handling_lite_t saved_h=g_vehicle_handling;
            vc_wheel_contact_t saved_c[4];
            uint8_t saved_mask=g_vc_wheel_contact_mask;
            float saved_long=g_vehicle_vlong,saved_lat=g_vehicle_vlat,saved_vy=g_vehicle_vy;
            float saved_wx=g_world_x,saved_wy=g_world_y,saved_wz=g_world_z;
            float saved_yaw=g_vehicle_yaw_rate;
            float saved_pv=g_body_pitch_vel,saved_rv=g_body_roll_vel;
            float saved_pitch=g_body_pitch,saved_roll=g_body_roll;
            float saved_heading=g_vehicle_heading;
            v3f_t saved_turn=g_vc_turn_world;
            v3f_t saved_right=g_vc_body_right,saved_up=g_vc_body_up,saved_forward=g_vc_body_forward;
            int saved_basis_valid=g_vc_body_basis_valid;
            float saved_sa=g_steer_angle;
            float saved_sfl=g_steer_fl,saved_sfr=g_steer_fr;
            uint8_t saved_gear=g_vc_current_gear;
            float saved_timer[4];
            float rear_thrust,yaw_after;
            memcpy(saved_timer,g_vc_wheel_timer,sizeof(saved_timer));
            int wi;
            memcpy(saved_c,g_vc_wheel_contact,sizeof(saved_c));

            g_vehicle_handling.drive_type='R';
            g_vehicle_handling.traction_mult=1.0f;
            g_vehicle_handling.traction_loss=0.80f;
            g_vehicle_handling.traction_bias=0.50f;
            g_vehicle_handling.brake_bias=0.50f;
            g_vehicle_handling.engine_accel=0.20f;
            g_vehicle_handling.max_forward=150.0f;
            g_vehicle_handling.max_reverse=40.0f;
            g_vehicle_handling.rolling_drag=0.0f;
            g_vehicle_handling.suspension_antidive=0.0f;
            g_vehicle_handling.flags=0;
            g_vc_current_gear=1;
            g_world_x=0.0f;g_world_y=0.0f;g_world_z=0.0f;
            g_vehicle_vlong=20.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
            g_vehicle_heading=0.0f;
            g_body_pitch=0.0f;g_body_roll=0.0f;
            vc_reset_turn_world();g_vc_body_basis_valid=0;
            vc_body_basis_from_euler();
            g_steer_angle=0.0f;g_steer_fl=0.0f;g_steer_fr=0.0f;
            g_vc_wheel_contact_mask=0x0fU;
            for(wi=0;wi<4;++wi){
                float sx=(wi==0||wi==2)?-130.0f:130.0f;
                float sz=(wi<2)?250.0f:-250.0f;
                g_vc_wheel_contact[wi].hit=1;
                g_vc_wheel_contact[wi].ratio=0.75f;
                g_vc_wheel_contact[wi].point=(v3f_t){sx,0.0f,sz};
                g_vc_wheel_contact[wi].normal=(v3f_t){0.0f,1.0f,0.0f};
                g_vc_wheel_contact[wi].spring_dir=(v3f_t){0.0f,-1.0f,0.0f};
                g_vc_wheel_contact[wi].surface=1;
                g_vc_wheel_state[wi]=VC_WHEEL_NORMAL;
                g_vc_wheel_timer[wi]=4.0f;
            }
            vc_apply_revc_wheel_forces(1.0f,0.0f,0.0f);
            rear_thrust=fabsf(g_vc_wheel_force_fwd[2])+fabsf(g_vc_wheel_force_fwd[3]);
            if(rear_thrust<=1.0e-4f ||
               fabsf(g_vc_wheel_force_fwd[0])>1.0e-4f ||
               fabsf(g_vc_wheel_force_fwd[1])>1.0e-4f ||
               fabsf(g_vehicle_yaw_rate)>0.005f ||
               fabsf(g_body_roll_vel)>0.005f){
                fprintf(stderr,
                    "RACER_SELFTEST_FAIL revc-wheel-drive rear=%.5f front=%.5f/%.5f yaw=%.6f roll=%.6f\n",
                    rear_thrust,g_vc_wheel_force_fwd[0],g_vc_wheel_force_fwd[1],
                    g_vehicle_yaw_rate,g_body_roll_vel);
                return 9;
            }

            g_vehicle_vlong=40.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
            vc_reset_turn_world();
            g_steer_angle=0.20f;g_steer_fl=0.20f;g_steer_fr=0.20f;
            for(wi=0;wi<4;++wi)g_vc_wheel_state[wi]=VC_WHEEL_NORMAL;
            vc_apply_revc_wheel_forces(0.0f,0.0f,0.0f);
            yaw_after=g_vehicle_yaw_rate;
            if(fabsf(yaw_after)<1.0e-5f){
                fprintf(stderr,
                    "RACER_SELFTEST_FAIL revc-wheel-steer yaw=%.7f side=%.4f/%.4f\n",
                    yaw_after,g_vc_wheel_force_side[0],g_vc_wheel_force_side[1]);
                return 10;
            }
            fprintf(stderr,
                "RACER_SELFTEST_REVC_WHEELS_OK rearThrust=%.4f steerYaw=%.6f states=%u%u%u%u\n",
                rear_thrust,yaw_after,
                (unsigned)g_vc_wheel_state[0],(unsigned)g_vc_wheel_state[1],
                (unsigned)g_vc_wheel_state[2],(unsigned)g_vc_wheel_state[3]);

            {
                float save_raw=g_vc_raw_steer_input;
                float save_slip=g_vehicle_slip;
                float rear,front,straight;
                g_vc_raw_steer_input=1.0f;
                g_vehicle_slip=0.25f;
                rear=vc_player_lateral_grip_scale(2);
                front=vc_player_lateral_grip_scale(0);
                g_vehicle_slip=0.0f;
                straight=vc_player_lateral_grip_scale(2);
                g_vc_raw_steer_input=save_raw;
                g_vehicle_slip=save_slip;
                if(!(rear>=0.69f&&rear<=0.71f) ||
                   !(front>=0.91f&&front<=0.93f) ||
                   fabsf(straight-1.0f)>1.0e-6f ||
                   !(rear<front)){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL player-slip rear=%.3f front=%.3f straight=%.3f\n",
                        rear,front,straight);
                    return 21;
                }
                fprintf(stderr,
                    "RACER_SELFTEST_PLAYER_SLIP_OK rear=%.3f front=%.3f straight=%.3f balanceStart=%u\n",
                    rear,front,straight,(unsigned)VC_PLAYER_BALANCE_START_TICKS);
            }

            {
                float oceanic=vc_player_stability_com_y(96.0f,240.0f);
                float zero=vc_player_stability_com_y(0.0f,240.0f);
                float low=vc_player_stability_com_y(-24.0f,240.0f);
                if(fabsf(oceanic-24.0f)>0.01f ||
                   fabsf(zero)>0.01f || fabsf(low+24.0f)>0.01f){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL player-com oceanic=%.2f zero=%.2f low=%.2f\n",
                        oceanic,zero,low);
                    return 22;
                }
                fprintf(stderr,
                    "RACER_SELFTEST_PLAYER_COM_OK source=96.00 effective=24.00 cap=0.10m\n");
            }

            {
                float base=24.0f;
                float raised=vc_player_balance_com_y(base,144.4f);
                float lowered=vc_player_balance_com_y(base,-42.9f);
                if(fabsf(raised-24.0f)>0.01f ||
                   fabsf(lowered+42.9f)>0.01f){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL balance-com raised=%.2f lowered=%.2f\n",
                        raised,lowered);
                    return 23;
                }
                fprintf(stderr,
                    "RACER_SELFTEST_BALANCE_COM_OK base=24.00 raised=24.00 lowered=-42.90\n");
            }

            {
                float save_mass=g_vehicle_handling.mass;
                float save_turn_mass=g_vehicle_handling.turn_mass_world;
                float eff;
                g_vehicle_handling.mass=1000.0f;
                g_vehicle_handling.turn_mass_world=200000000.0f;
                g_world_x=0.0f;g_world_y=0.0f;g_world_z=0.0f;
                eff=vc_revc_effective_turn_mass(
                    (v3f_t){0.0f,0.0f,500.0f},
                    (v3f_t){1.0f,0.0f,0.0f});
                g_vehicle_handling.mass=save_mass;
                g_vehicle_handling.turn_mass_world=save_turn_mass;
                if(!(eff>300.0f && eff<700.0f)){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL revc-effective-mass eff=%.3f\n",eff);
                    return 20;
                }
                fprintf(stderr,
                    "RACER_SELFTEST_REVC_EFFECTIVE_MASS_OK eff=%.3f full=1000.000\n",
                    eff);
            }

            {
                float latched_thrust;
                g_vehicle_vlong=20.0f;g_vehicle_vlat=0.0f;g_vehicle_vy=0.0f;
                vc_reset_turn_world();
                g_steer_angle=0.0f;g_steer_fl=0.0f;g_steer_fr=0.0f;
                g_vc_wheel_contact_mask=0x00U;
                g_vc_wheel_latched_mask=0x0fU;
                for(wi=0;wi<4;++wi){
                    g_vc_wheel_timer[wi]=3.0f;
                    g_vc_wheel_contact[wi].hit=1;
                    g_vc_wheel_contact[wi].ratio=1.0f;
                    g_vc_wheel_state[wi]=VC_WHEEL_NORMAL;
                }
                vc_apply_revc_wheel_forces(1.0f,0.0f,0.0f);
                latched_thrust=
                    fabsf(g_vc_wheel_force_fwd[2])+
                    fabsf(g_vc_wheel_force_fwd[3]);
                if(latched_thrust<=1.0e-4f){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL wheel-timer-continuity rear=%.6f\n",
                        latched_thrust);
                    return 12;
                }
                fprintf(stderr,
                    "RACER_SELFTEST_WHEEL_TIMER_OK mask=0x0 latched=0xf rearThrust=%.4f\n",
                    latched_thrust);
            }

            {
                vc_body_contact_t floor_col={0},wall_col={0},deep_col={0};
                uint8_t save_mask=g_vc_wheel_contact_mask;
                float save_bp=g_body_pitch,save_br=g_body_roll;
                float test_scale=vc_runtime_world_scale();
                float test_wr=active_vehicle_wheel_radius();
                floor_col.hit=1;floor_col.ny=1.0f;floor_col.depth=0.04f*test_scale;
                wall_col.hit=1;wall_col.nx=1.0f;wall_col.depth=0.04f*test_scale;
                deep_col=floor_col;deep_col.depth=fmaxf(test_wr,0.30f*test_scale);
                g_vc_wheel_contact_mask=0x0fU;
                for(wi=0;wi<4;++wi)g_vc_wheel_timer[wi]=4.0f;
                g_body_pitch=0.0f;g_body_roll=0.0f;
                if(!vc_body_contact_is_suspension_floor(
                        &floor_col,-0.01f*test_scale,test_wr,test_scale) ||
                   vc_body_contact_is_suspension_floor(
                        &wall_col,-0.01f*test_scale,test_wr,test_scale) ||
                   vc_body_contact_is_suspension_floor(
                        &deep_col,-0.01f*test_scale,test_wr,test_scale) ||
                   vc_body_contact_is_suspension_floor(
                        &floor_col,-0.20f*test_scale,test_wr,test_scale)){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL suspension-body-floor arbitration\n");
                    return 11;
                }
                g_vc_wheel_contact_mask=save_mask;
                g_body_pitch=save_bp;g_body_roll=save_br;
                fprintf(stderr,
                    "RACER_SELFTEST_SUSPENSION_BODY_FLOOR_OK shallow=deferred wall=active deep=active hard=active\n");
            }

            {
                vc_handling_lite_t torque_saved_h=g_vehicle_handling;
                float tx0=g_world_x,ty0=g_world_y,tz0=g_world_z;
                float bp0=g_body_pitch,br0=g_body_roll;
                float bpv0=g_body_pitch_vel,brv0=g_body_roll_vel;
                float yawv0=g_vehicle_yaw_rate;
                float tvx=0.0f,tvy=0.0f,tvz=0.0f;
                float left_roll,front_pitch;

                g_vehicle_handling.mass=1000.0f;
                g_vehicle_handling.turn_mass_world=1000.0f*200000.0f;
                g_vehicle_handling.centre_of_mass=(v3f_t){0.0f,0.0f,0.0f};
                g_world_x=0.0f;g_world_y=0.0f;g_world_z=0.0f;
                g_body_pitch=0.0f;g_body_roll=0.0f;
                vc_reset_turn_world();g_vc_body_basis_valid=0;g_vehicle_yaw_rate=0.0f;

                vc_apply_world_dv_at_point(
                    (v3f_t){0.0f,1.0f,0.0f},
                    (v3f_t){-100.0f,0.0f,0.0f},
                    0.0f,&tvx,&tvy,&tvz);
                left_roll=g_body_roll_vel;

                tvx=tvy=tvz=0.0f;
                vc_reset_turn_world();
                vc_apply_world_dv_at_point(
                    (v3f_t){0.0f,1.0f,0.0f},
                    (v3f_t){0.0f,0.0f,100.0f},
                    0.0f,&tvx,&tvy,&tvz);
                front_pitch=g_body_pitch_vel;

                if(!(left_roll<0.0f) || !(front_pitch<0.0f)){
                    fprintf(stderr,
                        "RACER_SELFTEST_FAIL body-torque-sign leftRoll=%.7f frontPitch=%.7f\n",
                        left_roll,front_pitch);
                    return 13;
                }
                fprintf(stderr,
                    "RACER_SELFTEST_BODY_TORQUE_OK leftRoll=%.7f frontPitch=%.7f\n",
                    left_roll,front_pitch);

                {
                    /*
                     * Pure left/right symmetry about COM. Keep Z=0 so the two
                     * upward impulses must cancel ALL torque; a shared positive
                     * Z would intentionally create a pitch impulse and would
                     * make this a bad symmetry fixture.
                     */
                    v3f_t lp={-100.0f,0.0f,0.0f},rp={100.0f,0.0f,0.0f};
                    v3f_t lw,rw;
                    float mag;
                    g_vehicle_heading=1.10f;
                    g_body_pitch=0.0f;g_body_roll=0.0f;
                    g_vc_body_basis_valid=0;
                    vc_body_basis_from_euler();
                    {
                        float ex=sinf(1.10f),ez=cosf(1.10f);
                        float ferr=fabsf(g_vc_body_forward.x-ex)+
                                   fabsf(g_vc_body_forward.z-ez)+
                                   fabsf(g_vc_body_forward.y);
                        if(ferr>1.0e-5f){
                            fprintf(stderr,
                                "RACER_SELFTEST_FAIL heading-basis fwd=%.7f/%.7f/%.7f expected=%.7f/0/%.7f err=%.8f\n",
                                g_vc_body_forward.x,g_vc_body_forward.y,g_vc_body_forward.z,
                                ex,ez,ferr);
                            return 16;
                        }
                        fprintf(stderr,
                            "RACER_SELFTEST_HEADING_BASIS_OK heading=1.10 fwd=%.6f/%.6f/%.6f\n",
                            g_vc_body_forward.x,g_vc_body_forward.y,g_vc_body_forward.z);
                    }
                    vc_reset_turn_world();
                    vc_body_rotate_local(lp,&lw);
                    vc_body_rotate_local(rp,&rw);
                    vc_apply_world_dv_at_point(
                        (v3f_t){0.0f,1.0f,0.0f},
                        lw,1.10f,&tvx,&tvy,&tvz);
                    vc_apply_world_dv_at_point(
                        (v3f_t){0.0f,1.0f,0.0f},
                        rw,1.10f,&tvx,&tvy,&tvz);
                    mag=sqrtf(
                        g_vc_turn_world.x*g_vc_turn_world.x+
                        g_vc_turn_world.y*g_vc_turn_world.y+
                        g_vc_turn_world.z*g_vc_turn_world.z);
                    if(mag>1.0e-5f){
                        fprintf(stderr,
                            "RACER_SELFTEST_FAIL world-turn-symmetry mag=%.8f omega=%.8f/%.8f/%.8f\n",
                            mag,g_vc_turn_world.x,g_vc_turn_world.y,g_vc_turn_world.z);
                        return 14;
                    }
                    fprintf(stderr,
                        "RACER_SELFTEST_WORLD_TURN_OK heading=1.10 symmetricMag=%.8f\n",
                        mag);
                }

                {
                    v3f_t save_turn2=g_vc_turn_world;
                    v3f_t save_r2=g_vc_body_right,save_u2=g_vc_body_up,save_f2=g_vc_body_forward;
                    int save_valid2=g_vc_body_basis_valid;
                    float save_h2=g_vehicle_heading,save_p2=g_body_pitch,save_ro2=g_body_roll;
                    float lr,lu,lf,ortho;
                    int step;

                    g_vehicle_heading=1.0f;
                    g_body_pitch=0.15f;
                    g_body_roll=-0.10f;
                    g_vc_body_basis_valid=0;
                    vc_body_basis_from_euler();
                    g_vc_turn_world=(v3f_t){0.003f,-0.004f,0.002f};

                    for(step=0;step<200;++step)
                        vc_integrate_turn_world();

                    lr=sqrtf(vc_v3_dot(g_vc_body_right,g_vc_body_right));
                    lu=sqrtf(vc_v3_dot(g_vc_body_up,g_vc_body_up));
                    lf=sqrtf(vc_v3_dot(g_vc_body_forward,g_vc_body_forward));
                    ortho=
                        fabsf(vc_v3_dot(g_vc_body_right,g_vc_body_up))+
                        fabsf(vc_v3_dot(g_vc_body_right,g_vc_body_forward))+
                        fabsf(vc_v3_dot(g_vc_body_up,g_vc_body_forward));
                    if(!isfinite(g_vehicle_heading)||
                       !isfinite(g_body_pitch)||!isfinite(g_body_roll)||
                       fabsf(lr-1.0f)>1.0e-4f||
                       fabsf(lu-1.0f)>1.0e-4f||
                       fabsf(lf-1.0f)>1.0e-4f||
                       ortho>2.0e-4f){
                        fprintf(stderr,
                            "RACER_SELFTEST_FAIL body-basis len=%.6f/%.6f/%.6f ortho=%.8f euler=%.6f/%.6f/%.6f\n",
                            lr,lu,lf,ortho,
                            g_vehicle_heading,g_body_pitch,g_body_roll);
                        return 15;
                    }
                    fprintf(stderr,
                        "RACER_SELFTEST_BODY_BASIS_OK len=%.6f/%.6f/%.6f ortho=%.8f\n",
                        lr,lu,lf,ortho);

                    g_vc_turn_world=save_turn2;
                    g_vc_body_right=save_r2;g_vc_body_up=save_u2;g_vc_body_forward=save_f2;
                    g_vc_body_basis_valid=save_valid2;
                    g_vehicle_heading=save_h2;g_body_pitch=save_p2;g_body_roll=save_ro2;
                }

                g_vehicle_handling=torque_saved_h;
                g_world_x=tx0;g_world_y=ty0;g_world_z=tz0;
                g_body_pitch=bp0;g_body_roll=br0;
                g_body_pitch_vel=bpv0;g_body_roll_vel=brv0;
                g_vehicle_yaw_rate=yawv0;
            }

            g_vehicle_handling=saved_h;
            g_world_x=saved_wx;g_world_y=saved_wy;g_world_z=saved_wz;
            g_vehicle_heading=saved_heading;
            g_vc_turn_world=saved_turn;
            g_vc_body_right=saved_right;g_vc_body_up=saved_up;g_vc_body_forward=saved_forward;
            g_vc_body_basis_valid=saved_basis_valid;
            memcpy(g_vc_wheel_contact,saved_c,sizeof(saved_c));
            g_vc_wheel_contact_mask=saved_mask;
            g_vehicle_vlong=saved_long;g_vehicle_vlat=saved_lat;g_vehicle_vy=saved_vy;
            g_vehicle_yaw_rate=saved_yaw;
            g_body_pitch_vel=saved_pv;g_body_roll_vel=saved_rv;
            g_body_pitch=saved_pitch;g_body_roll=saved_roll;
            g_steer_angle=saved_sa;
            g_steer_fl=saved_sfl;g_steer_fr=saved_sfr;
            g_vc_current_gear=saved_gear;
            memcpy(g_vc_wheel_timer,saved_timer,sizeof(saved_timer));
            g_vc_wheel_latched_mask=0;
        }
    }

    /*
     * Player vehicle pose must be camera-independent. The renderer now consumes
     * the physical body basis in world space; changing chase-camera heading may
     * change projection, never the world-space body point itself.
     */
    {
        v3f_t save_r=g_vc_body_right,save_u=g_vc_body_up,save_f=g_vc_body_forward;
        int save_valid=g_vc_body_basis_valid;
        float save_h=g_vehicle_heading,save_p=g_body_pitch,save_ro=g_body_roll;
        float save_cam=g_camera_heading;
        v3f_t a,b,local={123.0f,47.0f,281.0f};
        g_vehicle_heading=0.73f;
        g_body_pitch=0.18f;
        g_body_roll=-0.31f;
        g_vc_body_basis_valid=0;
        vc_body_basis_from_euler();
        g_camera_heading=-0.20f;
        vc_vehicle_local_to_world(local,1000.0f,2000.0f,-3000.0f,&a);
        g_camera_heading=wrap_angle(g_camera_heading+3.14159265f);
        vc_vehicle_local_to_world(local,1000.0f,2000.0f,-3000.0f,&b);
        if(fabsf(a.x-b.x)>1.0e-5f||
           fabsf(a.y-b.y)>1.0e-5f||
           fabsf(a.z-b.z)>1.0e-5f){
            fprintf(stderr,
                "RACER_SELFTEST_FAIL vehicle-world-camera-invariant a=%.6f/%.6f/%.6f b=%.6f/%.6f/%.6f\n",
                a.x,a.y,a.z,b.x,b.y,b.z);
            return 17;
        }
        fprintf(stderr,
            "RACER_SELFTEST_VEHICLE_WORLD_POSE_OK world=%.3f/%.3f/%.3f\n",
            a.x,a.y,a.z);
        g_vc_body_right=save_r;g_vc_body_up=save_u;g_vc_body_forward=save_f;
        g_vc_body_basis_valid=save_valid;
        g_vehicle_heading=save_h;g_body_pitch=save_p;g_body_roll=save_ro;
        g_camera_heading=save_cam;
    }

    /*
     * The player vehicle must share the Vice City Z buffer. A farther car pixel
     * cannot overwrite a closer building/road depth; a nearer pixel must pass.
     */
    {
        static const uint16_t tex[1]={0xffffU};
        const int px=12,py=12;
        const size_t at=(size_t)py*RW+px;
        const uint16_t sentinel=0x8123U;
        uint16_t city_depth=(uint16_t)(2949075.0f/1000.0f);
        uint16_t before;
        g_canvas[at]=sentinel;
        g_city_zbuf[at]=city_depth;
        g_vcveh_zpass_pixels=0;g_vcveh_zblocked_pixels=0;
        fill_tri_textured_z(
            10,10,0.0f,0.0f,1500.0f,
            22,10,0.0f,0.0f,1500.0f,
            10,22,0.0f,0.0f,1500.0f,
            1.0f,tex,1,1);
        if(g_canvas[at]!=sentinel || g_city_zbuf[at]!=city_depth){
            fprintf(stderr,
                "RACER_SELFTEST_FAIL vehicle-z far color=0x%04x z=%u city=%u\n",
                (unsigned)g_canvas[at],(unsigned)g_city_zbuf[at],(unsigned)city_depth);
            return 18;
        }
        before=g_city_zbuf[at];
        fill_tri_textured_z(
            10,10,0.0f,0.0f,500.0f,
            22,10,0.0f,0.0f,500.0f,
            10,22,0.0f,0.0f,500.0f,
            1.0f,tex,1,1);
        if(g_canvas[at]==sentinel || g_city_zbuf[at]<=before){
            fprintf(stderr,
                "RACER_SELFTEST_FAIL vehicle-z near color=0x%04x z=%u before=%u\n",
                (unsigned)g_canvas[at],(unsigned)g_city_zbuf[at],(unsigned)before);
            return 19;
        }
        fprintf(stderr,
            "RACER_SELFTEST_VEHICLE_Z_OCCLUSION_OK blocked=%u pass=%u\n",
            g_vcveh_zblocked_pixels,g_vcveh_zpass_pixels);
    }

    {
        /*
         * VCM3 GTA->Racer axis conversion swaps Y/Z and reverses winding.
         * For a wall in front of the camera the converted exterior/front
         * triangle therefore has +Z winding and must remain visible, while the
         * same triangle with reversed indices is the interior/back face.
         */
        v3f_t a={-1.0f,-1.0f,10.0f};
        v3f_t b={1.0f,-1.0f,10.0f};
        v3f_t c={0.0f,1.0f,10.0f}; /* +Z normal: converted GTA front */
        if(vc_triangle_backfacing(a,b,c) ||
           !vc_triangle_backfacing(a,c,b)){
            fprintf(stderr,
                "RACER_SELFTEST_FAIL vc-backface gta-handedness\n");
            return 21;
        }
        fprintf(stderr,
            "RACER_SELFTEST_VC_BACKFACE_OK mode=cullback winding=gta-zup-to-racer-yup\n");
    }

    {
        float hfov=2.0f*atanf(((float)RW*0.5f)/VC_FOCAL)*57.2957795f;
        if(fabsf(hfov-86.067f)>0.15f){
            fprintf(stderr,
                "RACER_SELFTEST_FAIL vc-fov focal=%.3f hfov=%.3f\n",
                (double)VC_FOCAL,(double)hfov);
            return 20;
        }
        fprintf(stderr,
            "RACER_SELFTEST_VC_FOV_OK base4x3=70.000 focal=%.3f hfov16x9=%.3f aspect=%.3f\n",
            (double)VC_FOCAL,(double)hfov,(double)RW/(double)RH);
    }

    /*
     * A triangle crossing the near plane and a side plane used to project to
     * enormous coordinates and overflow 32-bit edge math. Full frustum
     * clipping must keep every projected point within the small clip margin.
     */
    {
        vc_clip_v_t in[3],poly[12];
        sv3_t sp[12];
        int pc,j;
        in[0]=(vc_clip_v_t){{0.0f,0.0f,80.0f},0.0f,0.0f};
        in[1]=(vc_clip_v_t){{50000.0f,0.0f,30.0f},1.0f,0.0f};
        in[2]=(vc_clip_v_t){{0.0f,50000.0f,55.0f},0.0f,1.0f};
        pc=vc_clip_frustum_textured(in,poly);
        if(pc<3){
            fprintf(stderr,"RACER_SELFTEST_FAIL frustum pc=%d\n",pc);
            return 8;
        }
        for(j=0;j<pc;++j){
            city_project_camera(&poly[j].p,&sp[j]);
            if(sp[j].z<44.99f ||
               sp[j].sx<-9.5f || sp[j].sx>(float)RW+8.5f ||
               sp[j].sy<-9.5f || sp[j].sy>(float)RH+8.5f){
                fprintf(stderr,
                    "RACER_SELFTEST_FAIL frustum p=%d sx=%.2f sy=%.2f z=%.2f\n",
                    j,sp[j].sx,sp[j].sy,sp[j].z);
                return 9;
            }
        }
        fprintf(stderr,"RACER_SELFTEST_FRUSTUM_OK vertices=%d\n",pc);
    }

    if(!vc_selftest_scanline_spans()){
        fprintf(stderr,"RACER_SELFTEST_FAIL scanline-span coverage mismatch\n");
        return 24;
    }
    fprintf(stderr,"RACER_SELFTEST_SCANLINE_SPAN_OK exact-halfspace\n");

    if(g_vc_city_mode){
        vc_raster_stats_t rs;
        if(!vc_raster_worker_start()){
            fprintf(stderr,"RACER_SELFTEST_FAIL raster worker start\n");
            return 5;
        }
        vc_raster_worker_submit(0,RH/2);
        rs=vc_raster_worker_collect(NULL);
        vc_raster_worker_submit(0,RH/2);
        rs=vc_raster_worker_collect(NULL);
        vc_raster_worker_stop();
        fprintf(stderr,
            "RACER_SELFTEST_DUALRASTER_OK pending=%d stats=%llu/%llu/%llu\n",
            g_vc_raster_worker.pending,
            (unsigned long long)rs.zpass_pixels,
            (unsigned long long)rs.texture_samples,
            (unsigned long long)rs.correction_segments);
    }

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
    init_vc_color_chan_lut();
    {
        const char *m=getenv("RACER_VC_BACKFACE");
        if(m&&(!strcmp(m,"0")||!strcmp(m,"off")||!strcmp(m,"none")))
            g_vc_backface_cull=0;
        fprintf(stderr,"[racer] VC world backface cull=%s winding=gta-zup-to-racer-yup (RACER_VC_BACKFACE=off disables)\n",
                g_vc_backface_cull?"back":"none");
    }
    build_level();
    try_load_vc_map();
    try_load_vc_collision();
    if(try_load_vc_world()<0){
        fprintf(stderr,
            "[racer] FATAL: VFW world exists but is incomplete/corrupt; "
            "rebuild and redeploy full world\n");
        return 12;
    }
    try_load_vc_vehicle();
    /*
     * Handling is a separate GTA data layer.  Load it even when VCVEH exists:
     * VCVEH supplies DFF/TXD/native vehicle COL, VCHAND supplies the complete
     * handling.cfg row (drive type, gears, ABS, biases, suspension parameters).
     */
    try_load_vc_handling();
    try_load_vc_surface();
    vc_relocate_to_safe_spawn();
    reset_chase_camera();
    prefault_runtime_assets();

    if(video_open(&v)<0){video_close(&v);return 10;}
    probe_tde_backend(&v);
    input_open(&in);
    racer_control_open();
    pin_thread(0,"renderer");
    if(video_start(&v)<0){input_close(&in);video_close(&v);return 11;}
    if(g_vc_city_mode)vc_raster_worker_start();

    {
        uint64_t last_sim=mono_ns();
        uint64_t accumulator=0;
        uint64_t next_frame=last_sim+FRAME_NS;
        perf=last_sim;
        memset(&g_prof,0,sizeof(g_prof));
        last_presented=v.presented;

        fprintf(stderr,"[racer] fixed simulation/present target=60Hz %s free-drive reverse player=%s%s\n",
            g_vc_city_mode?(g_vc_world_mode?"vfw1-paged":"vcmap3-textured"):"osm-terrain-city",
            g_vc_vehicle.loaded?"vcveh-imported":"built-in-rally-sports",
            g_vc_city_mode?(g_vc_collision.version==2?
                " col=VCC2-gta-native debug-toggle=T(flat),Y(affine) hfov=86.07(base70@4:3 HOR+) gta-stream=object-lod-v4 fog=48..112m far=112m":
                " col=VCC1-legacy debug-toggle=T(flat),Y(affine) hfov=86.07(base70@4:3 HOR+) fog=48..112m far=112m"):"");

        while(!g_stop){
            uint64_t now=mono_ns();
            uint64_t elapsed=now-last_sim;
            int sim_steps=0;

            if(elapsed>FRAME_NS*MAX_SIM_CATCHUP)elapsed=FRAME_NS*MAX_SIM_CATCHUP;
            last_sim=now;
            accumulator+=elapsed;

            input_poll(&in);
            racer_control_poll();
            if(in.camera_cycle_pressed){
                camera_cycle_zoom();
                in.camera_cycle_pressed=0;
            }
            in.camera_view_toggle_pressed=0;
            g_camera_look_behind=in.camera_look_behind;
            g_camera_side_left=in.camera_side_left;
            g_camera_side_right=in.camera_side_right;
            g_camera_orbit_input_x=in.camera_orbit_x;
            g_camera_orbit_input_y=in.camera_orbit_y;

            while(accumulator>=FRAME_NS && sim_steps<MAX_SIM_CATCHUP){
                game_update(&in);
                accumulator-=FRAME_NS;
                sim_steps++;
                sim_ticks_window++;
            }
            if(g_vc_world_mode)vc_world_stream_update(0);

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
            if(frames>=120){
                double sec=(double)(now-perf)/1000000000.0;
                double render_fps=sec>0.0?(double)frames/sec:0.0;
                unsigned presented_now;
                unsigned presented_delta;
                uint64_t present_total,present_max;
                uint64_t tde_copy_total,tde_job_total,tde_job_max;
                unsigned present_count,tde_count;
                double inv=(g_prof.frames>0)?1.0/(double)g_prof.frames:0.0;

                pthread_mutex_lock(&v.lock);
                presented_now=v.presented;
                present_total=v.present_ns_total;
                present_max=v.present_ns_max;
                present_count=v.present_profile_count;
                tde_copy_total=v.tde_copy_ns_total;
                tde_job_total=v.tde_job_ns_total;
                tde_job_max=v.tde_job_ns_max;
                tde_count=v.tde_profile_count;
                v.present_ns_total=0;
                v.present_ns_max=0;
                v.present_profile_count=0;
                v.tde_copy_ns_total=0;
                v.tde_job_ns_total=0;
                v.tde_job_ns_max=0;
                v.tde_profile_count=0;
                pthread_mutex_unlock(&v.lock);

                presented_delta=presented_now-last_presented;

                fprintf(stderr,
                    "[racer] PERF stage8.9 render_fps=%.2f sim_hz=%.2f presented_fps=%.2f speed=%.1f vlong=%.2f vlat=%.2f yawrate=%.4f body=%.3f/%.3f bodyv=%.5f/%.5f bodyup=%.3f/%.3f/%.3f turnw=%.5f/%.5f/%.5f twheel=%u comY=%.1f rawsteer=%.3f basiserr=%.6f world=%.0f,%.0f,%.0f sector=%d,%d input=%d gas=%d brake=%d colblk=%u colv=%u vcfb=%u wcontact=0x%x wexact=0x%x wrescue=0x%x wlatched=0x%x floorsup=%u spring=%.2f/%.2f/%.2f/%.2f wny=%.2f/%.2f/%.2f/%.2f surf=%u/%u/%u/%u deckrej=%u bodySurf=%u colDepth=%.1f colN=%.2f/%.2f/%.2f colVn=%.2f cartris=%u tiny=%u screenrej=%u carz=%u/%u rack=%.3f ack=%.3f/%.3f heading=%.3f cam=%.3f arm=%.3f camdist=%.0f targetdist=%.0f camh=%.0f slip=%.3f wheel=%.3f vcq=%d vcsec=%d vccap=%d vcaff=%u vcclip=%u/%u/%u vccull=%u vcfog=%u vctiny=%u vclod=%u/%u vcobj=%u/%u/+%u objlod=%u/%u/%u/s%u objfr=%u vehicle=%s vcmode=%s\n",
                    render_fps,
                    sec>0.0?(double)sim_ticks_window/sec:0.0,
                    sec>0.0?(double)presented_delta/sec:0.0,
                    g_speed,g_vehicle_vlong,g_vehicle_vlat,g_vehicle_yaw_rate,
                    g_body_pitch,g_body_roll,g_body_pitch_vel,g_body_roll_vel,
                    g_vc_body_up.x,g_vc_body_up.y,g_vc_body_up.z,
                    g_vc_turn_world.x,g_vc_turn_world.y,g_vc_turn_world.z,
                    g_vc_two_wheel_ticks,g_vc_effective_com_y,g_vc_raw_steer_input,
                    fabsf(vc_v3_dot(g_vc_body_right,g_vc_body_up))+
                    fabsf(vc_v3_dot(g_vc_body_right,g_vc_body_forward))+
                    fabsf(vc_v3_dot(g_vc_body_up,g_vc_body_forward)),
                    g_world_x,g_world_y,g_world_z,
                    (int)floorf(g_world_x/OSM_CITY_SECTOR_WORLD),
                    (int)floorf(g_world_z/OSM_CITY_SECTOR_WORLD),
                    in.steer,in.gas,in.brake,g_vc_collision_blocks_window,
                    g_vc_collision.version,g_vc_visual_ground_fallback_window,
                    (unsigned)g_vc_wheel_contact_mask,
                    (unsigned)g_vc_wheel_exact_mask,
                    (unsigned)g_vc_wheel_rescue_mask,
                    (unsigned)g_vc_wheel_latched_mask,
                    g_vc_body_floor_suppressed_window,
                    g_vc_wheel_contact[0].hit?g_vc_wheel_contact[0].ratio:-1.0f,
                    g_vc_wheel_contact[1].hit?g_vc_wheel_contact[1].ratio:-1.0f,
                    g_vc_wheel_contact[2].hit?g_vc_wheel_contact[2].ratio:-1.0f,
                    g_vc_wheel_contact[3].hit?g_vc_wheel_contact[3].ratio:-1.0f,
                    g_vc_wheel_contact[0].hit?g_vc_wheel_contact[0].normal.y:0.0f,
                    g_vc_wheel_contact[1].hit?g_vc_wheel_contact[1].normal.y:0.0f,
                    g_vc_wheel_contact[2].hit?g_vc_wheel_contact[2].normal.y:0.0f,
                    g_vc_wheel_contact[3].hit?g_vc_wheel_contact[3].normal.y:0.0f,
                    (unsigned)g_vc_wheel_surface[0],(unsigned)g_vc_wheel_surface[1],
                    (unsigned)g_vc_wheel_surface[2],(unsigned)g_vc_wheel_surface[3],
                    g_vc_deck_rejects_window,
                    (unsigned)g_vc_last_body_surface,
                    g_vc_last_col_depth,g_vc_last_col_nx,g_vc_last_col_ny,g_vc_last_col_nz,g_vc_last_col_vn,
                    g_vcveh_last_draw_tris,g_vcveh_last_tiny_reject,g_vcveh_last_screen_reject,
                    g_vcveh_zpass_pixels,g_vcveh_zblocked_pixels,
                    g_steer_angle,
                    g_steer_fl,g_steer_fr,g_vehicle_heading,g_camera_heading,g_camera_arm_heading,
                    g_camera_distance,g_camera_target_distance,g_camera_height,g_vehicle_slip,
                    g_wheel_spin,
                    g_vc_last_queued,g_vc_last_visible_sectors,g_vc_last_cap_hit,
                    g_vc_frame_affine_tris,
                    g_vc_frame_clip_fast,g_vc_frame_clip_partial,g_vc_frame_clip_reject,
                    g_vc_frame_backface_reject,
                    g_vc_frame_fogflat_tris,g_vc_frame_far_tiny_reject,
                    g_vc_frame_lod_reject,g_vc_frame_lod_fade,
                    g_vc_frame_objects_active,g_vc_frame_objects_fading,g_vc_frame_objects_started,
                    g_vc_frame_object_lod[0],g_vc_frame_object_lod[1],
                    g_vc_frame_object_lod[2],g_vc_frame_object_lod_switches,
                    g_vc_frame_object_frustum_reject,
                    g_vc_vehicle.loaded?"vcveh":"fallback",
                    g_vc_world_mode?
                        (g_vc_debug_flat?"vfw1-flat":(g_vc_debug_affine?"vfw1-affine":"vfw1-perspective")):
                        (g_vc_debug_flat?"flat":(g_vc_debug_affine?"affine":"perspective")));
                fprintf(stderr,
                    "[racer] VC_WHEELS gear=%u state=%u/%u/%u/%u "
                    "fwd=%.2f/%.2f/%.2f/%.2f side=%.2f/%.2f/%.2f/%.2f "
                    "adh=%.3f/%.3f/%.3f/%.3f force=%.3f,%.3f/%.3f,%.3f/%.3f,%.3f/%.3f,%.3f "
                    "turnMass=%.0f/%.0f/%.0f/%.0f\n",
                    (unsigned)g_vc_current_gear,
                    (unsigned)g_vc_wheel_state[0],(unsigned)g_vc_wheel_state[1],
                    (unsigned)g_vc_wheel_state[2],(unsigned)g_vc_wheel_state[3],
                    g_vc_wheel_fwd_speed[0],g_vc_wheel_fwd_speed[1],
                    g_vc_wheel_fwd_speed[2],g_vc_wheel_fwd_speed[3],
                    g_vc_wheel_side_speed[0],g_vc_wheel_side_speed[1],
                    g_vc_wheel_side_speed[2],g_vc_wheel_side_speed[3],
                    g_vc_wheel_adhesion[0],g_vc_wheel_adhesion[1],
                    g_vc_wheel_adhesion[2],g_vc_wheel_adhesion[3],
                    g_vc_wheel_force_fwd[0],g_vc_wheel_force_side[0],
                    g_vc_wheel_force_fwd[1],g_vc_wheel_force_side[1],
                    g_vc_wheel_force_fwd[2],g_vc_wheel_force_side[2],
                    g_vc_wheel_force_fwd[3],g_vc_wheel_force_side[3],
                    g_vc_wheel_turn_mass[0],g_vc_wheel_turn_mass[1],
                    g_vc_wheel_turn_mass[2],g_vc_wheel_turn_mass[3]);

                g_vc_collision_blocks_window=0;
                g_vc_body_floor_suppressed_window=0;
                g_vc_visual_ground_fallback_window=0;
                g_vc_deck_rejects_window=0;

                fprintf(stderr,
                    "[racer] PROFILE avg_ms total=%.2f sky=%.2f track=%.2f props=%.2f shadow=%.2f car=%.2f hud=%.2f acquire=%.2f submit=%.2f present=%.2f max_ms total=%.2f track=%.2f props=%.2f car=%.2f acquire=%.2f submit=%.2f present=%.2f tde=%s mmz=%s abi=%d stage=%.2f job=%.2f jobmax=%.2f tdefail=%u flushfail=%u dualrast=%s\n",
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
                    (double)present_max/1000000.0,
                    v.tde_ready?"on":"off",
                    (v.mmz_ready&&v.mmz_direct)?"direct":(v.mmz_ready?"fallback":"off"),
                    v.mmz_abi,
                    tde_count?(double)tde_copy_total/(double)tde_count/1000000.0:0.0,
                    tde_count?(double)tde_job_total/(double)tde_count/1000000.0:0.0,
                    (double)tde_job_max/1000000.0,
                    v.tde_failures,v.mmz_flush_failures,
                    g_vc_raster_worker.ready?"on":"off");

                if(g_vc_prof.frames){
                    double vinv=1.0/(double)g_vc_prof.frames;
                    fprintf(stderr,
                        "[racer] VC_PROFILE avg_ms scan=%.2f queue=%.2f zclear=%.2f raster=%.2f max_ms scan=%.2f queue=%.2f raster=%.2f avg_xform=%.0f avg_tested=%.0f zpass_px=%.0f tex_samples=%.0f corr_segments=%.0f bbox_px=%.0f span_px=%.0f split=%.0f top=%.2f bottom=%.2f\n",
                        (double)g_vc_prof.scan_ns*vinv/1000000.0,
                        (double)g_vc_prof.queue_ns*vinv/1000000.0,
                        (double)g_vc_prof.zclear_ns*vinv/1000000.0,
                        (double)g_vc_prof.raster_ns*vinv/1000000.0,
                        (double)g_vc_prof.max_scan_ns/1000000.0,
                        (double)g_vc_prof.max_queue_ns/1000000.0,
                        (double)g_vc_prof.max_raster_ns/1000000.0,
                        (double)g_vc_prof.xformed_vertices*vinv,
                        (double)g_vc_prof.tested_tris*vinv,
                        (double)g_vc_prof.zpass_pixels*vinv,
                        (double)g_vc_prof.texture_samples*vinv,
                        (double)g_vc_prof.correction_segments*vinv,
                        (double)g_vc_prof.bbox_pixels*vinv,
                        (double)g_vc_prof.span_pixels*vinv,
                        (double)g_vc_prof.split_rows*vinv,
                        (double)g_vc_prof.raster_top_ns*vinv/1000000.0,
                        (double)g_vc_prof.raster_bottom_ns*vinv/1000000.0);
                }

                if(g_prof.max_total_ns>70000000ULL)
                    fprintf(stderr,"[racer] HITCH render_max_ms=%.2f (textures are baked; this is render workload, not disk texture streaming)\n",
                            (double)g_prof.max_total_ns/1000000.0);

                memset(&g_prof,0,sizeof(g_prof));
                memset(&g_vc_prof,0,sizeof(g_vc_prof));
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

    vc_raster_worker_stop();
    video_stop(&v);
    fprintf(stderr,"[racer] exit frame=%u presented=%u\n",g_frame,v.presented);
    free_vc_world();
    free_vc_map();
    free_vc_collision();
    input_close(&in);video_close(&v);
    return 0;
}
