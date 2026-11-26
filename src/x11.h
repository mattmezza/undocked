#ifndef UNDOCKED_X11_H
#define UNDOCKED_X11_H

#include "undocked.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
    Display *display;
    int screen, event;
    Window root;
} X11;

bool x11_open(X11 *, char *);
Backend x11_backend(X11 *);
bool x11_change(X11 *, XEvent *);
void x11_class(X11 *, Window, const char *, const char *);
unsigned long x11_color(Visual *, unsigned, unsigned, unsigned);

#endif
