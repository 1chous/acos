#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elevator.h"

#define FILE_BUF 4096

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->floors = 12;
    c->elevators = 3;
    c->capacity = 6;
    c->passengers = 25;
    c->arrival_min_ms = 500;
    c->arrival_max_ms = 2500;
    c->travel_ms = 400;
    c->door_ms = 400;
    c->board_ms = 250;
    c->alight_ms = 250;
    c->strategy = STRAT_NEAREST;
    c->refresh_ms = 100;
    strcpy(c->log_path, "elevator.log");
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *e = '\0';
    return s;
}

static int to_int(const char *s, int *out)
{
    char *end;
    long v;
    if (!*s) return -1;
    v = strtol(s, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return -1;
    *out = (int)v;
    return 0;
}

static int to_list(const char *s, int *dst, int max, int *count)
{
    char *end;
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        if (n >= max) return -1;
        dst[n] = (int)strtol(s, &end, 10);
        if (end == s) return -1;
        n++;
        s = end;
    }
    *count = n;
    return 0;
}

// Sets one option by name. The config file and the command line both go
// through here, so they always understand the same set of keys. In a file
// the keys use '_', on the command line '-'; both spellings work.
static int set_option(Config *c, const char *raw, const char *val)
{
    char key[24];
    size_t i;

    for (i = 0; raw[i] && i < sizeof(key) - 1; i++) key[i] = (raw[i] == '-') ? '_' : raw[i];
    key[i] = '\0';

    if (!strcmp(key, "floors"))         return to_int(val, &c->floors);
    if (!strcmp(key, "elevators"))      return to_int(val, &c->elevators);
    if (!strcmp(key, "capacity"))       return to_int(val, &c->capacity);
    if (!strcmp(key, "passengers"))     return to_int(val, &c->passengers);
    if (!strcmp(key, "arrival_min_ms")) return to_int(val, &c->arrival_min_ms);
    if (!strcmp(key, "arrival_max_ms")) return to_int(val, &c->arrival_max_ms);
    if (!strcmp(key, "travel_ms"))      return to_int(val, &c->travel_ms);
    if (!strcmp(key, "door_ms"))        return to_int(val, &c->door_ms);
    if (!strcmp(key, "board_ms"))       return to_int(val, &c->board_ms);
    if (!strcmp(key, "alight_ms"))      return to_int(val, &c->alight_ms);
    if (!strcmp(key, "max_seconds"))    return to_int(val, &c->max_seconds);
    if (!strcmp(key, "refresh_ms"))     return to_int(val, &c->refresh_ms);
    if (!strcmp(key, "plain"))          return to_int(val, &c->plain);
    if (!strcmp(key, "start_floors"))
        return to_list(val, c->start_floor, MAX_ELEVATORS, &c->n_start);
    if (!strcmp(key, "seed")) {
        int v;
        if (to_int(val, &v) < 0 || v < 0) return -1;
        c->seed = (unsigned)v;
        return 0;
    }
    if (!strcmp(key, "strategy")) {
        if (!strcmp(val, "fcfs")) { c->strategy = STRAT_FCFS; return 0; }
        if (!strcmp(val, "nearest")) { c->strategy = STRAT_NEAREST; return 0; }
        return -1;
    }
    if (!strcmp(key, "log")) {
        if (strlen(val) >= sizeof(c->log_path)) return -1;
        strcpy(c->log_path, val);
        return 0;
    }
    return -1;
}

static int parse_line(Config *c, char *line)
{
    char *hash = strchr(line, '#'), *eq;
    if (hash) *hash = '\0';
    line = trim(line);
    if (!*line) return 0;
    eq = strchr(line, '=');
    if (!eq) return -1;
    *eq = '\0';
    return set_option(c, trim(line), trim(eq + 1));
}

int config_load_file(Config *c, const char *path)
{
    char buf[FILE_BUF], *line;
    ssize_t total = 0, n;
    int fd, line_no = 0, bad = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "config: cannot open %s\n", path);
        return -1;
    }
    while ((n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) total += n;
    close(fd);
    if (n < 0 || total == sizeof(buf) - 1) {
        dprintf(2, "config: cannot read %s, or it is too big\n", path);
        return -1;
    }
    buf[total] = '\0';
    for (line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        line_no++;
        if (parse_line(c, line) < 0) {
            dprintf(2, "%s:%d: bad line\n", path, line_no);
            bad++;
        }
        line = nl ? nl + 1 : NULL;
    }
    return bad ? -1 : 0;
}

const char *config_arg_file(int argc, char **argv)
{
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "--config")) return argv[i + 1];
    return NULL;
}

int config_parse_args(Config *c, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            config_usage(argv[0], 1);
            return 1;
        }
        if (!strcmp(a, "--plain")) { c->plain = 1; continue; }
        if (!strcmp(a, "--config")) { i++; continue; }
        if (strncmp(a, "--", 2)) {
            dprintf(2, "unexpected argument: %s\n", a);
            return -1;
        }
        if (i + 1 >= argc) {
            dprintf(2, "option %s needs a value\n", a);
            return -1;
        }
        if (set_option(c, a + 2, argv[i + 1]) < 0) {
            dprintf(2, "bad option or value: %s %s\n", a, argv[i + 1]);
            return -1;
        }
        i++;
    }
    return 0;
}

#define NEED(cond, ...) \
    do { if (!(cond)) { dprintf(2, "config: " __VA_ARGS__); return -1; } } while (0)

// Checked before any thread starts: a bad value must stop the program,
// not crash it later.
int config_check(Config *c)
{
    NEED(c->floors >= 2 && c->floors <= MAX_FLOORS, "floors must be 2..%d\n", MAX_FLOORS);
    NEED(c->elevators >= 1 && c->elevators <= MAX_ELEVATORS, "elevators must be 1..%d\n", MAX_ELEVATORS);
    NEED(c->capacity >= 1 && c->capacity <= MAX_CAPACITY, "capacity must be 1..%d\n", MAX_CAPACITY);
    NEED(c->passengers >= 0 && c->passengers <= MAX_PASSENGERS, "passengers must be 0..%d\n", MAX_PASSENGERS);
    NEED(c->arrival_min_ms >= 0 && c->arrival_max_ms >= c->arrival_min_ms,
         "need 0 <= arrival_min_ms <= arrival_max_ms\n");
    NEED(c->travel_ms >= 1 && c->door_ms >= 1, "travel_ms and door_ms must be at least 1\n");
    NEED(c->board_ms >= 1 && c->alight_ms >= 1, "board_ms and alight_ms must be at least 1\n");
    NEED(c->max_seconds >= 0 && c->refresh_ms >= 1, "max_seconds >= 0 and refresh_ms >= 1\n");

    if (c->n_start == 0) {
        for (int i = 0; i < c->elevators; i++) c->start_floor[i] = 1;
        c->n_start = c->elevators;
    }
    NEED(c->n_start == c->elevators, "start_floors must list exactly %d values\n", c->elevators);
    for (int i = 0; i < c->elevators; i++)
        NEED(c->start_floor[i] >= 1 && c->start_floor[i] <= c->floors,
             "start floor of elevator %d is outside the building\n", i + 1);
    return 0;
}

void config_dump(const Config *c, int fd)
{
    dprintf(fd, "building: %d floors, %d elevators, capacity %d, start at",
            c->floors, c->elevators, c->capacity);
    for (int i = 0; i < c->elevators; i++) dprintf(fd, " %d", c->start_floor[i]);
    dprintf(fd, "\npassengers: %d, one every %d..%d ms\n", c->passengers,
            c->arrival_min_ms, c->arrival_max_ms);
    dprintf(fd, "timings ms: travel %d, doors %d, board %d, alight %d\n",
            c->travel_ms, c->door_ms, c->board_ms, c->alight_ms);
    dprintf(fd, "run: %s, seed %u, max_seconds %d, refresh %d ms, log %s, via %s\n\n",
            c->strategy == STRAT_FCFS ? "fcfs" : "nearest", c->seed,
            c->max_seconds, c->refresh_ms, c->log_path, sync_name());
}

void config_usage(const char *prog, int fd)
{
    dprintf(fd,
"Elevator group simulation, multithreaded.\nUsage: %s [--config FILE] [options]\n\n"
"  --floors N --elevators N --capacity N --start-floors \"1 5 9\"\n"
"  --passengers N --arrival-min-ms N --arrival-max-ms N\n"
"  --travel-ms N --door-ms N --board-ms N --alight-ms N\n"
"  --strategy fcfs|nearest --seed N --max-seconds N --refresh-ms N\n"
"  --plain --log FILE\n\n"
"Limits: floors 2..%d, elevators 1..%d, capacity 1..%d. Arguments win over the file.\n",
            prog, MAX_FLOORS, MAX_ELEVATORS, MAX_CAPACITY);
}
