#include "h3531-audio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * Hi3531 MPP V1.0.D.1 AO5/ch0 backend.
 *
 * The ABI below was recovered from the factory MBD6016E-E software and then
 * physically verified with the standalone AO smoke test on the real board.
 * The emulator thread never calls AO SendFrame. It only queues PCM. A
 * dedicated worker may block in the kernel while FBZX keeps video/CPU timing
 * independently on an absolute CLOCK_MONOTONIC frame clock.
 */

namespace {

static const int kContextIndex = 80; /* AOdev 5 * 16 + channel 0 */
static const unsigned kSampleRate = 48000;
static const unsigned kSamplesPerBlock = 160;
static const unsigned kBytesPerBlock = kSamplesPerBlock * sizeof(int16_t);
static const unsigned kQueueBlocks = 16;
static const unsigned kStartBlocks = 8;
static const uint64_t kFramePeriodNs = 19968000ULL; /* 69888 / 3.5 MHz */

static const unsigned long kIoctlInitContext = 0x40045800UL;
static const unsigned long kIoctlSetPubAttr = 0x40245801UL;
static const unsigned long kIoctlEnableDev = 0x00005803UL;
static const unsigned long kIoctlDisableDev = 0x00005804UL;
static const unsigned long kIoctlSendFrame = 0x40305805UL;
static const unsigned long kIoctlEnableChn = 0x00005808UL;
static const unsigned long kIoctlDisableChn = 0x00005809UL;

enum {
    H3531_AUDIO_BIT_WIDTH_16 = 1,
    H3531_AIO_MODE_I2S_MASTER = 0,
    H3531_AUDIO_SOUND_MODE_MONO = 0
};

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

/*
 * Exact old Hi3531 32-bit AUDIO_FRAME_S layout.
 *
 * Important: this ABI stores TWO 32-bit user virtual pointers at offsets
 * 0x08/0x0c. The driver reads u32Len at offset 0x24 and interprets it as a
 * BYTE count. With 16-bit mono PCM, 160 points therefore means u32Len=320.
 *
 *   0x00 enBitwidth
 *   0x04 enSoundmode
 *   0x08 pVirAddr[0]
 *   0x0c pVirAddr[1]
 *   0x10 u32PhyAddr[0]
 *   0x14 u32PhyAddr[1]
 *   0x18 u64TimeStamp
 *   0x20 u32Seq
 *   0x24 u32Len        (bytes)
 *   0x28 u32PoolId[0]
 *   0x2c u32PoolId[1]
 */
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

typedef char h3531_attr_size_must_be_36[(sizeof(H3531AioAttr) == 36) ? 1 : -1];
typedef char h3531_frame_size_must_be_48[(sizeof(H3531AudioFrame) == 48) ? 1 : -1];
typedef char h3531_frame_vir0_offset_must_be_8[(offsetof(H3531AudioFrame, pVirAddr[0]) == 8) ? 1 : -1];
typedef char h3531_frame_len_offset_must_be_36[(offsetof(H3531AudioFrame, u32Len) == 36) ? 1 : -1];
typedef char h3531_frame_pool_offset_must_be_40[(offsetof(H3531AudioFrame, u32PoolId[0]) == 40) ? 1 : -1];
typedef char h3531_pcm_block_must_be_320_bytes[(kBytesPerBlock == 320) ? 1 : -1];

static int g_fd = -1;
static int g_active = 0;
static int g_running = 0;
static int g_worker_started = 0;
static pthread_t g_worker;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

static int16_t g_queue[kQueueBlocks][kSamplesPerBlock];
static unsigned g_read_index = 0;
static unsigned g_write_index = 0;
static unsigned g_queue_count = 0;
static uint32_t g_seq = 0;
static float g_dc = 0.0f;

static uint64_t g_blocks_sent = 0;
static uint64_t g_queue_empty_waits = 0;
static uint64_t g_overruns = 0;
static uint64_t g_send_over_1ms = 0;
static uint64_t g_send_over_3ms = 0;
static uint64_t g_send_over_5ms = 0;
static uint64_t g_send_max_ns = 0;
static int g_last_send_rc = 0;

static uint64_t g_frame_deadline_ns = 0;
static int g_frame_clock_valid = 0;

static uint64_t monotonic_ns()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static void sleep_until_ns(uint64_t deadline)
{
    for (;;) {
        const uint64_t now = monotonic_ns();
        if (now == 0 || now >= deadline)
            return;
        uint64_t left = deadline - now;
        struct timespec req;
        req.tv_sec = static_cast<time_t>(left / 1000000000ULL);
        req.tv_nsec = static_cast<long>(left % 1000000000ULL);
        if (nanosleep(&req, &req) == 0)
            return;
        if (errno != EINTR)
            return;
    }
}

static int ao_ioctl(unsigned long request, void *arg)
{
    return ioctl(g_fd, request, arg);
}

static int ao_ioctl_noarg(unsigned long request)
{
    return ioctl(g_fd, request, 0);
}

static void disable_ao()
{
    if (g_fd < 0)
        return;
    (void)ao_ioctl_noarg(kIoctlDisableChn);
    (void)ao_ioctl_noarg(kIoctlDisableDev);
    close(g_fd);
    g_fd = -1;
}

static int send_pcm_block(int16_t *pcm)
{
    H3531AudioFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.enBitwidth = H3531_AUDIO_BIT_WIDTH_16;
    frame.enSoundmode = H3531_AUDIO_SOUND_MODE_MONO;
    frame.pVirAddr[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pcm));
    frame.u32Seq = g_seq++;
    frame.u32Len = kBytesPerBlock;

    const uint64_t before = monotonic_ns();
    const int rc = ao_ioctl(kIoctlSendFrame, &frame);
    const uint64_t after = monotonic_ns();
    g_last_send_rc = rc;

    if (before != 0 && after >= before) {
        const uint64_t elapsed = after - before;
        if (elapsed > g_send_max_ns)
            g_send_max_ns = elapsed;
        if (elapsed > 1000000ULL)
            ++g_send_over_1ms;
        if (elapsed > 3000000ULL)
            ++g_send_over_3ms;
        if (elapsed > 5000000ULL)
            ++g_send_over_5ms;
    }
    if (rc == 0)
        ++g_blocks_sent;
    return rc;
}

static void *audio_worker(void *)
{
    int16_t block[kSamplesPerBlock];

    /* Start with a small reservoir. After priming, never manufacture silence:
       FBZX naturally produces ~6 AO blocks per video frame, and the Hi3531
       driver owns its hardware/DMA queue. Between those bursts the worker waits
       for real PCM instead of filling AO with zeroes. */
    pthread_mutex_lock(&g_lock);
    while (g_running && g_queue_count < kStartBlocks)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);

    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_running && g_queue_count == 0) {
            ++g_queue_empty_waits;
            pthread_cond_wait(&g_cond, &g_lock);
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_lock);
            break;
        }

        memcpy(block, g_queue[g_read_index], sizeof(block));
        g_read_index = (g_read_index + 1U) % kQueueBlocks;
        --g_queue_count;
        pthread_mutex_unlock(&g_lock);

        const int rc = send_pcm_block(block);
        if (rc != 0) {
            fprintf(stderr,
                    "H3531 AO SendFrame failed rc=%d (0x%08x); disabling AO worker\n",
                    rc, static_cast<unsigned>(rc));
            pthread_mutex_lock(&g_lock);
            g_active = 0;
            g_running = 0;
            pthread_cond_broadcast(&g_cond);
            pthread_mutex_unlock(&g_lock);
            break;
        }
    }
    return 0;
}

static void queue_pcm_block(const int16_t *pcm)
{
    pthread_mutex_lock(&g_lock);

    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (g_queue_count == kQueueBlocks) {
        /* Keep latency bounded. Never block the emulator thread. */
        g_read_index = (g_read_index + 1U) % kQueueBlocks;
        --g_queue_count;
        ++g_overruns;
    }

    memcpy(g_queue[g_write_index], pcm, sizeof(g_queue[g_write_index]));
    g_write_index = (g_write_index + 1U) % kQueueBlocks;
    ++g_queue_count;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

} // namespace

extern "C" int h3531_audio_start(void)
{
    H3531AioAttr attr;
    int context = kContextIndex;

    if (g_active)
        return 0;

    g_fd = open("/dev/ao", O_RDWR);
    if (g_fd < 0)
        return -1;

    memset(&attr, 0, sizeof(attr));
    attr.enSamplerate = kSampleRate;
    attr.enBitwidth = H3531_AUDIO_BIT_WIDTH_16;
    attr.enWorkmode = H3531_AIO_MODE_I2S_MASTER;
    attr.enSoundmode = H3531_AUDIO_SOUND_MODE_MONO;
    attr.u32EXFlag = 0;
    attr.u32FrmNum = 30;
    attr.u32PtNumPerFrm = kSamplesPerBlock;
    attr.u32ChnCnt = 2;
    attr.u32ClkSel = 0;

    if (ao_ioctl(kIoctlInitContext, &context) != 0 ||
        ao_ioctl(kIoctlSetPubAttr, &attr) != 0 ||
        ao_ioctl_noarg(kIoctlEnableDev) != 0 ||
        ao_ioctl_noarg(kIoctlEnableChn) != 0) {
        disable_ao();
        return -1;
    }

    g_read_index = g_write_index = g_queue_count = 0;
    g_seq = 0;
    g_dc = 0.0f;
    g_blocks_sent = g_queue_empty_waits = g_overruns = 0;
    g_send_over_1ms = g_send_over_3ms = g_send_over_5ms = 0;
    g_send_max_ns = 0;
    g_last_send_rc = 0;
    g_frame_clock_valid = 0;
    g_running = 1;
    g_active = 1;

    if (pthread_create(&g_worker, 0, audio_worker, 0) != 0) {
        g_running = 0;
        g_active = 0;
        disable_ao();
        return -1;
    }
    g_worker_started = 1;

    fprintf(stderr,
            "H3531 AO buffered worker ready: 16x160 queue, 8-block prefill, frame-layout-v3, len=320B\n");
    return 0;
}

extern "C" void h3531_audio_submit_u8_mono(const uint8_t *samples, size_t count)
{
    int16_t block[kSamplesPerBlock];
    unsigned used = 0;

    if (!samples || !g_active)
        return;

    for (size_t i = 0; i < count; ++i) {
        const float original = static_cast<float>(samples[i]) - 128.0f;
        g_dc = (original + g_dc * 999.0f) * 0.001f + 1.0e-6f;
        float filtered = (original - g_dc) * 0.98f * 256.0f;
        if (filtered > 32767.0f)
            filtered = 32767.0f;
        if (filtered < -32768.0f)
            filtered = -32768.0f;
        block[used++] = static_cast<int16_t>(filtered);

        if (used == kSamplesPerBlock) {
            queue_pcm_block(block);
            used = 0;
        }
    }

    if (used != 0) {
        memset(block + used, 0, (kSamplesPerBlock - used) * sizeof(block[0]));
        queue_pcm_block(block);
    }
}

extern "C" void h3531_audio_pace_frame(int turbo)
{
    const uint64_t now = monotonic_ns();
    if (now == 0)
        return;

    if (turbo) {
        g_frame_clock_valid = 0;
        return;
    }

    if (!g_frame_clock_valid) {
        g_frame_deadline_ns = now + kFramePeriodNs;
        g_frame_clock_valid = 1;
        return;
    }

    /* Absolute deadlines avoid drift. If the board falls more than two frames
       behind, resynchronise instead of trying to catch up in a burst. */
    if (now + 2ULL * kFramePeriodNs < g_frame_deadline_ns ||
        now > g_frame_deadline_ns + 2ULL * kFramePeriodNs) {
        g_frame_deadline_ns = now + kFramePeriodNs;
        return;
    }

    if (now < g_frame_deadline_ns)
        sleep_until_ns(g_frame_deadline_ns);

    g_frame_deadline_ns += kFramePeriodNs;
}

extern "C" void h3531_audio_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);

    if (g_worker_started) {
        pthread_join(g_worker, 0);
        g_worker_started = 0;
    }

    if (g_fd >= 0)
        disable_ao();

    fprintf(stderr,
            "H3531 AO stats: blocks=%llu queue_empty_waits=%llu overruns=%llu "
            "send>1ms=%llu >3ms=%llu >5ms=%llu max_send_us=%llu last_rc=0x%08x\n",
            (unsigned long long)g_blocks_sent,
            (unsigned long long)g_queue_empty_waits,
            (unsigned long long)g_overruns,
            (unsigned long long)g_send_over_1ms,
            (unsigned long long)g_send_over_3ms,
            (unsigned long long)g_send_over_5ms,
            (unsigned long long)(g_send_max_ns / 1000ULL),
            static_cast<unsigned>(g_last_send_rc));

    g_active = 0;
    g_frame_clock_valid = 0;
}

extern "C" int h3531_audio_is_active(void)
{
    return g_active;
}
