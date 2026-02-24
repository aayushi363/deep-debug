// Source - https://stackoverflow.com/q/75657493
// Posted by David C. Rankin
// Retrieved 2026-02-24, License - CC BY-SA 4.0

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

#define handle_error_en(en, msg) \
  do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

#define handle_error(msg) \
  do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define NCPU 4
#define ITER_PER_CPU  100

typedef struct {
  int index, start, end;
  unsigned sum;
} loop_data;

pthread_barrier_t loop_barrier;   /* global barriers (could pass in data) */
pthread_barrier_t prep_barrier;

void *thread_fn (void *data)
{
  int done = 0, 
      i = 0;
  loop_data *thread_data = data;
  
  do {
    for (i = thread_data->start; i < thread_data->end; i++) {
      /* each arg gets separate loop_data - do work */
      thread_data->sum += i;
    }
    
    /* suspend on barrier and do any per-cycle cleanup */
    if (pthread_barrier_wait (&loop_barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
      puts ("PTHREAD_BARRIER_SERIAL_THREAD");
      /* no actual per-cycle cleanup, just set done flag */
      done = 1;
    }
    /* suspend on barrier until per-cycle cleanup complete */
    pthread_barrier_wait (&prep_barrier);
    
    printf ("thread index: %d, sum: %d\n", 
            thread_data->index, thread_data->sum);
    
  } while (!done);
  
  return data;
}

int main (void) {

  pthread_t id[NCPU];
  pthread_attr_t attr;
  loop_data arr[NCPU] = {{ .start = 0 }};
  void *res;
  int rtn = 0;
  
  /* initialize barriers and validate */
  if ((rtn = pthread_barrier_init (&loop_barrier, NULL, NCPU))) {
    handle_error_en (rtn, "pthread_barrier_init-loop_barrier");
  }
  if ((rtn = pthread_barrier_init (&prep_barrier, NULL, NCPU))) {
    handle_error_en (rtn, "pthread_barrier_init-prep_barrier");
  }
  
  /* initialize thread attributes (using defaults) and validate */
  if ((rtn = pthread_attr_init (&attr))) {
    handle_error_en (rtn, "pthread_attr_init");
  }
  
  /* set data index, start, end and create/validate each thread */ 
  for (int i = 0; i < NCPU; i++) {
    /* initialize index, start / end values */
    arr[i].index = i;
    arr[i].start = i * ITER_PER_CPU;
    arr[i].end = (i + 1) * ITER_PER_CPU;
    printf ("id: %d, start: %3d, end: %3d\n", i, arr[i].start, arr[i].end);
    /* create thread and validate */
    if ((rtn = pthread_create (&id[i], &attr, thread_fn, &arr[i]))) {
      handle_error_en (rtn, "pthread_create");
    }
  }

  /* join all threads and compare sums from threads with sums in main */
  for (int i = 0; i < NCPU; i++) {
    loop_data *data = NULL;
    /* join and validate */
    printf ("joining thread index: %d\n", i);
    if ((rtn = pthread_join (id[i], &res))) {
      fprintf (stderr, "error: thread %d\n", i);
      handle_error_en (rtn, "pthread_join");
    }
    data = res;   /* pointer to return struct provided through parameter */
    printf ("thread index: %d joined\n", data->index);
  }
  
  /* destroy barriers and validate */
  if ((rtn = pthread_barrier_destroy (&loop_barrier))) {
    handle_error_en (rtn, "pthread_barrier_destroy-loop_barrier");
  }
  if ((rtn = pthread_barrier_destroy (&prep_barrier))) {
    handle_error_en (rtn, "pthread_barrier_destroy-prep_barrier");
  }
}
