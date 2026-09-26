#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sym_name(KeySym s)
{
    const char *n = XKeysymToString(s);
    return n ? n : "NoSymbol";
}

static void dump_key(Display *dpy, int keycode, const char *label)
{
    int per = 0;
    KeySym *map = XGetKeyboardMapping(dpy, (KeyCode)keycode, 1, &per);
    int i;

    printf("[XKEYMAP] %s keycode=%d", label, keycode);
    if (!map) {
        printf(" mapping=<unavailable>\n");
        return;
    }

    for (i = 0; i < per; ++i)
        printf(" level%d=%s(0x%lx)", i, sym_name(map[i]), (unsigned long)map[i]);
    printf("\n");
    XFree(map);
}

static int force_one(Display *dpy, int keycode, KeySym sym)
{
    KeySym map[2];
    map[0] = sym;
    map[1] = NoSymbol;

    XChangeKeyboardMapping(dpy, keycode, 2, map, 1);
    return 0;
}

int main(int argc, char **argv)
{
    const char *display_name = NULL;
    Display *dpy;

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        puts("usage: h3531-xkeymap-fix [DISPLAY]");
        return 0;
    }

    if (argc > 1)
        display_name = argv[1];

    dpy = XOpenDisplay(display_name);
    if (!dpy) {
        fprintf(stderr, "[XKEYMAP] cannot open display %s\n",
                display_name ? display_name : "<default>");
        return 2;
    }

    printf("[XKEYMAP] display=%s vendor=%s protocol=%d.%d\n",
           DisplayString(dpy), ServerVendor(dpy),
           ProtocolVersion(dpy), ProtocolRevision(dpy));

    /* Linux evdev code + X keycode base 8:
     * KEY_BACKSPACE 14 -> 22
     * KEY_ENTER     28 -> 36
     * KEY_KPENTER   96 -> 104
     *
     * KDrive's EvdevKbdRead posts raw Linux codes and KdEnqueueKeyboardEvent
     * adds KD_MIN_KEYCODE=8. These keycodes therefore must resolve to the
     * canonical X keysyms below. */
    dump_key(dpy, 22, "before-backspace");
    dump_key(dpy, 36, "before-enter");
    dump_key(dpy, 104, "before-kpenter");

    force_one(dpy, 22, XK_BackSpace);
    force_one(dpy, 36, XK_Return);
    /* Treat keypad Enter as normal Return for old VTE/shell compatibility. */
    force_one(dpy, 104, XK_Return);

    XSync(dpy, False);

    dump_key(dpy, 22, "after-backspace");
    dump_key(dpy, 36, "after-enter");
    dump_key(dpy, 104, "after-kpenter");

    XCloseDisplay(dpy);
    puts("[XKEYMAP] H3531_XKEYMAP_FIX_OK");
    return 0;
}
