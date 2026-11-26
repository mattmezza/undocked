#ifndef UNDOCKED_H
#define UNDOCKED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OUTPUTS 24
#define MODES 128
#define PORT 64
#define NAME 64
#define PATH_SIZE 4096
#define ERROR_SIZE 256
#define PRESETS 128

typedef struct {
    int width, height, millihz;
} Mode;

typedef struct {
    char port[PORT];
    uint64_t edid;
    Mode mode;
    int x, y, rotation;
    bool enabled, primary;
} Placement;

typedef struct {
    int count;
    Placement output[OUTPUTS];
} Layout;

typedef struct {
    Placement current;
    char model[NAME];
    int mm_width, mm_height, count, preferred, rotations;
    bool internal;
    Mode modes[MODES];
} Output;

typedef struct {
    int count, width, height, max_width, max_height;
    Output output[OUTPUTS];
} Topology;

typedef struct {
    char name[NAME];
    bool automatic;
    Layout layout;
} Preset;

typedef struct {
    int settle, confirm, identify, width, height, snap, radius;
    bool automatic, match_ports, follow_lid, tray;
    char hook[PATH_SIZE], font[PATH_SIZE], mono[PATH_SIZE], presets[PATH_SIZE];
    char config[PATH_SIZE];
    char port[OUTPUTS][PORT], alias[OUTPUTS][NAME];
    int aliases;
} Config;

enum Command {
    GET = 1, TEST, APPLY, KEEP, REVERT, RECOVER, IDENTIFY, ENABLE_TRAY, RELOAD, DISABLE_TRAY
};
enum { REQUEST_VERSION = 1, IMMEDIATE = 1, STRICT_PORTS = 2 };

typedef struct {
    uint32_t version, command, flags;
    uint64_t expected;
    char preset[NAME];
    Layout layout;
} Request;

typedef struct {
    uint32_t version;
    bool ok;
    int remaining;
    char message[ERROR_SIZE];
    Topology topology;
    Layout extended;
} Reply;

typedef struct Backend Backend;
struct Backend {
    void *state;
    bool (*snapshot)(Backend *, Topology *, char *);
    bool (*apply)(Backend *, const Layout *, bool test, char *);
};

long long now_ms(void);
const char *executable(void);
bool config_load(Config *, char *);
const char *alias_for(const Config *, const char *);
bool preset_name(const char *);
bool preset_path(const Config *, const char *, char *, char *);
bool preset_read(const char *, Preset *, char *);
bool preset_write(const Config *, const Preset *, bool replace, char *);
int preset_list(const Config *, char names[PRESETS][NAME]);

bool same_mode(Mode, Mode);
void dimensions(Placement, int *, int *);
Layout current_layout(const Topology *);
bool layout_enable(const Topology *, Layout *, int, bool);
bool layout_equal(const Layout *, const Layout *);
bool layout_mirrored(const Layout *);
uint64_t topology_key(const Topology *);
bool resolve_layout(const Topology *, const Layout *, bool, Layout *, char *);
bool normalize_layout(const Topology *, Layout *, char *);
bool suggest(const Topology *, Layout *, const char *, char *);
bool extend_layout(const Reply *, Layout *, char *);
Layout recovery_layout(const Topology *, bool lid_closed);
bool lid_closed(void);

bool rpc(const Request *, Reply *, bool start);
int daemon_main(int tray, bool reconcile);
int daemon_restart(bool only_running);
int editor_main(void);
int launch_main(void);

#endif
