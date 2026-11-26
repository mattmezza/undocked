#include "undocked.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

long long
now_ms(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000LL + time.tv_nsec / 1000000;
}

const char *
executable(void)
{
    static char path[PATH_SIZE];
    if (!*path) {
        ssize_t size = readlink("/proc/self/exe", path, sizeof path - 1);
        if (size > 0) {
            path[size] = 0;
            const char deleted[] = " (deleted)";
            if ((size_t)size >= sizeof deleted - 1 &&
                !strcmp(path + size - (sizeof deleted - 1), deleted))
                path[size - (sizeof deleted - 1)] = 0;
        }
    }
    return *path ? path : "/proc/self/exe";
}

static char *
trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static bool
parse_int(const char *s, int *value, int min, int max)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < min || v > max)
        return false;
    *value = (int)v;
    return true;
}

static bool
parse_bool(const char *s, bool *value)
{
    if (!strcmp(s, "true")) *value = true;
    else if (!strcmp(s, "false")) *value = false;
    else return false;
    return true;
}

static bool
parse_arrangements(char *value, Config *config)
{
    bool seen[ARRANGEMENT_COUNT] = {0};
    config->quick_count = 0;
    for (char *key = strsep(&value, ","); key; key = strsep(&value, ",")) {
        key = trim(key);
        if (!*key && !value && !config->quick_count) return true;
        enum Arrangement arrangement = arrangement_kind(key);
        if (arrangement == ARRANGEMENT_COUNT || seen[arrangement]) return false;
        seen[arrangement] = true;
        config->quick[config->quick_count++] = arrangement;
    }
    return true;
}

static bool
path_value(char *path, const char *value, const char *relative)
{
    if (*value == '/')
        return snprintf(path, PATH_SIZE, "%s", value) < PATH_SIZE;
    if (value[0] == '~' && value[1] == '/') {
        const char *home = getenv("HOME");
        return home && snprintf(path, PATH_SIZE, "%s/%s", home, value + 2) < PATH_SIZE;
    }
    if (!*value) {
        *path = 0;
        return true;
    }
    return snprintf(path, PATH_SIZE, "%s/%s", relative, value) < PATH_SIZE;
}

bool
config_load(Config *config, char *error)
{
    *config = (Config){.settle = 650, .confirm = 15, .identify = 4, .width = 760,
                       .height = 500, .snap = 14, .radius = -1, .drawer_ms = 160, .automatic = true,
                       .quick = {ROW_CENTER, COLUMN_CENTER, TWO_ABOVE}, .quick_count = 3};
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    char root[PATH_SIZE];
    int size;
    if (xdg && *xdg == '/')
        size = snprintf(root, sizeof root, "%s/undocked", xdg);
    else
        size = snprintf(root, sizeof root, "%s/.config/undocked", home ? home : "");
    if (size < 0 || size >= (int)sizeof root ||
        !path_value(config->config, "config.ini", root) ||
        !path_value(config->presets, "presets", root)) {
        snprintf(error, ERROR_SIZE, "Config path is too long.");
        return false;
    }
    FILE *file = fopen(config->config, "r");
    if (!file) {
        if (errno == ENOENT) return true;
        snprintf(error, ERROR_SIZE, "Cannot open config.ini: %s", strerror(errno));
        return false;
    }
    char section[32] = "undocked", line[PATH_SIZE + 128];
    unsigned number = 0;
    bool valid = true;
    while (fgets(line, sizeof line, file)) {
        number++;
        char *key = trim(line);
        if (!*key || *key == '#' || *key == ';') continue;
        if (*key == '[') {
            char *end = strchr(++key, ']');
            if (!end || end[1] || end - key >= (long)sizeof section) { valid = false; break; }
            *end = 0;
            strcpy(section, key);
            continue;
        }
        char *value = strchr(key, '=');
        if (!value) { valid = false; break; }
        *value++ = 0;
        key = trim(key);
        value = trim(value);
        if (!strcmp(section, "ports")) {
            int i = config->aliases;
            if (i == OUTPUTS || strlen(key) >= PORT || strlen(value) >= NAME || !*key) {
                valid = false;
                break;
            }
            strcpy(config->port[i], key);
            strcpy(config->alias[i], value);
            config->aliases++;
        } else if (!strcmp(section, "undocked")) {
            if (!strcmp(key, "autoapply")) valid = parse_bool(value, &config->automatic);
            else if (!strcmp(key, "tray")) continue; /* Accept existing configuration files. */
            else if (!strcmp(key, "match_ports")) valid = parse_bool(value, &config->match_ports);
            else if (!strcmp(key, "follow_lid")) valid = parse_bool(value, &config->follow_lid);
            else if (!strcmp(key, "settle_ms")) valid = parse_int(value, &config->settle, 100, 10000);
            else if (!strcmp(key, "confirm_seconds")) valid = parse_int(value, &config->confirm, 5, 120);
            else if (!strcmp(key, "identify_seconds")) valid = parse_int(value, &config->identify, 1, 30);
            else if (!strcmp(key, "presets")) valid = *value && path_value(config->presets, value, root);
            else if (!strcmp(key, "after_change")) {
                valid = strlen(value) < sizeof config->hook;
                if (valid) strcpy(config->hook, value);
            } else valid = false;
        } else if (!strcmp(section, "ui")) {
            if (!strcmp(key, "width")) valid = parse_int(value, &config->width, 640, 1920);
            else if (!strcmp(key, "height")) valid = parse_int(value, &config->height, 480, 1440);
            else if (!strcmp(key, "snap_px")) valid = parse_int(value, &config->snap, 0, 60);
            else if (!strcmp(key, "corner_radius")) valid = parse_int(value, &config->radius, 0, 24);
            else if (!strcmp(key, "drawer_animation_ms")) valid = parse_int(value, &config->drawer_ms, 0, 1000);
            else if (!strcmp(key, "quick_arrangements")) valid = parse_arrangements(value, config);
            else if (!strcmp(key, "font")) valid = path_value(config->font, value, root);
            else if (!strcmp(key, "mono")) valid = path_value(config->mono, value, root);
            else valid = false;
        } else valid = false;
        if (!valid) break;
    }
    if (ferror(file)) valid = false;
    fclose(file);
    if (!valid)
        snprintf(error, ERROR_SIZE, "Invalid config.ini setting on line %u.", number);
    return valid;
}

const char *
alias_for(const Config *config, const char *port)
{
    for (int i = config->aliases - 1; i >= 0; i--)
        if (!strcmp(config->port[i], port)) return config->alias[i];
    return "";
}

bool
preset_name(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length >= NAME || !isalnum((unsigned char)name[0]) ||
        name[length - 1] == ' ')
        return false;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
        if (*c >= 128 || (!isalnum(*c) && !strchr(" -_.", *c))) return false;
    return true;
}

bool
preset_path(const Config *config, const char *name, char *path, char *error)
{
    if (!preset_name(name)) {
        snprintf(error, ERROR_SIZE, "Use a name of 1–63 letters, digits, spaces, dots, - or _.");
        return false;
    }
    if (snprintf(path, PATH_SIZE, "%s/%s.sh", config->presets, name) >= PATH_SIZE) {
        snprintf(error, ERROR_SIZE, "Preset path is too long.");
        return false;
    }
    return true;
}

bool
preset_read(const char *path, Preset *preset, char *error)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        snprintf(error, ERROR_SIZE, "Cannot read preset: %s", strerror(errno));
        return false;
    }

    *preset = (Preset){0};
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t length = strlen(base);
    if (length > 3 && length - 3 < NAME && !strcmp(base + length - 3, ".sh"))
        memcpy(preset->name, base, length - 3);

    char line[512];
    bool valid = fgets(line, sizeof line, file) && !strcmp(trim(line), "#!/bin/sh") &&
                 fgets(line, sizeof line, file) && !strcmp(trim(line), "# Undocked preset 1");
    bool have_auto = false;
    while (valid && fgets(line, sizeof line, file)) {
        char *s = trim(line);
        if (!strncmp(s, "# autoapply=", 12)) {
            valid = !have_auto && parse_bool(s + 12, &preset->automatic);
            have_auto = true;
        } else if (!strncmp(s, "# output ", 9) && preset->layout.count < OUTPUTS) {
            Placement *p = &preset->layout.output[preset->layout.count];
            int enabled, primary;
            char extra;
            valid = sscanf(s + 9, "%63s %16" SCNx64 " %1d %5d %5d %7d %6d %6d %3d %1d %c",
                           p->port, &p->edid, &enabled, &p->mode.width, &p->mode.height,
                           &p->mode.millihz, &p->x, &p->y, &p->rotation, &primary, &extra) == 10 &&
                    (enabled == 0 || enabled == 1) && (primary == 0 || primary == 1);
            if (valid) {
                p->enabled = enabled;
                p->primary = primary;
                preset->layout.count++;
            }
        } else if (*s && !strcmp(s, "exec undocked load \"$0\" \"$@\"")) {
            continue;
        } else if (*s) {
            valid = false;
        }
    }
    valid = valid && have_auto && preset->layout.count > 0 && !ferror(file);
    fclose(file);
    if (!valid)
        snprintf(error, ERROR_SIZE, "Invalid Undocked preset %.80s.", base);
    return valid;
}

static bool
mkdirs(char *path)
{
    for (char *p = path + 1; ; p++) {
        if (*p && *p != '/') continue;
        char end = *p;
        *p = 0;
        int status = mkdir(path, 0700);
        *p = end;
        if (status && errno != EEXIST) return false;
        if (!end) return true;
    }
}

bool
preset_write(const Config *config, const Preset *preset, bool replace, char *error)
{
    char path[PATH_SIZE], parent[PATH_SIZE], temp[PATH_SIZE];
    if (!preset_path(config, preset->name, path, error)) return false;
    strcpy(parent, config->presets);
    if (!mkdirs(parent) ||
        snprintf(temp, sizeof temp, "%s/.undocked-XXXXXX", parent) >= (int)sizeof temp) {
        snprintf(error, ERROR_SIZE, "Cannot create the presets directory.");
        return false;
    }
    int fd = mkstemp(temp);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "w");
    if (!file) {
        snprintf(error, ERROR_SIZE, "Cannot create preset: %s", strerror(errno));
        if (fd >= 0) { close(fd); unlink(temp); }
        return false;
    }
    fprintf(file, "#!/bin/sh\n# Undocked preset 1\n# autoapply=%s\n",
            preset->automatic ? "true" : "false");
    for (int i = 0; i < preset->layout.count; i++) {
        const Placement *p = &preset->layout.output[i];
        fprintf(file, "# output %s %016" PRIx64 " %d %d %d %d %d %d %d %d\n", p->port,
                p->edid, p->enabled, p->mode.width, p->mode.height, p->mode.millihz,
                p->x, p->y, p->rotation, p->primary);
    }
    fprintf(file, "exec undocked load \"$0\" \"$@\"\n");
    bool ok = fflush(file) == 0 && fchmod(fd, 0755) == 0 && fsync(fd) == 0;
    if (fclose(file)) ok = false;
    if (ok)
        ok = replace ? rename(temp, path) == 0 : link(temp, path) == 0;
    if (!ok)
        snprintf(error, ERROR_SIZE, errno == EEXIST ? "A preset named %.63s already exists." :
                 "Could not save preset %.63s.", preset->name);
    unlink(temp);
    return ok;
}

static int
compare_names(const void *a, const void *b)
{
    return strcmp(a, b);
}

int
preset_list(const Config *config, char names[PRESETS][NAME])
{
    DIR *dir = opendir(config->presets);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while (count < PRESETS && (entry = readdir(dir))) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || length - 3 >= NAME || strcmp(entry->d_name + length - 3, ".sh"))
            continue;
        memcpy(names[count], entry->d_name, length - 3);
        names[count][length - 3] = 0;
        Preset preset;
        char path[PATH_SIZE], error[ERROR_SIZE];
        if (preset_path(config, names[count], path, error) && preset_read(path, &preset, error))
            count++;
    }
    closedir(dir);
    qsort(names, (size_t)count, NAME, compare_names);
    return count;
}

bool
lid_closed(void)
{
    const char *root = "/proc/acpi/button/lid";
    DIR *dir = opendir(root);
    if (!dir) return false;
    bool closed = false;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_SIZE], line[64];
        snprintf(path, sizeof path, "%s/%s/state", root, entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        if (fgets(line, sizeof line, file) && strstr(line, "closed")) closed = true;
        fclose(file);
    }
    closedir(dir);
    return closed;
}
