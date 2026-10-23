#include <stdio.h>
#include <unistd.h>

#include "elevator.h"

static void *probe_thread(void *arg)
{
    Signal *s = arg;

    usleep(50 * 1000);
    sig_post(s);
    return NULL;
}

static int probe_signals(void)
{
    Signal s;
    pthread_t tid;

    sig_init(&s);
    if (pthread_create(&tid, NULL, probe_thread, &s) != 0) {
        dprintf(2, "cannot start a thread\n");
        return -1;
    }
    sig_wait(&s);
    pthread_join(tid, NULL);
    sig_destroy(&s);
    return 0;
}

int main(int argc, char **argv)
{
    Config cfg;
    const char *file;
    int r;

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

    config_dump(&cfg, 1);

    if (probe_signals() < 0)
        return 1;
    dprintf(1, "threads and signals work, ok\n");
    return 0;
}
