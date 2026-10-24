#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "elevator.h"

Sim sim;

static long base_ms;
static unsigned rng_state;
static pthread_t dispatcher_tid, generator_tid;

long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L - base_ms;
}

void sleep_ms(int ms)
{
    struct timespec ts;

    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int rnd(int lo, int hi)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return lo + (int)((rng_state >> 16) % (unsigned)(hi - lo + 1));
}

static const char *dir_name(Direction d)
{
    return d == DIR_UP ? "up" : d == DIR_DOWN ? "down" : "nowhere";
}

#define BROKEN(...) \
    do { sim.violations++; out_event("INVARIANT BROKEN: " __VA_ARGS__); } while (0)

static Call *find_call(int floor, Direction dir)
{
    for (int i = 0; i < MAX_CALLS; i++)
        if (sim.calls[i].active && sim.calls[i].floor == floor &&
            sim.calls[i].dir == dir)
            return &sim.calls[i];
    return NULL;
}

static void add_call(int floor, Direction dir)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        Call *c = &sim.calls[i];
        if (c->active) continue;
        c->floor = floor;
        c->dir = dir;
        c->elevator = -1;
        c->active = 1;
        sig_post(&sim.new_call);
        return;
    }
}

static int work_in_direction(const Elevator *e, Direction dir)
{
    if (dir == DIR_NONE) return 0;
    for (int i = 0; i < e->n_riders; i++)
        if ((sim.passengers[e->riders[i] - 1].to - e->floor) * dir > 0) return 1;
    for (int i = 0; i < MAX_CALLS; i++) {
        const Call *c = &sim.calls[i];
        if (c->active && c->elevator == e->id && (c->floor - e->floor) * dir > 0) return 1;
    }
    return 0;
}

static void pick_direction(Elevator *e)
{
    if (work_in_direction(e, e->dir)) return;
    if (work_in_direction(e, DIR_UP)) e->dir = DIR_UP;
    else if (work_in_direction(e, DIR_DOWN))
        e->dir = DIR_DOWN;
    else e->dir = DIR_NONE;
}

static int should_stop(const Elevator *e)
{
    for (int i = 0; i < e->n_riders; i++)
        if (sim.passengers[e->riders[i] - 1].to == e->floor) return 1;
    for (int i = 0; i < MAX_CALLS; i++) {
        const Call *c = &sim.calls[i];
        if (!c->active || c->floor != e->floor) continue;
        if (c->dir == e->dir) {
            if (c->elevator == e->id || e->n_riders < sim.cfg.capacity) return 1;
        } else if (c->elevator == e->id && !work_in_direction(e, e->dir)) {
            return 1;
        }
    }
    return 0;
}

static void turn_if_needed(Elevator *e)
{
    if (work_in_direction(e, e->dir)) return;
    for (int i = 0; i < MAX_CALLS; i++) {
        const Call *c = &sim.calls[i];
        if (c->active && c->floor == e->floor &&
            (c->elevator == e->id || c->elevator == -1)) {
            e->dir = c->dir;
            return;
        }
    }
}

static int assigned_calls(int id)
{
    int n = 0;

    for (int i = 0; i < MAX_CALLS; i++)
        if (sim.calls[i].active && sim.calls[i].elevator == id) n++;
    return n;
}

static int pick_elevator(const Call *call)
{
    int best = -1, best_cost = 0;

    for (int i = 0; i < sim.cfg.elevators; i++) {
        Elevator *e = &sim.elevators[i];
        int cost;

        if (e->n_riders >= sim.cfg.capacity) continue;
        if (sim.cfg.strategy == STRAT_FCFS) {
            cost = e->n_riders + assigned_calls(e->id);
            if (cost == 0) return e->id;
        } else {
            cost = abs(e->floor - call->floor);
            if (e->dir != DIR_NONE && (call->floor - e->floor) * e->dir < 0)
                cost += sim.cfg.floors;
            cost += assigned_calls(e->id) * sim.cfg.floors;
        }
        if (best < 0 || cost < best_cost) {
            best = e->id;
            best_cost = cost;
        }
    }
    return best;
}

static int take_out(Elevator *e, int *ids)
{
    int n = 0, kept = 0;

    for (int i = 0; i < e->n_riders; i++) {
        Passenger *p = &sim.passengers[e->riders[i] - 1];
        if (p->to != e->floor) {
            e->riders[kept++] = e->riders[i];
            continue;
        }
        if (!e->doors)
            BROKEN("passenger %d leaves elevator %d through closed doors", p->id, e->id);
        p->state = PS_DONE;
        p->elevator = -1;
        p->done_ms = now_ms();
        sim.delivered++;
        sim.total_ride += p->done_ms - p->boarded_ms;
        ids[n++] = p->id;
    }
    e->n_riders = kept;
    return n;
}

static int take_in(Elevator *e, int *ids, int *left)
{
    int n = 0;

    *left = 0;
    for (int i = 0; i < sim.n_passengers; i++) {
        Passenger *p = &sim.passengers[i];
        long waited;

        if (p->state != PS_WAITING || p->from != e->floor || p->dir != e->dir) continue;
        if (e->n_riders >= sim.cfg.capacity) {
            (*left)++;
            continue;
        }
        if (!e->doors)
            BROKEN("passenger %d enters elevator %d through closed doors", p->id, e->id);
        p->state = PS_RIDING;
        p->elevator = e->id;
        p->boarded_ms = now_ms();
        e->riders[e->n_riders++] = p->id;
        waited = p->boarded_ms - p->appeared_ms;
        sim.total_wait += waited;
        if (waited > sim.max_wait) sim.max_wait = waited;
        ids[n++] = p->id;
    }
    for (int i = 0; i < MAX_CALLS; i++) {
        Call *c = &sim.calls[i];
        if (c->active && c->floor == e->floor && c->dir == e->dir) c->active = 0;
    }
    if (*left > 0) {
        sim.refused_boardings++;
        add_call(e->floor, e->dir);
    }
    return n;
}

static void serve_floor(Elevator *e)
{
    int out_ids[MAX_CAPACITY], in_ids[MAX_PASSENGERS];
    int n_out, n_in, left, floor;

    pthread_mutex_lock(&sim.lock);
    turn_if_needed(e);
    e->state = ST_OPENING;
    floor = e->floor;
    pthread_mutex_unlock(&sim.lock);

    out_event("elevator %d opens the doors on floor %d", e->id, floor);
    sleep_ms(sim.cfg.door_ms);

    pthread_mutex_lock(&sim.lock);
    e->doors = 1;
    e->state = ST_UNLOADING;
    n_out = take_out(e, out_ids);
    pthread_mutex_unlock(&sim.lock);

    for (int i = 0; i < n_out; i++) {
        out_event("passenger %d leaves elevator %d on floor %d, delivered",
                  out_ids[i], e->id, floor);
        sig_post(&sim.passengers[out_ids[i] - 1].arrived);
    }
    if (n_out > 0) sig_post(&sim.new_call);
    sleep_ms(n_out * sim.cfg.alight_ms);

    pthread_mutex_lock(&sim.lock);
    e->state = ST_LOADING;
    n_in = take_in(e, in_ids, &left);
    pthread_mutex_unlock(&sim.lock);

    for (int i = 0; i < n_in; i++) {
        out_event("passenger %d enters elevator %d", in_ids[i], e->id);
        sig_post(&sim.passengers[in_ids[i] - 1].boarded);
    }
    if (left > 0)
        out_event("elevator %d is full, %d passenger(s) stay on floor %d",
                  e->id, left, floor);
    sleep_ms(n_in * sim.cfg.board_ms);

    pthread_mutex_lock(&sim.lock);
    e->state = ST_CLOSING;
    pthread_mutex_unlock(&sim.lock);

    out_event("elevator %d closes the doors on floor %d", e->id, floor);
    sleep_ms(sim.cfg.door_ms);

    pthread_mutex_lock(&sim.lock);
    e->doors = 0;
    e->state = ST_IDLE;
    pthread_mutex_unlock(&sim.lock);
}

static void *elevator_thread(void *arg)
{
    Elevator *e = arg;

    while (sim_running()) {
        int stop_here, floor;
        Direction dir;

        pthread_mutex_lock(&sim.lock);
        stop_here = should_stop(e);
        if (!stop_here) pick_direction(e);
        dir = e->dir;
        pthread_mutex_unlock(&sim.lock);

        if (stop_here) {
            serve_floor(e);
            continue;
        }
        if (dir == DIR_NONE) {
            sig_wait(&e->work);
            continue;
        }

        pthread_mutex_lock(&sim.lock);
        e->state = ST_MOVING;
        pthread_mutex_unlock(&sim.lock);

        sleep_ms(sim.cfg.travel_ms);
        if (!sim_running()) break;

        pthread_mutex_lock(&sim.lock);
        e->floor += dir;
        e->floors_passed++;
        floor = e->floor;
        pthread_mutex_unlock(&sim.lock);

        out_event("elevator %d reaches floor %d", e->id, floor);
    }
    return NULL;
}

static void *dispatcher_thread(void *arg)
{
    (void)arg;

    while (sim_running()) {
        int floor[MAX_CALLS], id[MAX_CALLS];
        Direction dir[MAX_CALLS];
        int n = 0;

        sig_wait(&sim.new_call);
        if (!sim_running()) break;

        pthread_mutex_lock(&sim.lock);
        for (int i = 0; i < MAX_CALLS; i++) {
            Call *c = &sim.calls[i];
            int chosen;

            if (!c->active || c->elevator != -1) continue;
            chosen = pick_elevator(c);
            if (chosen < 0) continue;
            c->elevator = chosen;
            floor[n] = c->floor;
            dir[n] = c->dir;
            id[n] = chosen;
            n++;
        }
        pthread_mutex_unlock(&sim.lock);

        for (int i = 0; i < n; i++) {
            out_event("dispatcher sends elevator %d to the call on floor %d (%s)",
                      id[i], floor[i], dir_name(dir[i]));
            sig_post(&sim.elevators[id[i] - 1].work);
        }
    }
    return NULL;
}

static void *passenger_thread(void *arg)
{
    Passenger *p = arg;

    sig_wait(&p->boarded);
    if (!sim_running()) return NULL;
    sig_wait(&p->arrived);
    return NULL;
}

static void *generator_thread(void *arg)
{
    (void)arg;

    for (int i = 0; i < sim.cfg.passengers && sim_running(); i++) {
        Passenger *p;
        int gap, to;

        pthread_mutex_lock(&sim.lock);
        p = &sim.passengers[sim.n_passengers];
        p->id = sim.n_passengers + 1;
        p->from = rnd(1, sim.cfg.floors);
        do { to = rnd(1, sim.cfg.floors); } while (to == p->from);
        p->to = to;
        p->dir = (to > p->from) ? DIR_UP : DIR_DOWN;
        p->elevator = -1;
        p->state = PS_WAITING;
        p->appeared_ms = now_ms();
        sig_init(&p->boarded);
        sig_init(&p->arrived);
        gap = rnd(sim.cfg.arrival_min_ms, sim.cfg.arrival_max_ms);
        sim.n_passengers++;
        if (!find_call(p->from, p->dir)) add_call(p->from, p->dir);
        pthread_mutex_unlock(&sim.lock);

        out_event("passenger %d appears on floor %d and wants floor %d (%s)",
                  p->id, p->from, p->to, dir_name(p->dir));
        pthread_create(&p->tid, NULL, passenger_thread, p);
        sleep_ms(gap);
    }
    return NULL;
}

void sim_check(void)
{
    pthread_mutex_lock(&sim.lock);

    for (int i = 0; i < sim.cfg.elevators; i++) {
        const Elevator *e = &sim.elevators[i];
        if (e->n_riders > sim.cfg.capacity)
            BROKEN("elevator %d carries %d, capacity is %d", e->id, e->n_riders,
                   sim.cfg.capacity);
        if (e->state == ST_MOVING && e->doors)
            BROKEN("elevator %d moves with open doors", e->id);
        if (e->floor < 1 || e->floor > sim.cfg.floors)
            BROKEN("elevator %d is on floor %d, outside the building", e->id, e->floor);
    }

    for (int i = 0; i < sim.n_passengers; i++) {
        const Passenger *p = &sim.passengers[i];
        int seen = 0, where = -1;

        for (int j = 0; j < sim.cfg.elevators; j++)
            for (int k = 0; k < sim.elevators[j].n_riders; k++)
                if (sim.elevators[j].riders[k] == p->id) {
                    seen++;
                    where = sim.elevators[j].id;
                }
        if (p->state == PS_RIDING && seen != 1)
            BROKEN("passenger %d rides but is found in %d cabins", p->id, seen);
        if (p->state != PS_RIDING && seen != 0)
            BROKEN("passenger %d does not ride but sits in a cabin", p->id);
        if (p->state == PS_RIDING && where != p->elevator)
            BROKEN("passenger %d thinks he is in elevator %d, he is in %d",
                   p->id, p->elevator, where);
    }

    for (int i = 0; i < MAX_CALLS; i++) {
        if (!sim.calls[i].active) continue;
        for (int j = i + 1; j < MAX_CALLS; j++)
            if (sim.calls[j].active && sim.calls[j].floor == sim.calls[i].floor &&
                sim.calls[j].dir == sim.calls[i].dir)
                BROKEN("two active calls on floor %d going %s", sim.calls[i].floor,
                       dir_name(sim.calls[i].dir));
    }
    pthread_mutex_unlock(&sim.lock);
}

void sim_init(const Config *cfg)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    base_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;

    memset(&sim, 0, sizeof(sim));
    sim.cfg = *cfg;
    sim.running = 1;
    pthread_mutex_init(&sim.lock, NULL);
    sig_init(&sim.new_call);
    rng_state = cfg->seed ? cfg->seed : (unsigned)time(NULL);

    for (int i = 0; i < cfg->elevators; i++) {
        Elevator *e = &sim.elevators[i];
        e->id = i + 1;
        e->floor = cfg->start_floor[i];
        e->dir = DIR_NONE;
        e->state = ST_IDLE;
        sig_init(&e->work);
    }
}

void sim_start(void)
{
    for (int i = 0; i < sim.cfg.elevators; i++)
        pthread_create(&sim.elevators[i].tid, NULL, elevator_thread,
                       &sim.elevators[i]);
    pthread_create(&dispatcher_tid, NULL, dispatcher_thread, NULL);
    pthread_create(&generator_tid, NULL, generator_thread, NULL);
}

void sim_stop(void)
{
    pthread_mutex_lock(&sim.lock);
    sim.running = 0;
    pthread_mutex_unlock(&sim.lock);

    sig_post(&sim.new_call);
    for (int i = 0; i < sim.cfg.elevators; i++) sig_post(&sim.elevators[i].work);
}

void sim_join(void)
{
    int n;

    pthread_join(generator_tid, NULL);

    pthread_mutex_lock(&sim.lock);
    n = sim.n_passengers;
    pthread_mutex_unlock(&sim.lock);
    for (int i = 0; i < n; i++) {
        sig_post(&sim.passengers[i].boarded);
        sig_post(&sim.passengers[i].arrived);
    }
    for (int i = 0; i < n; i++) pthread_join(sim.passengers[i].tid, NULL);

    for (int i = 0; i < sim.cfg.elevators; i++) pthread_join(sim.elevators[i].tid, NULL);
    pthread_join(dispatcher_tid, NULL);
}

int sim_running(void)
{
    int r;

    pthread_mutex_lock(&sim.lock);
    r = sim.running;
    pthread_mutex_unlock(&sim.lock);
    return r;
}

int sim_all_delivered(void)
{
    int done;

    pthread_mutex_lock(&sim.lock);
    done = (sim.delivered >= sim.cfg.passengers);
    pthread_mutex_unlock(&sim.lock);
    return done;
}
