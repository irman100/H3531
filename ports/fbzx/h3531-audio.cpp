#include "h3531-audio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Hi3531 MPP V1.0.D.1 AO5/ch0 backend.
 *
 * v14 performance architecture:
 *   - FBZX/emulation thread only accumulates raw U8 PCM.
 *   - Six 160-sample AO blocks (~20 ms at 48 kHz) are committed to the
 *     cross-thread ring under ONE mutex acquisition and ONE worker wakeup.
 *   - AO worker converts U8 -> S16 and owns every SendFrame ioctl.
 *   - AO ABI remains the physically proven 160 samples / 320 bytes per frame.
 *   - frame clock telemetry reports both deadline misses and actual wall FPS.
 */

namespace {

static const int kContextIndex = 80; /* AOdev 5 * 16 + channel 0 */
static const unsigned kSampleRate = 48000;
static const unsigned kSamplesPerBlock = 160;
static const unsigned kBytesPerAoBlock = kSamplesPerBlock * sizeof(int16_t);
static const unsigned kProducerBatchBlocks = 6;
static const unsigned kProducerBatchSamples = kSamplesPerBlock * kProducerBatchBlocks; /* 960 */
static const unsigned kQueueBlocks = 16;
static const unsigned kStartBlocks = kProducerBatchBlocks; /* one ~20 ms batch */
static const unsigned kPerfWindowFrames = 250;
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

struct H3531AudioFrame {
    int32_t enBitwidth;       /* 0x00 */
    int32_t enSoundmode;      /* 0x04 */
    uint32_t pVirAddr[2];     /* 0x08 */
    uint32_t u32PhyAddr[2];   /* 0x10 */
    uint64_t u64TimeStamp;    /* 0x18 */
    uint32_t u32Seq;          /* 0x20 */
    uint32_t u32Len;          /* 0x24: bytes */
    uint32_t u32PoolId[2];    /* 0x28 */
};

typedef char h3531_attr_size_must_be_36[(sizeof(H3531AioAttr) == 36) ? 1 : -1];
typedef char h3531_frame_size_must_be_48[(sizeof(H3531AudioFrame) == 48) ? 1 : -1];
typedef char h3531_frame_vir0_offset_must_be_8[(offsetof(H3531AudioFrame, pVirAddr[0]) == 8) ? 1 : -1];
typedef char h3531_frame_len_offset_must_be_36[(offsetof(H3531AudioFrame, u32Len) == 36) ? 1 : -1];
typedef char h3531_frame_pool_offset_must_be_40[(offsetof(H3531AudioFrame, u32PoolId[0]) == 40) ? 1 : -1];

static int g_fd = -1;
static int g_active = 0;
static int g_running = 0;
static int g_worker_started = 0;
static pthread_t g_worker;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

static uint8_t g_queue[kQueueBlocks][kSamplesPerBlock];
static unsigned g_read_index = 0;
static unsigned g_write_index = 0;
static unsigned g_queue_count = 0;
static uint32_t g_seq = 0;

/* Main-thread staging buffer. This removes five of six mutex/cond handoffs
   that v11-v13 performed for each Spectrum video frame. */
static uint8_t g_producer_batch[kProducerBatchSamples];
static unsigned g_producer_fill = 0;

/* Worker-only fixed-point DC estimate (Q16). */
static int32_t g_dc_q16 = 0;
static int g_dc_valid = 0;

/* AO / queue diagnostics. */
static volatile uint32_t g_blocks_sent = 0;
static volatile uint32_t g_queue_empty_waits = 0;
static volatile uint32_t g_overruns = 0;
static volatile uint32_t g_batches_queued = 0;
static volatile uint32_t g_submit_calls = 0;
static volatile uint32_t g_send_over_1ms = 0;
static volatile uint32_t g_send_over_3ms = 0;
static volatile uint32_t g_send_over_5ms = 0;
static volatile uint32_t g_send_max_us = 0;
static volatile uint32_t g_last_send_rc = 0;

/* Main-thread frame-clock telemetry. */
static uint64_t g_frame_deadline_ns = 0;
static int g_frame_clock_valid = 0;
static uint64_t g_perf_window_start_ns = 0;
static uint32_t g_perf_frames = 0;
static uint32_t g_perf_late = 0;
static uint32_t g_perf_late_1ms = 0;
static uint32_t g_perf_late_3ms = 0;
static uint32_t g_perf_late_5ms = 0;
static uint32_t g_perf_resync = 0;
static uint32_t g_perf_max_late_us = 0;

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
        const uint64_t left = deadline - now;
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

static void convert_u8_to_s16(const uint8_t *src, int16_t *dst)
{
    for (unsigned i = 0; i < kSamplesPerBlock; ++i) {
        const int32_t x_q16 = (static_cast<int32_t>(src[i]) - 128) << 16;
        if (!g_dc_valid) {
            g_dc_q16 = x_q16;
            g_dc_valid = 1;
        }
        g_dc_q16 += (x_q16 - g_dc_q16) >> 10;
        int32_t y = (x_q16 - g_dc_q16) >> 8;
        y = (y * 251) >> 8;
        if (y > 32767)
            y = 32767;
        else if (y < -32768)
            y = -32768;
        dst[i] = static_cast<int16_t>(y);
    }
}

static int send_pcm_block(int16_t *pcm)
{
    H3531AudioFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.enBitwidth = H3531_AUDIO_BIT_WIDTH_16;
    frame.enSoundmode = H3531_AUDIO_SOUND_MODE_MONO;
    frame.pVirAddr[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pcm));
    frame.u32Seq = g_seq++;
    frame.u32Len = kBytesPerAoBlock;

    const uint64_t before = monotonic_ns();
    const int rc = ao_ioctl(kIoctlSendFrame, &frame);
    const uint64_t after = monotonic_ns();
    g_last_send_rc = static_cast<uint32_t>(rc);

    if (before != 0 && after >= before) {
        const uint64_t elapsed_us64 = (after - before) / 1000ULL;
        const uint32_t elapsed_us = elapsed_us64 > 0xffffffffULL
            ? 0xffffffffU : static_cast<uint32_t>(elapsed_us64);
        if (elapsed_us > g_send_max_us)
            g_send_max_us = elapsed_us;
        if (elapsed_us > 1000U)
            ++g_send_over_1ms;
        if (elapsed_us > 3000U)
            ++g_send_over_3ms;
        if (elapsed_us > 5000U)
            ++g_send_over_5ms;
    }
    if (rc == 0)
        ++g_blocks_sent;
    return rc;
}

static void try_pin_worker_to_cpu1()
{
#if defined(__linux__)
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 2)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(1, &set);
    if (sched_setaffinity(0, sizeof(set), &set) == 0)
        fprintf(stderr, "H3531 AO worker pinned to CPU1\n");
    else
        fprintf(stderr, "H3531 AO worker CPU1 affinity unavailable errno=%d\n", errno);
#endif
}

static void *audio_worker(void *)
{
    uint8_t u8_block[kSamplesPerBlock];
    int16_t s16_block[kSamplesPerBlock];

    try_pin_worker_to_cpu1();

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

        memcpy(u8_block, g_queue[g_read_index], sizeof(u8_block));
        g_read_index = (g_read_index + 1U) % kQueueBlocks;
        --g_queue_count;
        pthread_mutex_unlock(&g_lock);

        convert_u8_to_s16(u8_block, s16_block);
        const int rc = send_pcm_block(s16_block);
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

static void queue_batch_6x160(const uint8_t *samples)
{
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    for (unsigned block = 0; block < kProducerBatchBlocks; ++block) {
        if (g_queue_count == kQueueBlocks) {
            g_read_index = (g_read_index + 1U) % kQueueBlocks;
            --g_queue_count;
            ++g_overruns;
        }
        memcpy(g_queue[g_write_index],
               samples + block * kSamplesPerBlock,
               kSamplesPerBlock);
        g_write_index = (g_write_index + 1U) % kQueueBlocks;
        ++g_queue_count;
    }

    ++g_batches_queued;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

static void reset_perf_window()
{
    g_perf_frames = 0;
    g_perf_late = 0;
    g_perf_late_1ms = 0;
    g_perf_late_3ms = 0;
    g_perf_late_5ms = 0;
    g_perf_resync = 0;
    g_perf_max_late_us = 0;
    g_perf_window_start_ns = monotonic_ns();
}

static void print_perf_window()
{
    unsigned q = 0;
    pthread_mutex_lock(&g_lock);
    q = g_queue_count;
    pthread_mutex_unlock(&g_lock);

    const uint64_t end_ns = monotonic_ns();
    uint32_t wall_ms = 0;
    uint32_t fps_x100 = 0;
    if (end_ns != 0 && g_perf_window_start_ns != 0 && end_ns > g_perf_window_start_ns) {
        const uint64_t elapsed_ns = end_ns - g_perf_window_start_ns;
        const uint64_t wall_ms64 = elapsed_ns / 1000000ULL;
        const uint64_t fps100_64 =
            (static_cast<uint64_t>(g_perf_frames) * 100ULL * 1000000000ULL) / elapsed_ns;
        wall_ms = wall_ms64 > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(wall_ms64);
        fps_x100 = fps100_64 > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(fps100_64);
    }

    fprintf(stderr,
            "H3531 perf14: frames=%u wall_ms=%u fps_x100=%u late=%u "
            ">1ms=%u >3ms=%u >5ms=%u max_late_us=%u resync=%u q=%u "
            "ov=%u empty=%u batches=%u submits=%u ao_blocks=%u "
            "sendmax_us=%u last_rc=0x%08x\n",
            g_perf_frames,
            wall_ms,
            fps_x100,
            g_perf_late,
            g_perf_late_1ms,
            g_perf_late_3ms,
            g_perf_late_5ms,
            g_perf_max_late_us,
            g_perf_resync,
            q,
            static_cast<unsigned>(g_overruns),
            static_cast<unsigned>(g_queue_empty_waits),
            static_cast<unsigned>(g_batches_queued),
            static_cast<unsigned>(g_submit_calls),
            static_cast<unsigned>(g_blocks_sent),
            static_cast<unsigned>(g_send_max_us),
            static_cast<unsigned>(g_last_send_rc));
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
    g_producer_fill = 0;
    g_seq = 0;
    g_dc_q16 = 0;
    g_dc_valid = 0;
    g_blocks_sent = 0;
    g_queue_empty_waits = 0;
    g_overruns = 0;
    g_batches_queued = 0;
    g_submit_calls = 0;
    g_send_over_1ms = 0;
    g_send_over_3ms = 0;
    g_send_over_5ms = 0;
    g_send_max_us = 0;
    g_last_send_rc = 0;
    g_frame_clock_valid = 0;
    reset_perf_window();
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
            "H3531 AO buffered worker ready: batch-v14 960U8 -> 6x160 AO, "
            "queue=16 blocks, prefill=6, frame-layout-v3, len=320B\n");
    return 0;
}

extern "C" void h3531_audio_submit_u8_mono(const uint8_t *samples, size_t count)
{
    if (!samples || !g_active)
        return;

    ++g_submit_calls;
    size_t pos = 0;
    while (pos < count) {
        const size_t space = kProducerBatchSamples - g_producer_fill;
        const size_t remain = count - pos;
        const size_t take = remain < space ? remain : space;
        memcpy(g_producer_batch + g_producer_fill, samples + pos, take);
        g_producer_fill += static_cast<unsigned>(take);
        pos += take;

        if (g_producer_fill == kProducerBatchSamples) {
            queue_batch_6x160(g_producer_batch);
            g_producer_fill = 0;
        }
    }
}

extern "C" void h3531_audio_pace_frame(int turbo)
{
    const uint64_t now = monotonic_ns();
    if (now == 0)
        return;

    if (turbo) {
        g_frame_clock_valid = 0;
        reset_perf_window();
        return;
    }

    if (!g_frame_clock_valid) {
        g_frame_deadline_ns = now + kFramePeriodNs;
        g_frame_clock_valid = 1;
        reset_perf_window();
        return;
    }

    ++g_perf_frames;

    if (now > g_frame_deadline_ns) {
        const uint64_t late_us64 = (now - g_frame_deadline_ns) / 1000ULL;
        const uint32_t late_us = late_us64 > 0xffffffffULL
            ? 0xffffffffU : static_cast<uint32_t>(late_us64);
        ++g_perf_late;
        if (late_us > 1000U)
            ++g_perf_late_1ms;
        if (late_us > 3000U)
            ++g_perf_late_3ms;
        if (late_us > 5000U)
            ++g_perf_late_5ms;
        if (late_us > g_perf_max_late_us)
            g_perf_max_late_us = late_us;
    }

    if (now + 2ULL * kFramePeriodNs < g_frame_deadline_ns ||
        now > g_frame_deadline_ns + 2ULL * kFramePeriodNs) {
        ++g_perf_resync;
        g_frame_deadline_ns = now + kFramePeriodNs;
    } else {
        if (now < g_frame_deadline_ns)
            sleep_until_ns(g_frame_deadline_ns);
        g_frame_deadline_ns += kFramePeriodNs;
    }

    if (g_perf_frames >= kPerfWindowFrames) {
        print_perf_window();
        reset_perf_window();
    }
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

    if (g_perf_frames != 0)
        print_perf_window();

    fprintf(stderr,
            "H3531 AO stats14: blocks=%u empty=%u overruns=%u batches=%u "
            "submits=%u send>1ms=%u >3ms=%u >5ms=%u max_send_us=%u "
            "last_rc=0x%08x\n",
            static_cast<unsigned>(g_blocks_sent),
            static_cast<unsigned>(g_queue_empty_waits),
            static_cast<unsigned>(g_overruns),
            static_cast<unsigned>(g_batches_queued),
            static_cast<unsigned>(g_submit_calls),
            static_cast<unsigned>(g_send_over_1ms),
            static_cast<unsigned>(g_send_over_3ms),
            static_cast<unsigned>(g_send_over_5ms),
            static_cast<unsigned>(g_send_max_us),
            static_cast<unsigned>(g_last_send_rc));

    g_active = 0;
    g_frame_clock_valid = 0;
    g_producer_fill = 0;
}

extern "C" int h3531_audio_is_active(void)
{
    return g_active;
}
