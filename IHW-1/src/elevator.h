#ifndef ELEVATOR_H
#define ELEVATOR_H

// Elevator group simulation, individual task 29.
// Sequential model: one process, one thread, discrete ticks.
// Floors are numbered 1..floors, elevators and passengers 1..n,
// but the arrays that hold them start at index 0.

#define MAX_FLOORS      32
#define MAX_ELEVATORS    8
#define MAX_CAPACITY    16
#define MAX_PASSENGERS 256
#define MAX_CALLS      128

typedef enum { STRAT_FCFS, STRAT_NEAREST } Strategy;

typedef struct {
    int floors, elevators, capacity;
    int start_floor[MAX_ELEVATORS];
    int n_start;                // how many start floors were given
    int passengers, arrival_min, arrival_max;
    int travel_ticks, door_ticks, board_ticks, alight_ticks;
    Strategy strategy;
    unsigned seed;              // 0 means take it from the clock
    int max_ticks;              // 0 means run until everybody is home
    int delay_ms, plain;
    char log_path[256];
} Config;

void config_defaults(Config *c);
const char *config_arg_file(int argc, char **argv);
int  config_load_file(Config *c, const char *path);
int  config_parse_args(Config *c, int argc, char **argv);
int  config_check(Config *c);
void config_dump(const Config *c, int fd);
void config_usage(const char *prog, int fd);

typedef enum { DIR_DOWN = -1, DIR_NONE = 0, DIR_UP = 1 } Direction;

// States of a cabin, in the order they normally follow
typedef enum {
    ST_IDLE, ST_MOVING, ST_OPENING, ST_UNLOADING, ST_LOADING, ST_CLOSING
} ElevatorState;

typedef enum { PS_WAITING, PS_RIDING, PS_DONE } PassengerState;

typedef struct {
    int id, from, to;
    Direction dir;              // where he wants to go
    PassengerState state;
    int elevator;               // his cabin while riding, -1 otherwise
    int appeared_tick, boarded_tick, done_tick;
} Passenger;

// A hall call: somebody pressed the up or down button on a floor.
// Several passengers share one call, and there is never more than one
// active call per floor and direction.
typedef struct {
    int floor;
    Direction dir;
    int elevator;               // cabin it was given to, -1 if none yet
    int created_tick;
    int active;                 // 0 marks a free slot in the array
} Call;

typedef struct {
    int id, floor;
    Direction dir;              // direction it is serving right now
    ElevatorState state;
    int timer;                  // ticks left in the current state
    int doors;                  // 1 while the doors are open
    int riders[MAX_CAPACITY];
    int n_riders;
    int floors_passed;          // for the final statistics
} Elevator;

typedef struct {
    Config cfg;
    Elevator elevators[MAX_ELEVATORS];
    Passenger passengers[MAX_PASSENGERS];
    Call calls[MAX_CALLS];
    int n_passengers;           // how many have appeared so far
    int next_arrival;           // tick of the next one
    int tick;
    int delivered, total_wait, max_wait, total_ride;
    int refused_boardings;      // times a full cabin left people behind
    int violations;             // broken invariants, must stay zero
} Sim;

void sim_init(Sim *s, const Config *cfg);
void sim_step(Sim *s);
int  sim_total(const Sim *s);
int  sim_done(const Sim *s);

int  out_open(const char *log_path, int plain);
int  out_log_fd(void);
void out_close(void);
void out_event(int tick, const char *fmt, ...);
void out_draw(const Sim *s);
void out_stats(const Sim *s, const char *reason);

#endif
