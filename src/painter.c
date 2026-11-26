#include "painter.h"

#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char geist_start[], geist_end[], geist_mono_start[], geist_mono_end[];

static cui_color
rgb(unsigned hex)
{
    return (cui_color){(unsigned char)(hex >> 16), (unsigned char)(hex >> 8), (unsigned char)hex, 255};
}

static const char *
resource(XrmDatabase db, const char *name)
{
    XrmValue value;
    char *type;
    return db && XrmGetResource(db, name, name, &type, &value) ? value.addr : NULL;
}

static void
color(XrmDatabase db, const char *name, cui_color *target)
{
    const char *s = resource(db, name);
    if (!s || *s++ != '#' || strlen(s) != 6) return;
    char *end;
    unsigned long hex = strtoul(s, &end, 16);
    if (!*end) *target = rgb((unsigned)hex);
}

static XrmDatabase
database(X11 *x)
{
    XrmInitialize();
    char *data = XResourceManagerString(x->display);
    return data ? XrmGetStringDatabase(data) : NULL;
}

void
theme_read(Theme *theme, X11 *x)
{
    *theme = (Theme){.background = rgb(0x171923), .panel = rgb(0x202330),
        .foreground = rgb(0xc0caf5), .muted = rgb(0x9aa4c6), .border = rgb(0x373e56),
        .accent = rgb(0x7aa2f7), .on_accent = rgb(0x131b2f), .warning = rgb(0xe0af68),
        .radius = 8};
    XrmDatabase db = database(x);
    color(db, "mwm.normbg", &theme->background);
    color(db, "mwm.statusbg", &theme->panel);
    color(db, "mwm.normfg", &theme->foreground);
    color(db, "mwm.normborder", &theme->border);
    color(db, "mwm.selbg", &theme->accent);
    color(db, "mwm.selfg", &theme->on_accent);
    color(db, "mwm.systemwarn", &theme->warning);
    color(db, "undocked.background", &theme->background);
    color(db, "undocked.panel", &theme->panel);
    color(db, "undocked.foreground", &theme->foreground);
    color(db, "undocked.muted", &theme->muted);
    color(db, "undocked.border", &theme->border);
    color(db, "undocked.accent", &theme->accent);
    color(db, "undocked.onAccent", &theme->on_accent);
    color(db, "undocked.warning", &theme->warning);
    const char *radius = resource(db, "undocked.cornerRadius");
    if (!radius) radius = resource(db, "mwm.segradius");
    if (radius) {
        char *end;
        long n = strtol(radius, &end, 10);
        if (!*end && n >= 0 && n <= 24) theme->radius = (int)n;
    }
    if (db) XrmDestroyDatabase(db);
}

bool
painter_resize(Painter *p, X11 *x, int width, int height)
{
    if (width == p->width && height == p->height) return true;
    if (width < 1 || height < 1 || width > 4096 || height > 4096) return false;
    XImage *image = XCreateImage(x->display, DefaultVisual(x->display, x->screen),
        (unsigned)DefaultDepth(x->display, x->screen), ZPixmap, 0, NULL,
        (unsigned)width, (unsigned)height, 32, 0);
    if (!image) return false;
    image->data = calloc((size_t)image->bytes_per_line, (size_t)height);
    if (!image->data || !cui_software_resize(p->software, (unsigned)width, (unsigned)height)) {
        XDestroyImage(image);
        return false;
    }
    if (p->image) XDestroyImage(p->image);
    p->image = image;
    p->width = width;
    p->height = height;
    return true;
}

bool
painter_open(Painter *p, X11 *x, const Config *config, int width, int height)
{
    *p = (Painter){0};
    theme_read(&p->theme, x);
    XrmDatabase db = database(x);
    const char *font = *config->font ? config->font : resource(db, "undocked.font");
    const char *mono = *config->mono ? config->mono : resource(db, "undocked.mono");
    if (font && *font) p->font = cui_font_open(font);
    if (mono && *mono) p->mono = cui_font_open(mono);
    if (db) XrmDestroyDatabase(db);
    if (!p->font)
        p->font = cui_font_open_memory(geist_start, (size_t)((uintptr_t)geist_end - (uintptr_t)geist_start));
    if (!p->mono)
        p->mono = cui_font_open_memory(geist_mono_start, (size_t)((uintptr_t)geist_mono_end - (uintptr_t)geist_mono_start));
    if (!p->font || !p->mono) goto failed;
    cui_font_set_style(p->font, (cui_text_style){.monospace = true}, p->mono);
    p->ui = cui_create((cui_config){.width = (float)width, .height = (float)height,
        .max_elements = 2048, .max_commands = 4096, .frame_bytes = 256 * 1024,
        .measure = cui_font_measure, .measure_styled = cui_font_measure_styled,
        .measure_user = p->font, .font_metrics = cui_font_get_metrics});
    p->software = cui_software_create(1, 1, p->font);
    if (p->ui && p->software && painter_resize(p, x, width, height)) return true;
failed:
    painter_close(p);
    return false;
}

void
painter_close(Painter *p)
{
    if (p->image) XDestroyImage(p->image);
    cui_software_destroy(p->software);
    cui_destroy(p->ui);
    cui_font_close(p->font);
    cui_font_close(p->mono);
}

void
painter_present(Painter *p, X11 *x, Window window, GC gc, cui_draw_list list)
{
    if (!cui_software_render(p->software, &list, p->theme.background)) return;
    const unsigned char *rgba = cui_software_pixels(p->software, NULL, NULL);
    Visual *visual = DefaultVisual(x->display, x->screen);
    bool direct = p->image->bits_per_pixel == 32 && p->image->byte_order == LSBFirst &&
        visual->red_mask == 0xff0000 && visual->green_mask == 0xff00 && visual->blue_mask == 0xff;
    for (int y = 0; y < p->height; y++) {
        unsigned char *line = (unsigned char *)p->image->data + y * p->image->bytes_per_line;
        for (int j = 0; j < p->width; j++, rgba += 4) {
            if (direct) {
                line[j * 4] = rgba[2];
                line[j * 4 + 1] = rgba[1];
                line[j * 4 + 2] = rgba[0];
                line[j * 4 + 3] = 0xff;
            } else XPutPixel(p->image, j, y, x11_color(visual, rgba[0], rgba[1], rgba[2]));
        }
    }
    XPutImage(x->display, window, gc, p->image, 0, 0, 0, 0, (unsigned)p->width, (unsigned)p->height);
}

void
display_icon(X11 *x, Drawable drawable, GC gc, int left, int top, int size, unsigned long color)
{
    /* Two overlapping Heroicons desktop outlines, with the back stand occluded. */
    Display *d = x->display;
    int small = size * 2 / 3, step = size - small - 2, tall = size / 2 - 2;
    XSetForeground(d, gc, color);
    XSetLineAttributes(d, gc, size >= 21 ? 2 : 1, LineSolid, CapRound, JoinRound);
    XDrawLine(d, drawable, gc, left + step, top + 1, left + size - 2, top + 1);
    XDrawLine(d, drawable, gc, left + size - 2, top + 1, left + size - 2, top + tall + 1);
    XDrawLine(d, drawable, gc, left + small, top + tall + 1, left + size - 2, top + tall + 1);
    XDrawLine(d, drawable, gc, left + step, top + 1, left + step, top + step);
    XDrawRectangle(d, drawable, gc, left + 1, top + step, (unsigned)(small - 1), (unsigned)tall);
    XDrawLine(d, drawable, gc, left + small / 2, top + step + tall,
              left + small / 2, top + step + tall + 3);
    XDrawLine(d, drawable, gc, left + small / 4, top + step + tall + 3,
              left + 3 * small / 4, top + step + tall + 3);
}

void
display_badge(Painter *p, X11 *x, Window window, GC gc, const char *number)
{
    float size = p->height * 0.65f;
    cui_text_style mono = {.monospace = true};
    cui_size metrics = cui_font_measure_styled(p->font, cui_string(number), size, mono);
    cui_draw_command cmds[] = {
        {.type = CUI_DRAW_RECT, .bounds = {0, 0, (float)p->width, (float)p->height},
         .color = p->theme.accent},
        {.type = CUI_DRAW_TEXT, .bounds = {(p->width - metrics.width) / 2, (p->height - metrics.height) / 2,
                                        metrics.width, metrics.height},
         .color = p->theme.on_accent, .text = cui_string(number), .font_size = size, .text_style = mono}
    };
    painter_present(p, x, window, gc, (cui_draw_list){.commands = cmds, .count = 2, .width = p->width, .height = p->height});
}
