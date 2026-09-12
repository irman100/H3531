#include "h3531-ao.h"

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
#define H3531_AO_BYTES (H3531_AO_SAMPLES * (int)sizeof(int16_t))

#define H3531_IOCTL_INIT_CONTEXT 0x40045800UL
#define H3531_IOCTL_SET_PUB_ATTR 0x40245801UL
#define H3531_IOCTL_ENABLE_DEV   0x00005803UL
#define H3531_IOCTL_DISABLE_DEV  0x00005804UL
#define H3531_IOCTL_SEND_FRAME   0x40305805UL
#define H3531_IOCTL_ENABLE_CHN   0x00005808UL
#define H3531_IOCTL_DISABLE_CHN  0x00005809UL

struct H3531AioAttr {
    int32_t enSamplerate;
    int32_t enBitwidth;
    int32_t enWorkmode;
    int32_t enSoundmode;
    uint32_t u32EXFlag;
    uint32_t u32FrmNum;
    uint32_t u32PtNumPerFrm;
    uint32_t u32ChnCnt;
    uint32_t u32ClkSel;
};

struct H3531AudioFrame {
    int32_t enBitwidth;
    int32_t enSoundmode;
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
static uint32_t ao_seq = 0;

static int ao_ioctl(unsigned long request, void *arg)
{
    return ioctl(ao_fd, request, arg);
}

static int ao_ioctl_noarg(unsigned long request)
{
    return ioctl(ao_fd, request, 0);
}

void h3531_ao_stop(void)
{
    if (ao_fd < 0)
        return;
    (void)ao_ioctl_noarg(H3531_IOCTL_DISABLE_CHN);
    (void)ao_ioctl_noarg(H3531_IOCTL_DISABLE_DEV);
    close(ao_fd);
    ao_fd = -1;
}

int h3531_ao_start(void)
{
    struct H3531AioAttr attr;
    int context = H3531_AO_CONTEXT_INDEX;

    if (ao_fd >= 0)
        return 0;

    ao_fd = open("/dev/ao", O_RDWR);
    if (ao_fd < 0) {
        fprintf(stderr, "H3531 AO: cannot open /dev/ao\n");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.enSamplerate = H3531_AO_SAMPLE_RATE;
    attr.enBitwidth = 1;       /* AUDIO_BIT_WIDTH_16 */
    attr.enWorkmode = 0;       /* I2S master */
    attr.enSoundmode = 0;      /* mono */
    attr.u32EXFlag = 0;
    attr.u32FrmNum = 30;
    attr.u32PtNumPerFrm = H3531_AO_SAMPLES;
    attr.u32ChnCnt = 2;
    attr.u32ClkSel = 0;

    if (ao_ioctl(H3531_IOCTL_INIT_CONTEXT, &context) != 0 ||
        ao_ioctl(H3531_IOCTL_SET_PUB_ATTR, &attr) != 0 ||
        ao_ioctl_noarg(H3531_IOCTL_ENABLE_DEV) != 0 ||
        ao_ioctl_noarg(H3531_IOCTL_ENABLE_CHN) != 0) {
        fprintf(stderr, "H3531 AO: initialization failed\n");
        h3531_ao_stop();
        return -1;
    }

    ao_seq = 0;
    printf("H3531 AO: 48000 Hz, S16 mono, AO5/ch0, 160 samples/frame\n");
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
    frame.u32Seq = ao_seq++;
    frame.u32Len = H3531_AO_BYTES;

    rc = ao_ioctl(H3531_IOCTL_SEND_FRAME, &frame);
    if (rc != 0)
        fprintf(stderr, "H3531 AO: SendFrame failed rc=%d (0x%08x)\n", rc, (unsigned)rc);
    return rc;
}
