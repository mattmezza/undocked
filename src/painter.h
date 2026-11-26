#ifndef UNDOCKED_PAINTER_H
#define UNDOCKED_PAINTER_H
#include "x11.h"
#include <cui/cui.h>
#include <cui/cui_software.h>

typedef struct {
    cui_color background, panel, foreground, muted, border, accent, on_accent, warning;
    int radius;
} Theme;

typedef struct {
    XImage *image;
    cui_font *font, *mono;
    cui_software *software;
    cui_context *ui;
    Theme theme;
    int width, height;
} Painter;

bool painter_open(Painter *, X11 *, const Config *, int, int);
bool painter_resize(Painter *, X11 *, int, int);
void painter_close(Painter *);
void painter_present(Painter *, X11 *, Window, GC, cui_draw_list);
void theme_read(Theme *, X11 *);
void display_icon(X11 *, Drawable, GC, int x, int y, int size, unsigned long color);
void display_badge(Painter *, X11 *, Window, GC, const char *);
#endif
