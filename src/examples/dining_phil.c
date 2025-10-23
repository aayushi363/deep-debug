#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define N_PHIL 3
#define MAX_MEALS 9000

#define THINK_US 18000         // Much shorter thinking time
#define EAT_US   9000          // Much shorter eating time
#define USLEEP_SHORT 1000

pthread_mutex_t forks[N_PHIL];
pthread_t phils[N_PHIL];

volatile int global_meals = 0;
pthread_mutex_t meals_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int flipped = 0;

volatile int thread_flipped[N_PHIL] = {0}; // One flag per philosopher
int thread_meal_count[N_PHIL] = {0};       // Individual meal counters
#define THREAD_FLIP_TRIGGER (1200 + (rand() % 101))  // Even lower - flip after just 5 meals

void do_think(int id) {
    usleep(THINK_US + (rand() % (THINK_US / 2)));
}

void do_eat(int id) {
    printf("Philosopher %d is eating\n", id);
    fflush(stdout);
    usleep(EAT_US + (rand() % (EAT_US / 2)));
}

// Picks forks in either deadlock-free or deadlock-prone order depending on 'flipped'
void pick_forks(int id) {
    int left = id;
    int right = (id + 1) % N_PHIL;

    // Check THIS thread's flipped status
    pthread_mutex_lock(&meals_lock);
    int current_flipped = thread_flipped[id];
    pthread_mutex_unlock(&meals_lock);

    int first_fork, second_fork;
    if (!current_flipped) {
        // Deadlock-free: pick forks in ascending order by fork ID
        if (left < right) {
            first_fork = left;
            second_fork = right;
        } else {
            first_fork = right;
            second_fork = left;
        }
    } else {
        // Force deadlock: ALL pick left first
        first_fork = left;
        second_fork = right;
        printf("Philosopher %d (DEADLOCK MODE) trying to pick fork %d\n", id, first_fork);
        fflush(stdout);
    }

    pthread_mutex_lock(&forks[first_fork]);
    if (current_flipped) {
        printf("Philosopher %d got fork %d, trying fork %d\n", id, first_fork, second_fork);
        fflush(stdout);
        usleep(500000); // MUCH longer delay - 0.5 seconds
    } else {
        usleep(USLEEP_SHORT);
    }
    pthread_mutex_lock(&forks[second_fork]);
}


void put_forks(int id) {
    int left = id;
    int right = (id + 1) % N_PHIL;

    pthread_mutex_unlock(&forks[left]);
    pthread_mutex_unlock(&forks[right]);
}

void *philosopher(void *arg) {
    int id = (int)(long)arg;

    while (1) {
        do_think(id);

        pick_forks(id);

        do_eat(id);

        put_forks(id);

        pthread_mutex_lock(&meals_lock);
        global_meals++;
        thread_meal_count[id]++;
        
        // Each thread flips its own flag after 10 meals
        if (!thread_flipped[id] && thread_meal_count[id] >= THREAD_FLIP_TRIGGER) {
            thread_flipped[id] = 1;
            printf("[info] Philosopher %d switched to deadlock-prone mode after %d meals\n", 
                   id, thread_meal_count[id]);
            printf("[info] Global meals: %d\n", global_meals);
            fflush(stdout);
        }
        
        if (global_meals >= MAX_MEALS) {
            pthread_mutex_unlock(&meals_lock);
            break;
        }
        pthread_mutex_unlock(&meals_lock);
    }
    return NULL;
}

// Add this function to monitor when all threads have flipped:
void check_all_flipped() {
    pthread_mutex_lock(&meals_lock);
    int all_flipped = 1;
    for (int i = 0; i < N_PHIL; i++) {
        if (!thread_flipped[i]) {
            all_flipped = 0;
            break;
        }
    }
    if (all_flipped) {
        printf("[WARNING] ALL PHILOSOPHERS NOW IN DEADLOCK-PRONE MODE!\n");
        fflush(stdout);
    }
    pthread_mutex_unlock(&meals_lock);
}

int main(void) {
    srand((unsigned)time(NULL));

    for (int i = 0; i < N_PHIL; ++i)
        pthread_mutex_init(&forks[i], NULL);

    for (int i = 0; i < N_PHIL; ++i) {
        if (pthread_create(&phils[i], NULL, philosopher, (void *)(long)i) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    for (int i = 0; i < N_PHIL; ++i)
        pthread_join(phils[i], NULL);

    for (int i = 0; i < N_PHIL; ++i)
        pthread_mutex_destroy(&forks[i]);
    pthread_mutex_destroy(&meals_lock);

    return 0;
}
