#include "painter.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Page { DISPLAYS, PRESETS_PAGE };

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
    bool inspector, modes, form, save_enter, pointer_down, drag_moved, frame_ready;
    size_t caret;
    int selected, dragging, start_x, start_y, from_x, from_y, offset, menu_count;
    float scale, origin_x, origin_y;
    cui_rect tiles[OUTPUTS], canvas;
    enum Page page;
    char name[NAME], notice[ERROR_SIZE];
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

enum ButtonStyle { QUIET, OUTLINE, SELECTED, PRIMARY };

static bool
button(Editor *e, const char *key, cui_rect bounds, const char *name, enum Icon icon,
       enum ButtonStyle style, bool disabled, bool mono, bool icon_only)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_id id = cui_id_from(ui, key);
    cui_interaction last = cui_interaction_state(ui, id);
    cui_color bg = {0}, border = {0}, fg = t.foreground;
    if (style == OUTLINE) { bg = t.panel; border = t.border; }
    if (style == SELECTED) { bg = mix(t.panel, t.accent, .15f); border = t.accent; fg = t.accent; }
    if (style == PRIMARY) { bg = t.accent; fg = t.on_accent; }
    if (disabled) {
        bg = style == QUIET ? (cui_color){0} : t.panel;
        border = style == QUIET ? (cui_color){0} : mix(t.panel, t.border, .64f);
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
    paint_icon(ui, icon, 17, fg);
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
            e->modes = false;
            strcpy(e->notice, "Connected displays changed. Layout refreshed.");
        }
        if (e->selected >= e->draft.count) e->selected = 0;
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
    if (!p->enabled) background = mix(t.background, t.panel, .45f);
    cui_interaction last = cui_interaction_state(ui, element);
    cui_begin_absolute_box(ui, element, (cui_box){.background = background,
        .border = n == e->selected || last.focus_visible ? t.accent : t.border,
        .border_width = n == e->selected ? 2 : 1, .radius = t.radius}, r);
    cui_interaction clicked = cui_interact(ui, element, CUI_ROLE_BUTTON, cui_string(p->port),
                                          disabled, n == e->selected, 0);
    if (clicked.activated && !e->drag_moved) {
        e->selected = n;
        e->inspector = true;
        e->modes = false;
        e->redraw = true;
    }
    label(e, (cui_rect){4, 2, r.w - 8, r.h > 65 ? r.h - 28 : r.h - 4},
        number, floorf(fminf(54, fminf(r.h * .55f, r.w * .38f))), p->enabled ? t.foreground : t.muted, true, true);
    if (r.h > 65)
        label(e, (cui_rect){6, r.h - 26, r.w - 12, 20}, p->port, 13, t.foreground, true, true);
    if (p->primary && r.w > 80 && r.h > 50) {
        fill(e, (cui_rect){9, 9, 64, 20}, mix(t.background, t.panel, .45f), t.radius);
        label(e, (cui_rect){9, 9, 64, 20}, "Primary", 11, t.accent, false, true);
    }
    cui_end_box(ui);
}

static void
draw_canvas(Editor *e, cui_rect frame, bool disabled)
{
    e->canvas = frame;
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, cui_id_from(ui, "canvas"), (cui_box){
        .background = mix(t.background, t.panel, .27f), .border = t.border, .border_width = 1,
        .radius = t.radius}, frame);
    cui_color dot = mix(t.background, t.border, .75f);
    int step = 22 + (int)fmaxf(frame.w, frame.h) / 120;
    for (int y = 16; y < frame.h - 11; y += step)
        for (int x = 16; x < frame.w - 11; x += step)
            fill(e, (cui_rect){(float)x, (float)y, 1, 1}, dot, 0);

    int inactive = 0;
    for (int i = 0; i < e->draft.count; i++) if (!e->draft.output[i].enabled) inactive++;
    int grid_columns = (int)((frame.w - 20) / 52);
    if (grid_columns < 1) grid_columns = 1;
    int rows = (inactive + grid_columns - 1) / grid_columns;
    cui_rect area = frame;
    area.h -= (float)rows * 42;
    projection(e, area);
    for (int i = 0, off = 0; i < e->draft.count; i++) {
        Placement p = e->draft.output[i];
        int w, h;
        dimensions(p, &w, &h);
        if (!p.enabled) {
            e->tiles[i] = (cui_rect){frame.x + 14 + 52 * (off % grid_columns),
                frame.y + frame.h - 42 * rows + 42 * (off / grid_columns), 44, 32};
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
            float right = fminf(r.x + r.w, frame.x + frame.w - 2);
            float bottom = fminf(r.y + r.h, frame.y + frame.h - 2);
            r.x = fmaxf(r.x, frame.x + 2);
            r.y = fmaxf(r.y, frame.y + 2);
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
        snprintf(text, size, "%d × %d  ·  %g Hz", mode.width, mode.height,
                 round(mode.millihz / 10.0) / 100);
    else snprintf(text, size, "%d × %d", mode.width, mode.height);
}

static bool
page_control(Editor *e, float right, float y, int count, int rows, int *offset)
{
    bool change = false;
    if (button(e, "prev", (cui_rect){right - 62, y, 29, 28}, "Previous page",
               ICON_UP, QUIET, !*offset, false, true)) {
        *offset = *offset > rows ? *offset - rows : 0;
        change = true;
    }
    if (button(e, "next", (cui_rect){right - 29, y, 29, 28}, "Next page",
               ICON_DOWN, QUIET, *offset + rows >= count, false, true)) {
        *offset += rows;
        change = true;
    }
    return change;
}

static void
draw_details(Editor *e, cui_rect frame, bool disabled)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, cui_id_from(ui, "details"), (cui_box){
        .background = t.panel, .border = t.border, .border_width = 1, .radius = t.radius}, frame);
    int selected = e->selected;
    Placement *p = &e->draft.output[selected];
    const Output *output = &e->live.topology.output[selected];
    const char *alias = alias_for(&e->config, p->port);
    char connector[80], text[128];
    float w = frame.w - 28;
    snprintf(connector, sizeof connector, "DISPLAY %d  /  %s", selected + 1, p->port);
    label(e, (cui_rect){14, 10, w - 26, 23}, connector, 12, t.accent, true, false);
    if (button(e, "inspector-close", (cui_rect){frame.w - 38, 9, 27, 26}, "Close inspector",
               ICON_CLOSE, QUIET, false, false, true)) {
        e->inspector = false;
        e->modes = false;
    }
    if (e->modes) {
        if (button(e, "modes-back", (cui_rect){14, 43, 76, 29}, "Back", ICON_BACK,
                   QUIET, false, false, false)) e->modes = false;
        label(e, (cui_rect){14, 78, w, 22}, "Resolution and refresh rate", 12, t.foreground, true, false);
        int count = output->count;
        int rows = (int)fmaxf(1, floorf((frame.h - 142) / 32));
        if (e->offset >= count) e->offset = count > rows ? count - rows : 0;
        for (int j = 0; j < rows && j + e->offset < count; j++) {
            int index = j + e->offset;
            char id[28];
            snprintf(id, sizeof id, "mode-%d", index);
            Mode mode = output->modes[index];
            mode_text(text, sizeof text, mode);
            if (button(e, id, (cui_rect){14, 106 + 32 * j, w, 28}, text,
                       index == output->preferred ? ICON_CHECK : ICON_NONE,
                       same_mode(mode, p->mode) ? SELECTED : OUTLINE, disabled, true, false)) {
                p->mode = mode;
                e->modes = false;
                edit(e);
            }
        }
        if (count > rows) page_control(e, frame.w - 14, frame.h - 33, count, rows, &e->offset);
        cui_end_box(ui);
        return;
    }

    float compact = frame.h < 239 ? 20 : 0;
    label(e, (cui_rect){14, 40, w, 31}, *alias ? alias : output->model, 20, t.foreground, false, false);
    int active = 0;
    for (int i = 0; i < e->draft.count; i++) active += e->draft.output[i].enabled;
    if (button(e, "power", (cui_rect){14, 78, w, 32}, p->enabled ? "Disable display" : "Enable display",
               ICON_POWER, OUTLINE, disabled || (p->enabled && active <= 1) || !output->count, false, false) &&
        layout_enable(&e->live.topology, &e->draft, selected, !p->enabled)) edit(e);
    fill(e, (cui_rect){14, 123 - compact * .4f, w, 1}, t.border, 0);
    label(e, (cui_rect){14, 130 - compact * .5f, w, 21}, "Resolution and refresh rate", 12, t.foreground, true, false);
    mode_text(text, sizeof text, p->mode);
    if (button(e, "mode", (cui_rect){14, 156 - compact, w, 34}, text, ICON_DOWN,
               OUTLINE, disabled || !p->enabled, true, false)) {
        e->modes = true;
        e->offset = 0;
    }
    snprintf(text, sizeof text, "%d°", p->rotation);
    if (button(e, "rotate", (cui_rect){14, 199 - compact, w * .44f, 32}, text, ICON_RESET, OUTLINE,
               disabled || !p->enabled || output->rotations == 1, true, false)) {
        do p->rotation = (p->rotation + 90) % 360;
        while (!(output->rotations & (1 << (p->rotation / 90))));
        edit(e);
    }
    if (button(e, "primary", (cui_rect){20 + w * .44f, 199 - compact, w * .56f - 6, 32},
               p->primary ? "Primary" : "Set primary", ICON_NONE, p->primary ? SELECTED : OUTLINE,
               disabled || !p->enabled || p->primary, false, false)) {
        for (int i = 0; i < e->draft.count; i++) e->draft.output[i].primary = i == selected;
        edit(e);
    }
    if (frame.h >= 266) {
        if (p->enabled) snprintf(text, sizeof text, "Position  %d, %d", p->x, p->y);
        else strcpy(text, "Connected · Disabled");
        label(e, (cui_rect){14, 240, w, 22}, text, 12, t.muted, true, false);
    }
    cui_end_box(ui);
}

static void
open_presets(Editor *e, bool form)
{
    e->page = PRESETS_PAGE;
    e->offset = 0;
    e->menu_count = preset_list(&e->config, e->presets);
    e->form = form;
    if (form) {
        e->save_auto = false;
        e->caret = 0;
        *e->name = *e->notice = 0;
    }
}

static void
save(Editor *e)
{
    if (!preset_name(e->name)) {
        strcpy(e->notice, "Use letters, digits, spaces, dots, - or _.");
        return;
    }
    Preset preset = {.automatic = e->save_auto, .layout = e->base};
    strcpy(preset.name, e->name);
    if (!normalize_layout(&e->live.topology, &preset.layout, e->notice) ||
        !preset_write(&e->config, &preset, false, e->notice)) return;
    open_presets(e, false);
    snprintf(e->notice, sizeof e->notice, "Saved %.63s.", e->name);
}

static void
draw_preset_form(Editor *e, cui_rect frame, bool disabled)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    float w = frame.w - 28;
    cui_begin_absolute_box(ui, cui_id_from(ui, "preset-form"), (cui_box){
        .background = t.panel, .border = t.border, .border_width = 1, .radius = t.radius}, frame);
    label(e, (cui_rect){14, 12, w, 29}, "New preset", 21, t.foreground, false, false);
    if (button(e, "form-close", (cui_rect){frame.w - 42, 13, 28, 28}, "Cancel new preset",
               ICON_CLOSE, QUIET, false, false, true)) e->form = false;
    label(e, (cui_rect){14, 55, w, 20}, "Preset name", 13, t.muted, false, false);
    fill(e, (cui_rect){14, 81, w, 39}, t.accent, t.radius);
    fill(e, (cui_rect){15, 82, w - 2, 37}, t.background, t.radius);
    cui_text_style regular = {0};
    size_t start = 0;
    cui_str visible = {e->name, e->caret};
    while (start < e->caret && cui_measure_text_ex(ui, visible, 15, regular).width > w - 26) {
        start++;
        visible = (cui_str){e->name + start, e->caret - start};
    }
    float caret_x = cui_measure_text_ex(ui, visible, 15, regular).width;
    label(e, (cui_rect){24, 83, w - 20, 34}, *e->name ? e->name + start : "Home office",
          15, *e->name ? t.foreground : t.muted, false, false);
    fill(e, (cui_rect){24 + floorf(caret_x), 92, 1, 17}, t.accent, 0);
    if (button(e, "form-auto", (cui_rect){14, 134, w, 33},
               e->save_auto ? "Autoapply enabled" : "Enable autoapply", e->save_auto ? ICON_CHECK : ICON_NONE,
               e->save_auto ? SELECTED : OUTLINE, disabled, false, false)) e->save_auto = !e->save_auto;
    if (button(e, "form-save", (cui_rect){14, 181, w, 35}, "Save preset", ICON_CHECK,
               PRIMARY, disabled || !*e->name, false, false)) save(e);
    cui_end_box(ui);
}

static void
draw_presets(Editor *e, bool blocked)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    float w = (float)e->paint.width, h = (float)e->paint.height;
    float panel = e->form ? fminf(280, w * .39f) : 0;
    float list_width = w - 44 - (panel ? panel + 14 : 0);
    bool can_save = !blocked && e->base.count && !e->editing;
    label(e, (cui_rect){24, 76, w - 195, 41}, "Saved presets", 28, t.foreground, false, false);
    char count[32];
    snprintf(count, sizeof count, "%d saved", e->menu_count);
    label(e, (cui_rect){25, 122, 240, 22}, count, 13, t.muted, true, false);
    if (button(e, "new-preset", (cui_rect){w - 159, 94, 136, 35}, "New preset", ICON_PLUS,
               e->form ? SELECTED : OUTLINE, !can_save, false, false)) open_presets(e, true);

    float section_y = 163;
    float section_h = h - section_y - 92;
    int rows = (int)fmaxf(1, floorf(section_h / 62));
    cui_begin_absolute_box(ui, cui_id_from(ui, "saved-presets"), (cui_box){
        .background = t.panel, .border = t.border, .border_width = 1, .radius = t.radius},
        (cui_rect){22, section_y, list_width, section_h});
    if (!e->menu_count) {
        icon_at(e, ICON_PRESETS, 18, 19, 24, t.accent);
        label(e, (cui_rect){52, 15, list_width - 63, 32}, "No saved presets", 20, t.foreground, false, false);
        label(e, (cui_rect){52, 49, list_width - 64, 21}, "Save the active display layout.",
              14, t.muted, false, false);
    }
    for (int row = 0; row < rows && row + e->offset < e->menu_count; row++) {
        int i = row + e->offset;
        float y = 5 + row * 62;
        char path[PATH_SIZE], error[ERROR_SIZE], id[32];
        Preset preset;
        Layout layout;
        bool read = preset_path(&e->config, e->presets[i], path, error) && preset_read(path, &preset, error);
        bool match = read && resolve_layout(&e->live.topology, &preset.layout, e->config.match_ports,
            &layout, error) && normalize_layout(&e->live.topology, &layout, error);
        if (row) fill(e, (cui_rect){14, y, list_width - 28, 1}, t.border, 0);
        label(e, (cui_rect){15, y + 5, list_width - 195, 25}, e->presets[i], 18, t.foreground, false, false);
        label(e, (cui_rect){15, y + 33, list_width - 185, 20}, match ? "Matches this setup" :
            (read ? "Different display setup" : "Preset unavailable"), 13, match ? t.accent : t.muted, false, false);
        snprintf(id, sizeof id, "use-%d", i);
        if (button(e, id, (cui_rect){list_width - 83, y + 15, 69, 31}, "Load", ICON_NONE,
                   match ? SELECTED : OUTLINE, blocked || !match, false, false)) {
            e->draft = layout;
            e->page = DISPLAYS;
            e->modes = false;
            e->form = false;
            edit(e);
        }
        snprintf(id, sizeof id, "auto-%d", i);
        if (button(e, id, (cui_rect){list_width - 174, y + 15, 83, 31},
                   read && preset.automatic ? "Auto on" : "Auto off", ICON_NONE,
                   read && preset.automatic ? SELECTED : OUTLINE, blocked || !read, false, false)) {
            preset.automatic = !preset.automatic;
            if (preset_write(&e->config, &preset, true, e->notice))
                snprintf(e->notice, sizeof e->notice, "Autoapply %s for %.63s.",
                         preset.automatic ? "enabled" : "disabled", preset.name);
        }
    }
    cui_end_box(ui);
    if (e->menu_count > rows)
        page_control(e, 22 + list_width, section_y + section_h + 7, e->menu_count, rows, &e->offset);
    if (e->form)
        draw_preset_form(e, (cui_rect){w - panel - 22, section_y, panel, 231}, !can_save);
}

static void
draw_displays(Editor *e, bool blocked)
{
    Theme t = e->paint.theme;
    float w = (float)e->paint.width, h = (float)e->paint.height;
    int active = 0;
    for (int i = 0; i < e->draft.count; i++) active += e->draft.output[i].enabled;
    char count[64];
    snprintf(count, sizeof count, "%d connected  /  %d active", e->draft.count, active);
    float rise = h < 530 ? 20 : 0;
    label(e, (cui_rect){24, 76 - rise * .5f, w - 285, 41}, "Display layout", 28, t.foreground, false, false);
    label(e, (cui_rect){25, 122 - rise * .7f, w - 285, 22}, count, 13, t.muted, true, false);

    if (button(e, "identify", (cui_rect){w - 248, 95, 105, 34}, "Identify", ICON_EYE,
               OUTLINE, !active, false, false)) ask(e, IDENTIFY, NULL);
    if (button(e, "inspector", (cui_rect){w - 134, 95, 112, 34}, "Inspector", ICON_INSPECT,
               e->inspector ? SELECTED : OUTLINE, !e->draft.count, false, false)) {
        e->inspector = !e->inspector;
        e->modes = false;
    }
    if (button(e, "extend", (cui_rect){22, 156 - rise, 98, 32}, "Extend", ICON_MONITOR,
               !e->mirrored ? SELECTED : OUTLINE, blocked || e->draft.count < 2, false, false)) set_mirror(e, false);
    if (button(e, "mirror", (cui_rect){127, 156 - rise, 99, 32}, "Mirror", ICON_MIRROR,
               e->mirrored ? SELECTED : OUTLINE, blocked || e->draft.count < 2, false, false)) set_mirror(e, true);

    bool show = e->inspector && e->draft.count;
    float side = w >= 800 ? 261 : 229;
    float canvas_width = w - 44 - (show ? side + 14 : 0);
    float body = h - 291 + rise;
    draw_canvas(e, (cui_rect){22, 201 - rise, canvas_width, body}, blocked);
    if (show) draw_details(e, (cui_rect){w - side - 22, 201 - rise, side, body}, blocked);

    if (e->draft.count >= 2) {
        float y = h - 80;
        if (button(e, "row", (cui_rect){22, y, 76, 28}, "Row", ICON_ROW, QUIET,
                   blocked, false, false)) use_suggestion(e, "row");
        if (button(e, "column", (cui_rect){104, y, 101, 28}, "Column", ICON_COLUMN, QUIET,
                   blocked, false, false)) use_suggestion(e, "column");
        if (e->draft.count == 3 &&
            button(e, "y", (cui_rect){211, y, 116, 28}, "Two above", ICON_TWO_ABOVE, QUIET,
                   blocked, false, false)) use_suggestion(e, "y");
    }
}

static void
draw(Editor *e)
{
    int w = e->paint.width, h = e->paint.height;
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    if (!cui_begin_frame(ui, (float)w, (float)h, .016f)) return;
    cui_begin_box(ui, cui_id_from(ui, "window"), (cui_box){.width = (float)w, .height = (float)h});
    fill(e, (cui_rect){0, 0, w, 58}, t.panel, 0);
    fill(e, (cui_rect){0, 57, w, 1}, t.border, 0);
    icon_at(e, ICON_DISPLAYS, 22, 14, 30, t.accent);
    bool pending = e->live.remaining > 0;
    if (button(e, "nav-displays", (cui_rect){68, 12, 108, 34}, "Displays", ICON_MONITOR,
               e->page == DISPLAYS ? SELECTED : QUIET, false, false, false)) {
        e->page = DISPLAYS;
        e->modes = false;
        e->form = false;
    }
    if (button(e, "nav-presets", (cui_rect){184, 12, 107, 34}, "Presets", ICON_PRESETS,
               e->page == PRESETS_PAGE ? SELECTED : QUIET, false, false, false) && e->page != PRESETS_PAGE)
        open_presets(e, false);
    if (e->page == PRESETS_PAGE) draw_presets(e, pending);
    else draw_displays(e, pending);

    fill(e, (cui_rect){0, h - 45, w, 45}, t.panel, 0);
    fill(e, (cui_rect){0, h - 45, w, 1}, t.border, 0);
    if (pending) {
        char confirm[96];
        snprintf(confirm, sizeof confirm, "Keep this layout?  Reverting in %ds", e->live.remaining);
        label(e, (cui_rect){22, h - 38, w - 230, 30}, confirm, 14, t.warning, false, false);
        if (button(e, "revert", (cui_rect){w - 213, h - 38, 94, 31}, "Revert", ICON_BACK,
                   OUTLINE, false, false, false)) ask(e, REVERT, NULL);
        if (button(e, "keep", (cui_rect){w - 112, h - 38, 90, 31}, "Keep", ICON_CHECK,
                   PRIMARY, false, false, false)) ask(e, KEEP, NULL);
    } else {
        const char *message = *e->notice ? e->notice :
            (e->editing ? "Layout has unapplied changes." :
             (e->page == DISPLAYS ? (e->draft.count > 1 ?
                 "Drag to arrange · Arrow keys to nudge" : e->live.message) : ""));
        label(e, (cui_rect){22, h - 38, w - 332, 30}, message, 14,
              *e->notice ? t.foreground : t.muted, false, false);
        if (e->page == DISPLAYS) {
            if (button(e, "reset", (cui_rect){w - 323, h - 38, 78, 31}, "Reset", ICON_RESET,
                       QUIET, !e->editing, false, false)) {
                e->draft = e->base;
                edit(e);
            }
            if (button(e, "save", (cui_rect){w - 236, h - 38, 121, 31}, "Save preset", ICON_PLUS,
                       OUTLINE, e->editing || !main_display(&e->base), false, false)) open_presets(e, true);
            if (button(e, "apply", (cui_rect){w - 106, h - 38, 84, 31}, "Apply", ICON_CHECK,
                       PRIMARY, !e->editing, false, false)) ask(e, APPLY, &e->draft);
        }
    }
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
        bool pager = e->page == PRESETS_PAGE || (e->inspector && e->modes);
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
        if (e->draft.count && inside(e->tiles[e->selected], x, y)) found = e->selected;
        else for (int i = e->draft.count - 1; i >= 0; i--)
            if (inside(e->tiles[i], x, y)) { found = i; break; }
        if (found >= 0) {
            if (e->selected != found) e->modes = false;
            e->selected = found;
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
        else if (e->form) { e->form = false; *e->notice = 0; }
        else if (e->page == PRESETS_PAGE) e->page = DISPLAYS;
        else if (e->modes) e->modes = false;
        else e->closing = true;
        return;
    }
    if (e->page == PRESETS_PAGE && e->form && !e->live.remaining && !e->editing && down) {
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
    if (down && e->page == DISPLAYS && !e->modes && !e->live.remaining && e->draft.count &&
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
