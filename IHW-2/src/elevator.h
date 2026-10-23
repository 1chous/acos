#ifndef ELEVATOR_H
#define ELEVATOR_H

#include <pthread.h>
#ifndef USE_COND
#include <semaphore.h>
#endif

#define MAX_FLOORS      32
#define MAX_ELEVATORS    8
#define MAX_CAPACITY    16
#define MAX_PASSENGERS 256
#define MAX_CALLS      128

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
    int floors, elevators, capacity;
    int start_floor[MAX_ELEVATORS];
    int n_start;
    int passengers;
    int arrival_min_ms, arrival_max_ms;
    int travel_ms;
    int door_ms;
    int board_ms, alight_ms;
    Strategy strategy;
    unsigned seed;
    int max_seconds;
    int refresh_ms;
    int plain;
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
    int id, from, to;
    Direction dir;
    PassengerState state;
    int elevator;
    pthread_t tid;
    Signal boarded;
    Signal arrived;
    long appeared_ms, boarded_ms, done_ms;
} Passenger;

typedef struct {
    int floor;
    Direction dir;
    int elevator;
    int active;
} Call;

typedef struct {
    int id, floor;
    Direction dir;
    ElevatorState state;
    int doors;
    int riders[MAX_CAPACITY];
    int n_riders;
    int floors_passed;
    pthread_t tid;
    Signal work;
} Elevator;

typedef struct {
    Config cfg;
    pthread_mutex_t lock;
    Elevator elevators[MAX_ELEVATORS];
    Passenger passengers[MAX_PASSENGERS];
    Call calls[MAX_CALLS];
    Signal new_call;
    int n_passengers;
    int delivered, refused_boardings, violations;
    long total_wait, max_wait, total_ride;
    int running;
} Sim;

#endif
