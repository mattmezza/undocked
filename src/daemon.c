#include "painter.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    X11 x;
    Backend backend;
    Config config;
    Topology topology, reported;
    Layout previous, extended;
    uint64_t checked;
    char reason[NAME], profile[NAME], status[ERROR_SIZE];
    long long settle, deadline, lid_check, next_tray, badges_until, last_resume;
    bool lid, checked_lid, tray, tray_dirty, force_hook, previous_valid;
    Window icon, tray_owner, badges[OUTPUTS];
    GC icon_gc, badge_gc;
    Colormap icon_colormap;
    Atom tray_visual, tray_colors, tray_selection, embed;
    int icon_width, icon_height, tray_override;
    Painter badge_painter;
    bool have_badge_painter;
} Session;

static volatile sig_atomic_t stopping;

static void
stop(int signal)
{
    (void)signal;
    stopping = 1;
}

static bool
socket_address(struct sockaddr_un *address)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR"), *display = getenv("DISPLAY");
    if (!display || !*display) return false;
    char directory[sizeof address->sun_path];
    if (runtime && *runtime == '/') {
        if (snprintf(directory, sizeof directory, "%s", runtime) >= (int)sizeof directory)
            return false;
    } else {
        snprintf(directory, sizeof directory, "/tmp/undocked-%lu", (unsigned long)getuid());
        if (mkdir(directory, 0700) && errno != EEXIST) return false;
    }
    struct stat info;
    if (lstat(directory, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0077)) return false;
    /* Map common local aliases (:0, :0.0, unix:0) to the same X screen. */
    if (!strncmp(display, "unix:", 5)) display += 4;
    char display_name[128];
    if (snprintf(display_name, sizeof display_name, "%s", display) >= (int)sizeof display_name)
        return false;
    char *screen = strrchr(display_name, '.');
    char *colon = strrchr(display_name, ':');
    if (screen && colon && screen > colon && !strcmp(screen, ".0")) *screen = 0;
    uint64_t key = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (unsigned char *)display_name; *p; p++)
        key = (key ^ *p) * UINT64_C(1099511628211);
    *address = (struct sockaddr_un){.sun_family = AF_UNIX};
    return snprintf(address->sun_path, sizeof address->sun_path, "%s/undocked-%016llx",
                    directory, (unsigned long long)key) < (int)sizeof address->sun_path;
}

static int
connection(const struct sockaddr_un *address)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (const struct sockaddr *)address, sizeof *address)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
start_session(void)
{
    pid_t pid = fork();
    if (pid != 0) return;
    if (setsid() < 0) _exit(1);
    int input = open("/dev/null", O_RDWR);
    if (input >= 0) {
        for (int fd = 0; fd <= 2; fd++) dup2(input, fd);
        if (input > 2) close(input);
    }
    const char *exec = executable();
    execl(exec, "undocked", "daemon", "--on-demand", (char *)NULL);
    _exit(127);
}

int
daemon_restart(bool only_running)
{
    struct sockaddr_un address;
    if (!socket_address(&address)) {
        if (only_running) return 0;
        fprintf(stderr, "undocked: A valid DISPLAY and private runtime directory are required.\n");
        return 1;
    }
    int fd = connection(&address);
    if (fd < 0 && only_running) return 0;
    if (fd >= 0) {
        struct ucred peer;
        socklen_t size = sizeof peer;
        bool owner = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
                     peer.uid == getuid() && peer.pid > 1;
        close(fd);
        char filename[64], target[PATH_SIZE];
        if (!owner) goto failed;
        snprintf(filename, sizeof filename, "/proc/%ld/exe", (long)peer.pid);
        ssize_t length = readlink(filename, target, sizeof target - 1);
        if (length < 1) goto failed;
        target[length] = 0;
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;
        if (strcmp(base, "undocked") && strcmp(base, "undocked (deleted)") &&
            strncmp(base, "undocked-linux-", 15)) goto failed;
        if (kill(peer.pid, SIGTERM) < 0 && errno != ESRCH) goto failed;
        long long limit = now_ms() + 6000;
        while ((fd = connection(&address)) >= 0) {
            close(fd);
            if (now_ms() >= limit) goto failed;
            struct timespec wait = {.tv_nsec = 25000000L};
            nanosleep(&wait, NULL);
        }
    }
    Reply reply;
    if (rpc(&(Request){.version = REQUEST_VERSION, .command = GET}, &reply, true)) return 0;
    fprintf(stderr, "undocked: %s\n", reply.message);
    return 1;

failed:
    fprintf(stderr, "undocked: Could not restart the running session daemon.\n");
    return 1;
}

bool
rpc(const Request *request, Reply *reply, bool start_daemon)
{
    *reply = (Reply){0};
    struct sockaddr_un address;
    if (!socket_address(&address)) {
        snprintf(reply->message, ERROR_SIZE, "A valid DISPLAY and private runtime directory are required.");
        return false;
    }
    int fd = connection(&address);
    if (fd < 0 && start_daemon) {
        start_session();
        for (int i = 0; i < 60 && fd < 0; i++) {
            struct timespec wait = {.tv_nsec = 25000000L};
            nanosleep(&wait, NULL);
            fd = connection(&address);
        }
    }
    if (fd >= 0) {
        struct timeval timeout = {.tv_sec = 6};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        ssize_t count = send(fd, request, sizeof *request, MSG_NOSIGNAL) == (ssize_t)sizeof *request ?
                        recv(fd, reply, sizeof *reply, MSG_TRUNC) : -1;
        close(fd);
        if ((count == (ssize_t)sizeof *reply || count == (ssize_t)offsetof(Reply, extended)) &&
            reply->version == REQUEST_VERSION) return reply->ok;
    }
    snprintf(reply->message, ERROR_SIZE, "Cannot reach the Undocked session. Run undocked daemon to diagnose.");
    return false;
}

static void
spawn(const char *command, const char *reason, const char *profile)
{
    pid_t child = fork();
    if (child != 0) return;
    if (command) {
        setenv("UNDOCKED_REASON", reason, 1);
        setenv("UNDOCKED_PRESET", profile, 1);
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0) { dup2(input, STDIN_FILENO); close(input); }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    } else {
        const char *exec = executable();
        if (profile) execl(exec, "undocked", profile, (char *)NULL);
        else execl(exec, "undocked", (char *)NULL);
    }
    _exit(127);
}

static void
close_badges(Session *s)
{
    for (int i = 0; i < OUTPUTS; i++) {
        if (s->badges[i]) XDestroyWindow(s->x.display, s->badges[i]);
        s->badges[i] = None;
    }
    s->badges_until = 0;
}

static void
identify(Session *s)
{
    close_badges(s);
    X11 *x = &s->x;
    if (!s->have_badge_painter) {
        if (!painter_open(&s->badge_painter, x, &s->config, 168, 116)) return;
        s->badge_gc = XCreateGC(x->display, x->root, 0, NULL);
        s->have_badge_painter = true;
    }
    Display *d = x->display;
    for (int i = 0; i < s->topology.count; i++) {
        Placement p = s->topology.output[i].current;
        if (!p.enabled) continue;
        bool first = true;
        char label[72];
        int used = snprintf(label, sizeof label, "%d", i + 1);
        int w, h;
        dimensions(p, &w, &h);
        for (int j = 0; j < s->topology.count; j++) {
            Placement q = s->topology.output[j].current;
            int qw, qh;
            dimensions(q, &qw, &qh);
            if (i == j || !q.enabled || p.x != q.x || p.y != q.y || w != qw || h != qh) continue;
            if (j < i) { first = false; break; }
            if (used < 32) used += snprintf(label + used, sizeof label - (size_t)used, " / %d", j + 1);
        }
        if (!first) continue;
        int bw = used <= 2 ? 168 : used * 40 + 28, bh = 116;
        if (bw > w) bw = w;
        if (bh > h) bh = h;
        if (!painter_resize(&s->badge_painter, x, bw, bh)) continue;
        XSetWindowAttributes attrs = {.override_redirect = True, .save_under = True};
        s->badges[i] = XCreateWindow(d, x->root, p.x + (w - bw) / 2, p.y + (h - bh) / 2,
            (unsigned)bw, (unsigned)bh, 0, CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWSaveUnder, &attrs);
        x11_class(x, s->badges[i], "undocked-identify", "Undocked");
        XStoreName(d, s->badges[i], "Undocked Identify");
        /* A window background pixmap keeps the badge painted through Expose. */
        Pixmap pixels = XCreatePixmap(d, s->badges[i], (unsigned)bw, (unsigned)bh,
                                     (unsigned)DefaultDepth(d, x->screen));
        display_badge(&s->badge_painter, x, pixels, s->badge_gc, label);
        XSetWindowBackgroundPixmap(d, s->badges[i], pixels);
        XFreePixmap(d, pixels);
        XMapRaised(d, s->badges[i]);
    }
    s->badges_until = now_ms() + s->config.identify * 1000LL;
    XFlush(d);
}

static cui_color
tray_foreground(Session *s, Theme theme)
{
    Atom type;
    int format;
    unsigned long count, rest;
    unsigned char *data = NULL;
    cui_color color = theme.accent;
    if (XGetWindowProperty(s->x.display, s->tray_owner, s->tray_colors, 0, 12, False,
        XA_CARDINAL, &type, &format, &count, &rest, &data) == Success &&
        format == 32 && count == 12 && data) {
        unsigned long *channels = (unsigned long *)data;
        color = (cui_color){channels[0] / 257, channels[1] / 257, channels[2] / 257, 255};
    }
    if (data) XFree(data);
    return color;
}

static unsigned
blend(unsigned ink, unsigned background, unsigned opacity)
{
    return (ink * opacity + background * (255 - opacity) + 127) / 255;
}

static void
paint_tray(Session *s)
{
    s->tray_dirty = false;
    Display *d = s->x.display;
    XWindowAttributes attr;
    if (!s->icon || !XGetWindowAttributes(d, s->icon, &attr) ||
        attr.width < 1 || attr.height < 1 || attr.width > 256 || attr.height > 256) return;
    Theme theme;
    theme_read(&theme, &s->x, &s->config);
    cui_color ink = tray_foreground(s, theme);
    XRenderPictFormat *format = XRenderFindVisualFormat(d, attr.visual);
    unsigned alpha = format && format->type == PictTypeDirect ? format->direct.alphaMask : 0;
    cui_color background = alpha ? (cui_color){0} : theme.panel;
    XImage *image = XCreateImage(d, attr.visual, (unsigned)attr.depth, ZPixmap, 0, NULL,
                                 (unsigned)attr.width, (unsigned)attr.height, 32, 0);
    if (!image) return;
    image->data = calloc((size_t)image->bytes_per_line, (size_t)attr.height);
    if (!image->data) { XDestroyImage(image); return; }
    int size = attr.width < attr.height ? attr.width : attr.height;
    int left = (attr.width - size) / 2, top = (attr.height - size) / 2;
    for (int y = 0; y < attr.height; y++) {
        for (int x = 0; x < attr.width; x++) {
            unsigned a = x >= left && x < left + size && y >= top && y < top + size ?
                icon_coverage(ICON_DISPLAYS, x - left, y - top, size) : 0;
            unsigned long pixel = x11_color(attr.visual, blend(ink.r, background.r, a),
                blend(ink.g, background.g, a), blend(ink.b, background.b, a));
            if (alpha) pixel |= (unsigned long)((a * alpha + 127) / 255) << format->direct.alpha;
            XPutPixel(image, x, y, pixel);
        }
    }
    Pixmap pixmap = XCreatePixmap(d, s->icon, (unsigned)attr.width, (unsigned)attr.height, (unsigned)attr.depth);
    XPutImage(d, pixmap, s->icon_gc, image, 0, 0, 0, 0, (unsigned)attr.width, (unsigned)attr.height);
    XDestroyImage(image);
    XSetWindowBackgroundPixmap(d, s->icon, pixmap);
    XClearArea(d, s->icon, 0, 0, 0, 0, False);
    XFreePixmap(d, pixmap);
    s->icon_width = attr.width;
    s->icon_height = attr.height;
}

static void
drop_icon(Session *s)
{
    Display *d = s->x.display;
    if (s->icon) XDestroyWindow(d, s->icon);
    if (s->icon_gc) XFreeGC(d, s->icon_gc);
    if (s->icon_colormap) XFreeColormap(d, s->icon_colormap);
    s->icon = None;
    s->icon_gc = NULL;
    s->icon_colormap = None;
    s->icon_width = s->icon_height = 0;
}

static void
set_tray(Session *s)
{
    s->tray = s->tray_override < 0 ? s->config.tray : s->tray_override != 0;
    s->next_tray = 0;
    if (!s->tray) {
        s->tray_dirty = false;
        drop_icon(s);
    }
}

static void
dock_icon(Session *s)
{
    Display *d = s->x.display;
    Window owner = XGetSelectionOwner(d, s->tray_selection);
    if (owner == s->tray_owner && s->icon) return;
    drop_icon(s);
    s->tray_owner = owner;
    if (!owner) return;

    Visual *visual = DefaultVisual(d, s->x.screen);
    int depth = DefaultDepth(d, s->x.screen);
    int format, count;
    unsigned long length, rest;
    Atom type;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, owner, s->tray_visual, 0, 1, False, XA_VISUALID,
        &type, &format, &length, &rest, &data) == Success && format == 32 && length == 1 && data) {
        XVisualInfo wanted = {.visualid = *(unsigned long *)data, .screen = s->x.screen};
        XVisualInfo *info = XGetVisualInfo(d, VisualIDMask | VisualScreenMask, &wanted, &count);
        if (info && count && info->class == TrueColor) { visual = info->visual; depth = info->depth; }
        if (info) XFree(info);
    }
    if (data) XFree(data);
    s->icon_colormap = XCreateColormap(d, s->x.root, visual, AllocNone);
    XSetWindowAttributes attrs = {.colormap = s->icon_colormap, .background_pixel = 0, .border_pixel = 0,
        .event_mask = ButtonPressMask | StructureNotifyMask};
    XSelectInput(d, owner, StructureNotifyMask | PropertyChangeMask);
    s->icon = XCreateWindow(d, s->x.root, 0, 0, 22, 22, 0, depth, InputOutput, visual,
                            CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    x11_class(&s->x, s->icon, "undocked-tray", "Undocked");
    XStoreName(d, s->icon, "Undocked Displays");
    XSizeHints hints = {.flags = PMinSize | PBaseSize, .min_width = 12, .min_height = 12,
                        .base_width = 22, .base_height = 22};
    XSetWMNormalHints(d, s->icon, &hints);
    Atom info_atom = XInternAtom(d, "_XEMBED_INFO", False);
    unsigned long info[] = {0, 1};
    XChangeProperty(d, s->icon, info_atom, info_atom, 32, PropModeReplace, (unsigned char *)info, 2);
    s->icon_gc = XCreateGC(d, s->icon, 0, NULL);
    XEvent message = {.xclient = {.type = ClientMessage, .display = d, .window = owner,
        .message_type = XInternAtom(d, "_NET_SYSTEM_TRAY_OPCODE", False), .format = 32}};
    message.xclient.data.l[2] = (long)s->icon;
    XSendEvent(d, owner, False, NoEventMask, &message);
    XFlush(d);
}

static void
tray_event(Session *s, const XEvent *event)
{
    if (event->type == ClientMessage && event->xclient.window == s->icon &&
        event->xclient.message_type == s->embed && event->xclient.data.l[1] == 0)
        s->tray_dirty = true;
    else if (event->type == MapNotify && event->xmap.window == s->icon) s->tray_dirty = true;
    else if (event->type == ConfigureNotify && event->xconfigure.window == s->icon &&
             (event->xconfigure.width != s->icon_width || event->xconfigure.height != s->icon_height))
        s->tray_dirty = true;
    else if (event->type == PropertyNotify &&
             ((event->xproperty.window == s->tray_owner && event->xproperty.atom == s->tray_colors) ||
              (event->xproperty.window == s->x.root && event->xproperty.atom == XA_RESOURCE_MANAGER)))
        s->tray_dirty = true;
    else if (event->type == DestroyNotify && event->xdestroywindow.window == s->icon) {
        s->icon = None;
        s->next_tray = 0;
    } else if (event->type == ReparentNotify && event->xreparent.window == s->icon &&
               event->xreparent.parent == s->x.root) {
        s->tray_owner = None;
        s->next_tray = 0;
    } else if (event->type == ButtonPress && event->xbutton.window == s->icon) {
        if (event->xbutton.button == Button1) spawn(NULL, NULL, NULL);
        if (event->xbutton.button == Button2) identify(s);
        if (event->xbutton.button == Button3) spawn(NULL, NULL, "launch");
    }
}

static bool
scan(Session *s, char *error)
{
    return s->backend.snapshot(&s->backend, &s->topology, error);
}

static bool
has_display(const Topology *t)
{
    for (int i = 0; i < t->count; i++)
        if (t->output[i].current.enabled) return true;
    return false;
}

static void
settle_after(Session *s, const char *reason, const char *profile)
{
    if (reason) snprintf(s->reason, sizeof s->reason, "%s", reason);
    if (profile) snprintf(s->profile, sizeof s->profile, "%s", profile);
    s->settle = now_ms() + s->config.settle;
}

static bool
recover(Session *s, char *error)
{
    if (!scan(s, error)) return false;
    Layout safe = recovery_layout(&s->topology, s->lid);
    if (!s->backend.apply(&s->backend, &safe, false, error)) return false;
    settle_after(s, "recover", "");
    return true;
}

static bool
restore(Session *s, char *error)
{
    s->deadline = 0;
    Layout old;
    bool ok = scan(s, error) && resolve_layout(&s->topology, &s->previous, true, &old, error) &&
              s->backend.apply(&s->backend, &old, false, error);
    s->previous_valid = false;
    if (!ok) ok = recover(s, error);
    if (ok) {
        s->force_hook = true;
        settle_after(s, "revert", "");
    }
    return ok;
}

static bool
apply_layout(Session *s, Layout layout, bool immediate, const char *reason, const char *profile, char *error)
{
    if (s->deadline) {
        snprintf(error, ERROR_SIZE, "Keep or revert the pending display change first.");
        return false;
    }
    if (!normalize_layout(&s->topology, &layout, error)) return false;
    Layout current = current_layout(&s->topology);
    if (layout_equal(&current, &layout)) return true;
    if (!s->backend.apply(&s->backend, &layout, true, error)) return false;
    s->previous = current;
    s->previous_valid = true;
    if (!s->backend.apply(&s->backend, &layout, false, error)) {
        char recovery[ERROR_SIZE];
        if (!restore(s, recovery)) fprintf(stderr, "undocked: recovery failed: %s\n", recovery);
        return false;
    }
    if (layout_mirrored(&layout) && !layout_mirrored(&current)) s->extended = current;
    if (!strcmp(reason, "apply")) {
        s->checked = topology_key(&s->topology);
        s->checked_lid = s->lid;
    }
    if (immediate) s->previous_valid = false;
    else s->deadline = now_ms() + s->config.confirm * 1000LL;
    settle_after(s, reason, profile);
    return true;
}

static void
autoapply(Session *s, const char *cause)
{
    if (!s->config.automatic) {
        snprintf(s->status, sizeof s->status, "Automatic switching is disabled.");
        return;
    }
    if (!s->topology.count) {
        snprintf(s->status, sizeof s->status, "No connected displays.");
        return;
    }
    char names[PRESETS][NAME], path[PATH_SIZE], error[ERROR_SIZE];
    Layout winner = {0};
    const char *name = NULL;
    int count = preset_list(&s->config, names);
    for (int i = 0; i < count; i++) {
        Preset preset;
        Layout layout;
        if (!preset_path(&s->config, names[i], path, error) || !preset_read(path, &preset, error) ||
            !preset.automatic || !resolve_layout(&s->topology, &preset.layout, s->config.match_ports, &layout, error) ||
            !normalize_layout(&s->topology, &layout, error)) continue;
        bool enables_lid = false;
        for (int j = 0; j < layout.count; j++)
            if (layout.output[j].enabled && s->topology.output[j].internal) enables_lid = true;
        if (s->lid && enables_lid) continue;
        if (name) {
            snprintf(s->status, sizeof s->status, "Multiple automatic presets match this display setup.");
            return;
        }
        winner = layout;
        name = names[i];
    }
    if (name) {
        if (apply_layout(s, winner, true, cause, name, error))
            snprintf(s->status, sizeof s->status, "Automatic preset: %.63s", name);
        else snprintf(s->status, sizeof s->status, "%s", error);
    } else snprintf(s->status, sizeof s->status, "No automatic preset matches this display setup.");
}

static void
reconcile(Session *s)
{
    char error[ERROR_SIZE];
    if (!scan(s, error)) return;
    uint64_t key = topology_key(&s->topology);
    bool changed = s->checked != key || s->checked_lid != s->lid;
    if (s->deadline) {
        Layout unused;
        if (!resolve_layout(&s->topology, &s->previous, true, &unused, error) &&
            !restore(s, error)) fprintf(stderr, "undocked: recovery failed: %s\n", error);
        else if (s->deadline) {
            s->settle = 0;
            return;
        }
        if (!scan(s, error)) return;
        key = topology_key(&s->topology);
    }
    if (changed) {
        const char *cause = !s->checked ? "startup" :
            (s->topology.count > s->reported.count ? "dock" :
             (s->topology.count < s->reported.count ? "undock" :
              (s->checked_lid != s->lid ? "lid" : "topology")));
        s->checked = key;
        s->checked_lid = s->lid;
        s->settle = 0;
        autoapply(s, cause);
        if (s->settle) return;
        if (!*s->reason) settle_after(s, cause, "");
    }
    if (s->topology.count && !has_display(&s->topology) && recover(s, error)) return;
    Layout before = current_layout(&s->reported), after = current_layout(&s->topology);
    bool effective = !layout_equal(&before, &after) ||
                     s->reported.width != s->topology.width || s->reported.height != s->topology.height;
    if ((effective || s->force_hook) && *s->config.hook)
        spawn(s->config.hook, *s->reason ? s->reason : "external", s->profile);
    s->reported = s->topology;
    s->settle = 0;
    s->force_hook = false;
    s->reason[0] = s->profile[0] = 0;
}

static void
process(Session *s, const Request *request, Reply *reply)
{
    *reply = (Reply){.version = REQUEST_VERSION};
    char *error = reply->message;
    if (request->version != REQUEST_VERSION ||
        !memchr(request->preset, 0, sizeof request->preset)) {
        snprintf(error, ERROR_SIZE, "Incompatible Undocked session. Restart the daemon.");
        return;
    }
    if (!scan(s, error)) return;
    switch (request->command) {
    case GET:
        reply->ok = true;
        snprintf(error, ERROR_SIZE, "%s", s->status);
        break;
    case TEST:
    case APPLY: {
        Layout layout;
        bool ports = s->config.match_ports || (request->flags & STRICT_PORTS);
        if (request->expected && request->expected != topology_key(&s->topology)) {
            snprintf(error, ERROR_SIZE, "The connected displays changed. Refresh the layout and try again.");
            break;
        }
        bool valid = request->layout.count >= 1 && request->layout.count <= OUTPUTS;
        for (int i = 0; valid && i < request->layout.count; i++)
            valid = memchr(request->layout.output[i].port, 0, PORT) != NULL;
        if (!valid) {
            snprintf(error, ERROR_SIZE, "Invalid layout.");
            break;
        }
        if (!resolve_layout(&s->topology, &request->layout, ports, &layout, error) ||
            !normalize_layout(&s->topology, &layout, error)) break;
        reply->ok = request->command == TEST ? s->backend.apply(&s->backend, &layout, true, error) :
            apply_layout(s, layout, request->flags & IMMEDIATE, "apply", request->preset, error);
        break;
    }
    case KEEP:
        if (s->deadline) {
            s->deadline = 0;
            s->previous_valid = false;
            settle_after(s, NULL, NULL);
            reply->ok = true;
        } else snprintf(error, ERROR_SIZE, "There is no pending display change.");
        break;
    case REVERT:
        if (s->previous_valid) reply->ok = restore(s, error);
        else snprintf(error, ERROR_SIZE, "There is no pending display change.");
        break;
    case RECOVER:
        s->deadline = 0;
        s->previous_valid = false;
        reply->ok = recover(s, error);
        break;
    case IDENTIFY:
        identify(s);
        reply->ok = true;
        break;
    case ENABLE_TRAY: case DISABLE_TRAY:
        s->tray_override = request->command == ENABLE_TRAY;
        set_tray(s);
        reply->ok = true;
        break;
    case RELOAD: {
        Config config;
        reply->ok = config_load(&config, error);
        if (reply->ok) {
            s->config = config;
            set_tray(s);
            s->checked = 0;
            settle_after(s, NULL, NULL);
        }
        break;
    }
    default:
        snprintf(error, ERROR_SIZE, "Unknown Undocked command.");
    }
    if (reply->ok && request->command != GET) scan(s, error);
    reply->topology = s->topology;
    reply->extended = s->extended;
    long long left = s->deadline - now_ms();
    reply->remaining = left > 0 ? (int)((left + 999) / 1000) : 0;
}

int
daemon_main(int tray, bool startup)
{
    static Session s;
    char error[ERROR_SIZE], lock_path[sizeof(struct sockaddr_un)];
    struct sockaddr_un address;
    if (!config_load(&s.config, error)) goto fail;
    s.tray_override = tray;
    if (!socket_address(&address)) {
        snprintf(error, sizeof error, "A valid DISPLAY and private runtime directory are required.");
        goto fail;
    }
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", address.sun_path) >= (int)sizeof lock_path) {
        snprintf(error, sizeof error, "Runtime directory path is too long.");
        goto fail;
    }
    int lock = open(lock_path, O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0) close(lock);
        if (tray >= 0) {
            Reply reply;
            enum Command command = tray ? ENABLE_TRAY : DISABLE_TRAY;
            if (!rpc(&(Request){.version = REQUEST_VERSION, .command = command}, &reply, false)) {
                snprintf(error, sizeof error, "%s", reply.message);
                goto fail;
            }
        }
        return 0;
    }
    if (!x11_open(&s.x, error)) goto fail;
    s.backend = x11_backend(&s.x);
    if (!scan(&s, error)) goto fail;
    s.reported = s.topology;
    set_tray(&s);
    s.lid = s.config.follow_lid && lid_closed();
    s.checked_lid = s.lid;
    s.checked = startup ? 0 : topology_key(&s.topology);
    if (startup) settle_after(&s, NULL, NULL);
    int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    unlink(address.sun_path);
    mode_t old_mask = umask(0077);
    int bound = socket_fd < 0 ? -1 : bind(socket_fd, (struct sockaddr *)&address, sizeof address);
    umask(old_mask);
    if (bound != 0 || listen(socket_fd, 12)) {
        snprintf(error, sizeof error, "Cannot open the Undocked session socket: %s", strerror(errno));
        goto fail;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    signal(SIGCHLD, SIG_IGN);
    Display *d = s.x.display;
    char tray_selection[64];
    snprintf(tray_selection, sizeof tray_selection, "_NET_SYSTEM_TRAY_S%d", s.x.screen);
    s.tray_selection = XInternAtom(d, tray_selection, False);
    s.tray_visual = XInternAtom(d, "_NET_SYSTEM_TRAY_VISUAL", False);
    s.tray_colors = XInternAtom(d, "_NET_SYSTEM_TRAY_COLORS", False);
    s.embed = XInternAtom(d, "_XEMBED", False);
    XSelectInput(d, s.x.root, StructureNotifyMask | PropertyChangeMask);
    while (!stopping) {
        while (XPending(d)) {
            XEvent event;
            XNextEvent(d, &event);
            if (x11_change(&s.x, &event)) {
                if (s.badges_until) close_badges(&s);
                settle_after(&s, NULL, NULL);
            } else if (s.tray) tray_event(&s, &event);
        }
        long long time = now_ms();
        if (s.deadline && time >= s.deadline && !restore(&s, error))
            fprintf(stderr, "undocked: rollback failed: %s\n", error);
        if (s.badges_until && time >= s.badges_until) close_badges(&s);
        if (s.tray && time >= s.next_tray) {
            dock_icon(&s);
            s.next_tray = time + 1000;
        }
        if (s.tray_dirty) paint_tray(&s);
        if (time >= s.lid_check) {
            struct timespec boot;
            if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
                long long resumed = boot.tv_sec * 1000LL + boot.tv_nsec / 1000000 - time;
                if (s.last_resume && resumed - s.last_resume > 1000) {
                    s.checked = 0;
                    settle_after(&s, "resume", "");
                }
                s.last_resume = resumed;
            }
            bool closed = s.config.follow_lid && lid_closed();
            if (s.lid != closed) {
                s.lid = closed;
                settle_after(&s, NULL, NULL);
            }
            s.lid_check = time + 1000;
        }
        if (s.settle && time >= s.settle) reconcile(&s);
        XFlush(d);
        struct pollfd wait[] = {{ConnectionNumber(d), POLLIN, 0}, {socket_fd, POLLIN, 0}};
        if (poll(wait, 2, 100) < 0 || !(wait[1].revents & POLLIN)) continue;
        int client = accept4(socket_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) continue;
        struct timeval timeout = {.tv_usec = 150000};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        Request request;
        Reply reply;
        if (recv(client, &request, sizeof request, MSG_TRUNC) == (ssize_t)sizeof request) {
            process(&s, &request, &reply);
            (void)send(client, &reply, sizeof reply, MSG_NOSIGNAL);
        }
        close(client);
    }
    if (s.deadline) (void)restore(&s, error);
    close_badges(&s);
    drop_icon(&s);
    if (s.have_badge_painter) painter_close(&s.badge_painter);
    close(socket_fd);
    unlink(address.sun_path);
    close(lock);
    XCloseDisplay(d);
    return 0;

fail:
    fprintf(stderr, "undocked: %s\n", error);
    return 1;
}
