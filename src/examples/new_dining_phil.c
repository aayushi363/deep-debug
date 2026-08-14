#include <pthread.h>
#include <stdio.h>
#include <unistd.h>  // for usleep
#define N 3

pthread_mutex_t forks[N];

void *philosopher(void *arg) {
    int id = *(int *)arg;
    int left  = id;
    int right = (id + 1) % N;

    // ALL philosophers grab left first → circular wait when N≥3
    pthread_mutex_lock(&forks[left]);
    usleep(1000); // encourages interleaving
    pthread_mutex_lock(&forks[right]);

    printf("Philosopher %d eating\n", id);

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
