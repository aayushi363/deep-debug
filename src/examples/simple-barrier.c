#include <pthread.h>
#include <stdio.h>

pthread_barrier_t barrier;

void* thread_func(void* arg) {
    int id = *(int*)arg;
    printf("Thread %d reached barrier\n", id);
    pthread_barrier_wait(&barrier);
    printf("Thread %d passed barrier\n", id);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    int id1 = 1, id2 = 2;
    
    pthread_barrier_init(&barrier, NULL, 2);
    
    pthread_create(&t1, NULL, thread_func, &id1);
    pthread_create(&t2, NULL, thread_func, &id2);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    pthread_barrier_destroy(&barrier);
    printf("Done\n");
    return 0;
}
