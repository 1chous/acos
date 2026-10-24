#ifndef ELEVATOR_H
#define ELEVATOR_H

// Elevator group simulation, task 29, multithreaded version.
// Every entity with behaviour runs in its own thread: a passenger, a
// cabin, the dispatcher. They share the world under one mutex and wake
// each other with signals. Floors are numbered from 1, the arrays that
// hold them from 0. Time is real time in milliseconds.

#include <pthread.h>
#ifndef USE_COND
#include <semaphore.h>
#endif

#define MAX_FLOORS      32
#define MAX_ELEVATORS    8
#define MAX_CAPACITY    16
#define MAX_PASSENGERS 256
#define MAX_CALLS      128

// A one way wake-up between two threads: one waits, another posts.
// Built on semaphores in sync_sem.c and on condition variables in
// sync_cond.c, which gives two builds of the same program.
#ifdef USE_COND
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count;
} Signal;
#else
typedef struct {
    sem_t *sem;
    char name[32];
} Signal;
#endif

void sig_init(Signal *s);
void sig_wait(Signal *s);
void sig_post(Signal *s);
void sig_destroy(Signal *s);
const char *sync_name(void);

typedef enum { STRAT_FCFS, STRAT_NEAREST } Strategy;

typedef struct {
    int floors, elevators, capacity, passengers;
    int start_floor[MAX_ELEVATORS], n_start;
    int arrival_min_ms, arrival_max_ms;
    int travel_ms, door_ms, board_ms, alight_ms;
    int max_seconds, refresh_ms, plain;   // max_seconds 0 means no limit
    Strategy strategy;
    unsigned seed;              // 0 means take it from the clock
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

typedef enum {
    ST_IDLE, ST_MOVING, ST_OPENING, ST_UNLOADING, ST_LOADING, ST_CLOSING
} ElevatorState;

typedef enum { PS_WAITING, PS_RIDING, PS_DONE } PassengerState;

typedef struct {
    int id, from, to, elevator;
    Direction dir;
    PassengerState state;
    pthread_t tid;
    Signal boarded, arrived;
    long appeared_ms, boarded_ms, done_ms;
} Passenger;

// A hall call: somebody pressed the up or down button on a floor.
// Several passengers share one call, and there is never more than one
// active call per floor and direction.
typedef struct {
    int floor, elevator, active;
    Direction dir;
} Call;

typedef struct {
    int id, floor, doors, n_riders, floors_passed;
    int riders[MAX_CAPACITY];
    Direction dir;
    ElevatorState state;
    pthread_t tid;
    Signal work;
} Elevator;

// The whole world. Every field below is shared by all threads and may
// only be touched while lock is held.
typedef struct {
    Config cfg;
    pthread_mutex_t lock;
    Elevator elevators[MAX_ELEVATORS];
    Passenger passengers[MAX_PASSENGERS];
    Call calls[MAX_CALLS];
    Signal new_call;
    int n_passengers, delivered, boarded_count, refused_boardings;
    int violations, running, draining;
    long total_wait, max_wait, total_ride;
} Sim;

extern Sim sim;

long now_ms(void);
void sleep_ms(int ms);
void sim_init(const Config *cfg);
void sim_start(void);
void sim_stop(void);
void sim_join(void);
int  sim_running(void);
int  sim_all_delivered(void);
void sim_check(void);
void sim_drain(void);

int  out_open(const char *log_path, int plain);
int  out_log_fd(void);
void out_close(void);
void out_event(const char *fmt, ...);
void out_draw(void);
void out_stats(const char *reason);

#endif
