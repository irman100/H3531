#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/ioctl.h>

typedef unsigned char hi_u8;
typedef enum { HI_FALSE = 0, HI_TRUE = 1 } hi_bool;

typedef struct {
    hi_bool alpha_en;
    hi_bool alpha_chn_en;
    hi_u8 alpha0;
    hi_u8 alpha1;
    hi_u8 global_alpha;
    hi_u8 reserved;
} hi_fb_alpha;

#define IOC_TYPE_HIFB 'F'
#define FBIOGET_ALPHA_HIFB _IOR(IOC_TYPE_HIFB, 92, hi_fb_alpha)
#define FBIOPUT_ALPHA_HIFB _IOW(IOC_TYPE_HIFB, 93, hi_fb_alpha)
#define H3531_FBSTATE_MAGIC 0x36364346U
#define H3531_FBSTATE_VERSION 1U

struct h3531_fbstate {
    uint32_t magic;
    uint32_t version;
    struct fb_var_screeninfo var;
    hi_fb_alpha alpha;
};

static int save_state(const char *dev, const char *path)
{
    int fd;
    FILE *f;
    struct h3531_fbstate st;

    memset(&st, 0, sizeof(st));
    st.magic = H3531_FBSTATE_MAGIC;
    st.version = H3531_FBSTATE_VERSION;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 2;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &st.var) < 0) {
        fprintf(stderr, "FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        close(fd);
        return 3;
    }
    if (ioctl(fd, FBIOGET_ALPHA_HIFB, &st.alpha) < 0) {
        fprintf(stderr, "FBIOGET_ALPHA_HIFB failed: %s\n", strerror(errno));
        close(fd);
        return 4;
    }
    close(fd);

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return 5;
    }
    if (fwrite(&st, 1, sizeof(st), f) != sizeof(st)) {
        fprintf(stderr, "short write to %s\n", path);
        fclose(f);
        return 6;
    }
    fclose(f);

    printf("saved %ux%u bpp=%u alpha=%u/%u global=%u\n",
           st.var.xres, st.var.yres, st.var.bits_per_pixel,
           (unsigned)st.alpha.alpha0, (unsigned)st.alpha.alpha1,
           (unsigned)st.alpha.global_alpha);
    return 0;
}

static int restore_state(const char *dev, const char *path)
{
    int fd;
    FILE *f;
    struct h3531_fbstate st;

    memset(&st, 0, sizeof(st));
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return 7;
    }
    if (fread(&st, 1, sizeof(st), f) != sizeof(st)) {
        fprintf(stderr, "short read from %s\n", path);
        fclose(f);
        return 8;
    }
    fclose(f);

    if (st.magic != H3531_FBSTATE_MAGIC || st.version != H3531_FBSTATE_VERSION) {
        fprintf(stderr, "invalid framebuffer state file\n");
        return 9;
    }

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 10;
    }

    if (ioctl(fd, FBIOPUT_VSCREENINFO, &st.var) < 0) {
        fprintf(stderr, "FBIOPUT_VSCREENINFO failed: %s\n", strerror(errno));
        close(fd);
        return 11;
    }

    if (ioctl(fd, FBIOPUT_ALPHA_HIFB, &st.alpha) < 0) {
        fprintf(stderr, "FBIOPUT_ALPHA_HIFB failed: %s\n", strerror(errno));
        close(fd);
        return 12;
    }

    close(fd);
    unlink(path);
    printf("restored %ux%u bpp=%u alpha=%u/%u global=%u\n",
           st.var.xres, st.var.yres, st.var.bits_per_pixel,
           (unsigned)st.alpha.alpha0, (unsigned)st.alpha.alpha1,
           (unsigned)st.alpha.global_alpha);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev;
    const char *path;

    if (argc != 4) {
        fprintf(stderr, "usage: %s save|restore /dev/fb0 state-file\n", argv[0]);
        return 64;
    }

    dev = argv[2];
    path = argv[3];

    if (!strcmp(argv[1], "save"))
        return save_state(dev, path);
    if (!strcmp(argv[1], "restore"))
        return restore_state(dev, path);

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 64;
}
