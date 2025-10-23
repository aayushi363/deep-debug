#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>

#define NODES 5
#define MAX_OPS 200
//#define MIN_SAFE_ITER (170 + (rand() % 21))  // Delay ABA interference until after this many combined iterations

pthread_mutex_t list_lock;
pthread_mutex_t iter_lock;
volatile bool p2_interfered = false;
int global_iter = 0;
int MIN_SAFE_ITER; // Set dynamically, can be adjusted

typedef struct node {
    int val;
    struct node *next;
} node_t;

node_t *top = NULL;

// Helper: busy wait with tiny random jitter
void busy_wait_us(long base_us) {
    long jitter = rand() % (base_us / 10 + 1);
    struct timespec start_time, cur_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    long start_us = start_time.tv_sec * 1000000 + start_time.tv_nsec / 1000;
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &cur_time);
        long elapsed = (cur_time.tv_sec * 1000000 + cur_time.tv_nsec / 1000) - start_us;
        if (elapsed >= base_us + jitter) break;
    }
}

bool mutex_compare_and_swap(node_t **p, node_t *old_val, node_t *new_val) {
    bool success = false;
    pthread_mutex_lock(&list_lock);
    if (*p == old_val) {
        *p = new_val;
        success = true;
    }
    pthread_mutex_unlock(&list_lock);
    return success;
}

void safe_push(node_t* n, int iter) {
    if (!n) return;
    pthread_mutex_lock(&list_lock);

    pthread_mutex_lock(&iter_lock);
    if (global_iter >= MIN_SAFE_ITER) {
        p2_interfered = true;
    }
    pthread_mutex_unlock(&iter_lock);

    n->next = top;
    top = n;
    pthread_mutex_unlock(&list_lock);
    printf("p2: Push #%d node %d back.\n", iter, n->val);
}

node_t* safe_pop(int iter) {
    pthread_mutex_lock(&list_lock);
    node_t* h = top;
    if (h != NULL) {
        top = h->next;
    }

    pthread_mutex_lock(&iter_lock);
    if (global_iter >= MIN_SAFE_ITER) {
        p2_interfered = true;
    }
    pthread_mutex_unlock(&iter_lock);

    pthread_mutex_unlock(&list_lock);
    if (h) printf("p2: Pop #%d node %d.\n", iter, h->val);
    return h;
}

void random_backoff() {
    int backoff = rand() % 3;
    if (backoff == 0) {
        busy_wait_us(30000);
    } else if (backoff == 1) {
        busy_wait_us(5000);
    } else {
        busy_wait_us(15000);
    }
}

void increment_global_iter() {
    pthread_mutex_lock(&iter_lock);
    global_iter++;
    pthread_mutex_unlock(&iter_lock);
}

int get_global_iter() {
    int val;
    pthread_mutex_lock(&iter_lock);
    val = global_iter;
    pthread_mutex_unlock(&iter_lock);
    return val;
}

void *p1(void *arg) {
    int iter = 0;
    while (iter < MAX_OPS) {
        busy_wait_us((10000 + rand() % 20000)*2);

        pthread_mutex_lock(&list_lock);
        node_t *original_top = top;
        pthread_mutex_unlock(&list_lock);

        // Reset interference only after MIN_SAFE_ITER
        if (get_global_iter() >= MIN_SAFE_ITER) {
            pthread_mutex_lock(&iter_lock);
            p2_interfered = false;
            pthread_mutex_unlock(&iter_lock);
        }

        if (original_top == NULL) {
            iter++;
            increment_global_iter();
            continue;
        }

        busy_wait_us((20000 + rand() % 10000)*2);

        if (mutex_compare_and_swap(&top, original_top, original_top->next)) {
            pthread_mutex_lock(&iter_lock);
            bool interfered = p2_interfered;
            pthread_mutex_unlock(&iter_lock);

            if (interfered && get_global_iter() >= MIN_SAFE_ITER) {
                printf("p1: *** ABA BUG DETECTED! *** iteration %d node %d\n", iter, original_top->val);
                fflush(stdout);
                free(original_top);
                assert(0 && "ABA problem detected!");
            } else {
                printf("p1: Pop succeeded, pushing node %d back (iter %d)\n", original_top->val, iter);
                safe_push(original_top, iter);
            }
        } else {
            // Branch: CAS failed, do random backoff
            random_backoff();
        }
        iter++;
        increment_global_iter();
    }
    printf("p1: Finished all iterations\n");
    return NULL;
}

void *p2(void *arg) {
    int iter = 0;
    while (iter < MAX_OPS) {
        busy_wait_us((15000 + rand() % 25000)*2);

        node_t *n = safe_pop(iter);
        if (n) {
            busy_wait_us((20000 + rand() % 30000)*2);
            safe_push(n, iter);
        }

        random_backoff();

        iter++;
        increment_global_iter();
    }
    printf("p2: Finished all iterations\n");
    return NULL;
}

int main() {
    srand((unsigned)time(NULL));
    MIN_SAFE_ITER = 270 + (rand() % 21);
    printf("MIN_SAFE_ITER set to %d\n", MIN_SAFE_ITER);
    pthread_mutex_init(&list_lock, NULL);
    pthread_mutex_init(&iter_lock, NULL);

    // Initialize linked list with multiple nodes to increase branching
    node_t *nodes[NODES];
    for (int i = 0; i < NODES; i++) {
        nodes[i] = malloc(sizeof(node_t));
        nodes[i]->val = i + 1;
        nodes[i]->next = (i == 0) ? NULL : nodes[i - 1];
    }
    top = nodes[NODES - 1];

    pthread_t t1, t2;
    pthread_create(&t1, NULL, p1, NULL);
    pthread_create(&t2, NULL, p2, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    pthread_mutex_destroy(&list_lock);
    pthread_mutex_destroy(&iter_lock);
    return 0;
}
