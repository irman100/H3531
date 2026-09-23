#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-kdrive-evdev-hotplug.py EVDEV_C")

p = Path(sys.argv[1])
s = p.read_text()

inc_anchor = '#include <errno.h>\n'
inc_add = '''#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "os.h"
'''
if inc_anchor not in s:
    raise SystemExit("include anchor not found")
s = s.replace(inc_anchor, inc_add, 1)

struct_old = '''    int fd;
} Kevdev;
'''
struct_new = '''    int fd;
    OsTimerPtr reconnect_timer;
} Kevdev;

#define H3531_EVDEV_RECONNECT_MS 500
#define H3531_EVDEV_SCAN_MAX 32

static void EvdevPtrRead(int evdevPort, void *closure);
static void EvdevKbdRead(int evdevPort, void *closure);

static int
H3531FindEvdev(Bool keyboard, char *path, size_t path_len)
{
    int i;

    for (i = 0; i < H3531_EVDEV_SCAN_MAX; i++) {
        int fd;
        char candidate[64];
        unsigned long evbits[NBITS(EV_MAX + 1)];

        memset(evbits, 0, sizeof(evbits));
        snprintf(candidate, sizeof(candidate), "/dev/input/event%d", i);

        fd = open(candidate, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }

        if (keyboard) {
            unsigned long keybits[NBITS(KEY_MAX + 1)];

            if (!ISBITSET(evbits, EV_KEY)) {
                close(fd);
                continue;
            }

            memset(keybits, 0, sizeof(keybits));
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
                close(fd);
                continue;
            }

            /*
             * Composite USB keyboards often expose a second event node for
             * media keys.  Require ordinary typing keys so we select the main
             * keyboard interface instead of that secondary node.
             */
            if (!ISBITSET(keybits, KEY_A) ||
                !ISBITSET(keybits, KEY_ENTER) ||
                !ISBITSET(keybits, KEY_SPACE)) {
                close(fd);
                continue;
            }
        }
        else {
            unsigned long relbits[NBITS(REL_MAX + 1)];

            if (!ISBITSET(evbits, EV_REL)) {
                close(fd);
                continue;
            }

            memset(relbits, 0, sizeof(relbits));
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0) {
                close(fd);
                continue;
            }

            if (!ISBITSET(relbits, REL_X) || !ISBITSET(relbits, REL_Y)) {
                close(fd);
                continue;
            }
        }

        strncpy(path, candidate, path_len - 1);
        path[path_len - 1] = '\\0';
        return fd;
    }

    return -1;
}

static Bool
H3531AttachPointer(KdPointerInfo *pi)
{
    Kevdev *ke;
    char path[64];
    int fd;

    if (!pi || !pi->driverPrivate)
        return FALSE;

    ke = pi->driverPrivate;
    fd = H3531FindEvdev(FALSE, path, sizeof(path));
    if (fd < 0)
        return FALSE;

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        perror("[H3531] grabbing reconnected evdev mouse failed");

    if (!KdRegisterFd(fd, EvdevPtrRead, pi)) {
        ioctl(fd, EVIOCGRAB, 0);
        close(fd);
        return FALSE;
    }

    ke->fd = fd;
    pi->path = strdup(path);
    ErrorF("[H3531] Stage6.6D5 evdev mouse reconnected: %s\\n", path);
    return TRUE;
}

static Bool
H3531AttachKeyboard(KdKeyboardInfo *ki)
{
    Kevdev *ke;
    char path[64];
    int fd;

    if (!ki || !ki->driverPrivate)
        return FALSE;

    ke = ki->driverPrivate;
    fd = H3531FindEvdev(TRUE, path, sizeof(path));
    if (fd < 0)
        return FALSE;

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        perror("[H3531] grabbing reconnected evdev keyboard failed");

    if (!KdRegisterFd(fd, EvdevKbdRead, ki)) {
        ioctl(fd, EVIOCGRAB, 0);
        close(fd);
        return FALSE;
    }

    ke->fd = fd;
    ki->path = strdup(path);
    ErrorF("[H3531] Stage6.6D5 evdev keyboard reconnected: %s\\n", path);
    return TRUE;
}

static CARD32
H3531ReconnectPointer(OsTimerPtr timer, CARD32 now, pointer arg)
{
    KdPointerInfo *pi = arg;
    Kevdev *ke;

    (void)now;

    if (!pi || !pi->driverPrivate)
        return 0;

    ke = pi->driverPrivate;
    ke->reconnect_timer = timer;

    if (ke->fd >= 0 || H3531AttachPointer(pi))
        return 0;

    return H3531_EVDEV_RECONNECT_MS;
}

static CARD32
H3531ReconnectKeyboard(OsTimerPtr timer, CARD32 now, pointer arg)
{
    KdKeyboardInfo *ki = arg;
    Kevdev *ke;

    (void)now;

    if (!ki || !ki->driverPrivate)
        return 0;

    ke = ki->driverPrivate;
    ke->reconnect_timer = timer;

    if (ke->fd >= 0 || H3531AttachKeyboard(ki))
        return 0;

    return H3531_EVDEV_RECONNECT_MS;
}

static void
H3531DisconnectPointer(KdPointerInfo *pi, int fd)
{
    Kevdev *ke;

    if (!pi || !pi->driverPrivate)
        return;

    ke = pi->driverPrivate;
    if (ke->fd != fd)
        return;

    ioctl(fd, EVIOCGRAB, 0);
    KdUnregisterFd(pi, fd, TRUE);
    ke->fd = -1;

    ke->reconnect_timer = TimerSet(ke->reconnect_timer, 0,
                                   H3531_EVDEV_RECONNECT_MS,
                                   H3531ReconnectPointer, pi);
    ErrorF("[H3531] Stage6.6D5 evdev mouse disconnected; waiting for replug\\n");
}

static void
H3531DisconnectKeyboard(KdKeyboardInfo *ki, int fd)
{
    Kevdev *ke;

    if (!ki || !ki->driverPrivate)
        return;

    ke = ki->driverPrivate;
    if (ke->fd != fd)
        return;

    ioctl(fd, EVIOCGRAB, 0);
    KdUnregisterFd(ki, fd, TRUE);
    ke->fd = -1;

    ke->reconnect_timer = TimerSet(ke->reconnect_timer, 0,
                                   H3531_EVDEV_RECONNECT_MS,
                                   H3531ReconnectKeyboard, ki);
    ErrorF("[H3531] Stage6.6D5 evdev keyboard disconnected; waiting for replug\\n");
}
'''
if struct_old not in s:
    raise SystemExit("Kevdev struct anchor not found")
s = s.replace(struct_old, struct_new, 1)

ptr_old = '''    n = read(evdevPort, &events, NUM_EVENTS * sizeof(struct input_event));
    if (n <= 0) {
        if (errno == ENODEV)
            DeleteInputDeviceRequest(pi->dixdev);
        return;
    }
'''
ptr_new = '''    n = read(evdevPort, &events, NUM_EVENTS * sizeof(struct input_event));
    if (n <= 0) {
        if (n == 0 || errno == ENODEV || errno == ENXIO ||
            errno == EIO || errno == EBADF)
            H3531DisconnectPointer(pi, evdevPort);
        return;
    }
'''
if ptr_old not in s:
    raise SystemExit("pointer read disconnect anchor not found")
s = s.replace(ptr_old, ptr_new, 1)

kbd_old = '''    n = read(evdevPort, &events, NUM_EVENTS * sizeof(struct input_event));
    if (n <= 0) {
        if (errno == ENODEV)
            DeleteInputDeviceRequest(ki->dixdev);
        return;
    }
'''
kbd_new = '''    n = read(evdevPort, &events, NUM_EVENTS * sizeof(struct input_event));
    if (n <= 0) {
        if (n == 0 || errno == ENODEV || errno == ENXIO ||
            errno == EIO || errno == EBADF)
            H3531DisconnectKeyboard(ki, evdevPort);
        return;
    }
'''
if kbd_old not in s:
    raise SystemExit("keyboard read disconnect anchor not found")
s = s.replace(kbd_old, kbd_new, 1)

# Initial enable: make the disconnected state explicit.
s = s.replace('''    pi->driverPrivate = ke;
    ke->fd = fd;

    return Success;
''','''    pi->driverPrivate = ke;
    ke->fd = fd;
    ke->reconnect_timer = NULL;

    return Success;
''',1)

s = s.replace('''    ki->driverPrivate = ke;
    ke->fd = fd;

    return Success;
''','''    ki->driverPrivate = ke;
    ke->fd = fd;
    ke->reconnect_timer = NULL;

    return Success;
''',1)

ptr_disable_old = '''static void
EvdevPtrDisable(KdPointerInfo * pi)
{
    Kevdev *ke;

    ke = pi->driverPrivate;

    if (!pi || !pi->driverPrivate)
        return;

    KdUnregisterFd(pi, ke->fd, TRUE);

    if (ioctl(ke->fd, EVIOCGRAB, 0) < 0)
        perror("Ungrabbing evdev mouse device failed");

    free(ke);
    pi->driverPrivate = 0;
}
'''
ptr_disable_new = '''static void
EvdevPtrDisable(KdPointerInfo * pi)
{
    Kevdev *ke;

    if (!pi || !pi->driverPrivate)
        return;

    ke = pi->driverPrivate;

    if (ke->reconnect_timer) {
        TimerFree(ke->reconnect_timer);
        ke->reconnect_timer = NULL;
    }

    if (ke->fd >= 0) {
        if (ioctl(ke->fd, EVIOCGRAB, 0) < 0 && errno != ENODEV)
            perror("Ungrabbing evdev mouse device failed");
        KdUnregisterFd(pi, ke->fd, TRUE);
        ke->fd = -1;
    }

    free(ke);
    pi->driverPrivate = 0;
}
'''
if ptr_disable_old not in s:
    raise SystemExit("pointer disable anchor not found")
s = s.replace(ptr_disable_old, ptr_disable_new, 1)

kbd_disable_old = '''static void
EvdevKbdDisable(KdKeyboardInfo * ki)
{
    Kevdev *ke;

    ke = ki->driverPrivate;

    if (!ki || !ki->driverPrivate)
        return;

    KdUnregisterFd(ki, ke->fd, TRUE);

    if (ioctl(ke->fd, EVIOCGRAB, 0) < 0)
        perror("Ungrabbing evdev keyboard device failed");

    free(ke);
    ki->driverPrivate = 0;
}
'''
kbd_disable_new = '''static void
EvdevKbdDisable(KdKeyboardInfo * ki)
{
    Kevdev *ke;

    if (!ki || !ki->driverPrivate)
        return;

    ke = ki->driverPrivate;

    if (ke->reconnect_timer) {
        TimerFree(ke->reconnect_timer);
        ke->reconnect_timer = NULL;
    }

    if (ke->fd >= 0) {
        if (ioctl(ke->fd, EVIOCGRAB, 0) < 0 && errno != ENODEV)
            perror("Ungrabbing evdev keyboard device failed");
        KdUnregisterFd(ki, ke->fd, TRUE);
        ke->fd = -1;
    }

    free(ke);
    ki->driverPrivate = 0;
}
'''
if kbd_disable_old not in s:
    raise SystemExit("keyboard disable anchor not found")
s = s.replace(kbd_disable_old, kbd_disable_new, 1)

if 'DeleteInputDeviceRequest(pi->dixdev)' in s or 'DeleteInputDeviceRequest(ki->dixdev)' in s:
    raise SystemExit("old delete-on-unplug behavior still present")

p.write_text(s)
print("H3531_D5_KDRIVE_EVDEV_HOTPLUG_PATCH_OK")
