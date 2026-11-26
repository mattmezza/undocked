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
    cui_font *font, *medium, *mono;
    cui_software *software;
    cui_context *ui;
    Theme theme;
    int width, height;
} Painter;

bool painter_open(Painter *, X11 *, const Config *, int, int);
bool painter_resize(Painter *, X11 *, int, int);
void painter_close(Painter *);
void painter_present(Painter *, X11 *, Window, GC, cui_draw_list);
void theme_read(Theme *, X11 *, const Config *);
void painter_repaint(Painter *, X11 *, Window, GC);

enum Icon {
    ICON_NONE = -1, ICON_MONITOR, ICON_ROW, ICON_COLUMN, ICON_TWO_ABOVE, ICON_EYE,
    ICON_MIRROR, ICON_PLUS, ICON_CLOSE, ICON_CHECK, ICON_BACK, ICON_FORWARD,
    ICON_UP, ICON_DOWN, ICON_RESET, ICON_COUNT
};
bool paint_icon(cui_context *, enum Icon, float size, cui_color);
void display_badge(Painter *, X11 *, Window, GC, const char *);
#endif
