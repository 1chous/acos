#include <stdio.h>

#include "elevator.h"

int main(int argc, char **argv)
{
    Config cfg;
    const char *file;
    int r;

    config_defaults(&cfg);
    file = config_arg_file(argc, argv);
    if (file && config_load_file(&cfg, file) < 0) return 1;
    r = config_parse_args(&cfg, argc, argv);
    if (r < 0) return 1;
    if (r > 0) return 0;
    if (config_check(&cfg) < 0) return 1;

    if (out_open(cfg.log_path) < 0) return 1;
    config_dump(&cfg, 1);
    config_dump(&cfg, out_log_fd());

    sim_init(&cfg);
    sim_start();

    while (!sim_all_delivered()) {
        sim_check();
        sleep_ms(sim.cfg.refresh_ms);
    }

    sim_stop();
    sim_join();

    dprintf(1, "\ndelivered %d of %d in %ld ms\n",
            sim.delivered, cfg.passengers, now_ms());
    out_close();
    return 0;
}
