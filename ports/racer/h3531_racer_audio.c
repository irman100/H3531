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
#define RA_MAX_RADIO 8

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
    int radio_on;
    int radio_index;
    int running;
    uint32_t impact_q15;
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
    "WAVE","WILD","KCHAT","FEVER","VROCK","VCPR","ESPANTOSO","EMOTION"
};
static FILE *g_radio_files[RA_MAX_RADIO]={0};
static uint8_t g_radio_present[RA_MAX_RADIO]={0};
static int g_radio_available=0;
static char g_radio_path[RA_MAX_RADIO][512];

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

static size_t radio_read_block(
    int16_t *out,size_t count,int enabled,int radio_index)
{
    size_t got=0;
    FILE *fp=NULL;
    if(enabled && radio_index>=0 && radio_index<RA_MAX_RADIO)
        fp=g_radio_files[radio_index];
    if(!enabled||!fp){
        memset(out,0,count*sizeof(*out));
        return 0;
    }

    while(got<count){
        size_t n=fread(out+got,sizeof(*out),count-got,fp);
        got+=n;
        if(got==count)break;
        if(feof(fp)){
            clearerr(fp);
            if(fseek(fp,0,SEEK_SET)!=0)break;
            g_radio_loops++;
            continue;
        }
        break;
    }
    if(got<count)memset(out+got,0,(count-got)*sizeof(*out));
    return got;
}

static void *audio_worker(void *unused)
{
    uint32_t phase=0U;
    uint32_t noise=0x13579BDFU;
    int32_t wind_lp=0;
    unsigned last_gear=0U;
    unsigned shift_blocks=0U;
    int16_t pcm[RA_SAMPLES];
    int16_t radio[RA_SAMPLES];
    (void)unused;

    for(;;){
        ra_shared_t s;
        uint32_t inc;
        float speed_abs,speed_norm,gear_span,within,engine_hz,shift_mul;
        int engine_amp,wind_amp,skid_amp;
        unsigned i;

        pthread_mutex_lock(&g_lock);
        s=g_shared;
        pthread_mutex_unlock(&g_lock);
        if(!s.running)break;

        if(last_gear!=0U&&s.gear!=last_gear)shift_blocks=60U; /* ~200 ms */
        last_gear=s.gear;

        speed_abs=fabsf(s.speed);
        speed_norm=s.max_speed>1.0f?speed_abs/s.max_speed:0.0f;
        if(speed_norm>1.0f)speed_norm=1.0f;
        if(s.gears<1U)s.gears=1U;
        gear_span=s.max_speed/(float)s.gears;
        if(gear_span<1.0f)gear_span=1.0f;
        within=fmodf(speed_abs,gear_span)/gear_span;

        /*
         * Compact GTA-like engine model: pitch climbs through each gear,
         * then dips briefly on a gear transition while physical torque is cut.
         */
        engine_hz=58.0f+155.0f*(0.20f+0.58f*within+0.22f*s.throttle);
        shift_mul=1.0f;
        if(shift_blocks){
            float t=(float)shift_blocks/60.0f;
            shift_mul=0.64f+0.36f*(1.0f-t);
            shift_blocks--;
        }
        engine_hz*=shift_mul;
        inc=phase_inc(engine_hz);

        engine_amp=950+(int)(2200.0f*(0.30f+0.70f*s.throttle));
        wind_amp=40+(int)(230.0f*speed_norm);
        skid_amp=(int)(1800.0f*fminf(1.0f,fabsf(s.slip)*3.2f));
        if(s.handbrake&&skid_amp<700)skid_amp=700;

        radio_read_block(radio,RA_SAMPLES,s.radio_on,s.radio_index);

        {
            uint32_t impact=s.impact_q15;
            for(i=0;i<RA_SAMPLES;++i){
                int32_t sample;
                int32_t t1,t2;
                int32_t rnd;

                phase+=inc;
            t1=tri_q15(phase);
            t2=tri_q15(phase*2U);
            sample=(t1*engine_amp)>>15;
            sample+=(t2*(engine_amp/4))>>15;

            noise=noise*1664525U+1013904223U;
            rnd=(int32_t)((noise>>16)&0xffffU)-32768;
            wind_lp+=(rnd-wind_lp)>>5;
            sample+=(wind_lp*wind_amp)>>15;

            if(skid_amp>0){
                int32_t hp=rnd-wind_lp;
                sample+=(hp*skid_amp)>>15;
            }

                if(impact){
                    uint32_t dec=(impact>>7)+6U;
                    int32_t thump=tri_q15(phase*3U);
                    sample+=(thump*(int32_t)impact)>>17;
                    sample+=(rnd*(int32_t)impact)>>18;
                    impact=impact>dec?impact-dec:0U;
                }

                if(s.radio_on&&g_radio_available)
                    sample+=(int32_t)radio[i]*3/8;

                pcm[i]=sat16(sample);
            }
            if(impact!=s.impact_q15){
                pthread_mutex_lock(&g_lock);
                if(g_shared.impact_q15<=s.impact_q15)
                    g_shared.impact_q15=impact;
                pthread_mutex_unlock(&g_lock);
            }
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
    memset(g_radio_files,0,sizeof(g_radio_files));
    memset(g_radio_path,0,sizeof(g_radio_path));

    if(asset_dir&&*asset_dir){
        for(ri=0;ri<RA_MAX_RADIO;++ri){
            snprintf(g_radio_path[ri],sizeof(g_radio_path[ri]),
                "%s/RADIO_%s.PCM",asset_dir,g_radio_names[ri]);
            g_radio_files[ri]=fopen(g_radio_path[ri],"rb");
            if(g_radio_files[ri]){
                g_radio_present[ri]=1U;
                g_radio_available++;
                fprintf(stderr,
                    "[racer-audio] radio source ready station=%s path=%s "
                    "format=s16le/48k/mono\n",
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
                "[racer-audio] no radio PCM files found; R3 will stay off\n");
    }

    if(ao_start()!=0){
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
    pthread_mutex_unlock(&g_lock);

    rc=pthread_create(&g_worker,NULL,audio_worker,NULL);
    if(rc!=0){
        fprintf(stderr,"[racer-audio] worker create failed rc=%d\n",rc);
        pthread_mutex_lock(&g_lock);
        g_shared.running=0;
        pthread_mutex_unlock(&g_lock);
        ao_disable();
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
        "[racer-audio] mixer ready engine=procedural shift=gear-dip impact=impulse "
        "city=wind skid=slip radioStations=%d R3=cycle+off\n",
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
    unsigned gear,unsigned gears,int handbrake,float slip)
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
    pthread_mutex_unlock(&g_lock);
}

void racer_audio_collision(float strength)
{
    uint32_t q;
    if(!g_active)return;
    if(strength<0.0f)strength=-strength;
    if(strength>1.0f)strength=1.0f;
    q=(uint32_t)(strength*32767.0f);
    if(q<2500U)return;

    pthread_mutex_lock(&g_lock);
    if(q>g_shared.impact_q15)g_shared.impact_q15=q;
    pthread_mutex_unlock(&g_lock);
    g_impacts++;
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
