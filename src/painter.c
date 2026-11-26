#include "painter.h"

#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char geist_start[], geist_end[], geist_mono_start[], geist_mono_end[];
extern const unsigned char geist_medium_start[], geist_medium_end[], undocked_icon_masks[];

enum { ICON_PX = 32 };

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
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    XrmDatabase db = NULL;
    if (XGetWindowProperty(x->display, x->root, XA_RESOURCE_MANAGER, 0, 262144, False, XA_STRING,
        &type, &format, &count, &after, &data) == Success && format == 8 && !after && data)
        db = XrmGetStringDatabase((char *)data);
    if (data) XFree(data);
    return db;
}

static double
linear(unsigned component)
{
    double s = component / 255.0;
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double
luminance(cui_color c)
{
    return .2126 * linear(c.r) + .7152 * linear(c.g) + .0722 * linear(c.b);
}

static double
contrast(cui_color a, cui_color b)
{
    double x = luminance(a), y = luminance(b);
    return (fmax(x, y) + .05) / (fmin(x, y) + .05);
}

static cui_color
readable(cui_color text, cui_color background, double ratio)
{
    cui_color dark = rgb(0x080b10), light = rgb(0xffffff);
    cui_color end = contrast(light, background) >= contrast(dark, background) ? light : dark;
    if (contrast(text, background) >= ratio) return text;
    for (int step = 1; step < 16; step++) {
        cui_color c = cui_color_mix_oklab(text, end, step / 16.0f);
        if (contrast(c, background) >= ratio) return c;
    }
    return end;
}

void
theme_read(Theme *theme, X11 *x, const Config *config)
{
    *theme = (Theme){.background = rgb(0x12151c), .panel = rgb(0x202631),
        .foreground = rgb(0xf0f3fb), .muted = rgb(0xaebbd0), .border = rgb(0x484f61),
        .accent = rgb(0x9ab9ff), .on_accent = rgb(0x111824), .warning = rgb(0xffc881),
        .radius = 4};
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
    if (contrast(theme->background, theme->panel) < 1.13) {
        cui_color opposite = luminance(theme->background) < .32 ? rgb(0xffffff) : rgb(0x080b10);
        theme->panel = cui_color_mix_oklab(theme->background, opposite, .085f);
    }
    theme->border = readable(theme->border, theme->panel, 1.65);
    theme->foreground = readable(theme->foreground, theme->panel, 8.0);
    theme->muted = readable(theme->muted, theme->panel, 5.2);
    theme->accent = readable(theme->accent, theme->panel, 5.4);
    theme->on_accent = readable(theme->on_accent, theme->accent, 7.0);
    theme->warning = readable(theme->warning, theme->panel, 5.2);
    const char *radius = resource(db, "undocked.cornerRadius");
    if (radius) {
        char *end;
        long n = strtol(radius, &end, 10);
        if (*radius && !*end && n >= 0 && n <= 24) theme->radius = (int)n;
    }
    if (config->radius >= 0) theme->radius = config->radius;
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
    theme_read(&p->theme, x, config);
    XrmDatabase db = database(x);
    const char *font = *config->font ? config->font : resource(db, "undocked.font");
    const char *mono = *config->mono ? config->mono : resource(db, "undocked.mono");
    if (font && *font) p->font = cui_font_open(font);
    if (mono && *mono) p->mono = cui_font_open(mono);
    if (db) XrmDestroyDatabase(db);
    if (!p->font) {
        p->font = cui_font_open_memory(geist_start, (size_t)((uintptr_t)geist_end - (uintptr_t)geist_start));
        p->medium = cui_font_open_memory(geist_medium_start,
            (size_t)((uintptr_t)geist_medium_end - (uintptr_t)geist_medium_start));
    }
    if (!p->mono)
        p->mono = cui_font_open_memory(geist_mono_start, (size_t)((uintptr_t)geist_mono_end - (uintptr_t)geist_mono_start));
    if (!p->font || !p->mono) goto failed;
    cui_font_set_style(p->font, (cui_text_style){.monospace = true}, p->mono);
    if (p->medium)
        cui_font_set_style(p->font, (cui_text_style){.weight = CUI_TEXT_WEIGHT_MEDIUM}, p->medium);
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
    cui_font_close(p->medium);
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
    painter_repaint(p, x, window, gc);
}

void
painter_repaint(Painter *p, X11 *x, Window window, GC gc)
{
    XPutImage(x->display, window, gc, p->image, 0, 0, 0, 0, (unsigned)p->width, (unsigned)p->height);
}

bool
paint_icon(cui_context *ui, enum Icon icon, float size, cui_color foreground)
{
    static unsigned char pixels[ICON_COUNT][ICON_PX * ICON_PX * 4];
    static bool initialized;
    if (icon == ICON_NONE) return false;
    if (!initialized) {
        for (int i = 0; i < ICON_COUNT; i++) {
            for (int j = 0; j < ICON_PX * ICON_PX; j++) {
                unsigned char *p = &pixels[i][j * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = undocked_icon_masks[i * ICON_PX * ICON_PX + j];
            }
        }
        initialized = true;
    }
    return cui_paint_bitmap(ui, 0, (cui_bitmap_paint){
        .bitmap = {pixels[icon], ICON_PX, ICON_PX, ICON_PX * 4, 1},
        .opacity = 1, .custom_tint = true, .tint = foreground
    }, (cui_box){.width = size, .height = size});
}

void
display_badge(Painter *p, X11 *x, Window window, GC gc, const char *number)
{
    float size = p->height * 0.65f;
    cui_text_style mono = {.monospace = true};
    cui_size metrics = cui_font_measure_styled(p->font, cui_string(number), size, mono);
    cui_draw_command cmds[] = {
        {.type = CUI_DRAW_RECT, .bounds = {0, 0, (float)p->width, (float)p->height},
         .color = p->theme.accent, .radius = p->theme.radius},
        {.type = CUI_DRAW_TEXT, .bounds = {(p->width - metrics.width) / 2, (p->height - metrics.height) / 2,
                                        metrics.width, metrics.height},
         .color = p->theme.on_accent, .text = cui_string(number), .font_size = size, .text_style = mono}
    };
    painter_present(p, x, window, gc, (cui_draw_list){.commands = cmds, .count = 2, .width = p->width, .height = p->height});
}
