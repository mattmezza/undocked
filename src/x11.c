#include "x11.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

typedef struct {
    RROutput id;
    XRROutputInfo *info;
    RRMode modes[MODES];
} Port;

typedef struct {
    Topology topology;
    XRRScreenResources *resources;
    Port ports[OUTPUTS];
    int min_width, min_height, mm_width, mm_height;
} Scan;

typedef struct {
    RRCrtc crtc;
    RRMode mode;
    int x, y;
    Rotation rotation;
    RROutput outputs[OUTPUTS];
    int count;
} DisplayGroup;

static int
ignore_error(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    return 0;
}

bool
x11_open(X11 *x, char *error)
{
    int major = 0, minor = 0, error_base;
    x->display = XOpenDisplay(NULL);
    if (!x->display) {
        snprintf(error, ERROR_SIZE, "Cannot connect to the X11 display.");
        return false;
    }
    if (!XRRQueryExtension(x->display, &x->event, &error_base) ||
        !XRRQueryVersion(x->display, &major, &minor) || (major == 1 && minor < 3)) {
        snprintf(error, ERROR_SIZE, "X11 RandR 1.3 or newer is required.");
        XCloseDisplay(x->display);
        return false;
    }
    XSetErrorHandler(ignore_error);
    x->screen = DefaultScreen(x->display);
    x->root = RootWindow(x->display, x->screen);
    XRRSelectInput(x->display, x->root,
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                   RROutputChangeNotifyMask | RROutputPropertyNotifyMask |
                   (major > 1 || minor >= 4 ? RRProviderChangeNotifyMask | RRResourceChangeNotifyMask : 0));
    return true;
}

bool
x11_change(X11 *x, XEvent *event)
{
    if (event->type == x->event + RRScreenChangeNotify) {
        XRRUpdateConfiguration(event);
        return true;
    }
    if (event->type != x->event + RRNotify) return false;
    XRRNotifyEvent *notify = (XRRNotifyEvent *)event;
    if (notify->subtype == RRNotify_OutputProperty) {
        XRROutputPropertyNotifyEvent *property = (XRROutputPropertyNotifyEvent *)event;
        return property->property == XInternAtom(x->display, "EDID", True) ||
               property->property == XInternAtom(x->display, "non-desktop", True);
    }
    return true;
}

void
x11_class(X11 *x, Window window, const char *instance, const char *class)
{
    XClassHint hint = {(char *)instance, (char *)class};
    XSetClassHint(x->display, window, &hint);
}

static unsigned long
color_part(unsigned number, unsigned long mask)
{
    if (!mask) return 0;
    unsigned shift = 0;
    while (!(mask & 1)) { shift++; mask >>= 1; }
    return ((number * mask + 127) / 255) << shift;
}

unsigned long
x11_color(Visual *visual, unsigned r, unsigned g, unsigned b)
{
    return color_part(r, visual->red_mask) | color_part(g, visual->green_mask) |
           color_part(b, visual->blue_mask);
}

static Mode
read_mode(const XRRModeInfo *info)
{
    double vtotal = info->vTotal;
    if (info->modeFlags & RR_DoubleScan) vtotal *= 2;
    if (info->modeFlags & RR_Interlace) vtotal /= 2;
    double hz = info->hTotal && vtotal ? 1000.0 * info->dotClock / (info->hTotal * vtotal) : 0;
    return (Mode){(int)info->width, (int)info->height, (int)(hz + 0.5)};
}

static const XRRModeInfo *
find_mode(const XRRScreenResources *resources, RRMode id)
{
    for (int i = 0; i < resources->nmode; i++)
        if (resources->modes[i].id == id) return &resources->modes[i];
    return NULL;
}

static void
read_edid(X11 *x, RROutput id, Output *output)
{
    Atom edid = XInternAtom(x->display, "EDID", True), type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    if (!edid || XRRGetOutputProperty(x->display, id, edid, 0, 32, False, False,
        AnyPropertyType, &type, &format, &count, &after, &data) != Success)
        return;
    const unsigned char magic[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (format == 8 && count >= 128 && data && !memcmp(data, magic, sizeof magic)) {
        unsigned sum = 0;
        uint64_t hash = UINT64_C(14695981039346656037);
        for (int i = 0; i < 128; i++) {
            sum += data[i];
            hash = (hash ^ data[i]) * UINT64_C(1099511628211);
        }
        if (!(sum % 256)) {
            output->current.edid = hash ? hash : 1;
            for (int offset = 54; offset <= 108; offset += 18) {
                const unsigned char *p = data + offset;
                if (p[0] || p[1] || p[2] || p[3] != 0xfc) continue;
                size_t n = 0;
                for (int j = 5; j < 18 && p[j] && p[j] != '\n'; j++)
                    output->model[n++] = isprint(p[j]) ? (char)p[j] : '?';
                while (n && output->model[n - 1] == ' ') n--;
                output->model[n] = 0;
                break;
            }
        }
    }
    if (data) XFree(data);
}

static bool
non_desktop(X11 *x, RROutput id)
{
    Atom name = XInternAtom(x->display, "non-desktop", True), type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    bool other = false;
    if (name && XRRGetOutputProperty(x->display, id, name, 0, 1, False, False,
        AnyPropertyType, &type, &format, &count, &after, &data) == Success && count == 1 && data)
        other = format == 32 && *(const unsigned long *)data != 0;
    if (data) XFree(data);
    return other;
}

static void
scan_free(Scan *scan)
{
    for (int i = 0; i < scan->topology.count; i++) XRRFreeOutputInfo(scan->ports[i].info);
    if (scan->resources) XRRFreeScreenResources(scan->resources);
}

static bool
scan_read(X11 *x, Scan *scan, bool refresh, char *error)
{
    *scan = (Scan){0};
    Display *d = x->display;
    Topology *t = &scan->topology;
    scan->resources = refresh ? XRRGetScreenResources(d, x->root) :
                                XRRGetScreenResourcesCurrent(d, x->root);
    XWindowAttributes wa;
    if (!scan->resources || !XGetWindowAttributes(d, x->root, &wa) ||
        !XRRGetScreenSizeRange(d, x->root, &scan->min_width, &scan->min_height,
                               &t->max_width, &t->max_height)) {
        snprintf(error, ERROR_SIZE, "Could not read X11 display resources.");
        scan_free(scan);
        return false;
    }
    t->width = wa.width;
    t->height = wa.height;
    scan->mm_width = DisplayWidthMM(d, x->screen);
    scan->mm_height = DisplayHeightMM(d, x->screen);
    RROutput primary = XRRGetOutputPrimary(d, x->root);
    for (int i = 0; i < scan->resources->noutput; i++) {
        RROutput id = scan->resources->outputs[i];
        XRROutputInfo *info = XRRGetOutputInfo(d, scan->resources, id);
        if (!info) continue;
        if (info->connection == RR_Disconnected || (!info->crtc && non_desktop(x, id)) ||
            (info->connection == RR_UnknownConnection && !info->crtc && !info->nmode)) {
            XRRFreeOutputInfo(info);
            continue;
        }
        if (t->count == OUTPUTS || info->nameLen >= PORT || info->nmode > MODES) {
            snprintf(error, ERROR_SIZE, "The X server exposed more display data than Undocked can hold.");
            XRRFreeOutputInfo(info);
            scan_free(scan);
            return false;
        }
        int n = t->count++;
        Output *out = &t->output[n];
        Port *port = &scan->ports[n];
        port->id = id;
        port->info = info;
        memcpy(out->current.port, info->name, (size_t)info->nameLen);
        for (char *c = out->current.port; *c; c++)
            if (isspace((unsigned char)*c)) *c = '_';
        out->internal = !strncmp(info->name, "eDP", 3) || !strncmp(info->name, "LVDS", 4) ||
                        !strncmp(info->name, "DSI", 3);
        out->mm_width = (int)info->mm_width;
        out->mm_height = (int)info->mm_height;
        out->rotations = RR_Rotate_0;
        for (int j = 0; j < info->ncrtc; j++) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(d, scan->resources, info->crtcs[j]);
            if (!crtc) continue;
            out->rotations |= crtc->rotations & 15;
            XRRFreeCrtcInfo(crtc);
        }
        for (int j = 0; j < info->nmode; j++) {
            const XRRModeInfo *m = find_mode(scan->resources, info->modes[j]);
            if (!m) continue;
            Mode mode = read_mode(m);
            port->modes[out->count] = m->id;
            out->modes[out->count++] = mode;
        }
        if (out->count) out->current.mode = out->modes[out->preferred];
        if (info->crtc) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(d, scan->resources, info->crtc);
            if (crtc) {
                const XRRModeInfo *m = find_mode(scan->resources, crtc->mode);
                if (m) {
                    out->current.enabled = true;
                    out->current.primary = primary == id;
                    out->current.mode = read_mode(m);
                    out->current.x = crtc->x;
                    out->current.y = crtc->y;
                    for (int k = 0; k < 4; k++)
                        if (crtc->rotation & (1 << k)) out->current.rotation = k * 90;
                }
                XRRFreeCrtcInfo(crtc);
            }
        }
        read_edid(x, id, out);
        if (!*out->model) strcpy(out->model, out->internal ? "Built-in display" : "External display");
    }
    /* Keep Identify numbers and layout comparisons stable across resource orders. */
    for (int i = 1; i < t->count; i++) {
        for (int j = i; j > 0 && strcmp(t->output[j - 1].current.port, t->output[j].current.port) > 0; j--) {
            Output output = t->output[j - 1];
            Port port = scan->ports[j - 1];
            t->output[j - 1] = t->output[j];
            scan->ports[j - 1] = scan->ports[j];
            t->output[j] = output;
            scan->ports[j] = port;
        }
    }
    return true;
}

static bool
snapshot(Backend *backend, Topology *topology, char *error)
{
    Scan scan;
    if (!scan_read(backend->state, &scan, false, error)) return false;
    *topology = scan.topology;
    scan_free(&scan);
    return true;
}

static bool
supports(const Port *port, RRCrtc crtc)
{
    for (int i = 0; i < port->info->ncrtc; i++)
        if (port->info->crtcs[i] == crtc) return true;
    return false;
}

static bool
clones(const Port *port, RROutput output)
{
    for (int i = 0; i < port->info->nclone; i++)
        if (port->info->clones[i] == output) return true;
    return false;
}

static bool
assign(const Scan *scan, const Layout *layout, const Rotation *rotations, int index,
       DisplayGroup *groups, unsigned *attempts)
{
    while (index < layout->count && !layout->output[index].enabled) index++;
    if (index == layout->count) return true;
    if (++*attempts > 100000) return false;
    const Port *port = &scan->ports[index];
    const Output *output = &scan->topology.output[index];
    const Placement *p = &layout->output[index];
    Rotation rotation = (Rotation)(1 << (p->rotation / 90));
    for (int pass = 0; pass < 2; pass++) {
        for (int n = 0; n < scan->resources->ncrtc; n++) {
            DisplayGroup *group = &groups[n];
            if ((pass == 0) != (group->crtc == port->info->crtc)) continue;
            if (!(rotations[n] & rotation) || !supports(port, group->crtc)) continue;
            if (group->count && (group->x != p->x || group->y != p->y || group->rotation != rotation))
                continue;
            bool allowed = true;
            for (int j = 0; j < group->count; j++)
                if (!clones(port, group->outputs[j])) allowed = false;
            if (!allowed) continue;
            for (int m = 0; m < output->count; m++) {
                if (!same_mode(output->modes[m], p->mode) ||
                    (group->count && group->mode != port->modes[m])) continue;
                group->mode = port->modes[m];
                group->x = p->x;
                group->y = p->y;
                group->rotation = rotation;
                group->outputs[group->count++] = port->id;
                if (assign(scan, layout, rotations, index + 1, groups, attempts)) return true;
                group->count--;
            }
        }
    }
    return false;
}

static int x_error;

static int
record_error(Display *display, XErrorEvent *event)
{
    (void)display;
    x_error = event->error_code;
    return 0;
}

static bool
set_size(X11 *x, const Scan *scan, int width, int height)
{
    x_error = 0;
    int mmw = scan->topology.width > 0 ? (int)((long)scan->mm_width * width / scan->topology.width) : 0;
    int mmh = scan->topology.height > 0 ? (int)((long)scan->mm_height * height / scan->topology.height) : 0;
    XRRSetScreenSize(x->display, x->root, width, height, mmw > 0 ? mmw : width * 25 / 96,
                     mmh > 0 ? mmh : height * 25 / 96);
    XSync(x->display, False);
    return !x_error;
}

static bool
set_crtc(X11 *x, DisplayGroup group)
{
    x_error = 0;
    XRRScreenResources *fresh = XRRGetScreenResourcesCurrent(x->display, x->root);
    if (!fresh) return false;
    Status status = XRRSetCrtcConfig(x->display, fresh, group.crtc, CurrentTime,
                                    group.count ? group.x : 0, group.count ? group.y : 0,
                                    group.count ? group.mode : None,
                                    group.count ? group.rotation : RR_Rotate_0,
                                    group.outputs, group.count);
    XRRFreeScreenResources(fresh);
    return status == RRSetConfigSuccess && !x_error;
}

static bool
group_equal(const DisplayGroup *group, const XRRCrtcInfo *current)
{
    if (!current || group->count != current->noutput) return false;
    if (!group->count) return !current->mode;
    if (group->mode != current->mode || group->rotation != current->rotation ||
        group->x != current->x || group->y != current->y) return false;
    for (int i = 0; i < group->count; i++) {
        bool present = false;
        for (int j = 0; j < current->noutput; j++)
            if (group->outputs[i] == current->outputs[j]) present = true;
        if (!present) return false;
    }
    return true;
}

static bool
apply(Backend *backend, const Layout *requested, bool test, char *error)
{
    X11 *x = backend->state;
    Display *d = x->display;
    Scan scan;
    Layout layout;
    bool ok = false;
    if (!test) XGrabServer(d);
    int (*old_handler)(Display *, XErrorEvent *) = XSetErrorHandler(record_error);
    if (!scan_read(x, &scan, false, error)) goto done;
    Topology *t = &scan.topology;
    if (!resolve_layout(t, requested, true, &layout, error) || !normalize_layout(t, &layout, error))
        goto free_scan;
    int n = scan.resources->ncrtc;
    if (n < 1 || n > 256) {
        snprintf(error, ERROR_SIZE, "No supported X11 display-controller configuration.");
        goto free_scan;
    }
    DisplayGroup groups[256] = {0};
    XRRCrtcInfo *current[256] = {0};
    Rotation rotations[256] = {0};
    bool changed[256] = {0};
    for (int i = 0; i < n; i++) {
        groups[i].crtc = scan.resources->crtcs[i];
        current[i] = XRRGetCrtcInfo(d, scan.resources, groups[i].crtc);
        if (current[i]) rotations[i] = current[i]->rotations;
    }
    unsigned attempts = 0;
    if (!assign(&scan, &layout, rotations, 0, groups, &attempts)) {
        snprintf(error, ERROR_SIZE, "The X server has no compatible controller assignment for this layout.");
        goto free_crtcs;
    }
    if (test) { ok = true; goto free_crtcs; }
    int width = scan.min_width, height = scan.min_height;
    RROutput primary = None;
    for (int i = 0; i < layout.count; i++) {
        Placement p = layout.output[i];
        if (!p.enabled) continue;
        int w, h;
        dimensions(p, &w, &h);
        if (p.x + w > width) width = p.x + w;
        if (p.y + h > height) height = p.y + h;
        if (p.primary) primary = scan.ports[i].id;
    }
    int grow_w = width > t->width ? width : t->width;
    int grow_h = height > t->height ? height : t->height;
    if ((grow_w != t->width || grow_h != t->height) && !set_size(x, &scan, grow_w, grow_h))
        goto rejected;
    for (int i = 0; i < n; i++) {
        changed[i] = !group_equal(&groups[i], current[i]);
        if (!changed[i] || !current[i] || !current[i]->mode) continue;
        bool moving_output = false;
        for (int j = 0; j < current[i]->noutput; j++)
            for (int k = 0; k < n; k++)
                if (k != i)
                    for (int l = 0; l < groups[k].count; l++)
                        if (current[i]->outputs[j] == groups[k].outputs[l]) moving_output = true;
        if ((moving_output || !groups[i].count) &&
            !set_crtc(x, (DisplayGroup){.crtc = groups[i].crtc})) goto rejected;
    }
    for (int i = 0; i < n; i++)
        if (changed[i] && groups[i].count && !set_crtc(x, groups[i])) goto rejected;
    if ((width != grow_w || height != grow_h) && !set_size(x, &scan, width, height)) goto rejected;
    x_error = 0;
    XRRSetOutputPrimary(d, x->root, primary);
    XSync(d, False);
    if (x_error) goto rejected;
    ok = true;
    goto free_crtcs;

rejected:
    snprintf(error, ERROR_SIZE, "The X server rejected the display change.");
free_crtcs:
    for (int i = 0; i < n; i++) if (current[i]) XRRFreeCrtcInfo(current[i]);
free_scan:
    scan_free(&scan);
done:
    if (!test) XUngrabServer(d);
    XSync(d, False);
    XSetErrorHandler(old_handler);
    return ok;
}

Backend
x11_backend(X11 *x)
{
    return (Backend){.state = x, .snapshot = snapshot, .apply = apply};
}
