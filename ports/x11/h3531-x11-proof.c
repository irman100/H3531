/*
 * H3531 Stage6.0 X11 hardware proof client.
 * Built against the same Buildroot Xlib as Xfbdev.
 * ESC or Q exits.
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void draw(Display *dpy, Window win, GC gc,
                 unsigned long fg, unsigned long bg,
                 unsigned long events, int mx, int my,
                 unsigned int button, KeySym key)
{
    char line[160];
    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, win, gc, 0, 0, 800, 500);
    XSetForeground(dpy, gc, fg);

    XDrawString(dpy, win, gc, 30, 55,
        "H3531 STAGE6.0 - XFBDEV HARDWARE PROOF", 39);
    XDrawString(dpy, win, gc, 30, 90,
        "X11 owns framebuffer + evdev. Move mouse and press keys.", 55);
    XDrawString(dpy, win, gc, 30, 120,
        "Press ESC or Q to exit the proof session.", 40);

    snprintf(line, sizeof(line), "Events: %lu", events);
    XDrawString(dpy, win, gc, 30, 180, line, (int)strlen(line));
    snprintf(line, sizeof(line), "Mouse: x=%d y=%d   last button=%u", mx, my, button);
    XDrawString(dpy, win, gc, 30, 215, line, (int)strlen(line));

    if (key != NoSymbol) {
        const char *name = XKeysymToString(key);
        snprintf(line, sizeof(line), "Last key: %s (0x%lx)",
                 name ? name : "unknown", (unsigned long)key);
    } else {
        snprintf(line, sizeof(line), "Last key: none");
    }
    XDrawString(dpy, win, gc, 30, 250, line, (int)strlen(line));

    XDrawRectangle(dpy, win, gc, 25, 285, 750, 150);
    XDrawString(dpy, win, gc, 45, 325,
        "SUCCESS CRITERIA:", 17);
    XDrawString(dpy, win, gc, 45, 355,
        "1. Stable picture, no framebuffer flicker.", 41);
    XDrawString(dpy, win, gc, 45, 382,
        "2. One clean X cursor, no mouse trail.", 36);
    XDrawString(dpy, win, gc, 45, 409,
        "3. Mouse coordinates and key names update here.", 46);
    XFlush(dpy);
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "H3531-X11-PROOF: XOpenDisplay failed (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        return 2;
    }

    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    unsigned long black = BlackPixel(dpy, scr);
    unsigned long white = WhitePixel(dpy, scr);

    XColor exact, screen;
    unsigned long bg = black, fg = white;
    Colormap cmap = DefaultColormap(dpy, scr);
    if (XAllocNamedColor(dpy, cmap, "#102038", &screen, &exact))
        bg = screen.pixel;
    if (XAllocNamedColor(dpy, cmap, "#e8f0ff", &screen, &exact))
        fg = screen.pixel;

    Window win = XCreateSimpleWindow(dpy, root, 220, 110, 800, 500,
                                     2, fg, bg);
    XStoreName(dpy, win, "H3531 Stage6.0 Xfbdev Proof");
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XMapRaised(dpy, win);

    unsigned long count = 0;
    int mx = 0, my = 0;
    unsigned int last_button = 0;
    KeySym last_key = NoSymbol;

    fprintf(stdout, "H3531-X11-PROOF: connected display=%s screen=%dx%d depth=%d\n",
            DisplayString(dpy), DisplayWidth(dpy, scr),
            DisplayHeight(dpy, scr), DefaultDepth(dpy, scr));
    fflush(stdout);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        count++;

        if (ev.type == MotionNotify) {
            mx = ev.xmotion.x;
            my = ev.xmotion.y;
        } else if (ev.type == ButtonPress || ev.type == ButtonRelease) {
            mx = ev.xbutton.x;
            my = ev.xbutton.y;
            last_button = ev.xbutton.button;
        } else if (ev.type == KeyPress) {
            last_key = XLookupKeysym(&ev.xkey, 0);
            fprintf(stdout, "KEY keycode=%u keysym=0x%lx name=%s\n",
                    ev.xkey.keycode, (unsigned long)last_key,
                    XKeysymToString(last_key) ? XKeysymToString(last_key) : "unknown");
            fflush(stdout);
            if (last_key == XK_Escape || last_key == XK_q || last_key == XK_Q)
                break;
        }

        if (ev.type == Expose || ev.type == MotionNotify ||
            ev.type == ButtonPress || ev.type == ButtonRelease ||
            ev.type == KeyPress) {
            draw(dpy, win, gc, fg, bg, count, mx, my, last_button, last_key);
        }
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    fprintf(stdout, "H3531-X11-PROOF: clean exit\n");
    return 0;
}
