#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "elevator.h"

// macOS does not implement sem_init - it returns ENOSYS - so named
// semaphores are the only portable choice. The name is made unique from
// the pid and a counter, and unlinked right after opening: the semaphore
// stays alive while we hold it, but a crash leaves nothing behind.
static int sig_counter;

void sig_init(Signal *s)
{
    snprintf(s->name, sizeof(s->name), "/elev_%d_%d", (int)getpid(), sig_counter++);
    sem_unlink(s->name);
    s->sem = sem_open(s->name, O_CREAT | O_EXCL, 0600, 0);
    if (s->sem == SEM_FAILED) {
        dprintf(2, "cannot create semaphore %s\n", s->name);
        exit(1);
    }
    sem_unlink(s->name);
}

void sig_wait(Signal *s)
{
    // Ctrl-C can interrupt the wait; that is not an error, wait again.
    // Shutdown wakes blocked threads by posting their signals.
    while (sem_wait(s->sem) != 0 && errno == EINTR) ;
}

void sig_post(Signal *s)
{
    sem_post(s->sem);
}

void sig_destroy(Signal *s)
{
    if (s->sem) {
        sem_close(s->sem);
        s->sem = NULL;
    }
}

const char *sync_name(void)
{
    return "semaphores";
}
