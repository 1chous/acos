#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "elevator.h"

// a broken invariant means the model reached a state the task forbids
#define BROKEN(...) \
    do { s->violations++; out_event(s->tick, "INVARIANT BROKEN: " __VA_ARGS__); } while (0)

// Own generator instead of rand(): the same seed must give the same
// run on any machine, otherwise the tests cannot compare journals.
static unsigned rng_state;

static int rnd(int lo, int hi)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return lo + (int)((rng_state >> 16) % (unsigned)(hi - lo + 1));
}

static const char *dir_name(Direction d)
{
    return d == DIR_UP ? "up" : d == DIR_DOWN ? "down" : "nowhere";
}

static Call *find_call(Sim *s, int floor, Direction dir)
{
    for (int i = 0; i < MAX_CALLS; i++)
        if (s->calls[i].active && s->calls[i].floor == floor && s->calls[i].dir == dir)
            return &s->calls[i];
    return NULL;
}

static void add_call(Sim *s, int floor, Direction dir)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        Call *c = &s->calls[i];
        if (c->active) continue;
        c->floor = floor;
        c->dir = dir;
        c->elevator = -1;
        c->created_tick = s->tick;
        c->active = 1;
        out_event(s->tick, "call registered on floor %d, %s", floor, dir_name(dir));
        return;
    }
    out_event(s->tick, "WARNING: no free call slot, call on floor %d is lost", floor);
}

// A new passenger presses the button, or just joins the people who
// are already waiting on this floor for the same direction.
static void add_passenger(Sim *s, int from, int to)
{
    Passenger *p;

    if (s->n_passengers >= MAX_PASSENGERS) return;
    p = &s->passengers[s->n_passengers];
    p->id = ++s->n_passengers;
    p->from = from;
    p->to = to;
    p->dir = (to > from) ? DIR_UP : DIR_DOWN;
    p->state = PS_WAITING;
    p->elevator = -1;
    p->appeared_tick = s->tick;
    p->boarded_tick = -1;
    p->done_tick = -1;
    out_event(s->tick, "passenger %d appears on floor %d and wants floor %d (%s)",
              p->id, from, to, dir_name(p->dir));
    if (find_call(s, from, p->dir))
        out_event(s->tick, "passenger %d joins the call on floor %d", p->id, from);
    else
        add_call(s, from, p->dir);
}

static void spawn(Sim *s)
{
    const Config *cfg = &s->cfg;
    int from, to;

    if (s->n_passengers >= cfg->passengers || s->tick < s->next_arrival) return;
    from = rnd(1, cfg->floors);
    do { to = rnd(1, cfg->floors); } while (to == from);
    add_passenger(s, from, to);
    s->next_arrival = s->tick + rnd(cfg->arrival_min, cfg->arrival_max);
}

// Is there any reason for the cabin to keep going that way: a rider
// who wants a floor over there, or a call that was given to it.
static int work_in_direction(Sim *s, const Elevator *e, Direction dir)
{
    if (dir == DIR_NONE) return 0;
    for (int i = 0; i < e->n_riders; i++)
        if ((s->passengers[e->riders[i] - 1].to - e->floor) * dir > 0) return 1;
    for (int i = 0; i < MAX_CALLS; i++) {
        const Call *c = &s->calls[i];
        if (c->active && c->elevator == e->id && (c->floor - e->floor) * dir > 0)
            return 1;
    }
    return 0;
}

// the current direction is finished first, as the task requires
static void pick_direction(Sim *s, Elevator *e)
{
    if (work_in_direction(s, e, e->dir)) return;
    if (work_in_direction(s, e, DIR_UP)) e->dir = DIR_UP;
    else if (work_in_direction(s, e, DIR_DOWN)) e->dir = DIR_DOWN;
    else e->dir = DIR_NONE;
}

// Does the cabin have a reason to stop on the floor it is on now?
// Besides its own calls it also takes calls of other cabins going the
// same way, as long as there is room: these are the calls on the way.
static int should_stop(Sim *s, const Elevator *e)
{
    for (int i = 0; i < e->n_riders; i++)
        if (s->passengers[e->riders[i] - 1].to == e->floor) return 1;
    for (int i = 0; i < MAX_CALLS; i++) {
        const Call *c = &s->calls[i];
        if (!c->active || c->floor != e->floor) continue;
        if (c->dir == e->dir) {
            if (c->elevator == e->id || e->n_riders < s->cfg.capacity) return 1;
        } else if (c->elevator == e->id && !work_in_direction(s, e, e->dir)) {
            return 1;
        }
    }
    return 0;
}

// starts a phase that lasts "ticks" ticks in total, this one included
static void set_phase(Elevator *e, ElevatorState st, int ticks)
{
    e->state = st;
    e->timer = (ticks > 0) ? ticks - 1 : 0;
}

// if nothing holds the cabin in its direction, this floor is where it
// turns around, so it takes the direction of the call waiting here
static void begin_stop(Sim *s, Elevator *e)
{
    if (!work_in_direction(s, e, e->dir)) {
        for (int i = 0; i < MAX_CALLS; i++) {
            const Call *c = &s->calls[i];
            if (c->active && c->floor == e->floor &&
                (c->elevator == e->id || c->elevator == -1)) {
                e->dir = c->dir;
                break;
            }
        }
    }
    set_phase(e, ST_OPENING, s->cfg.door_ticks);
    out_event(s->tick, "elevator %d opens the doors on floor %d", e->id, e->floor);
}

static int do_unloading(Sim *s, Elevator *e)
{
    int out = 0, kept = 0;

    for (int i = 0; i < e->n_riders; i++) {
        Passenger *p = &s->passengers[e->riders[i] - 1];
        // he gets out on the floor he asked for and nowhere else
        if (p->to != e->floor) { e->riders[kept++] = e->riders[i]; continue; }
        // and only while the doors are open
        if (!e->doors)
            BROKEN("passenger %d leaves elevator %d through closed doors", p->id, e->id);
        p->state = PS_DONE;
        p->elevator = -1;
        p->done_tick = s->tick;
        s->delivered++;
        s->total_ride += s->tick - p->boarded_tick;
        out_event(s->tick, "passenger %d leaves elevator %d on floor %d, delivered",
                  p->id, e->id, e->floor);
        out++;
    }
    e->n_riders = kept;
    return out;
}

// Everybody on this floor going our way steps in while there is room.
// The call is then answered; whoever did not fit gets a fresh call
// right away, which is what the task asks for.
static int do_boarding(Sim *s, Elevator *e)
{
    int taken = 0, left = 0;

    for (int i = 0; i < s->n_passengers; i++) {
        Passenger *p = &s->passengers[i];
        int wait;

        if (p->state != PS_WAITING || p->from != e->floor || p->dir != e->dir)
            continue;
        if (e->n_riders >= s->cfg.capacity) { left++; continue; }
        if (!e->doors)
            BROKEN("passenger %d enters elevator %d through closed doors", p->id, e->id);
        p->state = PS_RIDING;
        p->elevator = e->id;
        p->boarded_tick = s->tick;
        e->riders[e->n_riders++] = p->id;
        wait = s->tick - p->appeared_tick;
        s->total_wait += wait;
        if (wait > s->max_wait) s->max_wait = wait;
        out_event(s->tick, "passenger %d enters elevator %d and presses floor %d",
                  p->id, e->id, p->to);
        taken++;
    }
    for (int i = 0; i < MAX_CALLS; i++) {
        Call *c = &s->calls[i];
        if (c->active && c->floor == e->floor && c->dir == e->dir) c->active = 0;
    }
    if (left > 0) {
        s->refused_boardings++;
        out_event(s->tick, "elevator %d is full, %d passenger(s) stay on floor %d",
                  e->id, left, e->floor);
        add_call(s, e->floor, e->dir);
    }
    return taken;
}

static void start_closing(Sim *s, Elevator *e)
{
    set_phase(e, ST_CLOSING, s->cfg.door_ticks);
    out_event(s->tick, "elevator %d closes the doors on floor %d", e->id, e->floor);
}

// nobody is stepping out any more, so people can step in; if there is
// nobody to take either, the cabin closes up at once
static void start_boarding(Sim *s, Elevator *e)
{
    int n = do_boarding(s, e);
    if (n > 0) set_phase(e, ST_LOADING, n * s->cfg.board_ticks);
    else start_closing(s, e);
}

static void start_moving(Sim *s, Elevator *e)
{
    set_phase(e, ST_MOVING, s->cfg.travel_ticks);
    out_event(s->tick, "elevator %d starts going %s from floor %d",
              e->id, dir_name(e->dir), e->floor);
}

static void elevator_step(Sim *s, Elevator *e)
{
    int n;

    if (e->timer > 0) { e->timer--; return; }

    switch (e->state) {
    case ST_IDLE:
        if (should_stop(s, e)) { begin_stop(s, e); break; }
        pick_direction(s, e);
        if (e->dir != DIR_NONE) start_moving(s, e);
        break;
    case ST_MOVING:
        e->floor += e->dir;
        e->floors_passed++;
        out_event(s->tick, "elevator %d reaches floor %d", e->id, e->floor);
        if (should_stop(s, e)) begin_stop(s, e);
        else if (work_in_direction(s, e, e->dir))
            set_phase(e, ST_MOVING, s->cfg.travel_ticks);
        else e->state = ST_IDLE;
        break;
    case ST_OPENING:
        e->doors = 1;
        n = do_unloading(s, e);
        if (n > 0) set_phase(e, ST_UNLOADING, n * s->cfg.alight_ticks);
        else start_boarding(s, e);
        break;
    case ST_UNLOADING:
        start_boarding(s, e);
        break;
    case ST_LOADING:
        start_closing(s, e);
        break;
    case ST_CLOSING:
        e->doors = 0;
        pick_direction(s, e);
        if (e->dir == DIR_NONE) e->state = ST_IDLE;
        else start_moving(s, e);
        break;
    }
}

// Picks the cabin for a call. fcfs takes the first cabin with nothing
// to do, nearest takes the one that can reach the floor first; a cabin
// driving the other way has to finish its run, so it costs more.
static int pick_elevator(Sim *s, const Call *call)
{
    int best = -1, best_cost = 0;

    for (int i = 0; i < s->cfg.elevators; i++) {
        Elevator *e = &s->elevators[i];
        int cost;

        if (e->n_riders >= s->cfg.capacity) continue;
        if (s->cfg.strategy == STRAT_FCFS) {
            cost = e->n_riders;
            for (int j = 0; j < MAX_CALLS; j++)
                if (s->calls[j].active && s->calls[j].elevator == e->id) cost++;
            if (cost == 0) return e->id;
        } else {
            cost = abs(e->floor - call->floor);
            if (e->dir != DIR_NONE && (call->floor - e->floor) * e->dir < 0)
                cost += s->cfg.floors;
        }
        if (best < 0 || cost < best_cost) { best = e->id; best_cost = cost; }
    }
    return best;
}

// A call is handed out once and never taken back. That is what makes
// it impossible for two cabins to serve the same call.
static void dispatch(Sim *s)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        Call *c = &s->calls[i];
        int id;

        if (!c->active || c->elevator != -1) continue;
        id = pick_elevator(s, c);
        if (id < 0) continue;
        c->elevator = id;
        out_event(s->tick, "dispatcher sends elevator %d to the call on floor %d (%s)",
                  id, c->floor, dir_name(c->dir));
    }
}

// Four of the six invariants from the task are checked here after
// every tick. The two about the doors and the destination floor are
// checked in do_boarding and do_unloading, because those are the only
// places where a passenger can change where he is.
static void check_invariants(Sim *s)
{
    const Config *cfg = &s->cfg;

    for (int i = 0; i < cfg->elevators; i++) {
        const Elevator *e = &s->elevators[i];
        if (e->n_riders > cfg->capacity)
            BROKEN("elevator %d carries %d, capacity is %d", e->id, e->n_riders, cfg->capacity);
        if (e->state == ST_MOVING && e->doors)
            BROKEN("elevator %d moves with open doors", e->id);
        if (e->floor < 1 || e->floor > cfg->floors)
            BROKEN("elevator %d is on floor %d, outside the building", e->id, e->floor);
    }
    for (int i = 0; i < s->n_passengers; i++) {
        const Passenger *p = &s->passengers[i];
        int seen = 0, where = -1;

        for (int j = 0; j < cfg->elevators; j++)
            for (int k = 0; k < s->elevators[j].n_riders; k++)
                if (s->elevators[j].riders[k] == p->id) { seen++; where = s->elevators[j].id; }
        if (p->state == PS_RIDING && seen != 1)
            BROKEN("passenger %d rides but is found in %d cabins", p->id, seen);
        if (p->state != PS_RIDING && seen != 0)
            BROKEN("passenger %d does not ride but sits in a cabin", p->id);
        if (p->state == PS_RIDING && where != p->elevator)
            BROKEN("passenger %d thinks he is in elevator %d, he is in %d",
                   p->id, p->elevator, where);
    }
    for (int i = 0; i < MAX_CALLS; i++) {
        if (!s->calls[i].active) continue;
        for (int j = i + 1; j < MAX_CALLS; j++)
            if (s->calls[j].active && s->calls[j].floor == s->calls[i].floor &&
                s->calls[j].dir == s->calls[i].dir)
                BROKEN("two active calls on floor %d going %s",
                       s->calls[i].floor, dir_name(s->calls[i].dir));
    }
}

void sim_init(Sim *s, const Config *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    rng_state = cfg->seed ? cfg->seed : (unsigned)time(NULL);
    for (int i = 0; i < cfg->elevators; i++) {
        Elevator *e = &s->elevators[i];
        e->id = i + 1;
        e->floor = cfg->start_floor[i];
        e->dir = DIR_NONE;
        e->state = ST_IDLE;
    }
}

// one tick of model time
void sim_step(Sim *s)
{
    s->tick++;
    spawn(s);
    dispatch(s);
    for (int i = 0; i < s->cfg.elevators; i++)
        elevator_step(s, &s->elevators[i]);
    check_invariants(s);
}

int sim_total(const Sim *s) { return s->cfg.passengers; }

int sim_done(const Sim *s) { return s->delivered >= s->cfg.passengers; }
