#include <pthread.h>
#include <stdio.h>
#include <unistd.h>  // for usleep
#define N 3

pthread_mutex_t forks[N];

void *philosopher(void *arg) {
    int id = *(int *)arg;
    int left  = id;
    int right = (id + 1) % N;
    // Use stderr (unbuffered) instead of stdout (block-buffered when not a
    // terminal). With stdout, prints would sit in the FILE buffer and be lost
    // when the deadlock detector calls _exit(1) — _exit bypasses stdio flush.
    fprintf(stderr, "Philospher %d is trying to grab a fork \n", id);
    usleep(1000);
    // ALL philosophers grab left first → circular wait when N≥3
    pthread_mutex_lock(&forks[left]);
    usleep(1000); // encourages interleaving
    pthread_mutex_lock(&forks[right]);

    fprintf(stderr, "Philosopher %d eating\n", id);

    pthread_mutex_unlock(&forks[right]);
    pthread_mutex_unlock(&forks[left]);
    return NULL;
}

int main() {
    pthread_t threads[N];
    int ids[N];
    for (int i = 0; i < N; i++) {
        pthread_mutex_init(&forks[i], NULL);
        ids[i] = i;
    }
    for (int i = 0; i < N; i++)
        pthread_create(&threads[i], NULL, philosopher, &ids[i]);
    for (int i = 0; i < N; i++)
        pthread_join(threads[i], NULL);
}
