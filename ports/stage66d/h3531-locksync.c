#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define H3531_MAX_EVENTS 32
#define BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define NBITS(x) (((x) + BITS_PER_LONG - 1U) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) (((arr)[(unsigned)(bit) / BITS_PER_LONG] >> ((unsigned)(bit) % BITS_PER_LONG)) & 1UL)

typedef struct { int caps; int num; } LockState;

static int keyboard_capable(int fd)
{
    unsigned long evbits[NBITS(EV_MAX + 1)];
    unsigned long keybits[NBITS(KEY_MAX + 1)];
    memset(evbits, 0, sizeof(evbits));
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return 0;
    if (!TEST_BIT(EV_KEY, evbits)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return 0;
    return TEST_BIT(KEY_A, keybits) &&
           TEST_BIT(KEY_ENTER, keybits) &&
           TEST_BIT(KEY_SPACE, keybits);
}

static int open_keyboard(char *path, size_t path_len)
{
    int i;
    for (i = 0; i < H3531_MAX_EVENTS; ++i) {
        int fd;
        char p[64];
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        if (keyboard_capable(fd)) {
            snprintf(path, path_len, "%s", p);
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int read_physical(LockState *st)
{
    unsigned long leds[NBITS(LED_MAX + 1)];
    char path[64];
    int fd = open_keyboard(path, sizeof(path));
    if (fd < 0) return -1;
    memset(leds, 0, sizeof(leds));
    if (ioctl(fd, EVIOCGLED(sizeof(leds)), leds) < 0) {
        close(fd);
        return -1;
    }
    st->caps = TEST_BIT(LED_CAPSL, leds) ? 1 : 0;
    st->num = TEST_BIT(LED_NUML, leds) ? 1 : 0;
    close(fd);
    return 0;
}

static int write_physical(const LockState *st)
{
    struct input_event ev;
    char path[64];
    int fd = open_keyboard(path, sizeof(path));
    if (fd < 0) return -1;

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_LED; ev.code = LED_CAPSL; ev.value = st->caps ? 1 : 0;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { close(fd); return -1; }

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_LED; ev.code = LED_NUML; ev.value = st->num ? 1 : 0;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { close(fd); return -1; }

    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    (void)write(fd, &ev, sizeof(ev));

    close(fd);
    return 0;
}

static unsigned int numlock_mask(Display *dpy)
{
    unsigned int mask = XkbKeysymToModifiers(dpy, XK_Num_Lock);
    return mask ? mask : Mod2Mask;
}

static int x_state(Display *dpy, LockState *st)
{
    XkbStateRec state;
    unsigned int num = numlock_mask(dpy);
    if (XkbGetState(dpy, XkbUseCoreKbd, &state) != Success) return -1;
    st->caps = (state.locked_mods & LockMask) ? 1 : 0;
    st->num = (state.locked_mods & num) ? 1 : 0;
    return 0;
}

static int apply_x(Display *dpy, const LockState *st)
{
    unsigned int num = numlock_mask(dpy);
    unsigned int affect = LockMask | num;
    unsigned int values = (st->caps ? LockMask : 0) | (st->num ? num : 0);
    if (!XkbLockModifiers(dpy, XkbUseCoreKbd, affect, values)) return -1;
    XSync(dpy, False);
    return 0;
}

static int save_state(const char *file, const LockState *st)
{
    char tmp[256];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "CAPS=%d\nNUM=%d\n", st->caps, st->num);
    if (fclose(f) != 0) return -1;
    if (rename(tmp, file) != 0) return -1;
    return 0;
}

static int load_state(const char *file, LockState *st)
{
    FILE *f = fopen(file, "r");
    char line[64];
    if (!f) return -1;
    st->caps = 0;
    st->num = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "CAPS=", 5)) st->caps = atoi(line + 5) ? 1 : 0;
        else if (!strncmp(line, "NUM=", 4)) st->num = atoi(line + 4) ? 1 : 0;
    }
    fclose(f);
    return 0;
}

static int mode_snapshot(const char *file)
{
    LockState st;
    if (read_physical(&st) != 0) {
        fprintf(stderr, "h3531-locksync: cannot read physical keyboard LEDs\n");
        return 2;
    }
    if (save_state(file, &st) != 0) return 3;
    fprintf(stderr, "h3531-locksync: snapshot caps=%d num=%d\n", st.caps, st.num);
    return 0;
}

static int mode_apply(const char *display_name, const char *file)
{
    LockState st;
    Display *dpy;
    if (load_state(file, &st) != 0) return 4;
    dpy = XOpenDisplay(display_name);
    if (!dpy) return 5;
    if (apply_x(dpy, &st) != 0) {
        XCloseDisplay(dpy);
        return 6;
    }
    (void)write_physical(&st);
    XCloseDisplay(dpy);
    fprintf(stderr, "h3531-locksync: applied caps=%d num=%d\n", st.caps, st.num);
    return 0;
}

static int mode_daemon(const char *display_name)
{
    Display *dpy = XOpenDisplay(display_name);
    LockState now, last;
    int have_last = 0;
    if (!dpy) return 7;

    for (;;) {
        if (x_state(dpy, &now) == 0) {
            if (!have_last || now.caps != last.caps || now.num != last.num) {
                if (write_physical(&now) == 0) {
                    fprintf(stderr, "h3531-locksync: desktop caps=%d num=%d\n", now.caps, now.num);
                    last = now;
                    have_last = 1;
                }
            }
        }
        usleep(150000);
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "snapshot")) return mode_snapshot(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "apply")) return mode_apply(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "daemon")) return mode_daemon(argv[2]);
    fprintf(stderr, "usage: %s snapshot FILE | apply DISPLAY FILE | daemon DISPLAY\n", argv[0]);
    return 64;
}
