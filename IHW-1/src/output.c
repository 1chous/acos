#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "elevator.h"

#define LINE_LEN   160
#define FEED_LINES  10
#define SCREEN_BUF 16384

// Everything is written through plain file descriptors, no stdio
// streams. The last FEED_LINES events are kept to be shown under the
// building when the screen is redrawn.
static int log_fd = -1;
static int plain_mode;
static char feed[FEED_LINES][LINE_LEN];
static int feed_len[FEED_LINES];
static int feed_n;

int out_open(const char *log_path, int plain)
{
    plain_mode = plain;
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
        dprintf(2, "cannot create the journal %s\n", log_path);
        return -1;
    }
    if (!plain_mode) write(1, "\033[?25l", 6);
    return 0;
}

int out_log_fd(void) { return log_fd; }

void out_close(void)
{
    if (!plain_mode) write(1, "\033[?25h", 6);
    if (log_fd >= 0) { close(log_fd); log_fd = -1; }
}

// one line about one event: always into the journal, and either
// straight to the screen or into the feed under the building
void out_event(int tick, const char *fmt, ...)
{
    char line[LINE_LEN];
    va_list ap;
    int n;

    n = snprintf(line, sizeof(line), "[%4d] ", tick);
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > LINE_LEN - 2) n = LINE_LEN - 2;
    line[n++] = '\n';

    if (log_fd >= 0) write(log_fd, line, (size_t)n);
    if (plain_mode) { write(1, line, (size_t)n); return; }

    if (feed_n == FEED_LINES) {
        memmove(feed, feed + 1, sizeof(feed) - sizeof(feed[0]));
        memmove(feed_len, feed_len + 1, sizeof(feed_len) - sizeof(feed_len[0]));
        feed_n--;
    }
    memcpy(feed[feed_n], line, (size_t)n);
    feed_len[feed_n++] = n;
}

static char dir_char(Direction d)
{
    return d == DIR_UP ? '^' : d == DIR_DOWN ? 'v' : '-';
}

// Draws the building: floors from the top down, one column per shaft.
// A cabin is [d n] with closed doors and (d n) with open ones, where d
// is the direction and n the number of people inside. The right column
// counts who is waiting on the floor and which way they want to go.
void out_draw(const Sim *s)
{
    const Config *cfg = &s->cfg;
    char buf[SCREEN_BUF];
    int n = 0;

    if (plain_mode) return;

    n += snprintf(buf + n, SCREEN_BUF - n, "\033[H\033[2J");
    n += snprintf(buf + n, SCREEN_BUF - n,
                  "tick %-5d delivered %d/%d   strategy %s   in the building %d\n\n",
                  s->tick, s->delivered, sim_total(s),
                  cfg->strategy == STRAT_FCFS ? "fcfs" : "nearest",
                  s->n_passengers - s->delivered);
    n += snprintf(buf + n, SCREEN_BUF - n, "  fl");
    for (int e = 0; e < cfg->elevators; e++)
        n += snprintf(buf + n, SCREEN_BUF - n, "   E%-3d", e + 1);
    n += snprintf(buf + n, SCREEN_BUF - n, "  hall\n");

    for (int f = cfg->floors; f >= 1; f--) {
        int up = 0, down = 0;

        n += snprintf(buf + n, SCREEN_BUF - n, "  %2d", f);
        for (int e = 0; e < cfg->elevators; e++) {
            const Elevator *el = &s->elevators[e];
            if (el->floor != f)
                n += snprintf(buf + n, SCREEN_BUF - n, "    .  ");
            else if (el->doors)
                n += snprintf(buf + n, SCREEN_BUF - n, "  \033[1;32m(%c%d)\033[0m ",
                              dir_char(el->dir), el->n_riders);
            else
                n += snprintf(buf + n, SCREEN_BUF - n, "  \033[%sm[%c%d]\033[0m ",
                              el->state == ST_MOVING ? "1;33" : "1;37",
                              dir_char(el->dir), el->n_riders);
        }
        for (int i = 0; i < s->n_passengers; i++) {
            const Passenger *p = &s->passengers[i];
            if (p->state != PS_WAITING || p->from != f) continue;
            if (p->dir == DIR_UP) up++; else down++;
        }
        if (up) n += snprintf(buf + n, SCREEN_BUF - n, " %d^", up);
        if (down) n += snprintf(buf + n, SCREEN_BUF - n, " %dv", down);
        n += snprintf(buf + n, SCREEN_BUF - n, "\n");
    }
    n += snprintf(buf + n, SCREEN_BUF - n, "\n");

    write(1, buf, (size_t)n);
    for (int i = 0; i < feed_n; i++) write(1, feed[i], (size_t)feed_len[i]);
}

// same as out_event but without the tick, used by the summary
static void emit(const char *fmt, ...)
{
    char line[LINE_LEN];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > LINE_LEN - 1) n = LINE_LEN - 1;
    write(1, line, (size_t)n);
    if (log_fd >= 0) write(log_fd, line, (size_t)n);
}

// Final summary. The task asks to report the state of the requests
// that were not finished, so the open ones are listed at the end.
void out_stats(const Sim *s, const char *reason)
{
    int total = sim_total(s), left = 0;

    emit("\n--- simulation finished: %s ---\n", reason);
    emit("model time            %d ticks\n", s->tick);
    emit("delivered             %d of %d\n", s->delivered, total);
    if (s->delivered > 0) {
        int w = s->total_wait * 10 / s->delivered;
        int r = s->total_ride * 10 / s->delivered;
        emit("average waiting       %d.%d ticks\n", w / 10, w % 10);
        emit("longest waiting       %d ticks\n", s->max_wait);
        emit("average ride          %d.%d ticks\n", r / 10, r % 10);
    }
    emit("full cabin refusals   %d\n", s->refused_boardings);
    emit("invariants broken     %d\n", s->violations);
    for (int i = 0; i < s->cfg.elevators; i++)
        emit("elevator %d            %d floors passed, %d on board at the end\n",
             s->elevators[i].id, s->elevators[i].floors_passed, s->elevators[i].n_riders);

    for (int i = 0; i < s->n_passengers; i++) {
        const Passenger *p = &s->passengers[i];
        if (p->state == PS_DONE) continue;
        if (!left++) emit("\nrequests still open:\n");
        if (p->state == PS_WAITING)
            emit("  passenger %d waits on floor %d for floor %d since tick %d\n",
                 p->id, p->from, p->to, p->appeared_tick);
        else
            emit("  passenger %d rides elevator %d towards floor %d\n",
                 p->id, p->elevator, p->to);
    }
    if (total > s->n_passengers) {
        if (!left) emit("\nrequests still open:\n");
        emit("  %d passenger(s) never appeared\n", total - s->n_passengers);
    }
}
