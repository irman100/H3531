#define _GNU_SOURCE
#include "h3531_racer_audio.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <math.h>

#define RA_AO_CONTEXT_INDEX 80
#define RA_RATE 48000U
#define RA_SAMPLES 160U
#define RA_BYTES (RA_SAMPLES * (unsigned)sizeof(int16_t))
#define RA_MAX_RADIO 9

#define RA_IOCTL_INIT_CONTEXT 0x40045800UL
#define RA_IOCTL_SET_PUB_ATTR 0x40245801UL
#define RA_IOCTL_ENABLE_DEV   0x00005803UL
#define RA_IOCTL_DISABLE_DEV  0x00005804UL
#define RA_IOCTL_SEND_FRAME   0x40305805UL
#define RA_IOCTL_ENABLE_CHN   0x00005808UL
#define RA_IOCTL_DISABLE_CHN  0x00005809UL
#define RA_IOCTL_CLEAR_ATTR   0x0000580BUL
#define RA_IOCTL_QUERY_CHN    0x800C580FUL
#define RA_AO_NOT_PERM ((int32_t)0xA0168009U)

typedef struct {
    uint32_t enSamplerate;
    uint32_t enBitwidth;
    uint32_t enWorkmode;
    uint32_t enSoundmode;
    uint32_t u32EXFlag;
    uint32_t u32FrmNum;
    uint32_t u32PtNumPerFrm;
    uint32_t u32ChnCnt;
    uint32_t u32ClkSel;
} ra_aio_attr_t;

typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t busy_blocks;
} ra_ao_state_t;

typedef struct {
    uint32_t bit_width;
    uint32_t sound_mode;
    uint32_t vir_addr[2];
    uint32_t reserved0[5];
    uint32_t data_bytes;
    uint32_t reserved1[2];
} ra_audio_frame_t;

typedef char ra_attr_size[(sizeof(ra_aio_attr_t)==36)?1:-1];
typedef char ra_state_size[(sizeof(ra_ao_state_t)==12)?1:-1];
typedef char ra_frame_size[(sizeof(ra_audio_frame_t)==48)?1:-1];
typedef char ra_frame_len_offset[(offsetof(ra_audio_frame_t,data_bytes)==36)?1:-1];

typedef struct {
    float speed;
    float max_speed;
    float throttle;
    float slip;
    unsigned gear;
    unsigned gears;
    int handbrake;
    unsigned wheel_state_bits;
    unsigned surface_type;
    int radio_on;
    int radio_index;
    int running;
    uint32_t impact_q15;
    uint32_t impact_serial;
    int impact_kind;
} ra_shared_t;

static int g_fd=-1;
static int g_dev_enabled=0;
static int g_chn_enabled=0;
static int g_query_supported=1;
static int g_active=0;
static int g_worker_started=0;
static pthread_t g_worker;
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static ra_shared_t g_shared;

static const char *g_radio_names[RA_MAX_RADIO]={
    "WILD","FLASH","KCHAT","FEVER","VROCK","VCPR","ESPANTOSO","EMOTION","WAVE"
};
static FILE *g_radio_files[RA_MAX_RADIO]={0};
static uint8_t g_radio_present[RA_MAX_RADIO]={0};
static uint8_t g_radio_format[RA_MAX_RADIO]={0}; /* 1=24k mu-law, 2=48k s16 */
static int g_radio_available=0;
static char g_radio_path[RA_MAX_RADIO][512];

typedef struct {
    int16_t *data;
    uint32_t count;
} ra_sample_t;

static ra_sample_t g_engine_rev={0};
static ra_sample_t g_engine_idle={0};
static ra_sample_t g_road_noise={0};
static ra_sample_t g_skid={0};
static ra_sample_t g_gravel_skid={0};
static ra_sample_t g_landing={0};
static ra_sample_t g_impact={0};

static uint32_t g_blocks=0;
static uint32_t g_send_fail=0;
static uint32_t g_query_fail=0;
static uint32_t g_radio_loops=0;
static uint32_t g_impacts=0;

static int ao_ioctl(unsigned long req,void *arg)
{
    errno=0;
    return ioctl(g_fd,req,arg);
}

static int ao_ioctl_noarg(unsigned long req)
{
    errno=0;
    return ioctl(g_fd,req,0);
}

static void ao_disable(void)
{
    if(g_fd<0)return;
    if(g_chn_enabled){
        (void)ao_ioctl_noarg(RA_IOCTL_DISABLE_CHN);
        g_chn_enabled=0;
    }
    if(g_dev_enabled){
        (void)ao_ioctl_noarg(RA_IOCTL_DISABLE_DEV);
        g_dev_enabled=0;
    }
    close(g_fd);
    g_fd=-1;
}

static void ao_reset_existing(void)
{
    (void)ao_ioctl_noarg(RA_IOCTL_DISABLE_CHN);
    (void)ao_ioctl_noarg(RA_IOCTL_DISABLE_DEV);
    (void)ao_ioctl_noarg(RA_IOCTL_CLEAR_ATTR);
    g_dev_enabled=0;
    g_chn_enabled=0;
}

static int ao_start(void)
{
    ra_aio_attr_t attr;
    uint32_t context=RA_AO_CONTEXT_INDEX;
    int rc;

    g_fd=open("/dev/ao",O_RDWR);
    if(g_fd<0){
        fprintf(stderr,"[racer-audio] /dev/ao unavailable errno=%d\n",errno);
        return -1;
    }

    memset(&attr,0,sizeof(attr));
    attr.enSamplerate=RA_RATE;
    attr.enBitwidth=1U;
    attr.enWorkmode=0U;
    attr.enSoundmode=0U;
    attr.u32FrmNum=30U;
    attr.u32PtNumPerFrm=RA_SAMPLES;
    attr.u32ChnCnt=2U;

    rc=ao_ioctl(RA_IOCTL_INIT_CONTEXT,&context);
    if(rc!=0)goto fail;

    rc=ao_ioctl(RA_IOCTL_SET_PUB_ATTR,&attr);
    if((int32_t)rc==RA_AO_NOT_PERM){
        fprintf(stderr,"[racer-audio] recovering inherited AO5 state\n");
        ao_reset_existing();
        context=RA_AO_CONTEXT_INDEX;
        rc=ao_ioctl(RA_IOCTL_INIT_CONTEXT,&context);
        if(rc==0)rc=ao_ioctl(RA_IOCTL_SET_PUB_ATTR,&attr);
    }
    if(rc!=0)goto fail;

    rc=ao_ioctl_noarg(RA_IOCTL_ENABLE_DEV);
    if(rc!=0)goto fail;
    g_dev_enabled=1;

    rc=ao_ioctl_noarg(RA_IOCTL_ENABLE_CHN);
    if(rc!=0)goto fail;
    g_chn_enabled=1;
    g_query_supported=1;

    fprintf(stderr,
        "[racer-audio] AO ready /dev/ao AO5/ch0 48000Hz S16 mono 30x160\n");
    return 0;

fail:
    fprintf(stderr,
        "[racer-audio] AO init failed rc=%d (0x%08x) errno=%d\n",
        rc,(unsigned)rc,errno);
    ao_disable();
    return -1;
}

static int wait_free_block(void)
{
    ra_ao_state_t st;
    struct pollfd pfd;
    int rc;

    if(!g_query_supported)return 0;
    for(;;){
        memset(&st,0,sizeof(st));
        rc=ao_ioctl(RA_IOCTL_QUERY_CHN,&st);
        if(rc!=0){
            g_query_supported=0;
            g_query_fail++;
            return 0;
        }
        if(st.free_blocks>0U)return 0;

        pfd.fd=g_fd;pfd.events=POLLOUT;pfd.revents=0;
        do{rc=poll(&pfd,1,100);}while(rc<0&&errno==EINTR);
        if(rc<=0)return -1;
    }
}

static int ao_send(const int16_t *pcm)
{
    ra_audio_frame_t fr;
    uintptr_t p=(uintptr_t)pcm;
    int rc;

    if(g_fd<0||!pcm)return -1;
    if(sizeof(uintptr_t)>sizeof(uint32_t)&&p>UINT32_MAX)return -1;
    if(wait_free_block()!=0)return -1;

    memset(&fr,0,sizeof(fr));
    fr.bit_width=1U;
    fr.sound_mode=0U;
    fr.vir_addr[0]=(uint32_t)p;
    fr.vir_addr[1]=(uint32_t)p;
    fr.data_bytes=RA_BYTES;
    rc=ao_ioctl(RA_IOCTL_SEND_FRAME,&fr);
    if(rc==0)g_blocks++;
    else g_send_fail++;
    return rc;
}

static int32_t tri_q15(uint32_t phase)
{
    uint32_t p=phase>>16;
    if(p<32768U)return -32768+(int32_t)(p<<1);
    return 98303-(int32_t)(p<<1);
}

static int16_t sat16(int32_t v)
{
    if(v>32767)return 32767;
    if(v<-32768)return -32768;
    return (int16_t)v;
}

static uint32_t phase_inc(float hz)
{
    double v=(double)hz*(4294967296.0/(double)RA_RATE);
    if(v<1.0)v=1.0;
    if(v>4294967295.0)v=4294967295.0;
    return (uint32_t)v;
}

static int load_pcm_sample(
    const char *asset_dir,const char *name,ra_sample_t *s)
{
    char path[512];
    FILE *fp;
    long bytes;
    size_t got;

    if(!asset_dir||!*asset_dir||!name||!s)return 0;
    snprintf(path,sizeof(path),"%s/%s",asset_dir,name);
    fp=fopen(path,"rb");
    if(!fp)return 0;
    if(fseek(fp,0,SEEK_END)!=0){fclose(fp);return 0;}
    bytes=ftell(fp);
    if(bytes<=1 || bytes>16*1024*1024L || (bytes&1L)){
        fclose(fp);
        return 0;
    }
    if(fseek(fp,0,SEEK_SET)!=0){fclose(fp);return 0;}
    s->data=(int16_t*)malloc((size_t)bytes);
    if(!s->data){fclose(fp);return 0;}
    got=fread(s->data,1,(size_t)bytes,fp);
    fclose(fp);
    if(got!=(size_t)bytes){
        free(s->data);s->data=NULL;return 0;
    }
    s->count=(uint32_t)((size_t)bytes/sizeof(int16_t));
    fprintf(stderr,"[racer-audio] sfx ready %s samples=%u\n",
        name,(unsigned)s->count);
    return 1;
}

static void free_pcm_sample(ra_sample_t *s)
{
    if(!s)return;
    free(s->data);
    s->data=NULL;
    s->count=0U;
}

static inline int32_t sample_loop_q16(
    const ra_sample_t *s,uint32_t *phase,float rate)
{
    uint32_t idx,frac,next;
    int32_t a,b,out;
    uint32_t step;
    uint32_t xf;
    if(!s||!s->data||s->count<2U)return 0;
    if(rate<0.20f)rate=0.20f;
    if(rate>3.50f)rate=3.50f;
    step=(uint32_t)(rate*65536.0f);
    idx=(*phase)>>16;
    while(idx>=s->count){
        *phase-=s->count<<16;
        idx=(*phase)>>16;
    }
    next=idx+1U;
    if(next>=s->count)next=0U;
    frac=(*phase)&0xffffU;
    a=s->data[idx];b=s->data[next];
    out=a+(((b-a)*(int32_t)frac)>>16);

    /*
     * Source VC engine/skid samples are loop assets, but a raw end->start jump
     * exposes any DC/phase mismatch as the audible "saw" reported on hardware.
     * Blend the last ~21 ms into the beginning of the same sample.  This keeps
     * the loop continuous while preserving pitch modulation.
     */
    xf=s->count>2048U?1024U:(s->count/4U);
    if(xf>=8U && idx>=s->count-xf){
        uint32_t rel=idx-(s->count-xf);
        uint32_t hidx=rel;
        uint32_t hnext=hidx+1U;
        int32_t ha,hb,head;
        uint32_t mix=(rel<<15)/xf;
        if(hnext>=s->count)hnext=0U;
        ha=s->data[hidx];hb=s->data[hnext];
        head=ha+(((hb-ha)*(int32_t)frac)>>16);
        out=(int32_t)(((int64_t)out*(32768U-mix)+
                       (int64_t)head*mix)>>15);
    }

    *phase+=step;
    return out;
}

static int16_t mulaw_decode(uint8_t u)
{
    int t;
    u=(uint8_t)~u;
    t=((int)(u&0x0f)<<3)+0x84;
    t<<=(u&0x70)>>4;
    return (int16_t)((u&0x80)?(0x84-t):(t-0x84));
}

static size_t radio_read_block(
    int16_t *out,size_t count,int enabled,int radio_index)
{
    FILE *fp=NULL;
    size_t produced=0;
    uint8_t format=0U;

    if(enabled && radio_index>=0 && radio_index<RA_MAX_RADIO){
        fp=g_radio_files[radio_index];
        format=g_radio_format[radio_index];
    }
    if(!enabled||!fp){
        memset(out,0,count*sizeof(*out));
        return 0;
    }

    if(format==1U){
        /*
         * Compact radio is 24 kHz G.711 mu-law. AO runs at 48 kHz, so one
         * source byte becomes two output samples. This keeps the runtime
         * decoder tiny and cuts radio storage to one quarter of 48k/S16.
         */
        while(produced<count){
            uint8_t b;
            size_t n=fread(&b,1,1,fp);
            if(n!=1U){
                if(feof(fp)){
                    clearerr(fp);
                    if(fseek(fp,0,SEEK_SET)!=0)break;
                    g_radio_loops++;
                    continue;
                }
                break;
            }
            {
                int16_t s=mulaw_decode(b);
                out[produced++]=s;
                if(produced<count)out[produced++]=s;
            }
        }
    }else{
        while(produced<count){
            size_t n=fread(out+produced,sizeof(*out),count-produced,fp);
            produced+=n;
            if(produced==count)break;
            if(feof(fp)){
                clearerr(fp);
                if(fseek(fp,0,SEEK_SET)!=0)break;
                g_radio_loops++;
                continue;
            }
            break;
        }
    }
    if(produced<count)
        memset(out+produced,0,(count-produced)*sizeof(*out));
    return produced;
}

static void *audio_worker(void *unused)
{
    uint32_t fallback_phase=0U;
    uint32_t rev_phase=0U,idle_phase=0U,skid_phase=0U;
    uint32_t road_phase=0U,gravel_phase=0U;
    float engine_rpm_norm=0.0f;
    float skid_env=0.0f;
    float skid_pitch=1.0f;
    uint32_t skid_drift_phase=0U;
    uint32_t noise=0x13579BDFU;
    int32_t wind_lp=0;
    unsigned last_gear=0U;
    unsigned shift_blocks=0U;
    uint32_t seen_impact_serial=0U;
    uint32_t impact_pos=0U;
    uint32_t impact_amp=0U;
    int impact_kind=RACER_AUDIO_IMPACT_WALL;
    int16_t pcm[RA_SAMPLES];
    int16_t radio[RA_SAMPLES];
    (void)unused;

    for(;;){
        ra_shared_t s0;
        float speed_abs,speed_norm,gear_span,within,shift_mul;
        float rev_rate,idle_rate,target_rpm,skid_target,road_gain;
        float skid_rate;
        int rev_amp,idle_amp,wind_amp,skid_amp,road_amp;
        int loose_surface,grass_surface;
        unsigned skid_wheels=0U,fixed_wheels=0U,spin_wheels=0U,w;
        unsigned i;
        const ra_sample_t *impact_sample=NULL;

        pthread_mutex_lock(&g_lock);
        s0=g_shared;
        pthread_mutex_unlock(&g_lock);
        if(!s0.running)break;

        if(last_gear!=0U&&s0.gear!=last_gear)shift_blocks=60U;
        last_gear=s0.gear;

        if(s0.impact_serial!=seen_impact_serial){
            seen_impact_serial=s0.impact_serial;
            impact_pos=0U;
            impact_amp=s0.impact_q15;
            impact_kind=s0.impact_kind;
        }
        impact_sample=
            impact_kind==RACER_AUDIO_IMPACT_LAND?&g_landing:&g_impact;

        speed_abs=fabsf(s0.speed);
        speed_norm=s0.max_speed>1.0f?speed_abs/s0.max_speed:0.0f;
        if(speed_norm>1.0f)speed_norm=1.0f;
        if(s0.gears<1U)s0.gears=1U;
        gear_span=s0.max_speed/(float)s0.gears;
        if(gear_span<1.0f)gear_span=1.0f;

        /*
         * Do not derive RPM with fmod(speed, gear_span). That made the engine
         * saw up and down forever at steady high speed. Approximate the VC
         * transmission ratio from current gear instead: vehicle speed divided
         * by the current gear's share of maximum speed. The shift envelope
         * below still supplies the short RPM dip.
         */
        if(s0.gear==0U){
            target_rpm=fminf(1.0f,speed_abs/fmaxf(1.0f,s0.max_speed*0.38f));
        }else{
            unsigned g=s0.gear>s0.gears?s0.gears:s0.gear;
            target_rpm=speed_norm*(float)s0.gears/(float)(g?g:1U);
            if(target_rpm<0.16f)target_rpm=0.16f;
            if(target_rpm>1.0f)target_rpm=1.0f;
        }
        engine_rpm_norm+=(target_rpm-engine_rpm_norm)*0.075f;
        within=engine_rpm_norm;

        shift_mul=1.0f;
        if(shift_blocks){
            float t=(float)shift_blocks/60.0f;
            shift_mul=0.70f+0.30f*(1.0f-t);
            shift_blocks--;
        }

        /*
         * Oceanic uses the Cadillac audio family in Vice City
         * (REV_9 / IDLE_9). Pitch rises inside each gear and dips during
         * the short transmission torque cut.
         */
        /*
         * Stable base engine path. Do not layer ACCEL/AFTER_ACCEL/FINGER_OFF
         * until the complete reVC player-engine state machine is implemented:
         * those samples are channel/state driven, not free-running overlays.
         */
        rev_rate=(0.72f+0.92f*within+0.16f*s0.throttle)*shift_mul;
        idle_rate=0.92f+0.08f*s0.throttle;
        rev_amp=(int)(7600.0f*(0.16f+0.84f*fmaxf(s0.throttle,speed_norm)));
        idle_amp=(int)(4200.0f*(1.0f-0.66f*fmaxf(s0.throttle,speed_norm)));
        if(shift_blocks)rev_amp=rev_amp*3/4;

        wind_amp=20+(int)(80.0f*speed_norm);
        road_gain=(speed_norm-0.025f)/0.45f;
        if(road_gain<0.0f)road_gain=0.0f;
        if(road_gain>1.0f)road_gain=1.0f;
        road_amp=(s0.surface_type==255U)?0:(int)(2100.0f*road_gain);
        loose_surface=
            s0.surface_type==3U || s0.surface_type==4U ||
            s0.surface_type==18U || s0.surface_type==19U ||
            s0.surface_type==33U;
        grass_surface=(s0.surface_type==2U || s0.surface_type==25U);

        /*
         * Vice City distinguishes NORMAL / SPINNING / SKIDDING / FIXED wheel
         * states. A steering slip angle by itself is not a tyre-squeal event.
         * Build the sound from those actual physics states, then gate very low
         * speeds where the old global-slip mixer produced continuous scraping
         * while parking or reversing.
         */
        for(w=0U;w<4U;++w){
            unsigned st=(s0.wheel_state_bits>>(w*2U))&3U;
            if(st==1U)spin_wheels++;
            else if(st==2U)skid_wheels++;
            else if(st==3U)fixed_wheels++;
        }
        skid_target=0.0f;
        if(speed_abs>=10.0f){
            float slip_mag=fabsf(s0.slip);
            if(skid_wheels)
                skid_target=fminf(1.0f,0.28f*(float)skid_wheels+slip_mag*1.8f);
            if(fixed_wheels)
                skid_target=fmaxf(skid_target,
                    fminf(1.0f,0.34f*(float)fixed_wheels+speed_norm*0.45f));
            if(spin_wheels && s0.throttle>0.35f)
                skid_target=fmaxf(skid_target,
                    fminf(0.78f,0.24f*(float)spin_wheels+s0.throttle*0.30f));
        }
        if(s0.handbrake && speed_abs>=8.0f)
            skid_target=fmaxf(skid_target,fminf(1.0f,0.35f+speed_norm*0.70f));
        if(s0.speed<0.0f && speed_abs<18.0f && !s0.handbrake)
            skid_target=0.0f;
        if(skid_target>skid_env)
            skid_env+=(skid_target-skid_env)*0.22f;
        else
            skid_env+=(skid_target-skid_env)*0.08f;
        if(skid_env<0.015f)skid_env=0.0f;
        skid_amp=(int)(7000.0f*skid_env);

        /*
         * Keep one continuous skid bed for the whole slide. A very slow,
         * shallow pitch wander prevents a long drift from revealing the exact
         * same short waveform period without changing the character of VC's
         * tyre sample.
         */
        skid_drift_phase+=1U;
        {
            float drift=0.012f*sinf((float)skid_drift_phase*0.0045f);
            float target_pitch=(loose_surface?0.82f:0.92f)+
                (loose_surface?0.30f:0.42f)*skid_env+drift;
            skid_pitch+=(target_pitch-skid_pitch)*0.020f;
            skid_rate=skid_pitch;
        }

        radio_read_block(radio,RA_SAMPLES,s0.radio_on,s0.radio_index);

        for(i=0;i<RA_SAMPLES;++i){
            int32_t sample=0;
            int32_t rnd;

            if(g_engine_idle.data && g_engine_rev.data){
                sample+=(sample_loop_q16(&g_engine_idle,&idle_phase,idle_rate)*idle_amp)>>15;
                sample+=(sample_loop_q16(&g_engine_rev,&rev_phase,rev_rate)*rev_amp)>>15;
            }else{
                /*
                 * Quiet fallback only. The old loud triangle oscillator was
                 * the user's "гулёж"; authentic imported SFX take precedence.
                 */
                uint32_t inc=phase_inc(85.0f+95.0f*within);
                fallback_phase+=inc;
                sample+=(tri_q15(fallback_phase)*700)>>15;
            }

            noise=noise*1664525U+1013904223U;
            rnd=(int32_t)((noise>>16)&0xffffU)-32768;
            wind_lp+=(rnd-wind_lp)>>5;
            sample+=(wind_lp*wind_amp)>>15;

            if(road_amp>0 && g_road_noise.data)
                sample+=(sample_loop_q16(&g_road_noise,&road_phase,0.96f+0.12f*speed_norm)*road_amp)>>15;

            if(skid_amp>0){
                const ra_sample_t *tyre=
                    (loose_surface||grass_surface) && g_gravel_skid.data
                        ?&g_gravel_skid:&g_skid;
                uint32_t *tyre_phase=
                    tyre==&g_gravel_skid?&gravel_phase:&skid_phase;
                int tyre_amp=grass_surface?skid_amp/3:skid_amp;
                if(tyre->data)
                    sample+=(sample_loop_q16(tyre,tyre_phase,skid_rate)*tyre_amp)>>15;
                else
                    sample+=((rnd-wind_lp)*tyre_amp)>>17;
            }

            if(impact_amp){
                if(impact_sample && impact_sample->data &&
                   impact_pos<impact_sample->count){
                    sample+=((int32_t)impact_sample->data[impact_pos++]*
                             (int32_t)impact_amp)>>15;
                    if(impact_pos>=impact_sample->count)impact_amp=0U;
                }else{
                    /* Dry short fallback, deliberately no splash/noisy tail. */
                    sample+=(rnd*(int32_t)impact_amp)>>17;
                    impact_amp=impact_amp>900U?impact_amp-900U:0U;
                }
            }

            if(s0.radio_on&&g_radio_available)
                sample+=(int32_t)radio[i]/2;

            pcm[i]=sat16(sample);
        }

        if(ao_send(pcm)!=0){
            fprintf(stderr,
                "[racer-audio] SendFrame failed; audio thread stopping\n");
            pthread_mutex_lock(&g_lock);
            g_shared.running=0;
            pthread_mutex_unlock(&g_lock);
            break;
        }
    }
    return NULL;
}

int racer_audio_start(const char *asset_dir)
{
    int rc;
    int ri;
    memset(&g_shared,0,sizeof(g_shared));
    g_shared.radio_index=-1;
    g_blocks=g_send_fail=g_query_fail=g_radio_loops=g_impacts=0U;
    g_radio_available=0;
    memset(g_radio_present,0,sizeof(g_radio_present));
    memset(g_radio_format,0,sizeof(g_radio_format));
    memset(g_radio_files,0,sizeof(g_radio_files));
    memset(g_radio_path,0,sizeof(g_radio_path));

    if(asset_dir&&*asset_dir){
        (void)load_pcm_sample(asset_dir,"ENGINE_REV.PCM",&g_engine_rev);
        (void)load_pcm_sample(asset_dir,"ENGINE_IDLE.PCM",&g_engine_idle);
        (void)load_pcm_sample(asset_dir,"ROAD_NOISE.PCM",&g_road_noise);
        (void)load_pcm_sample(asset_dir,"SKID.PCM",&g_skid);
        (void)load_pcm_sample(asset_dir,"GRAVEL_SKID.PCM",&g_gravel_skid);
        (void)load_pcm_sample(asset_dir,"LANDING.PCM",&g_landing);
        (void)load_pcm_sample(asset_dir,"IMPACT.PCM",&g_impact);

        for(ri=0;ri<RA_MAX_RADIO;++ri){
            snprintf(g_radio_path[ri],sizeof(g_radio_path[ri]),
                "%s/RADIO_%s.MULAW",asset_dir,g_radio_names[ri]);
            g_radio_files[ri]=fopen(g_radio_path[ri],"rb");
            if(g_radio_files[ri]){
                g_radio_present[ri]=1U;
                g_radio_format[ri]=1U;
                g_radio_available++;
                fprintf(stderr,
                    "[racer-audio] radio source ready station=%s path=%s "
                    "format=mulaw/24k/mono\n",
                    g_radio_names[ri],g_radio_path[ri]);
                continue;
            }

            snprintf(g_radio_path[ri],sizeof(g_radio_path[ri]),
                "%s/RADIO_%s.PCM",asset_dir,g_radio_names[ri]);
            g_radio_files[ri]=fopen(g_radio_path[ri],"rb");
            if(g_radio_files[ri]){
                g_radio_present[ri]=1U;
                g_radio_format[ri]=2U;
                g_radio_available++;
                fprintf(stderr,
                    "[racer-audio] radio source ready station=%s path=%s "
                    "format=s16le/48k/mono legacy\n",
                    g_radio_names[ri],g_radio_path[ri]);
            }
        }

        /* Backward compatibility with the first single-station build. */
        if(g_radio_available==0){
            snprintf(g_radio_path[0],sizeof(g_radio_path[0]),
                "%s/RADIO0.PCM",asset_dir);
            g_radio_files[0]=fopen(g_radio_path[0],"rb");
            if(g_radio_files[0]){
                g_radio_present[0]=1U;
                g_radio_available=1;
                fprintf(stderr,
                    "[racer-audio] legacy radio source ready station=WAVE path=%s\n",
                    g_radio_path[0]);
            }
        }

        if(g_radio_available==0)
            fprintf(stderr,
                "[racer-audio] no radio files found; stick-click radio will stay off\n");
        fprintf(stderr,
            "[racer-audio] radio scan dir=%s stations=%d formats=mulaw24k|legacy48k\n",
            asset_dir,g_radio_available);
    }

    if(ao_start()!=0){
        free_pcm_sample(&g_engine_rev);
        free_pcm_sample(&g_engine_idle);
        free_pcm_sample(&g_road_noise);
        free_pcm_sample(&g_skid);
        free_pcm_sample(&g_gravel_skid);
        free_pcm_sample(&g_landing);
        free_pcm_sample(&g_impact);
        for(ri=0;ri<RA_MAX_RADIO;++ri){
            if(g_radio_files[ri]){
                fclose(g_radio_files[ri]);
                g_radio_files[ri]=NULL;
            }
        }
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_shared.running=1;
    g_shared.max_speed=180.0f;
    g_shared.gear=1U;
    g_shared.gears=5U;

    /*
     * Radio is ON by default. Pick the first station that was actually opened,
     * before the audio worker starts, so playback does not depend on a gamepad
     * button being recognised by this particular USB adapter.
     */
    if(g_radio_available>0){
        for(ri=0;ri<RA_MAX_RADIO;++ri){
            if(g_radio_present[ri]){
                g_shared.radio_on=1;
                g_shared.radio_index=ri;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);

    if(g_shared.radio_on && g_shared.radio_index>=0 &&
       g_shared.radio_index<RA_MAX_RADIO)
        fprintf(stderr,
            "[racer-audio] radio default=on station=%s\n",
            g_radio_names[g_shared.radio_index]);

    rc=pthread_create(&g_worker,NULL,audio_worker,NULL);
    if(rc!=0){
        fprintf(stderr,"[racer-audio] worker create failed rc=%d\n",rc);
        pthread_mutex_lock(&g_lock);
        g_shared.running=0;
        pthread_mutex_unlock(&g_lock);
        ao_disable();
        free_pcm_sample(&g_engine_rev);
        free_pcm_sample(&g_engine_idle);
        free_pcm_sample(&g_road_noise);
        free_pcm_sample(&g_skid);
        free_pcm_sample(&g_gravel_skid);
        free_pcm_sample(&g_landing);
        free_pcm_sample(&g_impact);
        for(ri=0;ri<RA_MAX_RADIO;++ri){
            if(g_radio_files[ri]){
                fclose(g_radio_files[ri]);
                g_radio_files[ri]=NULL;
            }
        }
        return -1;
    }
    g_worker_started=1;
    g_active=1;
    fprintf(stderr,
        "[racer-audio] mixer ready engine=%s shift=gear-dip impact=%s "
        "landing=%s skid=%s city=wind radioStations=%d start+sticks=cycle+off "
        "rpm=gear-ratio tyre=gta-wheel-state\n",
        (g_engine_rev.data&&g_engine_idle.data)?"vc-oceanic-rev9-idle9":"quiet-fallback",
        g_impact.data?"vc-car-panel":"dry-fallback",
        g_landing.data?"vc-tyre-bump":"dry-fallback",
        g_skid.data?"vc-skid":"noise-fallback",
        g_radio_available);
    return 0;
}

void racer_audio_stop(void)
{
    if(!g_active&&g_fd<0)return;

    pthread_mutex_lock(&g_lock);
    g_shared.running=0;
    pthread_mutex_unlock(&g_lock);

    if(g_worker_started){
        pthread_join(g_worker,NULL);
        g_worker_started=0;
    }
    ao_disable();
    free_pcm_sample(&g_engine_rev);
    free_pcm_sample(&g_engine_idle);
    free_pcm_sample(&g_engine_accel);
    free_pcm_sample(&g_engine_cruise);
    free_pcm_sample(&g_engine_release);
    free_pcm_sample(&g_road_noise);
    free_pcm_sample(&g_skid);
    free_pcm_sample(&g_gravel_skid);
    free_pcm_sample(&g_landing);
    free_pcm_sample(&g_impact);
    {
        int ri;
        for(ri=0;ri<RA_MAX_RADIO;++ri){
            if(g_radio_files[ri]){
                fclose(g_radio_files[ri]);
                g_radio_files[ri]=NULL;
            }
        }
    }
    fprintf(stderr,
        "[racer-audio] stats blocks=%u send_fail=%u query_fail=%u "
        "radio_loops=%u impacts=%u\n",
        g_blocks,g_send_fail,g_query_fail,g_radio_loops,g_impacts);
    g_active=0;
}

int racer_audio_is_active(void)
{
    return g_active;
}

void racer_audio_update_vehicle(
    float speed,float max_speed,float throttle,
    unsigned gear,unsigned gears,int handbrake,float slip,
    unsigned wheel_state_bits,unsigned surface_type)
{
    if(!g_active)return;
    pthread_mutex_lock(&g_lock);
    g_shared.speed=speed;
    g_shared.max_speed=max_speed>1.0f?max_speed:180.0f;
    g_shared.throttle=throttle<0.0f?0.0f:(throttle>1.0f?1.0f:throttle);
    g_shared.gear=gear;
    g_shared.gears=gears?gears:1U;
    g_shared.handbrake=handbrake?1:0;
    g_shared.slip=slip;
    g_shared.wheel_state_bits=wheel_state_bits;
    g_shared.surface_type=surface_type;
    pthread_mutex_unlock(&g_lock);
}

void racer_audio_collision(float strength,int kind)
{
    uint32_t q;
    if(!g_active)return;
    if(strength<0.0f)strength=-strength;
    if(strength>1.0f)strength=1.0f;
    q=(uint32_t)(strength*32767.0f);
    if(q<1800U)return;

    pthread_mutex_lock(&g_lock);
    g_shared.impact_q15=q;
    g_shared.impact_kind=
        kind==RACER_AUDIO_IMPACT_LAND
            ?RACER_AUDIO_IMPACT_LAND:RACER_AUDIO_IMPACT_WALL;
    g_shared.impact_serial++;
    pthread_mutex_unlock(&g_lock);
    g_impacts++;
    fprintf(stderr,"[racer-audio] impact kind=%s strength=%.2f\n",
        kind==RACER_AUDIO_IMPACT_LAND?"land":"wall",strength);
}

void racer_audio_radio_cycle(void)
{
    int on,index=-1,start,probe;
    if(!g_active)return;

    pthread_mutex_lock(&g_lock);
    if(g_radio_available<=0){
        g_shared.radio_on=0;
        g_shared.radio_index=-1;
    }else if(!g_shared.radio_on){
        for(probe=0;probe<RA_MAX_RADIO;++probe){
            if(g_radio_present[probe]){
                g_shared.radio_index=probe;
                g_shared.radio_on=1;
                break;
            }
        }
    }else{
        start=g_shared.radio_index;
        for(probe=1;probe<=RA_MAX_RADIO;++probe){
            int candidate=(start+probe)%RA_MAX_RADIO;
            if(g_radio_present[candidate]){
                if(candidate<=start){
                    /* Wrapping past the last installed station means OFF. */
                    g_shared.radio_on=0;
                    g_shared.radio_index=-1;
                }else{
                    g_shared.radio_index=candidate;
                }
                break;
            }
        }
    }
    on=g_shared.radio_on;
    index=g_shared.radio_index;
    pthread_mutex_unlock(&g_lock);

    if(on && index>=0 && index<RA_MAX_RADIO)
        fprintf(stderr,"[racer-audio] radio=on station=%s\n",
            g_radio_names[index]);
    else
        fprintf(stderr,"[racer-audio] radio=off stations=%d\n",
            g_radio_available);
}

int racer_audio_radio_enabled(void)
{
    int on;
    pthread_mutex_lock(&g_lock);
    on=g_shared.radio_on;
    pthread_mutex_unlock(&g_lock);
    return on;
}

int racer_audio_radio_available(void)
{
    return g_radio_available;
}
