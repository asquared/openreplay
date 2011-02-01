#include "thread.h"
#include <stdexcept>

#include <stdio.h>
#include <unistd.h>

Thread::Thread( ) {
    /* not much to do here */
}

Thread::~Thread( ) {
    /* neither here */
}

void Thread::start(void) {
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) != 0) {
        throw std::runtime_error("failed to init thread attributes");
    }

    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE) != 0) {
        throw std::runtime_error("failed to make thread joinable");
    }

    if (pthread_create(&thread, &attr, start_routine, (void *) this) != 0) {
        throw std::runtime_error("failed to start thread");
    }

    /* and we're off! */
}

void Thread::join(void) {
    if (pthread_join(thread, NULL) != 0) {
        throw std::runtime_error("failed to join thread");
    }
}

void *Thread::start_routine(void *arg) {
    Thread *thiz = (Thread *)arg;
    thiz->run( ); /* virtual function call to subclass, hopefully */
    return NULL;
}

void Thread::run(void) {
    /* we're boring by default*/
    fprintf(stderr, "running thread with non-overriden run?\n");
}

pid_t Thread::id(void) {
//    return gettid( );
    /* awful nonportable hack */
    return thread;
}
