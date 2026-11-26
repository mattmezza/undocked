#include "undocked.h"

#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char usage[] =
    "Usage: undocked [command]\n"
    "  (no command)                       Open the display editor\n"
    "  daemon [--tray]                    Watch RandR changes in this X session\n"
    "  launch                            Pick actions and presets in Launch\n"
    "  status [--json]                    List outputs, modes and session state\n"
    "  presets [--json]                   List saved presets and match reasons\n"
    "  preset NAME [--dry-run|--yes]       Apply a saved preset\n"
    "  load FILE [--dry-run|--yes]         Apply a portable preset script\n"
    "  save NAME [--auto] [--replace]      Save the current layout as a script\n"
    "  auto NAME on|off                   Set automatic matching for a preset\n"
    "  layout row|column|y|mirror [--dry-run|--yes]\n"
    "  output PORT on|off|primary|mode WxH[@Hz]|rotate DEG|move X Y [--dry-run|--yes]\n"
    "  identify | keep | revert | recover | reload\n";

static void
json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static const char *
boolean(bool b)
{
    return b ? "true" : "false";
}

static int
fail(const char *error)
{
    fprintf(stderr, "undocked: %s\n", error);
    return 1;
}

static bool
get(Reply *reply)
{
    return rpc(&(Request){.version = REQUEST_VERSION, .command = GET}, reply, true);
}

static void
print_status(const Reply *reply, const Config *config, bool json)
{
    const Topology *t = &reply->topology;
    if (json) {
        printf("{\"version\":1,\"confirmation_seconds\":%d,\"status\":", reply->remaining);
        json_string(reply->message);
        printf(",\"framebuffer\":[%d,%d],\"outputs\":[", t->width, t->height);
    }
    for (int i = 0; i < t->count; i++) {
        const Output *out = &t->output[i];
        Placement p = out->current;
        if (!json) {
            printf("%d\t%s\t%s", i + 1, p.port, out->model);
            if (*alias_for(config, p.port)) printf(" (%s)", alias_for(config, p.port));
            if (!p.enabled) printf("\toff\n");
            else printf("\t%dx%d @ %.3fHz %+d%+d%s\n", p.mode.width, p.mode.height,
                        p.mode.millihz / 1000.0, p.x, p.y, p.primary ? " primary" : "");
            continue;
        }
        printf("%s{\"number\":%d,\"port\":", i ? "," : "", i + 1);
        json_string(p.port);
        printf(",\"model\":");
        json_string(out->model);
        printf(",\"alias\":");
        json_string(alias_for(config, p.port));
        printf(",\"edid\":\"%016" PRIx64 "\",\"enabled\":%s,\"primary\":%s,\"internal\":%s,"
               "\"position\":[%d,%d],\"rotation\":%d,\"physical_mm\":[%d,%d],"
               "\"mode\":{\"width\":%d,\"height\":%d,\"millihz\":%d},\"modes\":[",
               p.edid, boolean(p.enabled), boolean(p.primary), boolean(out->internal), p.x, p.y,
               p.rotation, out->mm_width, out->mm_height, p.mode.width, p.mode.height, p.mode.millihz);
        for (int j = 0; j < out->count; j++)
            printf("%s{\"width\":%d,\"height\":%d,\"millihz\":%d,\"preferred\":%s}",
                   j ? "," : "", out->modes[j].width, out->modes[j].height,
                   out->modes[j].millihz, boolean(j == out->preferred));
        printf("]}");
    }
    if (json) puts("]}");
    else {
        if (*reply->message) puts(reply->message);
        if (reply->remaining) printf("Keep or revert within %d seconds.\n", reply->remaining);
    }
}

static bool
read_named(const Config *config, const char *name, Preset *preset, char *error)
{
    char path[PATH_SIZE];
    return preset_path(config, name, path, error) && preset_read(path, preset, error);
}

static bool
match_preset(const Config *config, const Topology *topology, const Preset *preset, char *error)
{
    Layout layout;
    return resolve_layout(topology, &preset->layout, config->match_ports, &layout, error) &&
           normalize_layout(topology, &layout, error);
}

static int
print_presets(const Config *config, bool json)
{
    char names[PRESETS][NAME];
    int count = preset_list(config, names);
    Reply reply;
    bool live = json && get(&reply);
    if (json) printf("{\"version\":1,\"presets\":[");
    for (int i = 0; i < count; i++) {
        if (!json) {
            puts(names[i]);
            continue;
        }
        Preset preset;
        char reason[ERROR_SIZE] = "";
        bool ok = read_named(config, names[i], &preset, reason);
        bool matched = ok && live && match_preset(config, &reply.topology, &preset, reason);
        printf("%s{\"name\":", i ? "," : "");
        json_string(names[i]);
        printf(",\"autoapply\":%s,\"matches\":%s,\"reason\":",
               boolean(ok && preset.automatic), live ? boolean(matched) : "null");
        json_string(live ? reason : "X11 session unavailable");
        printf("}");
    }
    if (json) puts("]}");
    return 0;
}

static bool
apply_option(const char *arg, bool *dry, bool *yes)
{
    if (!strcmp(arg, "--dry-run")) *dry = true;
    else if (!strcmp(arg, "--yes")) *yes = true;
    else return false;
    return !(*dry && *yes);
}

static int
submit(Layout layout, uint64_t key, const char *preset, bool dry, bool yes)
{
    Request request = {.version = REQUEST_VERSION, .command = dry ? TEST : APPLY,
        .flags = (yes ? IMMEDIATE : 0) | (key ? STRICT_PORTS : 0), .expected = key, .layout = layout};
    if (preset) snprintf(request.preset, sizeof request.preset, "%s", preset);
    Reply reply;
    if (!rpc(&request, &reply, true)) return fail(reply.message);
    if (dry) puts("Layout is supported.");
    else if (reply.remaining)
        printf("Applied. Run undocked keep within %d seconds to keep the change.\n", reply.remaining);
    else puts("Layout active.");
    return 0;
}

static bool
set_mode(const Output *out, Placement *p, const char *s)
{
    int w, h, consumed = 0;
    if (sscanf(s, "%5dx%5d%n", &w, &h, &consumed) != 2 || !consumed) return false;
    int rate = 0;
    if (s[consumed] == '@') {
        char *end;
        double hz = strtod(s + consumed + 1, &end);
        if (end == s + consumed + 1 || *end || !isfinite(hz) || hz < 0 || hz > 2000)
            return false;
        rate = (int)lround(hz * 1000);
    } else if (s[consumed]) return false;
    for (int i = 0; i < out->count; i++) {
        Mode m = out->modes[i];
        if (m.width == w && m.height == h && (s[consumed] != '@' || m.millihz == rate)) {
            p->mode = m;
            p->enabled = true;
            return true;
        }
    }
    return false;
}

static bool
number(const char *s, int *out)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < -65535 || n > 65535) return false;
    *out = (int)n;
    return true;
}

static int
set_output(int argc, char **argv)
{
    if (argc < 4) return fail("Use undocked output PORT ACTION.");
    const char *action = argv[3];
    int required = !strcmp(action, "move") ? 6 :
        (!strcmp(action, "mode") || !strcmp(action, "rotate") ? 5 : 4);
    bool dry = false, yes = false;
    if (argc < required || argc > required + 1 ||
        (argc == required + 1 && !apply_option(argv[required], &dry, &yes)))
        return fail("Invalid output arguments. See undocked --help.");
    Reply reply;
    if (!get(&reply)) return fail(reply.message);
    Topology *t = &reply.topology;
    Layout layout = current_layout(t);
    int index = 0;
    while (index < t->count && strcmp(t->output[index].current.port, argv[2])) index++;
    if (index == t->count) return fail("That output is disconnected or unknown.");
    Placement *p = &layout.output[index];
    const Output *out = &t->output[index];
    if (!strcmp(action, "on") && out->count) {
        if (!p->enabled) {
            int right = 0;
            for (int i = 0; i < layout.count; i++) {
                if (!layout.output[i].enabled) continue;
                int w, h;
                dimensions(layout.output[i], &w, &h);
                if (layout.output[i].x + w > right) right = layout.output[i].x + w;
            }
            p->x = right;
            p->y = 0;
            p->mode = out->modes[out->preferred];
            p->enabled = true;
        }
    } else if (!strcmp(action, "off")) p->enabled = false;
    else if (!strcmp(action, "primary") && p->enabled) {
        for (int i = 0; i < layout.count; i++) layout.output[i].primary = i == index;
    } else if (!strcmp(action, "mode")) {
        if (!set_mode(out, p, argv[4])) return fail("That mode is unavailable on this output.");
    } else if (!strcmp(action, "rotate") && p->enabled) {
        if (!number(argv[4], &p->rotation)) return fail("Use 0, 90, 180 or 270 degrees.");
    } else if (!strcmp(action, "move") && p->enabled) {
        if (!number(argv[4], &p->x) || !number(argv[5], &p->y)) return fail("Use integer coordinates.");
    } else return fail("That output action is unavailable.");
    return submit(layout, topology_key(t), NULL, dry, yes);
}

int
launch_main(void)
{
    Config config;
    char error[ERROR_SIZE];
    if (!config_load(&config, error)) return fail(error);
    FILE *input = tmpfile(), *output = tmpfile();
    if (!input || !output) {
        if (input) fclose(input);
        if (output) fclose(output);
        return fail("Cannot prepare the Launch picker.");
    }
    Reply reply;
    bool live = get(&reply);
    fputs("Open display editor\nIdentify displays\n", input);
    if (live && reply.remaining) fputs("Keep display changes\nRevert display changes\n", input);
    char names[PRESETS][NAME];
    int count = preset_list(&config, names);
    for (int i = 0; i < count; i++) {
        Preset preset;
        if (live && read_named(&config, names[i], &preset, error) &&
            match_preset(&config, &reply.topology, &preset, error))
            fprintf(input, "Switch to %s\n", names[i]);
    }
    rewind(input);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fileno(input), STDIN_FILENO);
        dup2(fileno(output), STDOUT_FILENO);
        execlp("launch", "launch", "pick", "--require-match", "-p", "Displays", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    bool waited = pid > 0 && waitpid(pid, &status, 0) == pid;
    fclose(input);
    rewind(output);
    char choice[NAME + 32];
    bool selected = fgets(choice, sizeof choice, output) != NULL;
    fclose(output);
    if (!waited || !WIFEXITED(status) || WEXITSTATUS(status) == 127)
        return fail("Launch is unavailable on PATH.");
    if (WEXITSTATUS(status) || !selected) return 0;
    choice[strcspn(choice, "\r\n")] = 0;
    if (!strcmp(choice, "Open display editor")) return editor_main();
    enum Command action = !strcmp(choice, "Identify displays") ? IDENTIFY :
        (!strcmp(choice, "Keep display changes") ? KEEP :
         (!strcmp(choice, "Revert display changes") ? REVERT : 0));
    Request request = {.version = REQUEST_VERSION, .command = (uint32_t)action};
    if (!action && !strncmp(choice, "Switch to ", 10)) {
        Preset preset;
        if (!read_named(&config, choice + 10, &preset, error)) return fail(error);
        request.command = APPLY;
        request.layout = preset.layout;
        strcpy(request.preset, preset.name);
    }
    if (!request.command) return 0;
    if (!rpc(&request, &reply, true)) {
        /* The workflow may have been launched without a terminal. */
        if (request.command == APPLY) return editor_main();
        return fail(reply.message);
    }
    return request.command == APPLY && reply.remaining ? editor_main() : 0;
}

int
main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
    if (argc == 1) return editor_main();
    const char *command = argv[1];
    if (!strcmp(command, "--help") || !strcmp(command, "help")) { fputs(usage, stdout); return 0; }
    if (!strcmp(command, "--version")) { puts("undocked " UNDOCKED_VERSION); return 0; }
    if (!strcmp(command, "daemon")) {
        if (argc == 2) return daemon_main(false, true);
        if (argc == 3 && !strcmp(argv[2], "--tray")) return daemon_main(true, true);
        if (argc == 3 && !strcmp(argv[2], "--on-demand")) return daemon_main(false, false);
        return fail("Use undocked daemon [--tray].");
    }
    if (!strcmp(command, "launch") && argc == 2) return launch_main();
    if (!strcmp(command, "output")) return set_output(argc, argv);

    Config config;
    char error[ERROR_SIZE];
    if (!config_load(&config, error)) return fail(error);
    bool json = argc == 3 && !strcmp(argv[2], "--json");
    if (!strcmp(command, "presets") && (argc == 2 || json)) return print_presets(&config, json);
    Reply reply;
    if (!strcmp(command, "status") && (argc == 2 || json)) {
        if (!get(&reply)) return fail(reply.message);
        print_status(&reply, &config, json);
        return 0;
    }
    if (!strcmp(command, "save") && argc >= 3 && argc <= 5) {
        Preset preset = {0};
        bool replace = false;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--auto")) preset.automatic = true;
            else if (!strcmp(argv[i], "--replace")) replace = true;
            else return fail("Use undocked save NAME [--auto] [--replace].");
        }
        if (!preset_name(argv[2])) return fail("Invalid preset name.");
        if (!get(&reply)) return fail(reply.message);
        if (reply.remaining) return fail("Keep the pending layout before saving it.");
        preset.layout = current_layout(&reply.topology);
        if (!normalize_layout(&reply.topology, &preset.layout, error)) return fail(error);
        strcpy(preset.name, argv[2]);
        return preset_write(&config, &preset, replace, error) ? 0 : fail(error);
    }
    if (!strcmp(command, "auto") && argc == 4) {
        Preset preset;
        if (strcmp(argv[3], "on") && strcmp(argv[3], "off")) return fail("Use undocked auto NAME on|off.");
        if (!read_named(&config, argv[2], &preset, error)) return fail(error);
        preset.automatic = !strcmp(argv[3], "on");
        return preset_write(&config, &preset, true, error) ? 0 : fail(error);
    }
    if (argc == 3 || argc == 4) {
        bool dry = false, yes = false;
        if (argc == 3 || apply_option(argv[3], &dry, &yes)) {
            if (!strcmp(command, "layout")) {
                if (!get(&reply)) return fail(reply.message);
                Layout layout = current_layout(&reply.topology);
                if (!suggest(&reply.topology, &layout, argv[2], error)) return fail(error);
                return submit(layout, topology_key(&reply.topology), NULL, dry, yes);
            }
            bool path = !strcmp(command, "load");
            if (path || !strcmp(command, "preset")) {
                Preset preset;
                if (!(path ? preset_read(argv[2], &preset, error) :
                          read_named(&config, argv[2], &preset, error))) return fail(error);
                return submit(preset.layout, 0, preset.name, dry, yes);
            }
        }
    }
    const struct {const char *name; enum Command command;} actions[] = {
        {"identify", IDENTIFY}, {"keep", KEEP}, {"revert", REVERT}, {"recover", RECOVER}, {"reload", RELOAD}
    };
    for (size_t i = 0; argc == 2 && i < sizeof actions / sizeof actions[0]; i++)
        if (!strcmp(command, actions[i].name))
            return rpc(&(Request){.version = REQUEST_VERSION, .command = actions[i].command},
                       &reply, true) ? 0 : fail(reply.message);
    fputs(usage, stderr);
    return 2;
}
