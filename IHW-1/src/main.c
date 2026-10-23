#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "elevator.h"

// set from the signal handler, so nothing but a flag is touched there
static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

// pause between ticks so that the screen can be followed by eye
static void wait_ms(int ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    Config cfg;
    Sim sim;
    const char *file, *reason;
    int r;

    // defaults, then the config file, then the command line on top
    config_defaults(&cfg);
    file = config_arg_file(argc, argv);
    if (file && config_load_file(&cfg, file) < 0)
        return 1;
    r = config_parse_args(&cfg, argc, argv);
    if (r < 0)
        return 1;
    if (r > 0)
        return 0;
    if (config_check(&cfg) < 0)
        return 1;

    // from here on Ctrl-C only raises the flag and the run ends tidily
    install_handlers();
    if (out_open(cfg.log_path, cfg.plain) < 0)
        return 1;
    config_dump(&cfg, out_log_fd());
    if (cfg.plain)
        config_dump(&cfg, 1);

    sim_init(&sim, &cfg);
    out_draw(&sim);
    while (!sim_done(&sim) && !interrupted) {
        if (cfg.max_ticks > 0 && sim.tick >= cfg.max_ticks)
            break;
        sim_step(&sim);
        out_draw(&sim);
        wait_ms(cfg.delay_ms);
    }

    // three ways to finish, all of them end with the same summary
    if (interrupted)
        reason = "interrupted by the user";
    else if (sim_done(&sim))
        reason = "all passengers delivered";
    else
        reason = "tick limit reached";

    out_stats(&sim, reason);
    out_close();
    // 2 tells the test scripts that the model broke its own rules
    return sim.violations ? 2 : 0;
}
