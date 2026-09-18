#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>

typedef unsigned char hi_u8;
typedef enum {
    HI_FALSE = 0,
    HI_TRUE = 1
} hi_bool;

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

static void print_alpha(const char *tag, const hi_fb_alpha *a)
{
    printf("%s alpha_en=%d alpha_chn_en=%d alpha0=%u alpha1=%u global=%u reserved=%u\n",
           tag,
           (int)a->alpha_en,
           (int)a->alpha_chn_en,
           (unsigned)a->alpha0,
           (unsigned)a->alpha1,
           (unsigned)a->global_alpha,
           (unsigned)a->reserved);
}

static int get_alpha(int fd, hi_fb_alpha *a)
{
    memset(a, 0, sizeof(*a));
    if (ioctl(fd, FBIOGET_ALPHA_HIFB, a) < 0) {
        fprintf(stderr, "FBIOGET_ALPHA_HIFB failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int put_alpha(int fd, const hi_fb_alpha *a)
{
    hi_fb_alpha tmp = *a;
    if (ioctl(fd, FBIOPUT_ALPHA_HIFB, &tmp) < 0) {
        fprintf(stderr, "FBIOPUT_ALPHA_HIFB failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int save_alpha(const char *path, const hi_fb_alpha *a)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fwrite(a, 1, sizeof(*a), f) != sizeof(*a)) {
        fprintf(stderr, "short write to %s\n", path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int load_alpha(const char *path, hi_fb_alpha *a)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fread(a, 1, sizeof(*a), f) != sizeof(*a)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int do_restore(int fd, const char *path, int quiet_missing)
{
    hi_fb_alpha saved, verify;

    if (load_alpha(path, &saved) < 0) {
        if (!quiet_missing)
            fprintf(stderr, "no valid saved alpha state at %s\n", path);
        return quiet_missing ? 0 : 2;
    }

    print_alpha("restore-request", &saved);
    if (put_alpha(fd, &saved) < 0)
        return 3;
    if (get_alpha(fd, &verify) < 0)
        return 4;
    print_alpha("restore-verify", &verify);
    unlink(path);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/fb0";
    const char *save = "/var/h3531-hifb-alpha.saved";
    int fd;
    hi_fb_alpha cur, next, verify;

    if (argc < 2) {
        fprintf(stderr, "usage: %s abi|status|push-opaque|restore|restore-if-saved [device] [savefile]\n", argv[0]);
        return 64;
    }

    if (!strcmp(argv[1], "abi")) {
        printf("sizeof(hi_bool)=%u sizeof(hi_fb_alpha)=%u GET=0x%08lx PUT=0x%08lx\n",
               (unsigned)sizeof(hi_bool),
               (unsigned)sizeof(hi_fb_alpha),
               (unsigned long)FBIOGET_ALPHA_HIFB,
               (unsigned long)FBIOPUT_ALPHA_HIFB);
        return sizeof(hi_fb_alpha) == 12 ? 0 : 65;
    }
    if (argc >= 3)
        dev = argv[2];
    if (argc >= 4)
        save = argv[3];

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 1;
    }

    if (!strcmp(argv[1], "status")) {
        if (get_alpha(fd, &cur) < 0) {
            close(fd);
            return 2;
        }
        print_alpha("current", &cur);
        close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "restore") || !strcmp(argv[1], "restore-if-saved")) {
        int rc = do_restore(fd, save, !strcmp(argv[1], "restore-if-saved"));
        close(fd);
        return rc;
    }

    if (strcmp(argv[1], "push-opaque")) {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        close(fd);
        return 64;
    }

    /* Recover from an interrupted prior session before saving a new baseline. */
    if (access(save, R_OK) == 0) {
        fprintf(stderr, "saved alpha state already exists; restoring it first\n");
        if (do_restore(fd, save, 0) != 0) {
            close(fd);
            return 5;
        }
    }

    if (get_alpha(fd, &cur) < 0) {
        close(fd);
        return 6;
    }
    print_alpha("before", &cur);

    if (save_alpha(save, &cur) < 0) {
        close(fd);
        return 7;
    }

    next = cur;
    next.alpha_en = HI_TRUE;
    next.alpha_chn_en = HI_TRUE;
    next.alpha0 = 255;
    next.alpha1 = 255;
    next.global_alpha = 255;

    print_alpha("set-request", &next);
    if (put_alpha(fd, &next) < 0) {
        unlink(save);
        close(fd);
        return 8;
    }

    if (get_alpha(fd, &verify) < 0) {
        fprintf(stderr, "verification GET failed; restoring saved alpha state\n");
        do_restore(fd, save, 0);
        close(fd);
        return 9;
    }
    print_alpha("set-verify", &verify);

    if (verify.alpha_en != HI_TRUE ||
        verify.alpha_chn_en != HI_TRUE ||
        verify.alpha0 != 255 ||
        verify.alpha1 != 255 ||
        verify.global_alpha != 255) {
        fprintf(stderr, "HIFB driver did not accept requested opaque alpha state; restoring\n");
        do_restore(fd, save, 0);
        close(fd);
        return 10;
    }

    close(fd);
    return 0;
}
