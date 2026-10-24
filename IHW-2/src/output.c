#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "elevator.h"

#define LINE_LEN 200

static int log_fd = -1;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

int out_open(const char *log_path)
{
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
        dprintf(2, "cannot create the journal %s\n", log_path);
        return -1;
    }
    return 0;
}

int out_log_fd(void)
{
    return log_fd;
}

void out_close(void)
{
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
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
    write(1, line, (size_t)n);
    if (log_fd >= 0) write(log_fd, line, (size_t)n);
    pthread_mutex_unlock(&out_lock);
}
