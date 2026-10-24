#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "elevator.h"

// A signal interrupts whichever thread the system picks, and that thread
// may well be holding a mutex. So the handler does the only safe thing:
// sets a flag. The main loop below notices it and shuts down tidily.
static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t drain_flag;

static void on_signal(int sig)
{
    if (sig == SIGUSR1) drain_flag = 1;
    else stop_flag = 1;
}

static void install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
}

int main(int argc, char **argv)
{
    Config cfg;
    const char *file, *reason;
    int r, timeout = 0;

    // defaults, then the config file, then the command line on top
    config_defaults(&cfg);
    file = config_arg_file(argc, argv);
    if (file && config_load_file(&cfg, file) < 0) return 1;
    r = config_parse_args(&cfg, argc, argv);
    if (r < 0) return 1;
    if (r > 0) return 0;
    if (config_check(&cfg) < 0) return 1;

    install_handlers();
    if (out_open(cfg.log_path, cfg.plain) < 0) return 1;
    config_dump(&cfg, out_log_fd());
    if (cfg.plain) config_dump(&cfg, 1);

    sim_init(&cfg);
    sim_start();

    while (!sim_all_delivered() && !stop_flag) {
        // SIGUSR1 does not stop the program, it changes the rules:
        // no new passengers, finish with those already in the building
        if (drain_flag) {
            drain_flag = 0;
            sim_drain();
        }
        if (cfg.max_seconds > 0 && now_ms() >= cfg.max_seconds * 1000L) {
            timeout = 1;
            break;
        }
        sim_check();
        out_draw();
        sleep_ms(cfg.refresh_ms);
    }

    sim_stop();
    sim_join();

    if (stop_flag) reason = "interrupted by the user";
    else if (timeout) reason = "time limit reached";
    else if (sim.draining) reason = "stopped by SIGUSR1, everybody in the building delivered";
    else reason = "all passengers delivered";

    out_stats(reason);
    out_close();
    return sim.violations ? 2 : 0;
}
