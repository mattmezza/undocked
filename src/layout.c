#include "undocked.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

bool
same_mode(Mode a, Mode b)
{
    return a.width == b.width && a.height == b.height && a.millihz == b.millihz;
}

void
dimensions(Placement p, int *width, int *height)
{
    bool sideways = p.rotation == 90 || p.rotation == 270;
    *width = sideways ? p.mode.height : p.mode.width;
    *height = sideways ? p.mode.width : p.mode.height;
}

Layout
current_layout(const Topology *topology)
{
    Layout result = {.count = topology->count};
    for (int i = 0; i < topology->count; i++)
        result.output[i] = topology->output[i].current;
    return result;
}

bool
layout_equal(const Layout *a, const Layout *b)
{
    if (a->count != b->count)
        return false;
    for (int i = 0; i < a->count; i++) {
        const Placement *p = &a->output[i], *q = &b->output[i];
        if (strcmp(p->port, q->port) || p->edid != q->edid ||
            p->enabled != q->enabled || p->primary != q->primary)
            return false;
        if (p->enabled && (!same_mode(p->mode, q->mode) || p->x != q->x ||
                           p->y != q->y || p->rotation != q->rotation))
            return false;
    }
    return true;
}

uint64_t
topology_key(const Topology *t)
{
    uint64_t key = UINT64_C(14695981039346656037);
    for (int i = 0; i < t->count; i++) {
        const Placement *p = &t->output[i].current;
        for (const unsigned char *c = (const unsigned char *)p->port; *c; c++)
            key = (key ^ *c) * UINT64_C(1099511628211);
        key = (key ^ p->edid) * UINT64_C(1099511628211);
    }
    key ^= (unsigned)t->count;
    return key ? key : 1;
}

bool
resolve_layout(const Topology *t, const Layout *saved, bool ports, Layout *layout, char *error)
{
    if (saved->count != t->count || saved->count < 1 || saved->count > OUTPUTS) {
        snprintf(error, ERROR_SIZE, "Preset expects %d connected outputs; found %d.",
                 saved->count, t->count);
        return false;
    }
    bool used[OUTPUTS] = {0};
    int mapping[OUTPUTS];
    for (int i = 0; i < saved->count; i++) {
        const Placement *p = &saved->output[i];
        mapping[i] = -1;
        for (int j = 0; j < t->count; j++) {
            const Placement *q = &t->output[j].current;
            if (!used[j] && strcmp(p->port, q->port) == 0 &&
                (!p->edid || p->edid == q->edid)) {
                mapping[i] = j;
                used[j] = true;
                break;
            }
        }
    }
    for (int i = 0; i < saved->count; i++) {
        const Placement *p = &saved->output[i];
        if (mapping[i] >= 0)
            continue;
        int found = -1, live_count = 0, saved_count = 0;
        if (!ports && p->edid) {
            for (int j = 0; j < t->count; j++) {
                if (p->edid == t->output[j].current.edid) {
                    found = j;
                    live_count++;
                }
                saved_count += p->edid == saved->output[j].edid;
            }
        }
        if (live_count != 1 || saved_count != 1 || used[found]) {
            snprintf(error, ERROR_SIZE, "Cannot match %.63s to a unique connected display.", p->port);
            return false;
        }
        mapping[i] = found;
        used[found] = true;
    }

    layout->count = t->count;
    for (int i = 0; i < saved->count; i++) {
        int j = mapping[i];
        layout->output[j] = saved->output[i];
        memcpy(layout->output[j].port, t->output[j].current.port, PORT);
        layout->output[j].edid = t->output[j].current.edid;
    }
    return true;
}

bool
normalize_layout(const Topology *t, Layout *layout, char *error)
{
    int left = INT_MAX, top = INT_MAX, first = -1, primary = -1;
    for (int i = 0; i < layout->count; i++) {
        Placement *p = &layout->output[i];
        if (!p->enabled)
            continue;
        const Output *out = &t->output[i];
        bool valid = false;
        for (int j = 0; j < out->count; j++)
            if (same_mode(p->mode, out->modes[j]))
                valid = true;
        if (!valid || p->rotation < 0 || p->rotation > 270 || p->rotation % 90 ||
            !(out->rotations & (1 << (p->rotation / 90))) ||
            p->x < -65535 || p->x > 65535 || p->y < -65535 || p->y > 65535) {
            snprintf(error, ERROR_SIZE, "Unsupported mode, orientation or position for %s.", p->port);
            return false;
        }
        if (p->x < left) left = p->x;
        if (p->y < top) top = p->y;
        if (first < 0) first = i;
        if (p->primary && primary < 0) primary = i;
    }
    if (first < 0) {
        snprintf(error, ERROR_SIZE, "Enable at least one display.");
        return false;
    }
    if (primary < 0) primary = first;
    for (int i = 0; i < layout->count; i++) {
        Placement *p = &layout->output[i];
        p->primary = i == primary;
        if (!p->enabled)
            continue;
        p->x -= left;
        p->y -= top;
        int w, h;
        dimensions(*p, &w, &h);
        if (p->x > 32767 || p->y > 32767 || p->x + w > t->max_width ||
            p->y + h > t->max_height) {
            snprintf(error, ERROR_SIZE, "Layout exceeds the %d × %d framebuffer limit.",
                     t->max_width, t->max_height);
            return false;
        }
    }
    return true;
}

static void
enable(const Output *out, Placement *p)
{
    if (!p->enabled) {
        p->enabled = true;
        p->mode = out->modes[out->preferred];
        p->rotation = 0;
    }
}

Layout
recovery_layout(const Topology *t, bool closed)
{
    Layout result = current_layout(t);
    int best = -1;
    for (int i = 0; i < t->count; i++) {
        const Output *out = &t->output[i];
        if (out->count && (best < 0 || (out->internal != t->output[best].internal &&
                                       out->internal != closed)))
            best = i;
    }
    for (int i = 0; i < t->count; i++) {
        Placement *p = &result.output[i];
        p->enabled = p->primary = i == best;
        if (p->enabled) {
            p->mode = t->output[i].modes[t->output[i].preferred];
            p->x = p->y = p->rotation = 0;
        }
    }
    return result;
}

bool
suggest(const Topology *t, Layout *layout, const char *kind, char *error)
{
    bool row = strcmp(kind, "row") == 0, column = strcmp(kind, "column") == 0;
    bool y = strcmp(kind, "y") == 0, mirror = strcmp(kind, "mirror") == 0;
    if (!row && !column && !y && !mirror) {
        snprintf(error, ERROR_SIZE, "Choose row, column, y or mirror.");
        return false;
    }
    int indices[OUTPUTS], count = 0, primary = -1, panel = -1;
    for (int i = 0; i < t->count; i++) {
        if (!t->output[i].count)
            continue;
        indices[count++] = i;
        if (layout->output[i].primary) primary = i;
        if (t->output[i].internal) panel = i;
    }
    if (count < 2 || (y && count != 3)) {
        snprintf(error, ERROR_SIZE, y ? "This layout needs three displays." : "Connect at least two displays.");
        return false;
    }
    if (mirror) {
        const Output *first = &t->output[indices[0]];
        Mode best[OUTPUTS] = {0};
        long long score = -1;
        for (int i = 0; i < first->count; i++) {
            Mode mode = first->modes[i], candidate[OUTPUTS] = {mode};
            int j = 1;
            for (; j < count; j++) {
                const Output *out = &t->output[indices[j]];
                int match = -1;
                for (int k = 0; k < out->count; k++) {
                    Mode m = out->modes[k];
                    if (m.width != mode.width || m.height != mode.height) continue;
                    if (match < 0 || m.millihz > out->modes[match].millihz) match = k;
                    if (same_mode(m, mode)) { match = k; break; }
                }
                if (match < 0) break;
                candidate[j] = out->modes[match];
            }
            long long rank = (long long)mode.width * mode.height * 1000000 + mode.millihz;
            if (j == count && rank > score) {
                score = rank;
                memcpy(best, candidate, sizeof best);
            }
        }
        if (score < 0) {
            snprintf(error, ERROR_SIZE, "The connected displays share no mirror resolution.");
            return false;
        }
        for (int i = 0; i < count; i++) {
            Placement *p = &layout->output[indices[i]];
            p->enabled = true;
            p->mode = best[i];
            p->x = p->y = p->rotation = 0;
        }
    } else {
        int offset = 0, bottom = panel >= 0 ? panel : (primary >= 0 ? primary : indices[0]);
        int top_height = 0;
        for (int i = 0; i < count; i++) {
            int index = indices[i];
            Placement *p = &layout->output[index];
            enable(&t->output[index], p);
            int w, h;
            dimensions(*p, &w, &h);
            if (y && index == bottom) continue;
            p->x = column ? 0 : offset;
            p->y = column ? offset : 0;
            offset += column ? h : w;
            if (h > top_height) top_height = h;
        }
        if (y) {
            Placement *p = &layout->output[bottom];
            int w, h;
            dimensions(*p, &w, &h);
            p->x = (offset - w) / 2;
            p->y = top_height;
            for (int i = 0; i < count; i++) {
                if (indices[i] == bottom) continue;
                Placement *upper = &layout->output[indices[i]];
                dimensions(*upper, &w, &h);
                upper->y = top_height - h;
            }
        }
    }
    return normalize_layout(t, layout, error);
}
