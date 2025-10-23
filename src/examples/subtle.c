#include <stdlib.h>                     // exit_failure, exit_success
#include <stdio.h>                      // stdin, stdout, printf
#include <pthread.h>                    // threads
#include <string.h>                     // string
#include <unistd.h>                     // sleep
#include <stdbool.h>                    // bool
#include <fcntl.h>                      // open


struct event {
    pthread_mutex_t critical;
    pthread_mutex_t signalM;
    pthread_cond_t signalC;
    int eventCount;
};

struct allVars {
    struct event inEvents;
    struct event outEvents;
    int bufferSize;
    char buffer[10][128];
};


/**
 * Advance the EventCount
 */
void advanceIn(struct event *inEvents) {
    // increment the inEvents counter
    pthread_mutex_lock(&inEvents->critical);
    inEvents->eventCount++;
    pthread_mutex_unlock(&inEvents->critical);

    // signal await to continue
    pthread_mutex_lock(&inEvents->signalM);
    pthread_cond_signal(&inEvents->signalC);
    pthread_mutex_unlock(&inEvents->signalM);
}
void advanceOut(struct event *outEvents) {
    // increment the outEvents counter
    pthread_mutex_lock(&outEvents->critical);
    outEvents->eventCount++;
    pthread_mutex_unlock(&outEvents->critical);

    // signal await to continue
    pthread_mutex_lock(&outEvents->signalM);
    pthread_cond_signal(&outEvents->signalC);
    pthread_mutex_unlock(&outEvents->signalM);
}



/**
 * Wait for ticket and buffer availability
 */
void awaitIn(struct event *inEvents, int ticket) {

    int eventCount;

    // get the counter
    pthread_mutex_lock(&inEvents->critical);
    eventCount = inEvents->eventCount;
    pthread_mutex_unlock(&inEvents->critical);

    // lock signaling mutex
    pthread_mutex_lock(&inEvents->signalM);

    // loop until the ticket machine shows your number
    while (ticket > eventCount) {
        // wait until a ticket is called
        pthread_cond_wait(&inEvents->signalC, &inEvents->signalM);

        // get the counter
        pthread_mutex_lock(&inEvents->critical);
        eventCount = inEvents->eventCount;
        pthread_mutex_unlock(&inEvents->critical);
    }

    // unlock signaling mutex
    pthread_mutex_unlock(&inEvents->signalM);
}
void awaitOut(struct event *outEvents, int ticket) {

    int eventCount;

    // get the counter
    pthread_mutex_lock(&outEvents->critical);
    eventCount = outEvents->eventCount;
    pthread_mutex_unlock(&outEvents->critical);

    // lock signaling mutex
    pthread_mutex_lock(&outEvents->signalM);

    // loop until the ticket machine shows your number
    while (ticket > eventCount) {
        // wait until a ticket is called
        pthread_cond_wait(&outEvents->signalC, &outEvents->signalM);

        // get the counter
        pthread_mutex_lock(&outEvents->critical);
        eventCount = outEvents->eventCount;
        pthread_mutex_unlock(&outEvents->critical);
    }

    // unlock signaling mutex
    pthread_mutex_unlock(&outEvents->signalM);
}



/**
 * Add to buffer
 */
void putBuffer(struct allVars *allVars, char data[]) {
    // get the current write position
    pthread_mutex_lock(&allVars->inEvents.critical);
    int in = allVars->inEvents.eventCount;
    pthread_mutex_unlock(&allVars->inEvents.critical);

    // wait until theres a space free in the buffer
    awaitOut(&allVars->outEvents, in - allVars->bufferSize + 1);   // set to 2 to keep 1 index distance

    // add data to buffer
    strcpy(allVars->buffer[in % allVars->bufferSize], data);

    // increment the eventCounter and signal
    advanceIn(&allVars->inEvents);
}



/**
 * Get from buffer
 */
char *getBuffer(struct allVars *allVars) {
    // get the current read position
    pthread_mutex_lock(&allVars->outEvents.critical);
    int out = allVars->outEvents.eventCount;
    pthread_mutex_unlock(&allVars->outEvents.critical);

    // wait until theres something in the buffer
    awaitIn(&allVars->inEvents, out + 1);

    // allocate memory for returned string
    char *str = malloc(128);

    // get the buffer data
    strcpy(str, allVars->buffer[out % allVars->bufferSize]);

    // increment the eventCounter and signal
    advanceOut(&allVars->outEvents);

    return str;
}



/** child thread (producer) */
void *childThread(void *allVars) {
    char str[12];
    int count = 0;

    while (true) {
        sprintf(str, "%d", count++);
        putBuffer(allVars, str);
        usleep(300);
    }

    pthread_exit(EXIT_SUCCESS);
}

/** child2 thread (consumer) */
void *childThread2(void *allVars) {
    char str[10];
    int count = 0;

    while (true) {
        char *out = getBuffer(allVars);
        printf("buf: %s\n", out);
        free(out);
        usleep(600);
    }

    pthread_exit(EXIT_SUCCESS);
}



int main(int argc, char *argv[]) {
    if (argc != 2) {
      fprintf(stderr, "Usage: %s <bufferrSize>\n", argv[0]);
      exit(1);
    }
    int bufferSize = atoi(argv[1]);

    // init structs
    struct event inEvents = {
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_COND_INITIALIZER,
        0
    };
    struct event outEvents = {
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_COND_INITIALIZER,
        0
    };
    struct allVars allVars = {
        inEvents,       // events
        outEvents,
        -1,             // bufferSize
        {"", {""}}      // buffer[][]
    };
    allVars.bufferSize = bufferSize;
    // NOTE: allVars will have copied PTHREAD_MUTEX_INTIIALIZER and COND
#if 1
    // Added to also use XXX_init functions; Not strictly necessary.
    pthread_mutex_init(&allVars.inEvents.critical, NULL);
    pthread_mutex_init(&allVars.inEvents.signalM, NULL);
    pthread_mutex_init(&allVars.outEvents.critical, NULL);
    pthread_mutex_init(&allVars.outEvents.signalM, NULL);
    pthread_cond_init(&allVars.inEvents.signalC, NULL);
    pthread_cond_init(&allVars.outEvents.signalC, NULL);
#endif

    // create child thread (producer)
    pthread_t thread;
    if (pthread_create(&thread, NULL, childThread, &allVars)) {
        fprintf(stderr, "failed to create child thread");
        exit(EXIT_FAILURE);
    }
    pthread_t thread2;
    if (pthread_create(&thread2, NULL, childThread2, &allVars)) {
        fprintf(stderr, "failed to create child2 thread");
        exit(EXIT_FAILURE);
    }

    pthread_join(thread, NULL);
    pthread_join(thread2, NULL);


#if 0
    // (consumer)
    while (true) {
        char *out = getBuffer(&allVars);
        printf("buf: %s\n", out);
        free(out);
    }
#endif


    return (EXIT_SUCCESS);
}
