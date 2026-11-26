#include "painter.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Menu { NONE, MODES_MENU, PRESETS_MENU, SAVE_MENU };

typedef struct {
    X11 x;
    Window window;
    Atom close, wake;
    GC gc;
    Painter paint;
    Config config;
    Reply live;
    Layout base, draft, extended;
    bool editing, closing, mirrored, save_auto, have_extended;
    int selected, dragging, start_x, start_y, from_x, from_y, offset, menu_count;
    float scale, origin_x, origin_y;
    cui_rect tiles[OUTPUTS], canvas;
    enum Menu menu;
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
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, 0, (cui_box){
        .align_x = center ? CUI_ALIGN_CENTER : CUI_ALIGN_START, .align_y = CUI_ALIGN_CENTER}, bounds);
    cui_text_style style = {.monospace = mono};
    cui_str visible = cui_string(text);
    if (bounds.w <= 0 || bounds.h <= 0) { cui_end_box(ui); return; }
    while (visible.length && cui_measure_text_ex(ui, visible, size, style).width > bounds.w) {
        visible.length--;
        while (visible.length && ((unsigned char)visible.data[visible.length] & 0xc0) == 0x80)
            visible.length--;
    }
    cui_text_line(ui, visible, size, color, size + 4, style);
    cui_end_box(ui);
}

static bool
button(Editor *e, const char *key, cui_rect bounds, const char *name, bool accent, bool disabled)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_id id = cui_id_from(ui, key);
    cui_interaction previous = cui_interaction_state(ui, id);
    cui_color bg = accent ? t.accent : mix(t.panel, t.border, 0.18f);
    cui_color fg = accent ? t.on_accent : t.foreground;
    if (disabled) { bg = t.panel; fg = t.muted; }
    else if (previous.pressed || previous.hovered)
        bg = mix(bg, accent ? t.foreground : t.accent, previous.pressed ? .25f : .14f);
    cui_begin_absolute_box(ui, id, (cui_box){
        .background = bg, .border = previous.focus_visible ? t.accent : t.border, .border_width = 1,
        .radius = t.radius, .align_x = CUI_ALIGN_CENTER, .align_y = CUI_ALIGN_CENTER
    }, bounds);
    cui_interaction state = cui_interact(ui, id, CUI_ROLE_BUTTON, cui_string(name), disabled, false, 0);
    cui_text_line(ui, cui_string(name), 13, fg, 17, (cui_text_style){0});
    cui_end_box(ui);
    return state.activated;
}

static bool
is_mirror(const Layout *layout)
{
    const Placement *first = NULL;
    int count = 0;
    for (int i = 0; i < layout->count; i++) {
        const Placement *p = &layout->output[i];
        if (!p->enabled) continue;
        count++;
        if (!first) first = p;
        else if (p->x != first->x || p->y != first->y || p->mode.width != first->mode.width ||
                 p->mode.height != first->mode.height || p->rotation != first->rotation) return false;
    }
    return count >= 2;
}

static void
receive(Editor *e, Reply reply, bool reset)
{
    uint64_t old = topology_key(&e->live.topology);
    uint64_t next = topology_key(&reply.topology);
    bool changed = e->live.version && old != next;
    bool finished = e->live.remaining && !reply.remaining;
    e->live = reply;
    e->base = current_layout(&e->live.topology);
    if (reset || changed || !e->editing || finished) {
        e->draft = e->base;
        e->editing = false;
        e->dragging = -1;
        e->mirrored = is_mirror(&e->draft);
        if (changed) {
            e->have_extended = false;
            e->menu = NONE;
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
    e->mirrored = is_mirror(&e->draft);
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
    } else use_suggestion(e, "row");
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
    cui_color background = mix(t.panel, t.accent, n == e->selected ? .21f : .07f);
    if (!p->enabled) background = mix(t.background, t.panel, .45f);
    cui_interaction last = cui_interaction_state(ui, element);
    cui_begin_absolute_box(ui, element, (cui_box){.background = background,
        .border = n == e->selected || last.focus_visible ? t.accent : t.border,
        .border_width = n == e->selected ? 2 : 1, .radius = t.radius}, r);
    if (cui_interact(ui, element, CUI_ROLE_BUTTON, cui_string(p->port), disabled, n == e->selected, 0).activated)
        e->selected = n;
    label(e, (cui_rect){4, 2, r.w - 8, r.h > 65 ? r.h - 22 : r.h - 4},
        number, fminf(54, fminf(r.h * .57f, r.w * .4f)), p->enabled ? t.foreground : t.muted, true, true);
    if (r.h > 65)
        label(e, (cui_rect){6, r.h - 22, r.w - 12, 17}, p->port, 12, t.muted, true, true);
    if (p->primary && r.w > 60 && r.h > 50)
        fill(e, (cui_rect){r.w - 15, 9, 6, 6}, t.accent, 3);
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
        .radius = t.radius + 4}, frame);
    cui_color dot = mix(t.background, t.border, .72f);
    int step = 22 + (int)fmaxf(frame.w, frame.h) / 120;
    for (int y = 16; y < frame.h - 11; y += step)
        for (int x = 16; x < frame.w - 11; x += step)
            fill(e, (cui_rect){(float)x, (float)y, 2, 2}, dot, 1);

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
set_enabled(Editor *e, int index)
{
    Placement *p = &e->draft.output[index];
    if (p->enabled) p->enabled = false;
    else {
        int right = 0;
        for (int i = 0; i < e->draft.count; i++) {
            Placement other = e->draft.output[i];
            if (!other.enabled) continue;
            int width, height;
            dimensions(other, &width, &height);
            if (other.x + width > right) right = other.x + width;
        }
        const Output *out = &e->live.topology.output[index];
        p->enabled = true;
        p->mode = out->modes[out->preferred];
        p->x = right;
        p->y = 0;
    }
    edit(e);
}

static void
draw_details(Editor *e, cui_rect frame, bool disabled)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    cui_begin_absolute_box(ui, cui_id_from(ui, "details"), (cui_box){
        .background = t.panel, .border = t.border, .border_width = 1,
        .radius = t.radius + 4}, frame);
    if (!e->draft.count) { cui_end_box(ui); return; }
    int selected = e->selected;
    Placement *p = &e->draft.output[selected];
    const Output *output = &e->live.topology.output[selected];
    const char *alias = alias_for(&e->config, p->port);
    char connector[80], text[128];
    float width = frame.w - 28;
    snprintf(connector, sizeof connector, "%02d  /  %s", selected + 1, p->port);
    label(e, (cui_rect){14, 12, width, 19}, connector, 12, t.accent, true, false);
    label(e, (cui_rect){14, 33, width, 30}, *alias ? alias : output->model, 19, t.foreground, false, false);
    bool off = p->enabled;
    int count = 0;
    for (int i = 0; i < e->draft.count; i++) count += e->draft.output[i].enabled;
    if (button(e, "power", (cui_rect){14, 72, width, 31}, off ? "Disable display" : "Enable display",
               false, disabled || (off && count <= 1) || !output->count))
        set_enabled(e, selected);
    fill(e, (cui_rect){14, 117, width, 1}, t.border, 0);
    label(e, (cui_rect){14, 124, width, 19}, "Resolution and refresh rate", 12, t.muted, false, false);
    if (p->mode.millihz)
        snprintf(text, sizeof text, "%d × %d   ·   %g Hz", p->mode.width, p->mode.height,
                 round(p->mode.millihz / 10.0) / 100);
    else snprintf(text, sizeof text, "%d × %d", p->mode.width, p->mode.height);
    if (button(e, "mode", (cui_rect){14, 148, width, 31}, text, false, disabled || !p->enabled)) {
        e->menu = MODES_MENU;
        e->offset = 0;
    }
    snprintf(text, sizeof text, "Rotate  %d°", p->rotation);
    if (button(e, "rotate", (cui_rect){14, 188, width * .47f, 31}, text, false,
               disabled || !p->enabled || output->rotations == 1)) {
        do p->rotation = (p->rotation + 90) % 360;
        while (!(output->rotations & (1 << (p->rotation / 90))));
        edit(e);
    }
    if (button(e, "primary", (cui_rect){20 + width * .47f, 188, width * .53f - 6, 31},
               p->primary ? "Primary" : "Set primary", p->primary, disabled || !p->enabled || p->primary)) {
        for (int i = 0; i < e->draft.count; i++) e->draft.output[i].primary = i == selected;
        edit(e);
    }
    if (frame.h >= 258) {
        if (p->enabled) snprintf(text, sizeof text, "Position  %d, %d", p->x, p->y);
        else strcpy(text, "Connected · Disabled");
        label(e, (cui_rect){14, 229, width, 18}, text, 12, t.muted, true, false);
    }
    cui_end_box(ui);
}

static bool
save(Editor *e)
{
    if (!preset_name(e->name)) {
        strcpy(e->notice, "Enter a name using letters, digits, spaces, dots, - or _.");
        return false;
    }
    Preset preset = {.automatic = e->save_auto, .layout = e->base};
    strcpy(preset.name, e->name);
    if (!normalize_layout(&e->live.topology, &preset.layout, e->notice) ||
        !preset_write(&e->config, &preset, false, e->notice)) return false;
    snprintf(e->notice, sizeof e->notice, "Saved %.63s.", e->name);
    e->menu = NONE;
    return true;
}

static void
draw_menu(Editor *e)
{
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    int w = e->paint.width, h = e->paint.height;
    float width = 460, height = e->menu == SAVE_MENU ? 220 : fminf(450, h - 42);
    int rows = (int)(height - 110) / 36;
    cui_begin_portal(ui, cui_id_from(ui, "shade"), (cui_box){
        .background = {0, 0, 0, 144}}, (cui_rect){0, 0, (float)w, (float)h}, 12);
    cui_end_box(ui);
    cui_begin_portal(ui, cui_id_from(ui, "menu"), (cui_box){
        .background = t.panel, .border = t.border, .border_width = 1, .radius = t.radius + 6},
        (cui_rect){(w - width) / 2, (h - height) / 2, width, height}, 13);
    const char *title = e->menu == SAVE_MENU ? "Save preset" :
        (e->menu == PRESETS_MENU ? "Saved presets" : "Resolution and refresh rate");
    label(e, (cui_rect){18, 13, width - 90, 33}, title, 22, t.foreground, false, false);
    if (button(e, "menu-close", (cui_rect){width - 74, 16, 56, 28}, "Close", false, false)) {
        e->menu = NONE;
        cui_end_box(ui);
        return;
    }

    if (e->menu == SAVE_MENU) {
        fill(e, (cui_rect){18, 62, width - 36, 43}, t.background, t.radius);
        char value[NAME + 8];
        snprintf(value, sizeof value, "%s▏", e->name);
        label(e, (cui_rect){28, 64, width - 56, 40}, *e->name ? value : "Type a preset name",
              16, *e->name ? t.foreground : t.muted, false, false);
        if (button(e, "auto", (cui_rect){18, 119, 154, 32},
                   e->save_auto ? "Autoapply on" : "Autoapply off", e->save_auto, false)) e->save_auto = !e->save_auto;
        if (button(e, "save-submit", (cui_rect){width - 116, 119, 98, 32}, "Save", true, !*e->name)) save(e);
        label(e, (cui_rect){18, 165, width - 36, 36}, e->notice, 13, t.warning, false, false);
        cui_end_box(ui);
        return;
    }

    const Output *out = &e->live.topology.output[e->selected];
    int count = e->menu == MODES_MENU ? out->count : e->menu_count;
    if (e->offset >= count) e->offset = count > 0 ? count - 1 : 0;
    if (!count)
        label(e, (cui_rect){18, 60, width - 36, 40}, "Save a layout to add your first preset.",
              14, t.muted, false, false);
    for (int j = 0; j < rows && e->offset + j < count; j++) {
        int i = e->offset + j;
        float y = 57 + j * 36;
        char id[32], label_text[100];
        snprintf(id, sizeof id, "choice-%d", i);
        if (e->menu == MODES_MENU) {
            Mode mode = out->modes[i];
            snprintf(label_text, sizeof label_text, "%d × %d   ·   %.3f Hz%s",
                mode.width, mode.height, mode.millihz / 1000.0, i == out->preferred ? "   ·   Preferred" : "");
            if (button(e, id, (cui_rect){18, y, width - 36, 30}, label_text,
                       same_mode(mode, e->draft.output[e->selected].mode), false)) {
                e->draft.output[e->selected].mode = mode;
                e->menu = NONE;
                edit(e);
                break;
            }
        } else {
            char path[PATH_SIZE], error[ERROR_SIZE], auto_id[32];
            Preset preset;
            Layout layout;
            bool read = preset_path(&e->config, e->presets[i], path, error) && preset_read(path, &preset, error);
            bool match = read && resolve_layout(&e->live.topology, &preset.layout, e->config.match_ports,
                &layout, error) && normalize_layout(&e->live.topology, &layout, error);
            if (button(e, id, (cui_rect){18, y, width - 130, 30}, e->presets[i], false, !match)) {
                e->draft = layout;
                e->menu = NONE;
                edit(e);
                break;
            }
            snprintf(auto_id, sizeof auto_id, "auto-%d", i);
            if (button(e, auto_id, (cui_rect){width - 104, y, 86, 30},
                       read && preset.automatic ? "Auto on" : "Auto off", read && preset.automatic, !read)) {
                preset.automatic = !preset.automatic;
                if (!preset_write(&e->config, &preset, true, e->notice)) e->menu = NONE;
            }
        }
    }
    if (e->menu != NONE && count > rows) {
        if (button(e, "up", (cui_rect){18, height - 45, 82, 28}, "Previous", false, !e->offset))
            e->offset = e->offset > rows ? e->offset - rows : 0;
        if (button(e, "down", (cui_rect){108, height - 45, 70, 28}, "Next", false, e->offset + rows >= count))
            e->offset += rows;
    }
    cui_end_box(ui);
}

static void
draw(Editor *e)
{
    int w = e->paint.width, h = e->paint.height;
    Theme t = e->paint.theme;
    cui_context *ui = e->paint.ui;
    if (!cui_begin_frame(ui, (float)w, (float)h, .016f)) return;
    cui_begin_box(ui, cui_id_from(ui, "window"), (cui_box){.width = (float)w, .height = (float)h});
    bool pending = e->live.remaining > 0, blocked = pending || e->menu != NONE;
    char count[64], confirm[96];
    snprintf(count, sizeof count, "%d connected", e->draft.count);
    label(e, (cui_rect){58, 20, w - 220.0f, 42}, "Displays", 30, t.foreground, false, false);
    label(e, (cui_rect){59, 66, w - 180.0f, 20}, count, 12, t.muted, true, false);
    if (button(e, "identify", (cui_rect){w - 122.0f, 33, 100, 34}, "Identify", false, e->menu != NONE))
        ask(e, IDENTIFY, NULL);
    float side = w >= 800 ? 249 : 222;
    float cw = w - side - 60;
    float body = h - 233;
    if (button(e, "extend", (cui_rect){22, 100, 78, 30}, "Extend", !e->mirrored, blocked || e->draft.count < 2))
        set_mirror(e, false);
    if (button(e, "mirror", (cui_rect){106, 100, 77, 30}, "Mirror", e->mirrored, blocked || e->draft.count < 2))
        set_mirror(e, true);
    if (button(e, "presets", (cui_rect){w - 113.0f, 100, 91, 30}, "Presets", false, blocked)) {
        e->menu_count = preset_list(&e->config, e->presets);
        e->offset = 0;
        e->menu = PRESETS_MENU;
    }
    draw_canvas(e, (cui_rect){22, 140, cw, body}, blocked);
    draw_details(e, (cui_rect){38 + cw, 140, side, body}, blocked);
    float suggestions_y = 149 + body;
    if (e->draft.count >= 2) {
        label(e, (cui_rect){23, suggestions_y, 76, 26}, "Arrange", 12, t.muted, false, false);
        if (button(e, "row", (cui_rect){96, suggestions_y, 60, 27}, "Row", false, blocked))
            use_suggestion(e, "row");
        if (button(e, "column", (cui_rect){162, suggestions_y, 74, 27}, "Column", false, blocked))
            use_suggestion(e, "column");
        if (e->draft.count == 3 &&
            button(e, "y", (cui_rect){242, suggestions_y, 106, 27}, "Two above", false, blocked))
            use_suggestion(e, "y");
    }
    fill(e, (cui_rect){22, h - 46.0f, w - 44.0f, 1}, t.border, 0);
    if (pending) {
        snprintf(confirm, sizeof confirm, "Keep this layout?  Reverting in %ds", e->live.remaining);
        label(e, (cui_rect){22, h - 39.0f, w - 225.0f, 28}, confirm, 14, t.warning, false, false);
        if (button(e, "revert", (cui_rect){w - 198.0f, h - 38.0f, 81, 29}, "Revert", false, false))
            ask(e, REVERT, NULL);
        if (button(e, "keep", (cui_rect){w - 108.0f, h - 38.0f, 86, 29}, "Keep", true, false))
            ask(e, KEEP, NULL);
    } else {
        const char *message = *e->notice ? e->notice :
            (e->editing ? "Layout has unapplied changes." :
             (e->draft.count > 1 ? "Drag displays to arrange them." : e->live.message));
        label(e, (cui_rect){22, h - 39.0f, w - 320.0f, 28}, message, 13,
              *e->notice ? t.warning : t.muted, false, false);
        if (button(e, "reset", (cui_rect){w - 286.0f, h - 38.0f, 63, 29}, "Reset", false, blocked || !e->editing)) {
            e->draft = e->base;
            edit(e);
        }
        if (button(e, "save", (cui_rect){w - 215.0f, h - 38.0f, 105, 29},
                   "Save preset", false, blocked || e->editing || !e->draft.count)) {
            e->menu = SAVE_MENU;
            e->save_auto = false;
            *e->name = *e->notice = 0;
        }
        if (button(e, "apply", (cui_rect){w - 102.0f, h - 38.0f, 80, 29},
                   "Apply", true, blocked || !e->editing))
            ask(e, APPLY, &e->draft);
    }
    if (e->menu != NONE) draw_menu(e);
    cui_end_box(ui);
    painter_present(&e->paint, &e->x, e->window, e->gc, cui_end_frame(ui));
    if (e->menu == NONE) {
        cui_color fg = t.accent;
        unsigned long pixel = x11_color(DefaultVisual(e->x.display, e->x.screen), fg.r, fg.g, fg.b);
        display_icon(&e->x, e->window, e->gc, 22, 28, 26, pixel);
    }
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
handle_pointer(Editor *e, XEvent event)
{
    int x = event.type == MotionNotify ? event.xmotion.x : event.xbutton.x;
    int y = event.type == MotionNotify ? event.xmotion.y : event.xbutton.y;
    if (event.type != MotionNotify && event.xbutton.button != Button1) {
        if (event.type == ButtonPress && e->menu != NONE &&
            (event.xbutton.button == Button4 || event.xbutton.button == Button5)) {
            if (event.xbutton.button == Button4 && e->offset > 0) e->offset--;
            if (event.xbutton.button == Button5) e->offset++;
        }
        return;
    }
    cui_queue_event(e->paint.ui, (cui_event){
        .type = event.type == MotionNotify ? CUI_EVENT_POINTER_MOVE :
                (event.type == ButtonPress ? CUI_EVENT_POINTER_DOWN : CUI_EVENT_POINTER_UP),
        .x = (float)x, .y = (float)y});
    if (e->live.remaining || e->menu != NONE) { e->dragging = -1; return; }
    if (event.type == ButtonPress && inside(e->canvas, x, y)) {
        int found = -1;
        if (e->draft.count && inside(e->tiles[e->selected], x, y)) found = e->selected;
        else for (int i = e->draft.count - 1; i >= 0; i--)
            if (inside(e->tiles[i], x, y)) { found = i; break; }
        if (found >= 0) {
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
    } else if (event.type == ButtonRelease) {
        if (e->dragging >= 0 && (x != e->start_x || y != e->start_y)) drag(e, x, y);
        e->dragging = -1;
    } else if (event.type == MotionNotify && e->dragging >= 0 &&
               (abs(x - e->start_x) > 2 || abs(y - e->start_y) > 2)) drag(e, x, y);
}

static void
keyboard(Editor *e, XKeyEvent event)
{
    KeySym symbol;
    char bytes[16];
    int n = XLookupString(&event, bytes, sizeof bytes, &symbol, NULL);
    bool down = event.type == KeyPress;
    if (down && symbol == XK_Escape) {
        if (e->menu != NONE) { e->menu = NONE; *e->notice = 0; }
        else if (e->live.remaining) ask(e, REVERT, NULL);
        else e->closing = true;
        return;
    }
    if (e->menu == SAVE_MENU && down) {
        size_t length = strlen(e->name);
        if (symbol == XK_Return) { save(e); return; }
        if (symbol == XK_BackSpace) { if (length) e->name[length - 1] = 0; return; }
        if (n == 1 && length < NAME - 1 && bytes[0] >= 32 && bytes[0] <= 126 && !(event.state & ControlMask)) {
            e->name[length] = bytes[0];
            e->name[length + 1] = 0;
            return;
        }
    }
    if (down && e->menu == NONE && !e->live.remaining && e->draft.count &&
        (symbol == XK_Left || symbol == XK_Right || symbol == XK_Up || symbol == XK_Down)) {
        Placement *p = &e->draft.output[e->selected];
        if (p->enabled && !e->mirrored) {
            int by = event.state & ShiftMask ? 100 : 20;
            int direction = symbol == XK_Left || symbol == XK_Up ? -1 : 1;
            int *coordinate = symbol == XK_Left || symbol == XK_Right ? &p->x : &p->y;
            *coordinate += direction * by;
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
    Window existing = XGetSelectionOwner(d, selection);
    if (existing) {
        XEvent wake = {.xclient = {.type = ClientMessage, .window = existing,
            .message_type = e.wake, .display = d, .format = 32}};
        XSendEvent(d, existing, False, NoEventMask, &wake);
        XCloseDisplay(d);
        return 0;
    }
    if (!ask(&e, GET, NULL)) {
        fprintf(stderr, "undocked: %s\n", e.notice);
        XCloseDisplay(d);
        return 1;
    }
    int width = e.config.width, height = e.config.height;
    if (width > e.live.topology.width - 36 && e.live.topology.width >= 676) width = e.live.topology.width - 36;
    if (height > e.live.topology.height - 60 && e.live.topology.height >= 540) height = e.live.topology.height - 60;
    if (!painter_open(&e.paint, &e.x, &e.config, width, height)) {
        fprintf(stderr, "undocked: Could not initialize the CUI renderer.\n");
        XCloseDisplay(d);
        return 1;
    }
    int left = (e.live.topology.width - width) / 2, top = (e.live.topology.height - height) / 2;
    e.window = XCreateSimpleWindow(d, e.x.root, left > 0 ? left : 0, top > 0 ? top : 0,
        (unsigned)width, (unsigned)height, 0, 0, 0);
    e.gc = XCreateGC(d, e.window, 0, NULL);
    x11_class(&e.x, e.window, "undocked", "Undocked");
    XStoreName(d, e.window, "Undocked");
    Atom dialog = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(d, e.window, XInternAtom(d, "_NET_WM_WINDOW_TYPE", False),
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)&dialog, 1);
    XSizeHints hints = {.flags = PMinSize | PMaxSize, .min_width = 640, .min_height = 480,
                        .max_width = 1920, .max_height = 1440};
    XSetWMNormalHints(d, e.window, &hints);
    e.close = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, e.window, &e.close, 1);
    XSelectInput(d, e.window, ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | KeyPressMask | KeyReleaseMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask);
    XSetSelectionOwner(d, selection, e.window, CurrentTime);
    XMapRaised(d, e.window);
    bool dirty = true;
    long long next = 0;
    while (!e.closing) {
        while (XPending(d)) {
            XEvent event;
            XNextEvent(d, &event);
            dirty = true;
            switch (event.type) {
            case ClientMessage:
                if (event.xclient.message_type == e.wake) {
                    XEvent active = {.xclient = {.type = ClientMessage, .window = e.window, .display = d,
                        .message_type = XInternAtom(d, "_NET_ACTIVE_WINDOW", False), .format = 32}};
                    active.xclient.data.l[0] = 2;
                    XSendEvent(d, e.x.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &active);
                    XMapRaised(d, e.window);
                    next = 0;
                } else if ((Atom)event.xclient.data.l[0] == e.close) e.closing = true;
                break;
            case ConfigureNotify:
                painter_resize(&e.paint, &e.x, event.xconfigure.width, event.xconfigure.height);
                break;
            case ButtonPress: case ButtonRelease: case MotionNotify:
                handle_pointer(&e, event);
                break;
            case KeyPress: case KeyRelease:
                keyboard(&e, event.xkey);
                break;
            case LeaveNotify:
                cui_queue_event(e.paint.ui, (cui_event){.type = CUI_EVENT_POINTER_LEAVE});
                break;
            case FocusOut:
                e.dragging = -1;
                cui_queue_event(e.paint.ui, (cui_event){.type = CUI_EVENT_CANCEL});
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
            draw(&e);
            dirty = false;
        }
        struct pollfd events = {ConnectionNumber(d), POLLIN, 0};
        poll(&events, 1, 100);
    }
    painter_close(&e.paint);
    XFreeGC(d, e.gc);
    XCloseDisplay(d);
    return 0;
}
