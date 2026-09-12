from pathlib import Path
import sys

p = Path(sys.argv[1] if len(sys.argv) > 1 else "src/video/dummy/SDL_nullvideo.c")
s = p.read_text()

inc='#include <linux/fb.h>\n'
assert s.count(inc)==1, s.count(inc)
s=s.replace(inc, inc+'#include <stdint.h>\n', 1)

marker='static void DUMMY_UpdateRects(_THIS, int numrects, SDL_Rect *rects);\n'
assert s.count(marker)==1, s.count(marker)
api=r'''static unsigned char *g_fbzx_dirty_rows = NULL;
static unsigned char *g_fbzx_dirty_base = NULL;
static unsigned long g_fbzx_dirty_bytes = 0;
static unsigned g_fbzx_dirty_pitch = 0;
static unsigned g_fbzx_dirty_height = 0;

void H3531_FBZX_MarkDirtyAddress(const void *address) {
    uintptr_t a=(uintptr_t)address;
    uintptr_t b=(uintptr_t)g_fbzx_dirty_base;
    unsigned long off;
    unsigned row;
    if(!g_fbzx_dirty_rows || !g_fbzx_dirty_base || !g_fbzx_dirty_pitch || !g_fbzx_dirty_height)
        return;
    if(a < b) return;
    off=(unsigned long)(a-b);
    if(off >= g_fbzx_dirty_bytes) return;
    row=(unsigned)(off/(unsigned long)g_fbzx_dirty_pitch);
    if(row < g_fbzx_dirty_height)
        g_fbzx_dirty_rows[row]=1;
}

'''
s=s.replace(marker, marker+'\n'+api, 1)

old='''    if(this->hidden->prev_buffer) {
        SDL_free(this->hidden->prev_buffer);
        this->hidden->prev_buffer=NULL;
    }
    this->hidden->prev_valid=0;
}'''
new='''    if(this->hidden->prev_buffer) {
        SDL_free(this->hidden->prev_buffer);
        this->hidden->prev_buffer=NULL;
    }
    if(this->hidden->row_changed) {
        SDL_free(this->hidden->row_changed);
        this->hidden->row_changed=NULL;
    }
    g_fbzx_dirty_rows=NULL;
    g_fbzx_dirty_base=NULL;
    g_fbzx_dirty_bytes=0;
    g_fbzx_dirty_pitch=0;
    g_fbzx_dirty_height=0;
    this->hidden->prev_valid=0;
}'''
assert s.count(old)==1, s.count(old)
s=s.replace(old,new,1)

old='''        this->hidden->prev_buffer=SDL_malloc(bytes);
        if(!this->hidden->prev_buffer) {
            free_shadow_buffers(this);
            SDL_SetError("H3531: fast16 previous-frame allocation failed");
            return NULL;
        }
        if(!SDL_ReallocFormat(current,16,0x7C00u,0x03E0u,0x001Fu,0x8000u)) {'''
new='''        this->hidden->prev_buffer=SDL_malloc(bytes);
        if(!this->hidden->prev_buffer) {
            free_shadow_buffers(this);
            SDL_SetError("H3531: fast16 previous-frame allocation failed");
            return NULL;
        }
        this->hidden->row_changed=(unsigned char*)SDL_malloc((size_t)height);
        if(!this->hidden->row_changed) {
            free_shadow_buffers(this);
            SDL_SetError("H3531: fast16 dirty-row allocation failed");
            return NULL;
        }
        SDL_memset(this->hidden->row_changed,1,(size_t)height);
        if(!SDL_ReallocFormat(current,16,0x7C00u,0x03E0u,0x001Fu,0x8000u)) {'''
assert s.count(old)==1, s.count(old)
s=s.replace(old,new,1)

old='''    current->pitch=width*bytespp;
    current->pixels=this->hidden->buffer;
    if(build_scale_tables(this)<0) {'''
new='''    current->pitch=width*bytespp;
    current->pixels=this->hidden->buffer;
    if(fast16) {
        g_fbzx_dirty_rows=this->hidden->row_changed;
        g_fbzx_dirty_base=(unsigned char*)this->hidden->buffer;
        g_fbzx_dirty_bytes=(unsigned long)bytes;
        g_fbzx_dirty_pitch=(unsigned)current->pitch;
        g_fbzx_dirty_height=(unsigned)height;
    }
    if(build_scale_tables(this)<0) {'''
assert s.count(old)==1, s.count(old)
s=s.replace(old,new,1)

s=s.replace('dirty-row scaler enabled\\n', 'true-dirty-v21 scaler enabled\\n', 1)

old='''        if(!this->hidden->prev_valid || SDL_memcmp(s0,p0,640u*2u)!=0) {
            scale_row_640_to_960_1555(s0,d0);
            SDL_memcpy(d1,d0,960u*2u);
            SDL_memcpy(p0,s0,640u*2u);
            ++changed;
        }
        if(!this->hidden->prev_valid || SDL_memcmp(s1,p1,640u*2u)!=0) {
            scale_row_640_to_960_1555(s1,d2);
            SDL_memcpy(p1,s1,640u*2u);
            ++changed;
        }'''
new='''        if(!this->hidden->prev_valid || this->hidden->row_changed[sy0]) {
            scale_row_640_to_960_1555(s0,d0);
            SDL_memcpy(d1,d0,960u*2u);
            this->hidden->row_changed[sy0]=0;
            ++changed;
        }
        if(!this->hidden->prev_valid || this->hidden->row_changed[sy1]) {
            scale_row_640_to_960_1555(s1,d2);
            this->hidden->row_changed[sy1]=0;
            ++changed;
        }'''
assert s.count(old)==1, s.count(old)
s=s.replace(old,new,1)

p.write_text(s)
print("H3531 v21: SDL true dirty-row map enabled")
