#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "elevator.h"

#define LINE_LEN   200
#define FEED_LINES  10
#define SCREEN_BUF 16384

// Output has its own mutex, separate from the one around the model:
// writing to a terminal is a system call, and doing it under the model
// mutex would stop every thread for its duration.
static int log_fd = -1;
static int plain_mode;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
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

void out_event(const char *fmt, ...)
{
    char line[LINE_LEN];
    va_list ap;
    int n;

    n = snprintf(line, sizeof(line), "[%6ld ms] ", now_ms());
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > LINE_LEN - 2) n = LINE_LEN - 2;
    line[n++] = '\n';

    pthread_mutex_lock(&out_lock);
    if (log_fd >= 0) write(log_fd, line, (size_t)n);
    if (plain_mode) {
        write(1, line, (size_t)n);
    } else {
        if (feed_n == FEED_LINES) {
            memmove(feed, feed + 1, sizeof(feed) - sizeof(feed[0]));
            memmove(feed_len, feed_len + 1, sizeof(feed_len) - sizeof(feed_len[0]));
            feed_n--;
        }
        memcpy(feed[feed_n], line, (size_t)n);
        feed_len[feed_n++] = n;
    }
    pthread_mutex_unlock(&out_lock);
}

static char dir_char(Direction d)
{
    return d == DIR_UP ? '^' : d == DIR_DOWN ? 'v' : '-';
}

// Draws the building: floors top down, one column per shaft. The frame
// is built in memory while the mutex is held - that is a consistent
// snapshot - and written out only after it has been released.
void out_draw(void)
{
    char buf[SCREEN_BUF];
    int n = 0;

    if (plain_mode) return;

    pthread_mutex_lock(&sim.lock);
    n += snprintf(buf + n, SCREEN_BUF - n, "\033[H\033[2J");
    n += snprintf(buf + n, SCREEN_BUF - n,
                  "%ld ms   delivered %d/%d   %s   threads %d%s\n\n",
                  now_ms(), sim.delivered, sim.cfg.passengers,
                  sim.cfg.strategy == STRAT_FCFS ? "fcfs" : "nearest",
                  sim.cfg.elevators + sim.n_passengers - sim.delivered + 3,
                  sim.draining ? "   DRAINING" : "");
    n += snprintf(buf + n, SCREEN_BUF - n, "  fl");
    for (int e = 0; e < sim.cfg.elevators; e++)
        n += snprintf(buf + n, SCREEN_BUF - n, "   E%-3d", e + 1);
    n += snprintf(buf + n, SCREEN_BUF - n, "  hall\n");

    for (int f = sim.cfg.floors; f >= 1; f--) {
        int up = 0, down = 0;

        n += snprintf(buf + n, SCREEN_BUF - n, "  %2d", f);
        for (int e = 0; e < sim.cfg.elevators; e++) {
            const Elevator *el = &sim.elevators[e];
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
        for (int i = 0; i < sim.n_passengers; i++) {
            const Passenger *p = &sim.passengers[i];
            if (p->state != PS_WAITING || p->from != f) continue;
            if (p->dir == DIR_UP) up++; else down++;
        }
        if (up) n += snprintf(buf + n, SCREEN_BUF - n, " %d^", up);
        if (down) n += snprintf(buf + n, SCREEN_BUF - n, " %dv", down);
        n += snprintf(buf + n, SCREEN_BUF - n, "\n");
    }
    n += snprintf(buf + n, SCREEN_BUF - n, "\n");
    pthread_mutex_unlock(&sim.lock);

    pthread_mutex_lock(&out_lock);
    write(1, buf, (size_t)n);
    for (int i = 0; i < feed_n; i++) write(1, feed[i], (size_t)feed_len[i]);
    pthread_mutex_unlock(&out_lock);
}

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

void out_stats(const char *reason)
{
    int left = 0;

    pthread_mutex_lock(&sim.lock);
    emit("\n--- simulation finished: %s ---\n", reason);
    emit("real time             %ld ms\n", now_ms());
    emit("delivered             %d of %d\n", sim.delivered, sim.cfg.passengers);
    emit("passengers appeared   %d\n", sim.n_passengers);
    if (sim.boarded_count > 0) {
        long w = sim.total_wait * 10 / sim.boarded_count;
        emit("average waiting       %ld.%ld ms\n", w / 10, w % 10);
        emit("longest waiting       %ld ms\n", sim.max_wait);
    }
    if (sim.delivered > 0) {
        long r = sim.total_ride * 10 / sim.delivered;
        emit("average ride          %ld.%ld ms\n", r / 10, r % 10);
    }
    emit("full cabin refusals   %d\n", sim.refused_boardings);
    emit("invariants broken     %d\n", sim.violations);
    emit("synchronization       %s\n", sync_name());
    for (int i = 0; i < sim.cfg.elevators; i++)
        emit("elevator %d            %d floors passed, %d on board at the end\n",
             sim.elevators[i].id, sim.elevators[i].floors_passed,
             sim.elevators[i].n_riders);

    for (int i = 0; i < sim.n_passengers; i++) {
        const Passenger *p = &sim.passengers[i];
        if (p->state == PS_DONE) continue;
        if (!left++) emit("\nrequests still open:\n");
        if (p->state == PS_WAITING)
            emit("  passenger %d waits on floor %d for floor %d since %ld ms\n",
                 p->id, p->from, p->to, p->appeared_ms);
        else
            emit("  passenger %d rides elevator %d towards floor %d\n",
                 p->id, p->elevator, p->to);
    }
    if (sim.cfg.passengers > sim.n_passengers) {
        if (!left) emit("\nrequests still open:\n");
        emit("  %d passenger(s) never appeared\n",
             sim.cfg.passengers - sim.n_passengers);
    }
    pthread_mutex_unlock(&sim.lock);
}
