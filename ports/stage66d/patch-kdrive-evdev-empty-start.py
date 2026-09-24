#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-kdrive-evdev-empty-start.py EVDEV_C")

p = Path(sys.argv[1])
s = p.read_text()

def replace_function(src: str, signature: str, replacement: str) -> str:
    start = src.find(signature)
    if start < 0:
        raise SystemExit("function not found: " + signature)
    brace = src.find("{", start)
    if brace < 0:
        raise SystemExit("opening brace not found: " + signature)
    depth = 0
    i = brace
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                return src[:start] + replacement.rstrip() + src[end:]
        i += 1
    raise SystemExit("unterminated function: " + signature)

attach_ptr = r'''static Bool
H3531AttachPointer(KdPointerInfo *pi)
{
    Kevdev *ke;
    char path[64];
    int fd;
    unsigned long ev[NBITS(EV_MAX + 1)];

    if (!pi || !pi->driverPrivate)
        return FALSE;

    ke = pi->driverPrivate;
    fd = H3531FindEvdev(FALSE, path, sizeof(path));
    if (fd < 0)
        return FALSE;

    memset(ev, 0, sizeof(ev));
    memset(ke->rel, 0, sizeof(ke->rel));
    memset(ke->abs, 0, sizeof(ke->abs));
    memset(ke->prevabs, 0, sizeof(ke->prevabs));
    memset(ke->key, 0, sizeof(ke->key));
    memset(ke->relbits, 0, sizeof(ke->relbits));
    memset(ke->absbits, 0, sizeof(ke->absbits));
    memset(ke->keybits, 0, sizeof(ke->keybits));
    ke->max_rel = -1;
    ke->max_abs = -1;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) {
        close(fd);
        return FALSE;
    }

    if (ISBITSET(ev, EV_KEY))
        (void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(ke->keybits)), ke->keybits);

    if (ISBITSET(ev, EV_REL)) {
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(ke->relbits)), ke->relbits) < 0) {
            close(fd);
            return FALSE;
        }
        for (ke->max_rel = REL_MAX; ke->max_rel >= 0; ke->max_rel--)
            if (ISBITSET(ke->relbits, ke->max_rel))
                break;
    }

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        perror("[H3531] grabbing evdev mouse failed");

    if (!KdRegisterFd(fd, EvdevPtrRead, pi)) {
        ioctl(fd, EVIOCGRAB, 0);
        close(fd);
        return FALSE;
    }

    ke->fd = fd;
    pi->path = strdup(path);
    ErrorF("[H3531] Stage6.8.0A evdev mouse attached: %s\n", path);
    return TRUE;
}'''

ptr_init = r'''static Status
EvdevPtrInit(KdPointerInfo * pi)
{
    int fd = -1;

    if (!pi)
        return BadImplementation;

    if (pi->path)
        fd = open(pi->path, O_RDWR);

    if (fd >= 0)
        close(fd);
    else
        ErrorF("[H3531] Stage6.8.0A mouse absent during Init; creating detached XInput slot\n");

    pi->name = strdup("Evdev mouse");
    return Success;
}'''

ptr_enable = r'''static Status
EvdevPtrEnable(KdPointerInfo * pi)
{
    Kevdev *ke;

    if (!pi)
        return BadImplementation;

    ke = calloc(1, sizeof(Kevdev));
    if (!ke)
        return BadAlloc;

    ke->fd = -1;
    ke->max_rel = -1;
    ke->max_abs = -1;
    ke->reconnect_timer = NULL;
    pi->driverPrivate = ke;

    if (H3531AttachPointer(pi))
        return Success;

    ke->reconnect_timer = TimerSet(NULL, 0,
                                   H3531_EVDEV_RECONNECT_MS,
                                   H3531ReconnectPointer, pi);
    ErrorF("[H3531] Stage6.8.0A evdev mouse detached at X start; waiting for hotplug\n");
    return Success;
}'''

kbd_init = r'''static Status
EvdevKbdInit(KdKeyboardInfo * ki)
{
    int fd = -1;

    if (!ki)
        return BadImplementation;

    if (ki->path)
        fd = open(ki->path, O_RDWR);

    if (fd >= 0)
        close(fd);
    else
        ErrorF("[H3531] Stage6.8.0A keyboard absent during Init; creating detached XInput slot\n");

    ki->name = strdup("Evdev keyboard");
    readMapping(ki);
    return Success;
}'''

kbd_enable = r'''static Status
EvdevKbdEnable(KdKeyboardInfo * ki)
{
    Kevdev *ke;

    if (!ki)
        return BadImplementation;

    ke = calloc(1, sizeof(Kevdev));
    if (!ke)
        return BadAlloc;

    ke->fd = -1;
    ke->reconnect_timer = NULL;
    ki->driverPrivate = ke;

    if (H3531AttachKeyboard(ki))
        return Success;

    ke->reconnect_timer = TimerSet(NULL, 0,
                                   H3531_EVDEV_RECONNECT_MS,
                                   H3531ReconnectKeyboard, ki);
    ErrorF("[H3531] Stage6.8.0A evdev keyboard detached at X start; waiting for hotplug\n");
    return Success;
}'''

s = replace_function(s, "static Bool\nH3531AttachPointer(KdPointerInfo *pi)", attach_ptr)
s = replace_function(s, "static Status\nEvdevPtrInit(KdPointerInfo * pi)", ptr_init)
s = replace_function(s, "static Status\nEvdevPtrEnable(KdPointerInfo * pi)", ptr_enable)
s = replace_function(s, "static Status\nEvdevKbdInit(KdKeyboardInfo * ki)", kbd_init)
s = replace_function(s, "static Status\nEvdevKbdEnable(KdKeyboardInfo * ki)", kbd_enable)

markers = [
    "Stage6.8.0A evdev mouse detached at X start",
    "Stage6.8.0A evdev keyboard detached at X start",
    "Stage6.8.0A evdev mouse attached",
]
for marker in markers:
    if marker not in s:
        raise SystemExit("missing marker after patch: " + marker)

p.write_text(s)
print("H3531_STAGE680A_EMPTY_START_HOTPLUG_PATCH_OK")
