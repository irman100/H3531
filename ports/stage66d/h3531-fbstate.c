#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
#define H3531_FBSTATE_MAGIC 0x36364446U
#define H3531_FBSTATE_VERSION 2U

struct h3531_fbstate {
    uint32_t magic;
    uint32_t version;
    uint32_t smem_len;
    uint32_t reserved;
    struct fb_var_screeninfo var;
    hi_fb_alpha alpha;
};

static int save_state(const char *dev, const char *path)
{
    int fd;
    FILE *f;
    void *map;
    struct fb_fix_screeninfo fix;
    struct h3531_fbstate st;

    memset(&st, 0, sizeof(st));
    memset(&fix, 0, sizeof(fix));
    st.magic = H3531_FBSTATE_MAGIC;
    st.version = H3531_FBSTATE_VERSION;

    fd = open(dev, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno)); return 2; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &st.var) < 0) { close(fd); return 3; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) { close(fd); return 4; }
    if (ioctl(fd, FBIOGET_ALPHA_HIFB, &st.alpha) < 0) { close(fd); return 5; }
    st.smem_len = fix.smem_len;
    if (!st.smem_len) { close(fd); return 6; }

    map = mmap(NULL, st.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return 7; }
    f = fopen(path, "wb");
    if (!f) { munmap(map, st.smem_len); close(fd); return 8; }
    if (fwrite(&st, 1, sizeof(st), f) != sizeof(st) ||
        fwrite(map, 1, st.smem_len, f) != st.smem_len) {
        fclose(f); munmap(map, st.smem_len); close(fd); return 9;
    }
    fclose(f);
    munmap(map, st.smem_len);
    close(fd);
    printf("saved %ux%u bpp=%u alpha=%u/%u global=%u pixels=%u\n",
           st.var.xres, st.var.yres, st.var.bits_per_pixel,
           (unsigned)st.alpha.alpha0, (unsigned)st.alpha.alpha1,
           (unsigned)st.alpha.global_alpha, (unsigned)st.smem_len);
    return 0;
}

static int restore_state(const char *dev, const char *path)
{
    int fd;
    FILE *f;
    void *map;
    size_t copy_len;
    struct fb_fix_screeninfo fix;
    struct h3531_fbstate st;

    memset(&st, 0, sizeof(st));
    memset(&fix, 0, sizeof(fix));
    f = fopen(path, "rb");
    if (!f) return 10;
    if (fread(&st, 1, sizeof(st), f) != sizeof(st)) { fclose(f); return 11; }
    if (st.magic != H3531_FBSTATE_MAGIC || st.version != H3531_FBSTATE_VERSION || !st.smem_len) { fclose(f); return 12; }

    fd = open(dev, O_RDWR);
    if (fd < 0) { fclose(f); return 13; }
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &st.var) < 0) { fclose(f); close(fd); return 14; }
    if (ioctl(fd, FBIOPUT_ALPHA_HIFB, &st.alpha) < 0) { fclose(f); close(fd); return 15; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) { fclose(f); close(fd); return 16; }

    copy_len = st.smem_len < fix.smem_len ? st.smem_len : fix.smem_len;
    map = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { fclose(f); close(fd); return 17; }
    if (fread(map, 1, copy_len, f) != copy_len) {
        munmap(map, fix.smem_len); fclose(f); close(fd); return 18;
    }
    msync(map, copy_len, MS_SYNC);
    munmap(map, fix.smem_len);
    fclose(f);
    close(fd);
    unlink(path);
    printf("restored %ux%u bpp=%u alpha=%u/%u global=%u pixels=%u\n",
           st.var.xres, st.var.yres, st.var.bits_per_pixel,
           (unsigned)st.alpha.alpha0, (unsigned)st.alpha.alpha1,
           (unsigned)st.alpha.global_alpha, (unsigned)copy_len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s save|restore /dev/fb0 state-file\n", argv[0]);
        return 64;
    }
    if (!strcmp(argv[1], "save")) return save_state(argv[2], argv[3]);
    if (!strcmp(argv[1], "restore")) return restore_state(argv[2], argv[3]);
    return 64;
}
