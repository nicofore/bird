


#include <pthread.h>

#include <semaphore.h>

#include <stdlib.h>

#include "lib/rcu.h"



#define TBUFFER_SIZE 256






typedef struct threadpool_t{
    //Thread
    pthread_t *threads;
    //Number of threads
    uint numThreads;
    //Function that is run by the thread
    void (*function)(void *);
    //Buffer which is a producer consummer
    void* buffer[TBUFFER_SIZE];

    //Index to put the next element in the buffer
    uint indexIn;
    //Index to get the next element in the buffer
    uint indexOut;

    //Mutex and semaphores to protect the buffer
    sem_t semProducer;     
    sem_t semConsumer;    
    pthread_mutex_t mutexBuffer;
} threadpool_t;

typedef struct args_t{
    uint threadnumber;
    threadpool_t *tpool;
    struct rcu_thread* rcu_thread;
} args_t;

typedef struct argsf_t{
    uint threadnumber;
    void* data;
} argsf_t;


void* threadpool_thread(void *threadpool_);

void threadpool_init(threadpool_t *threadpool, uint numThreads, void (*function)(void *));

void threadpool_add(threadpool_t *threadpool, void *data);

void threadpool_free(threadpool_t *threadpool);