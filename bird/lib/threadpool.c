#include "threadpool.h"


void * threadpool_thread(void* arg){
    args_t *args = (args_t *)arg;
    uint threadnumber = args->threadnumber;
    threadpool_t *threadpool = args->tpool;
    while (1){
         //Try to get a task from the buffer
        sem_wait(&(threadpool->semConsumer));
        pthread_mutex_lock(&(threadpool->mutexBuffer));
        void *data = threadpool->buffer[threadpool->indexOut];
        threadpool->buffer[threadpool->indexOut] = NULL;
        threadpool->indexOut = (threadpool->indexOut + 1) % TBUFFER_SIZE;
        pthread_mutex_unlock(&(threadpool->mutexBuffer));
        sem_post(&(threadpool->semProducer));

        argsf_t argsf = {};
        argsf.threadnumber = threadnumber;
        argsf.data = data;

        //Run the task
        threadpool->function(&argsf);

    }
}


void threadpool_init(threadpool_t *threadpool, uint numThreads, void (*function)(void *)){
    threadpool->numThreads = numThreads;
    threadpool->function = function;
    threadpool->threads = (pthread_t *)malloc(sizeof(pthread_t) * numThreads);
    threadpool->indexIn = 0;
    threadpool->indexOut = 0;

    sem_init(&(threadpool->semProducer), 0, TBUFFER_SIZE);
    sem_init(&(threadpool->semConsumer), 0, 0);
    pthread_mutex_init(&(threadpool->mutexBuffer), NULL);

    args_t *arg = (args_t *)malloc(sizeof(args_t) * numThreads);

    for (uint i = 0; i < numThreads; i++){
        arg[i].threadnumber = i;
        arg[i].tpool = threadpool;
        pthread_create(&(threadpool->threads[i]), NULL, threadpool_thread, &(arg[i]));
    }
}

void threadpool_add(threadpool_t *threadpool, void *data){
    //Try to put the task in the buffer
    sem_wait(&(threadpool->semProducer));
    pthread_mutex_lock(&(threadpool->mutexBuffer));
    threadpool->buffer[threadpool->indexIn] = data;
    threadpool->indexIn = (threadpool->indexIn + 1) % TBUFFER_SIZE;
    pthread_mutex_unlock(&(threadpool->mutexBuffer));
    sem_post(&(threadpool->semConsumer));

}

void threadpool_free(threadpool_t *threadpool){
    
    for (uint i = 0; i < threadpool->numThreads; i++){
        pthread_cancel(threadpool->threads[i]);
    }
    free(threadpool->threads);
    sem_destroy(&(threadpool->semProducer));
    sem_destroy(&(threadpool->semConsumer));
    pthread_mutex_destroy(&(threadpool->mutexBuffer));
}