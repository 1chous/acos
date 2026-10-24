#include <pthread.h>

#include "elevator.h"

// The same four functions as in sync_sem.c, built on condition variables.
// A condition variable does not remember signals: one sent while nobody
// waits is lost for good. So the count is kept by hand, which turns the
// pair mutex plus condition variable into a counting semaphore.

void sig_init(Signal *s)
{
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count = 0;
}

void sig_wait(Signal *s)
{
    pthread_mutex_lock(&s->lock);
    // while, not if: pthread_cond_wait may return without any signal
    while (s->count == 0)
        pthread_cond_wait(&s->cond, &s->lock);
    s->count--;
    pthread_mutex_unlock(&s->lock);
}

void sig_post(Signal *s)
{
    pthread_mutex_lock(&s->lock);
    s->count++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

void sig_destroy(Signal *s)
{
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
}

const char *sync_name(void)
{
    return "condition variables";
}
