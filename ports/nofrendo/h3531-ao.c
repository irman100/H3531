#include "h3531-ao.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define H3531_AO_CONTEXT_INDEX 80
#define H3531_AO_SAMPLE_RATE 48000
#define H3531_AO_SAMPLES 160

#define H3531_IOCTL_INIT_CONTEXT 0x40045800UL
#define H3531_IOCTL_SET_PUB_ATTR 0x40245801UL
#define H3531_IOCTL_ENABLE_DEV   0x00005803UL
#define H3531_IOCTL_DISABLE_DEV  0x00005804UL
#define H3531_IOCTL_SEND_FRAME   0x40305805UL
#define H3531_IOCTL_ENABLE_CHN   0x00005808UL
#define H3531_IOCTL_DISABLE_CHN  0x00005809UL
#define H3531_IOCTL_CLEAR_ATTR   0x0000580BUL

/* HI_ERR_AO_NOT_PERM as returned by the stock Hi3531 AO driver. */
#define H3531_AO_NOT_PERM ((int32_t)0xA0168009U)

struct H3531AioAttr {
    uint32_t enSamplerate;
    uint32_t enBitwidth;
    uint32_t enWorkmode;
    uint32_t enSoundmode;
    uint32_t u32EXFlag;
    uint32_t u32FrmNum;
    uint32_t u32PtNumPerFrm;
    uint32_t u32ChnCnt;
    uint32_t u32ClkSel;
};

struct H3531AudioFrame {
    uint32_t enBitwidth;
    uint32_t enSoundmode;
    uint32_t pVirAddr[2];
    uint32_t u32PhyAddr[2];
    uint64_t u64TimeStamp;
    uint32_t u32Seq;
    uint32_t u32Len;
    uint32_t u32PoolId[2];
};

typedef char h3531_attr_size_must_be_36[(sizeof(struct H3531AioAttr) == 36) ? 1 : -1];
typedef char h3531_frame_size_must_be_48[(sizeof(struct H3531AudioFrame) == 48) ? 1 : -1];
typedef char h3531_frame_len_offset_must_be_36[(offsetof(struct H3531AudioFrame, u32Len) == 36) ? 1 : -1];

static int ao_fd = -1;
static int ao_dev_enabled = 0;
static int ao_chn_enabled = 0;
static uint32_t ao_seq = 0;

static int ao_ioctl(unsigned long request, void *arg)
{
    errno = 0;
    return ioctl(ao_fd, request, arg);
}

static int ao_ioctl_noarg(unsigned long request)
{
    errno = 0;
    return ioctl(ao_fd, request, 0);
}

static int fail_stage(const char *stage, int rc)
{
    fprintf(stderr, "H3531 AO v5: %s failed rc=%d (0x%08x) errno=%d\n",
            stage, rc, (unsigned)rc, errno);
    return -1;
}

static void make_attr(struct H3531AioAttr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->enSamplerate = H3531_AO_SAMPLE_RATE;
    attr->enBitwidth = 1;       /* AUDIO_BIT_WIDTH_16 */
    attr->enWorkmode = 0;       /* I2S master */
    attr->enSoundmode = 0;      /* mono */
    attr->u32EXFlag = 0;
    attr->u32FrmNum = 30;
    attr->u32PtNumPerFrm = H3531_AO_SAMPLES;
    attr->u32ChnCnt = 2;
    attr->u32ClkSel = 0;
}

static void reset_existing_ao5_state(void)
{
    int rc;

    /*
     * Monitor/Sofia may leave AO5 enabled while handing graphical ownership
     * to an application.  In that state SetPubAttr returns HI_ERR_AO_NOT_PERM.
     * We already have context 80 selected, so stop the old channel/device,
     * clear the old attributes, and configure our proven 48-kHz profile.
     */
    rc = ao_ioctl_noarg(H3531_IOCTL_DISABLE_CHN);
    fprintf(stderr, "H3531 AO v5: recovery DisableChn rc=%d (0x%08x)\n",
            rc, (unsigned)rc);
    rc = ao_ioctl_noarg(H3531_IOCTL_DISABLE_DEV);
    fprintf(stderr, "H3531 AO v5: recovery DisableDev rc=%d (0x%08x)\n",
            rc, (unsigned)rc);
    rc = ao_ioctl_noarg(H3531_IOCTL_CLEAR_ATTR);
    fprintf(stderr, "H3531 AO v5: recovery ClearPubAttr rc=%d (0x%08x)\n",
            rc, (unsigned)rc);

    ao_dev_enabled = 0;
    ao_chn_enabled = 0;
}

void h3531_ao_stop(void)
{
    if (ao_fd < 0)
        return;
    if (ao_chn_enabled) {
        (void)ao_ioctl_noarg(H3531_IOCTL_DISABLE_CHN);
        ao_chn_enabled = 0;
    }
    if (ao_dev_enabled) {
        (void)ao_ioctl_noarg(H3531_IOCTL_DISABLE_DEV);
        ao_dev_enabled = 0;
    }
    close(ao_fd);
    ao_fd = -1;
}

int h3531_ao_start(void)
{
    struct H3531AioAttr attr;
    uint32_t context = H3531_AO_CONTEXT_INDEX;
    int rc;

    if (ao_fd >= 0)
        return 0;

    ao_fd = open("/dev/ao", O_RDWR);
    if (ao_fd < 0) {
        fprintf(stderr, "H3531 AO v5: open /dev/ao failed errno=%d\n", errno);
        return -1;
    }

    make_attr(&attr);

    rc = ao_ioctl(H3531_IOCTL_INIT_CONTEXT, &context);
    if (rc != 0) {
        fail_stage("InitContext", rc);
        close(ao_fd);
        ao_fd = -1;
        return -1;
    }

    rc = ao_ioctl(H3531_IOCTL_SET_PUB_ATTR, &attr);
    if ((int32_t)rc == H3531_AO_NOT_PERM) {
        fprintf(stderr,
                "H3531 AO v5: SetPubAttr returned NOT_PERM; recovering inherited AO5 state\n");
        reset_existing_ao5_state();
        context = H3531_AO_CONTEXT_INDEX;
        rc = ao_ioctl(H3531_IOCTL_INIT_CONTEXT, &context);
        if (rc == 0)
            rc = ao_ioctl(H3531_IOCTL_SET_PUB_ATTR, &attr);
    }
    if (rc != 0) {
        fail_stage("SetPubAttr", rc);
        close(ao_fd);
        ao_fd = -1;
        return -1;
    }

    rc = ao_ioctl_noarg(H3531_IOCTL_ENABLE_DEV);
    if (rc != 0) {
        fail_stage("EnableDev", rc);
        close(ao_fd);
        ao_fd = -1;
        return -1;
    }
    ao_dev_enabled = 1;

    rc = ao_ioctl_noarg(H3531_IOCTL_ENABLE_CHN);
    if (rc != 0) {
        fail_stage("EnableChn", rc);
        h3531_ao_stop();
        return -1;
    }
    ao_chn_enabled = 1;

    ao_seq = 0;
    fprintf(stderr,
            "H3531 AO v5 ready: 48000 Hz S16 mono AO5/ch0 30x160, frame-len=160 samples\n");
    return 0;
}

int h3531_ao_send_160(const int16_t *pcm)
{
    struct H3531AudioFrame frame;
    int rc;

    if (ao_fd < 0 || pcm == NULL)
        return -1;

    memset(&frame, 0, sizeof(frame));
    frame.enBitwidth = 1;
    frame.enSoundmode = 0;
    frame.pVirAddr[0] = (uint32_t)(uintptr_t)pcm;
    frame.pVirAddr[1] = (uint32_t)(uintptr_t)pcm;
    frame.u32Seq = ao_seq++;
    /* Stock MPP AUDIO_FRAME_S uses sample points here, not byte count. */
    frame.u32Len = H3531_AO_SAMPLES;

    rc = ao_ioctl(H3531_IOCTL_SEND_FRAME, &frame);
    if (rc != 0)
        fprintf(stderr,
                "H3531 AO v5: SendFrame failed rc=%d (0x%08x) errno=%d\n",
                rc, (unsigned)rc, errno);
    return rc;
}
