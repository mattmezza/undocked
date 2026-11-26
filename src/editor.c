#include "painter.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Page { DISPLAYS, PRESETS_PAGE, ARRANGE, DISPLAY_MODES };

typedef struct {
    X11 x;
    Window window;
    Atom close, protocols, wake;
    GC gc;
    Painter paint;
    Config config;
    Reply live;
    Layout base, draft, extended;
    bool editing, closing, mirrored, save_auto, have_extended, redraw;
    bool form, focus_name, save_enter, pointer_down, drag_moved, frame_ready;
    size_t caret;
    int selected, dragging, start_x, start_y, from_x, from_y, offset, menu_count;
    float scale, origin_x, origin_y;
    cui_rect tiles[OUTPUTS], canvas;
    enum Page page;
    char name[NAME], notice[ERROR_SIZE], form_error[ERROR_SIZE];
    char presets[PRESETS][NAME];
} Editor;

static cui_color
mix(cui_color a, cui_color b, float amount)
{
    return cui_color_mix_oklab(a, b, amount);
}

static void
fill(Editor *e, cui_rect bounds, cui_color color, float radius)
{
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, 0, (cui_box){.background = color, .radius = radius}, bounds);
    cui_end_box(ui);
}

static void
label(Editor *e, cui_rect bounds, const char *text, float size, cui_color color, bool mono, bool center)
{
    if (bounds.w <= 0 || bounds.h <= 0) return;
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, 0, (cui_box){
        .align_x = center ? CUI_ALIGN_CENTER : CUI_ALIGN_START, .align_y = CUI_ALIGN_CENTER}, bounds);
    cui_text_style style = {.monospace = mono,
        .weight = !mono && size >= 18 ? CUI_TEXT_WEIGHT_MEDIUM : CUI_TEXT_WEIGHT_NORMAL};
    cui_str visible = cui_string(text);
    while (visible.length && cui_measure_text_ex(ui, visible, size, style).width > bounds.w) {
        visible.length--;
        while (visible.length && ((unsigned char)visible.data[visible.length] & 0xc0) == 0x80)
            visible.length--;
    }
    cui_text_line(ui, visible, size, color, size + 4, style);
    cui_end_box(ui);
}

enum ButtonStyle { QUIET, SELECTED, PRIMARY };

static bool
button(Editor *e, const char *key, cui_rect bounds, const char *name, enum Icon icon,
       enum ButtonStyle style, bool disabled, bool mono, bool icon_only)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_id id = cui_id_from(ui, key);
    cui_interaction last = cui_interaction_state(ui, id);
    cui_color bg = {0}, border = {0}, fg = t.foreground;
    if (style == SELECTED) { bg = mix(t.background, t.accent, .12f); fg = t.accent; }
    if (style == PRIMARY) { bg = t.accent; fg = t.on_accent; }
    if (disabled) {
        bg = (cui_color){0};
        fg = mix(t.panel, t.muted, .65f);
    } else if (last.pressed || last.hovered) {
        if (!bg.a) bg = t.panel;
        bg = mix(bg, style == PRIMARY ? t.foreground : t.accent, last.pressed ? .23f : .1f);
    }
    if (last.focus_visible && !disabled) border = style == PRIMARY ? t.on_accent : t.accent;
    cui_begin_absolute_box(ui, id, (cui_box){
        .background = bg, .border = border, .border_width = border.a ? 1 : 0,
        .radius = t.radius, .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER
    }, bounds);
    cui_interaction state = cui_interact(ui, id, CUI_ROLE_BUTTON, cui_string(name), disabled,
                                         style == SELECTED, 0);
    cui_begin_box(ui, 0, (cui_box){.row = true, .gap = 7, .align_y = CUI_ALIGN_CENTER});
    paint_icon(ui, icon, 16, fg);
    if (!icon_only)
        cui_text_line(ui, cui_string(name), 14, fg, 18, (cui_text_style){
            .monospace = mono, .weight = mono ? CUI_TEXT_WEIGHT_NORMAL : CUI_TEXT_WEIGHT_MEDIUM});
    cui_end_box(ui);
    cui_end_box(ui);
    if (state.activated) e->redraw = true;
    return state.activated;
}

static void
icon_at(Editor *e, enum Icon icon, float x, float y, float size, cui_color color)
{
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, 0, (cui_box){0}, (cui_rect){x, y, size, size});
    paint_icon(ui, icon, size, color);
    cui_end_box(ui);
}

static const Placement *
main_display(const Layout *layout)
{
    const Placement *first = NULL;
    for (int i = 0; i < layout->count; i++) {
        const Placement *p = &layout->output[i];
        if (!p->enabled) continue;
        if (p->primary) return p;
        if (!first) first = p;
    }
    return first;
}

static void
keep_visible(Editor *e)
{
    if (!e->window) return;
    Display *d = e->x.display;
    int x, y;
    Window child;
    if (!XTranslateCoordinates(d, e->window, e->x.root, 0, 0, &x, &y, &child)) return;
    int width = e->paint.width, height = e->paint.height;
    for (int i = 0; i < e->base.count; i++) {
        Placement p = e->base.output[i];
        if (!p.enabled) continue;
        int w, h;
        dimensions(p, &w, &h);
        if (x >= p.x && y >= p.y && x + width <= p.x + w && y + height <= p.y + h)
            return;
    }
    const Placement *primary = main_display(&e->base);
    if (!primary) return;
    int w, h;
    dimensions(*primary, &w, &h);
    if (width > w || height > h) {
        if (width > w) width = w;
        if (height > h) height = h;
        XSizeHints hints = {.flags = PMinSize | PMaxSize, .min_width = width < 640 ? width : 640,
            .min_height = height < 480 ? height : 480, .max_width = 1920, .max_height = 1440};
        XSetWMNormalHints(d, e->window, &hints);
    }
    XMoveResizeWindow(d, e->window, primary->x + (w - width) / 2, primary->y + (h - height) / 2,
                      (unsigned)width, (unsigned)height);
}

static void
receive(Editor *e, Reply reply, bool reset)
{
    uint64_t old = topology_key(&e->live.topology);
    uint64_t next = topology_key(&reply.topology);
    bool changed = e->live.version && old != next;
    bool confirmation_changed = (e->live.remaining > 0) != (reply.remaining > 0);
    Layout active = current_layout(&reply.topology);
    bool moved = !layout_equal(&active, &e->base);
    e->live = reply;
    e->base = active;
    if (moved && e->frame_ready) keep_visible(e);
    if (reset || changed || confirmation_changed || (!e->editing && moved && e->dragging < 0)) {
        e->draft = e->base;
        e->editing = false;
        e->dragging = -1;
        e->mirrored = layout_mirrored(&e->draft);
        if (changed) {
            e->have_extended = false;
            e->selected = -1;
            if (e->page == DISPLAY_MODES) e->page = DISPLAYS;
            e->form = false;
            strcpy(e->notice, "Connected displays changed. Layout refreshed.");
        }
        if (e->selected >= e->draft.count) e->selected = -1;
    }
}

static bool
ask(Editor *e, enum Command action, const Layout *layout)
{
    Request request = {.version = REQUEST_VERSION, .command = (uint32_t)action};
    if (layout) {
        request.expected = topology_key(&e->live.topology);
        request.flags = STRICT_PORTS;
        request.layout = *layout;
    }
    Reply reply;
    if (!rpc(&request, &reply, true)) {
        snprintf(e->notice, sizeof e->notice, "%s", reply.message);
        return false;
    }
    receive(e, reply, action == APPLY || action == KEEP || action == REVERT);
    if (action != GET) *e->notice = 0;
    return true;
}

static void
edit(Editor *e)
{
    e->editing = !layout_equal(&e->base, &e->draft);
    e->mirrored = layout_mirrored(&e->draft);
    *e->notice = 0;
}

static void
use_suggestion(Editor *e, const char *kind)
{
    Layout next = e->draft;
    if (!suggest(&e->live.topology, &next, kind, e->notice)) return;
    e->draft = next;
    edit(e);
}

static void
set_mirror(Editor *e, bool mirror)
{
    if (mirror == e->mirrored) return;
    if (mirror) {
        e->extended = e->draft;
        e->have_extended = true;
        use_suggestion(e, "mirror");
    } else if (e->have_extended) {
        e->draft = e->extended;
        edit(e);
    } else if (extend_layout(&e->live, &e->draft, e->notice)) edit(e);
}

static bool
inside(cui_rect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void
projection(Editor *e, cui_rect frame)
{
    if (e->dragging >= 0) return;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < e->draft.count; i++) {
        Placement p = e->draft.output[i];
        if (!p.enabled) continue;
        int w, h;
        dimensions(p, &w, &h);
        if (p.x < x0) x0 = p.x;
        if (p.y < y0) y0 = p.y;
        if (p.x + w > x1) x1 = p.x + w;
        if (p.y + h > y1) y1 = p.y + h;
    }
    e->scale = fminf(fmaxf(16, frame.w - 48) / fmaxf(x1 - x0, 1),
                     fmaxf(16, frame.h - 48) / fmaxf(y1 - y0, 1));
    if (e->scale > 1.0f) e->scale = 1.0f;
    e->origin_x = frame.x + (frame.w - (x1 + x0) * e->scale) / 2;
    e->origin_y = frame.y + (frame.h - (y1 + y0) * e->scale) / 2;
}

static void
tile(Editor *e, int n, cui_rect r, bool disabled)
{
    const Placement *p = &e->draft.output[n];
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    char id[PORT + 16], number[12];
    snprintf(id, sizeof id, "display-%s", p->port);
    snprintf(number, sizeof number, "%d", n + 1);
    cui_id element = cui_id_from(ui, id);
    cui_color background = mix(t.panel, t.accent, n == e->selected ? .19f : .07f);
    if (!p->enabled) background = t.panel;
    cui_interaction last = cui_interaction_state(ui, element);
    cui_begin_absolute_box(ui, element, (cui_box){.background = background,
        .border = t.accent, .border_width = n == e->selected || last.focus_visible ? 1 : 0,
        .radius = t.radius}, r);
    cui_interaction clicked = cui_interact(ui, element, CUI_ROLE_BUTTON, cui_string(p->port),
                                          disabled, n == e->selected, 0);
    if (clicked.activated && !e->drag_moved) {
        e->selected = n;
        e->redraw = true;
    }
    if (!p->enabled) {
        char port[PORT + 16];
        snprintf(port, sizeof port, "%s  %s", number, p->port);
        label(e, (cui_rect){6, 0, r.w - 12, r.h}, port, 12, t.muted, true, true);
    } else {
        label(e, (cui_rect){4, 0, r.w - 8, r.h > 60 ? r.h - 21 : r.h}, number,
              floorf(fminf(48, fminf(r.h * .52f, r.w * .38f))), t.foreground, true, true);
        if (r.h > 60)
            label(e, (cui_rect){6, r.h * .5f + 16, r.w - 12, 19}, p->port, 13, t.muted, true, true);
        if (p->primary && r.w > 83 && r.h > 75)
            label(e, (cui_rect){10, 6, 67, 17}, "Primary", 11, t.accent, false, false);
    }
    cui_end_box(ui);
}

static void
draw_canvas(Editor *e, cui_rect frame, bool disabled)
{
    e->canvas = frame;
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, cui_id_from(ui, "canvas"), (cui_box){0}, frame);
    cui_color dot = mix(t.background, t.border, .42f);
    int step = 20 + (int)fmaxf(frame.w, frame.h) / 120;
    for (int y = 16; y < frame.h - 11; y += step)
        for (int x = 16; x < frame.w - 11; x += step)
            fill(e, (cui_rect){(float)x, (float)y, 1, 1}, dot, 0);

    int inactive = 0;
    for (int i = 0; i < e->draft.count; i++) if (!e->draft.output[i].enabled) inactive++;
    int grid_columns = (int)((frame.w - 20) / 111);
    if (grid_columns < 1) grid_columns = 1;
    int rows = (inactive + grid_columns - 1) / grid_columns;
    cui_rect area = frame;
    int row_height = (int)fminf(34, frame.h / fmaxf(1, 2 * rows));
    area.h -= (float)rows * row_height;
    projection(e, area);
    for (int i = 0, off = 0; i < e->draft.count; i++) {
        Placement p = e->draft.output[i];
        int w, h;
        dimensions(p, &w, &h);
        if (!p.enabled) {
            e->tiles[i] = (cui_rect){frame.x + 12 + 111 * (off % grid_columns),
                frame.y + frame.h - row_height * (rows - off / grid_columns), 104, row_height - 5};
            off++;
        } else {
            float shift = e->mirrored ? (float)i * 9 : 0;
            e->tiles[i] = (cui_rect){e->origin_x + p.x * e->scale + shift,
                e->origin_y + p.y * e->scale + shift, fmaxf(16, w * e->scale), fmaxf(16, h * e->scale)};
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < e->draft.count; i++) {
            if ((i == e->selected) != (pass == 1)) continue;
            cui_rect r = e->tiles[i];
            float right = fminf(r.x + r.w - 1, frame.x + frame.w - 2);
            float bottom = fminf(r.y + r.h - 1, frame.y + frame.h - 2);
            r.x = fmaxf(r.x + 1, frame.x + 2);
            r.y = fmaxf(r.y + 1, frame.y + 2);
            r.w = right - r.x;
            r.h = bottom - r.y;
            if (r.w < 12 || r.h < 12) continue;
            r.x -= frame.x;
            r.y -= frame.y;
            tile(e, i, r, disabled);
        }
    }
    if (!e->draft.count)
        label(e, (cui_rect){10, 10, frame.w - 20, frame.h - 20}, "No displays connected", 16, t.muted, false, true);
    cui_end_box(ui);
}

static void
mode_text(char *text, size_t size, Mode mode)
{
    if (mode.millihz)
        snprintf(text, size, "%d×%d · %g Hz", mode.width, mode.height,
                 round(mode.millihz / 10.0) / 100);
    else snprintf(text, size, "%d×%d", mode.width, mode.height);
}

static bool
checkbox(Editor *e, const char *key, cui_rect bounds, const char *name,
         bool checked, bool disabled)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_id id = cui_id_from(ui, key);
    cui_interaction last = cui_interaction_state(ui, id);
    cui_color ink = disabled ? t.muted : t.foreground;
    cui_begin_absolute_box(ui, id, (cui_box){
        .background = !disabled && (last.hovered || last.focus_visible) ? t.panel : (cui_color){0},
        .radius = t.radius
    }, bounds);
    cui_interaction state = cui_interact(ui, id, CUI_ROLE_CHECKBOX, cui_string(name), disabled, checked, 0);
    float y = floorf((bounds.h - 16) / 2);
    cui_color mark = checked ? t.accent : mix(t.border, t.muted, .4f);
    if (disabled) mark = t.border;
    fill(e, (cui_rect){6, y, 16, 16}, mark, fminf(t.radius, 3));
    if (checked) icon_at(e, ICON_CHECK, 8, y + 2, 12, disabled ? t.muted : t.on_accent);
    else fill(e, (cui_rect){7, y + 1, 14, 14}, t.background, fminf(t.radius, 2));
    label(e, (cui_rect){30, 0, bounds.w - 30, bounds.h}, name, 14, ink, false, false);
    cui_end_box(ui);
    if (state.activated) e->redraw = true;
    return state.activated;
}

static bool
action_row(Editor *e, const char *key, cui_rect bounds, const char *name, enum Icon icon,
           enum Icon trail, bool selected, bool disabled, bool mono)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_id id = cui_id_from(ui, key);
    cui_interaction last = cui_interaction_state(ui, id);
    cui_color ink = disabled ? t.muted : (selected ? t.accent : t.foreground);
    cui_begin_absolute_box(ui, id, (cui_box){
        .background = selected || (!disabled && (last.hovered || last.focus_visible)) ?
            mix(t.background, t.accent, selected ? .13f : .08f) : (cui_color){0},
        .radius = t.radius,
        .border = t.accent, .border_width = !disabled && last.focus_visible ? 1 : 0
    }, bounds);
    cui_interaction state = cui_interact(ui, id, CUI_ROLE_BUTTON, cui_string(name), disabled, selected, 0);
    float x = 10, y = floorf((bounds.h - 16) / 2);
    if (icon != ICON_NONE) {
        icon_at(e, icon, x, y, 16, ink);
        x += 26;
    }
    label(e, (cui_rect){x, 0, bounds.w - x - (trail == ICON_NONE ? 10 : 33), bounds.h},
          name, mono ? 13 : 14, ink, mono, false);
    if (trail != ICON_NONE) icon_at(e, trail, bounds.w - 25, y, 16, ink);
    cui_end_box(ui);
    if (state.activated) e->redraw = true;
    return state.activated;
}

static void
page_control(Editor *e, float right, float y, int count, int rows)
{
    int last = count > rows ? count - rows : 0;
    if (e->offset > last) e->offset = last;
    if (!last) return;
    if (button(e, "prev", (cui_rect){right - 65, y, 29, 28}, "Previous page",
               ICON_UP, QUIET, !e->offset, false, true))
        e->offset = e->offset > rows ? e->offset - rows : 0;
    if (button(e, "next", (cui_rect){right - 29, y, 29, 28}, "Next page",
               ICON_DOWN, QUIET, e->offset >= last, false, true))
        e->offset = e->offset + rows < last ? e->offset + rows : last;
}

static void
open_page(Editor *e, enum Page page)
{
    e->page = page;
    e->offset = 0;
    e->form = false;
    *e->notice = 0;
    if (page == PRESETS_PAGE) e->menu_count = preset_list(&e->config, e->presets);
}

static float
drawer_width(const Editor *e)
{
    return e->selected >= 0 ? fminf(250, e->paint.width * .45f) : 0;
}

static void
draw_details(Editor *e, cui_rect frame, bool disabled)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, cui_id_from(ui, "details"), (cui_box){
        .background = mix(t.background, t.panel, .76f)}, frame);
    fill(e, (cui_rect){0, 0, 1, frame.h}, mix(t.background, t.border, .5f), 0);
    int selected = e->selected;
    Placement *p = &e->draft.output[selected];
    const Output *output = &e->live.topology.output[selected];
    const char *alias = alias_for(&e->config, p->port);
    char connector[PORT + 16], text[64];
    float w = frame.w - 36;
    snprintf(connector, sizeof connector, "%d  %s", selected + 1, p->port);
    label(e, (cui_rect){18, 19, w - 30, 30}, connector, 13, t.accent, true, false);
    if (button(e, "details-close", (cui_rect){frame.w - 45, 19, 27, 30}, "Close display details",
               ICON_CLOSE, QUIET, false, false, true)) e->selected = -1;
    label(e, (cui_rect){18, 52, w, 34}, *alias ? alias : output->model, 21, t.foreground, false, false);
    int active = 0;
    for (int i = 0; i < e->draft.count; i++) active += e->draft.output[i].enabled;
    if (checkbox(e, "power", (cui_rect){12, 95, w + 6, 31}, "Enabled", p->enabled,
                 disabled || (p->enabled && active <= 1) || !output->count) &&
        layout_enable(&e->live.topology, &e->draft, selected, !p->enabled)) edit(e);
    if (p->enabled) {
        label(e, (cui_rect){18, 148, w, 19}, "Resolution and refresh rate", 12, t.muted, true, false);
        mode_text(text, sizeof text, p->mode);
        if (action_row(e, "mode", (cui_rect){8, 169, w + 10, 32}, text,
                       ICON_NONE, ICON_FORWARD, false, disabled, true)) {
            open_page(e, DISPLAY_MODES);
            for (int i = 0; i < output->count; i++)
                if (same_mode(output->modes[i], p->mode)) { e->offset = i; break; }
        }
        label(e, (cui_rect){18, 211, 82, 31}, "Rotation", 14, t.muted, false, false);
        snprintf(text, sizeof text, "%d°", p->rotation);
        if (button(e, "rotate", (cui_rect){frame.w - 100, 211, 83, 31}, text, ICON_RESET,
                   QUIET, disabled || output->rotations == 1, true, false)) {
            do p->rotation = (p->rotation + 90) % 360;
            while (!(output->rotations & (1 << (p->rotation / 90))));
            edit(e);
        }
        if (checkbox(e, "primary", (cui_rect){12, 253, w + 6, 31}, "Primary display", p->primary,
                     disabled || p->primary)) {
            for (int i = 0; i < e->draft.count; i++) e->draft.output[i].primary = i == selected;
            edit(e);
        }
    }
    cui_end_box(ui);
}

static void
subpage_title(Editor *e, const char *title, bool disabled)
{
    if (button(e, "back", (cui_rect){23, 19, 32, 32}, "Back to displays", ICON_BACK,
               QUIET, disabled, false, true)) open_page(e, DISPLAYS);
    label(e, (cui_rect){66, 13, e->paint.width - 230, 43}, title, 26,
          e->paint.theme.foreground, false, false);
}

static void
draw_modes(Editor *e, bool blocked)
{
    Theme t = e->paint.theme;
    float w = (float)e->paint.width, h = (float)e->paint.height;
    const Output *output = &e->live.topology.output[e->selected];
    Placement *p = &e->draft.output[e->selected];
    subpage_title(e, "Display mode", blocked);
    label(e, (cui_rect){26, 63, w - 52, 22}, "Resolution and refresh rate", 13, t.muted, true, false);
    label(e, (cui_rect){w - 146, 63, 120, 22}, p->port, 13, t.accent, true, false);
    int rows = (int)fmaxf(1, floorf((h - 151) / 33));
    int last = output->count > rows ? output->count - rows : 0;
    if (e->offset > last) e->offset = last;
    for (int j = 0; j < rows && j + e->offset < output->count; j++) {
        int i = j + e->offset;
        char id[28], text[80];
        Mode mode = output->modes[i];
        mode_text(text, sizeof text, mode);
        if (i == output->preferred) strcat(text, "  ·  Preferred");
        snprintf(id, sizeof id, "mode-%d", i);
        bool selected = same_mode(mode, p->mode);
        if (action_row(e, id, (cui_rect){23, 91 + j * 33, w - 46, 31}, text, ICON_NONE,
                       selected ? ICON_CHECK : ICON_NONE, selected, blocked, true)) {
            p->mode = mode;
            open_page(e, DISPLAYS);
            edit(e);
        }
    }
    page_control(e, w - 24, h - 48, output->count, rows);
}

static void
draw_arrangements(Editor *e, bool blocked)
{
    static const struct { const char *key, *name; enum Icon icon; } choices[] = {
        {"row", "Row", ICON_ROW}, {"column", "Column", ICON_COLUMN},
        {"y", "Two above", ICON_TWO_ABOVE}, {"mirror", "Mirror", ICON_MIRROR},
        {"extend", "Extend", ICON_MONITOR}
    };
    subpage_title(e, "Arrange displays", blocked);
    float y = 70;
    for (size_t i = 0; i < sizeof choices / sizeof *choices; i++) {
        const char *kind = choices[i].key;
        if ((!strcmp(kind, "y") && e->draft.count != 3) ||
            (!strcmp(kind, "extend") && !e->mirrored) ||
            (!strcmp(kind, "mirror") && e->mirrored)) continue;
        if (action_row(e, kind, (cui_rect){23, y, e->paint.width - 46, 40},
                       choices[i].name, choices[i].icon, ICON_NONE, false, blocked, false)) {
            if (!strcmp(kind, "mirror") || !strcmp(kind, "extend")) set_mirror(e, !e->mirrored);
            else use_suggestion(e, kind);
            e->page = DISPLAYS;
            break;
        }
        y += 44;
    }
}

static void
save(Editor *e)
{
    if (!preset_name(e->name)) {
        strcpy(e->form_error, "Use letters, digits, spaces, dots, - or _.");
        return;
    }
    Preset preset = {.automatic = e->save_auto, .layout = e->base};
    strcpy(preset.name, e->name);
    if (!normalize_layout(&e->live.topology, &preset.layout, e->form_error) ||
        !preset_write(&e->config, &preset, false, e->form_error)) return;
    open_page(e, PRESETS_PAGE);
    snprintf(e->notice, sizeof e->notice, "Saved %.63s.", e->name);
}

static void
draw_preset_form(Editor *e, float width, float height)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    float w = fminf(376, width - 32), h = 214;
    fill(e, (cui_rect){0, 0, width, height}, (cui_color){0, 0, 0, 155}, 0);
    cui_id scope = cui_id_from(ui, "preset-dialog-scope");
    if (!cui_begin_focus_scope(ui, scope, true)) return;
    cui_begin_absolute_box(ui, cui_id_from(ui, "preset-dialog"), (cui_box){
        .background = t.panel, .radius = t.radius}, (cui_rect){floorf((width - w) / 2),
        floorf((height - h) / 2), w, h});
    cui_interact(ui, cui_id_from(ui, "preset-dialog"), CUI_ROLE_DIALOG,
                 cui_string("New preset"), false, false, 0);
    cui_drop_shadow shadow = {.offset_y = 8, .sigma = 14, .color = {0, 0, 0, 152}};
    cui_set_drop_shadows(ui, &shadow, 1, (cui_corner_radii){t.radius, t.radius, t.radius, t.radius});
    label(e, (cui_rect){20, 12, w - 40, 32}, "New preset", 22, t.foreground, false, false);
    label(e, (cui_rect){20, 50, w - 40, 21}, "Name", 13, t.muted, false, false);

    cui_id input = cui_id_from(ui, "preset-name");
    bool focused = e->focus_name || cui_focused(ui) == input;
    cui_begin_absolute_box(ui, input, (cui_box){.background = t.background, .radius = t.radius,
        .border = focused ? t.accent : t.border, .border_width = 1}, (cui_rect){20, 75, w - 40, 37});
    cui_interact(ui, input, CUI_ROLE_TEXTBOX, cui_string("Preset name"), false, false, 0);
    cui_set_semantic_text_value_v1(ui, input, cui_string(e->name));
    if (e->focus_name) {
        cui_focus(ui, input);
        e->focus_name = false;
    }
    cui_text_style regular = {0};
    size_t start = 0;
    cui_str visible = {e->name, e->caret};
    while (start < e->caret && cui_measure_text_ex(ui, visible, 15, regular).width > w - 63) {
        start++;
        visible = (cui_str){e->name + start, e->caret - start};
    }
    float caret_x = cui_measure_text_ex(ui, visible, 15, regular).width;
    label(e, (cui_rect){10, 0, w - 60, 37}, e->name + start, 15, t.foreground, false, false);
    if (focused) fill(e, (cui_rect){10 + floorf(caret_x), 10, 1, 17}, t.accent, 0);
    cui_end_box(ui);

    if (checkbox(e, "form-auto", (cui_rect){14, 117, w - 34, 31}, "Autoapply on connection",
                 e->save_auto, false)) e->save_auto = !e->save_auto;
    label(e, (cui_rect){20, 149, w - 40, 20}, e->form_error, 12, t.warning, false, false);
    if (button(e, "form-close", (cui_rect){w - 188, 165, 72, 33}, "Cancel",
               ICON_NONE, QUIET, false, false, false)) e->form = false;
    if (button(e, "form-save", (cui_rect){w - 109, 165, 89, 33}, "Save",
               ICON_NONE, PRIMARY, !*e->name, false, false)) save(e);
    cui_end_box(ui);
    cui_end_focus_scope(ui);
}

static void
draw_presets(Editor *e, bool blocked)
{
    Theme t = e->paint.theme;
    float w = (float)e->paint.width, h = (float)e->paint.height;
    bool can_save = !blocked && main_display(&e->base) && !e->editing;
    subpage_title(e, "Presets", blocked);
    if (button(e, "new-preset", (cui_rect){w - 145, 19, 121, 32}, "New preset", ICON_PLUS,
               PRIMARY, !can_save, false, false)) {
        e->form = e->focus_name = true;
        e->save_auto = false;
        e->caret = 0;
        *e->name = *e->form_error = 0;
    }
    if (!e->menu_count)
        label(e, (cui_rect){26, 77, w - 52, 26}, "No saved presets yet.", 15, t.muted, false, false);
    int rows = (int)fmaxf(1, floorf((h - 119) / 58));
    int last = e->menu_count > rows ? e->menu_count - rows : 0;
    if (e->offset > last) e->offset = last;
    for (int row = 0; row < rows && row + e->offset < e->menu_count; row++) {
        int i = row + e->offset;
        float y = 66 + row * 58;
        char path[PATH_SIZE], error[ERROR_SIZE], id[NAME + 8];
        Preset preset;
        Layout layout;
        bool found = preset_path(&e->config, e->presets[i], path, error);
        bool read = found && preset_read(path, &preset, error);
        bool match = read && resolve_layout(&e->live.topology, &preset.layout, e->config.match_ports,
            &layout, error) && normalize_layout(&e->live.topology, &layout, error);
        if (row) fill(e, (cui_rect){25, y, w - 50, 1}, mix(t.background, t.border, .47f), 0);
        label(e, (cui_rect){26, y + 3, w - 258, 27}, e->presets[i], 17, t.foreground, false, false);
        if (found) {
            const char *home = getenv("HOME");
            size_t len = home ? strlen(home) : 0;
            if (len && !strncmp(path, home, len) && path[len] == '/') {
                memmove(path + 1, path + len, strlen(path + len) + 1);
                path[0] = '~';
            }
            label(e, (cui_rect){26, y + 31, w - 250, 19}, path, 12, t.muted, true, false);
        }
        snprintf(id, sizeof id, "auto-%s", e->presets[i]);
        if (checkbox(e, id, (cui_rect){w - 224, y + 13, 105, 31}, "Autoapply",
                     read && preset.automatic, blocked || !read)) {
            preset.automatic = !preset.automatic;
            if (preset_write(&e->config, &preset, true, e->notice)) *e->notice = 0;
        }
        if (match) {
            snprintf(id, sizeof id, "load-%s", e->presets[i]);
            if (action_row(e, id, (cui_rect){w - 103, y + 13, 79, 31}, "Load", ICON_NONE,
                           ICON_FORWARD, false, blocked, false)) {
                e->draft = layout;
                open_page(e, DISPLAYS);
                edit(e);
            }
        } else label(e, (cui_rect){w - 126, y + 13, 102, 31}, read ? "Different setup" :
                     "Unavailable", 12, t.muted, false, true);
    }
    page_control(e, w - 24, h - 48, e->menu_count, rows);
}

static void
draw_displays(Editor *e, bool blocked)
{
    Theme t = e->paint.theme;
    float side = drawer_width(e), w = e->paint.width - side, h = (float)e->paint.height;
    label(e, (cui_rect){24, 13, w - 168, 43}, "Displays", 27, t.foreground, false, false);
    if (action_row(e, "nav-presets", (cui_rect){w - 121, 19, 97, 32}, "Presets", ICON_NONE,
                   ICON_FORWARD, false, blocked, false)) open_page(e, PRESETS_PAGE);
    draw_canvas(e, (cui_rect){24, 63, w - 48, h - 145}, blocked);
    if (side) draw_details(e, (cui_rect){w, 0, side, h}, blocked);

    if (!blocked) {
        if (button(e, "identify", (cui_rect){23, h - 53, 84, 33}, "Identify", ICON_EYE,
                   QUIET, !main_display(&e->base), false, false)) ask(e, IDENTIFY, NULL);
        if (e->draft.count > 1 &&
            button(e, "arrange", (cui_rect){111, h - 53, 91, 33}, "Arrange", ICON_ROW,
                   QUIET, false, false, false)) open_page(e, ARRANGE);
        if (e->editing) {
            if (button(e, "reset", (cui_rect){w - 182, h - 53, 66, 33}, "Reset", ICON_NONE,
                       QUIET, false, false, false)) {
                e->draft = e->base;
                edit(e);
            }
            if (button(e, "apply", (cui_rect){w - 104, h - 53, 80, 33}, "Apply", ICON_NONE,
                       PRIMARY, false, false, false)) ask(e, APPLY, &e->draft);
        }
    }
}

static void
draw(Editor *e)
{
    int w = e->paint.width, h = e->paint.height;
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    if (!cui_begin_frame(ui, (float)w, (float)h, .016f)) return;
    bool pending = e->live.remaining > 0;
    bool modal = e->form && !pending;
    cui_begin_box(ui, cui_id_from(ui, "window"), (cui_box){.width = (float)w, .height = (float)h});
    cui_begin_absolute_box(ui, 0, (cui_box){.inert = modal}, (cui_rect){0, 0, w, h});
    switch (e->page) {
    case DISPLAYS: draw_displays(e, pending); break;
    case PRESETS_PAGE: draw_presets(e, pending); break;
    case ARRANGE: draw_arrangements(e, pending); break;
    case DISPLAY_MODES: draw_modes(e, pending); break;
    }
    float action_width = w - (e->page == DISPLAYS ? drawer_width(e) : 0);
    if (pending) {
        char confirm[96];
        snprintf(confirm, sizeof confirm, "Keep this layout?  Reverting in %ds", e->live.remaining);
        float message_y = action_width < 550 ? h - 81 : h - 53;
        float message_width = action_width - (action_width < 550 ? 48 : 235);
        label(e, (cui_rect){24, message_y, message_width, 33}, confirm, 14, t.warning, false, false);
        if (button(e, "revert", (cui_rect){action_width - 192, h - 53, 78, 33}, "Revert", ICON_NONE,
                   QUIET, false, false, false)) ask(e, REVERT, NULL);
        if (button(e, "keep", (cui_rect){action_width - 105, h - 53, 81, 33}, "Keep", ICON_NONE,
                   PRIMARY, false, false, false)) ask(e, KEEP, NULL);
    } else if (*e->notice) {
        label(e, (cui_rect){25, h - (e->page == DISPLAYS ? 79 : 52), action_width - 111, 26},
              e->notice, 13, t.muted, false, false);
    }
    cui_end_box(ui);
    if (modal) draw_preset_form(e, w, h);
    cui_end_box(ui);
    painter_present(&e->paint, &e->x, e->window, e->gc, cui_end_frame(ui));
    e->frame_ready = true;
    XFlush(e->x.display);
}

static int
near_zero(int offset, int best)
{
    return abs(offset) < abs(best) ? offset : best;
}

static void
drag(Editor *e, int x, int y)
{
    Placement *p = &e->draft.output[e->dragging];
    p->x = e->from_x + (int)lroundf((x - e->start_x) / e->scale);
    p->y = e->from_y + (int)lroundf((y - e->start_y) / e->scale);
    p->x = (int)fmax(-32000, fmin(32000, p->x));
    p->y = (int)fmax(-32000, fmin(32000, p->y));
    int threshold = (int)(e->config.snap / e->scale);
    int sx = threshold + 1, sy = threshold + 1, w, h;
    dimensions(*p, &w, &h);
    for (int i = 0; i < e->draft.count; i++) {
        Placement q = e->draft.output[i];
        if (i == e->dragging || !q.enabled) continue;
        int qw, qh;
        dimensions(q, &qw, &qh);
        if (p->y <= q.y + qh + threshold && p->y + h >= q.y - threshold) {
            sx = near_zero(q.x - p->x, sx);
            sx = near_zero(q.x + qw - p->x - w, sx);
            sx = near_zero(q.x + qw - p->x, sx);
            sx = near_zero(q.x - p->x - w, sx);
        }
        if (p->x <= q.x + qw + threshold && p->x + w >= q.x - threshold) {
            sy = near_zero(q.y - p->y, sy);
            sy = near_zero(q.y + qh - p->y - h, sy);
            sy = near_zero(q.y + qh - p->y, sy);
            sy = near_zero(q.y - p->y - h, sy);
        }
    }
    if (abs(sx) <= threshold) p->x += sx;
    if (abs(sy) <= threshold) p->y += sy;
    edit(e);
}

static void
cancel_pointer(Editor *e)
{
    if (!e->pointer_down) return;
    e->pointer_down = false;
    e->dragging = -1;
    cui_queue_event(e->paint.ui, (cui_event){.type = CUI_EVENT_CANCEL});
}

static void
handle_pointer(Editor *e, XEvent event)
{
    bool motion = event.type == MotionNotify;
    bool press = event.type == ButtonPress;
    unsigned state = motion ? event.xmotion.state : event.xbutton.state;
    int x = motion ? event.xmotion.x : event.xbutton.x;
    int y = motion ? event.xmotion.y : event.xbutton.y;
    if (state & Mod4Mask) {
        cancel_pointer(e);
        return;
    }
    if (!motion && event.xbutton.button != Button1) {
        bool pager = !e->form && (e->page == PRESETS_PAGE || e->page == DISPLAY_MODES);
        if (press && pager && event.xbutton.button == Button4 && e->offset > 0) e->offset--;
        if (press && pager && event.xbutton.button == Button5) e->offset++;
        return;
    }
    if (!motion && !press && !e->pointer_down) return;
    if (press) e->drag_moved = false;
    if (!motion) e->pointer_down = press;
    cui_queue_event(e->paint.ui, (cui_event){
        .type = motion ? CUI_EVENT_POINTER_MOVE : (press ? CUI_EVENT_POINTER_DOWN : CUI_EVENT_POINTER_UP),
        .x = (float)x, .y = (float)y});
    if (!e->live.remaining && e->page == DISPLAYS && press && inside(e->canvas, x, y)) {
        int found = -1;
        if (e->selected >= 0 && inside(e->tiles[e->selected], x, y)) found = e->selected;
        else for (int i = e->draft.count - 1; i >= 0; i--)
            if (inside(e->tiles[i], x, y)) { found = i; break; }
        if (found >= 0) {
            Placement p = e->draft.output[found];
            if (p.enabled && !e->mirrored) {
                e->dragging = found;
                e->start_x = x;
                e->start_y = y;
                e->from_x = p.x;
                e->from_y = p.y;
            }
        }
    }
    if (!press && e->dragging >= 0 &&
        (e->drag_moved || abs(x - e->start_x) > 2 || abs(y - e->start_y) > 2)) {
        e->drag_moved = true;
        drag(e, x, y);
    }
    if (!motion && !press) e->dragging = -1;
}

static void
keyboard(Editor *e, XKeyEvent event)
{
    KeySym symbol;
    char bytes[16];
    int n = XLookupString(&event, bytes, sizeof bytes, &symbol, NULL);
    bool down = event.type == KeyPress;
    if (event.state & Mod4Mask) return;
    if (symbol == XK_Return && !down && e->save_enter) {
        e->save_enter = false;
        return;
    }
    if (down && symbol == XK_Escape) {
        if (e->live.remaining) ask(e, REVERT, NULL);
        else if (e->form) e->form = false;
        else if (e->page != DISPLAYS) open_page(e, DISPLAYS);
        else if (e->selected >= 0) e->selected = -1;
        else e->closing = true;
        return;
    }
    if (e->form && !e->live.remaining && down && (e->focus_name ||
        cui_focused(e->paint.ui) == cui_id_from(e->paint.ui, "preset-name"))) {
        size_t length = strlen(e->name);
        switch (symbol) {
        case XK_Return:
            e->save_enter = true;
            if (length && main_display(&e->base)) save(e);
            return;
        case XK_Home: e->caret = 0; return;
        case XK_End: e->caret = length; return;
        case XK_Left: if (e->caret) e->caret--; return;
        case XK_Right: if (e->caret < length) e->caret++; return;
        case XK_BackSpace:
            if (e->caret) {
                memmove(e->name + e->caret - 1, e->name + e->caret, length - e->caret + 1);
                e->caret--;
            }
            return;
        case XK_Delete:
            if (e->caret < length)
                memmove(e->name + e->caret, e->name + e->caret + 1, length - e->caret);
            return;
        default:
            if (n == 1 && length < NAME - 1 && bytes[0] >= 32 && bytes[0] <= 126 && !(event.state & ControlMask)) {
                memmove(e->name + e->caret + 1, e->name + e->caret, length - e->caret + 1);
                e->name[e->caret++] = bytes[0];
                return;
            }
        }
    }
    if (down && e->page == DISPLAYS && !e->live.remaining && e->selected >= 0 &&
        (symbol == XK_Left || symbol == XK_Right || symbol == XK_Up || symbol == XK_Down)) {
        Placement *p = &e->draft.output[e->selected];
        if (p->enabled && !e->mirrored) {
            int by = event.state & ShiftMask ? 100 : 20;
            int direction = symbol == XK_Left || symbol == XK_Up ? -1 : 1;
            int *coordinate = symbol == XK_Left || symbol == XK_Right ? &p->x : &p->y;
            *coordinate = (int)fmax(-32000, fmin(32000, *coordinate + direction * by));
            edit(e);
        }
        return;
    }
    cui_key key = symbol == XK_Tab || symbol == XK_ISO_Left_Tab ? CUI_KEY_TAB :
        (symbol == XK_Return ? CUI_KEY_ENTER : (symbol == XK_space ? CUI_KEY_SPACE : CUI_KEY_NONE));
    if (key != CUI_KEY_NONE)
        cui_queue_event(e->paint.ui, (cui_event){.type = down ? CUI_EVENT_KEY_DOWN : CUI_EVENT_KEY_UP,
            .key = key, .modifiers = (event.state & ShiftMask) ? CUI_MOD_SHIFT : 0});
}

static void
wake_editor(Editor *e)
{
    Display *d = e->x.display;
    XWindowAttributes attrs;
    int x, y;
    Window child;
    if (!XGetWindowAttributes(d, e->window, &attrs) ||
        !XTranslateCoordinates(d, e->window, e->x.root, 0, 0, &x, &y, &child)) return;
    bool visible = false;
    for (int i = 0; i < e->base.count; i++) {
        Placement p = e->base.output[i];
        if (!p.enabled) continue;
        int w, h;
        dimensions(p, &w, &h);
        if (x < p.x + w && x + attrs.width > p.x && y < p.y + h && y + attrs.height > p.y) {
            visible = true;
            break;
        }
    }
    /* mwm moves windows on other tags off-screen. Withdraw so mapping manages it on this tag. */
    if (!visible || attrs.map_state != IsViewable) {
        XWithdrawWindow(d, e->window, e->x.screen);
        keep_visible(e);
    }
    XMapRaised(d, e->window);
    XEvent active = {.xclient = {.type = ClientMessage, .window = e->window, .display = d,
        .message_type = XInternAtom(d, "_NET_ACTIVE_WINDOW", False), .format = 32}};
    active.xclient.data.l[0] = 2;
    XSendEvent(d, e->x.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &active);
    XFlush(d);
}

static void
send_wake(Editor *e, Window existing)
{
    Display *d = e->x.display;
    XEvent event = {.xclient = {.type = ClientMessage, .window = existing,
        .message_type = e->wake, .display = d, .format = 32}};
    XSendEvent(d, existing, False, NoEventMask, &event);
}

int
editor_main(void)
{
    static Editor e;
    e.dragging = -1;
    e.selected = -1;
    char error[ERROR_SIZE];
    if (!config_load(&e.config, error) || !x11_open(&e.x, error)) {
        fprintf(stderr, "undocked: %s\n", error);
        return 1;
    }
    Display *d = e.x.display;
    char selection_name[64];
    snprintf(selection_name, sizeof selection_name, "_UNDOCKED_EDITOR_S%d", e.x.screen);
    Atom selection = XInternAtom(d, selection_name, False);
    e.wake = XInternAtom(d, "_UNDOCKED_SHOW_EDITOR", False);
    XGrabServer(d);
    Window existing = XGetSelectionOwner(d, selection);
    if (existing) send_wake(&e, existing);
    XUngrabServer(d);
    if (existing) {
        XCloseDisplay(d);
        return 0;
    }
    XFlush(d);
    if (!ask(&e, GET, NULL)) {
        fprintf(stderr, "undocked: %s\n", e.notice);
        XCloseDisplay(d);
        return 1;
    }
    int width = e.config.width, height = e.config.height;
    int area_x = 0, area_y = 0, area_width = e.live.topology.width, area_height = e.live.topology.height;
    const Placement *anchor = main_display(&e.base);
    if (anchor) {
        area_x = anchor->x;
        area_y = anchor->y;
        dimensions(*anchor, &area_width, &area_height);
    }
    if (width > area_width && area_width > 0) width = area_width;
    if (height > area_height && area_height > 0) height = area_height;
    if (!painter_open(&e.paint, &e.x, &e.config, width, height)) {
        fprintf(stderr, "undocked: Could not initialize the CUI renderer.\n");
        XCloseDisplay(d);
        return 1;
    }
    int left = area_x + (area_width - width) / 2, top = area_y + (area_height - height) / 2;
    e.window = XCreateSimpleWindow(d, e.x.root, left > 0 ? left : 0, top > 0 ? top : 0,
        (unsigned)width, (unsigned)height, 0, 0, 0);
    e.gc = XCreateGC(d, e.window, 0, NULL);
    x11_class(&e.x, e.window, "undocked", "Undocked");
    XStoreName(d, e.window, "Undocked");
    Atom dialog = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(d, e.window, XInternAtom(d, "_NET_WM_WINDOW_TYPE", False),
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)&dialog, 1);
    XSizeHints hints = {.flags = PMinSize | PMaxSize, .min_width = width < 640 ? width : 640, .min_height = height < 480 ? height : 480,
                        .max_width = 1920, .max_height = 1440};
    XSetWMNormalHints(d, e.window, &hints);
    e.close = XInternAtom(d, "WM_DELETE_WINDOW", False);
    e.protocols = XInternAtom(d, "WM_PROTOCOLS", False);
    XSetWMProtocols(d, e.window, &e.close, 1);
    XSelectInput(d, e.window, ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | KeyPressMask | KeyReleaseMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask);
    XSelectInput(d, e.x.root, PropertyChangeMask);
    XGrabServer(d);
    existing = XGetSelectionOwner(d, selection);
    if (existing) send_wake(&e, existing);
    else XSetSelectionOwner(d, selection, e.window, CurrentTime);
    XUngrabServer(d);
    if (existing) {
        painter_close(&e.paint);
        XCloseDisplay(d);
        return 0;
    }
    XMapRaised(d, e.window);
    bool dirty = true;
    long long next = 0;
    while (!e.closing) {
        while (XPending(d)) {
            XEvent event;
            XNextEvent(d, &event);
            switch (event.type) {
            case ClientMessage:
                if (event.xclient.window != e.window || event.xclient.format != 32) break;
                if (event.xclient.message_type == e.wake) {
                    wake_editor(&e);
                    if (e.page == PRESETS_PAGE) e.menu_count = preset_list(&e.config, e.presets);
                    dirty = true;
                    next = 0;
                } else if (event.xclient.message_type == e.protocols &&
                           (Atom)event.xclient.data.l[0] == e.close) e.closing = true;
                break;
            case DestroyNotify:
                if (event.xdestroywindow.window == e.window) e.closing = true;
                break;
            case SelectionClear:
                if (event.xselectionclear.selection == selection) e.closing = true;
                break;
            case ConfigureNotify:
                if (event.xconfigure.width != e.paint.width || event.xconfigure.height != e.paint.height) {
                    painter_resize(&e.paint, &e.x, event.xconfigure.width, event.xconfigure.height);
                    dirty = true;
                }
                break;
            case Expose:
                if (!dirty && e.frame_ready && !event.xexpose.count) {
                    painter_repaint(&e.paint, &e.x, e.window, e.gc);
                    XFlush(d);
                }
                break;
            case ButtonPress: case ButtonRelease: case MotionNotify:
                handle_pointer(&e, event);
                dirty = true;
                break;
            case KeyPress: case KeyRelease:
                keyboard(&e, event.xkey);
                dirty = true;
                break;
            case PropertyNotify:
                if (event.xproperty.window == e.x.root && event.xproperty.atom == XA_RESOURCE_MANAGER) {
                    theme_read(&e.paint.theme, &e.x, &e.config);
                    dirty = true;
                }
                break;
            case LeaveNotify:
                cui_queue_event(e.paint.ui, (cui_event){.type = CUI_EVENT_POINTER_LEAVE});
                dirty = true;
                break;
            case FocusOut:
                cancel_pointer(&e);
                dirty = true;
                break;
            default:
                if (x11_change(&e.x, &event)) next = 0;
            }
        }
        long long time = now_ms();
        if (time >= next) {
            Layout before = e.base;
            int remaining = e.live.remaining;
            char previous[ERROR_SIZE];
            strcpy(previous, e.live.message);
            ask(&e, GET, NULL);
            if (remaining != e.live.remaining || !layout_equal(&before, &e.base) ||
                strcmp(previous, e.live.message)) dirty = true;
            next = time + 400;
        }
        if (dirty && !e.closing) {
            e.redraw = false;
            draw(&e);
            dirty = e.redraw;
        }
        if (dirty) continue;
        struct pollfd events = {ConnectionNumber(d), POLLIN, 0};
        poll(&events, 1, 100);
    }
    painter_close(&e.paint);
    XFreeGC(d, e.gc);
    XCloseDisplay(d);
    return 0;
}
