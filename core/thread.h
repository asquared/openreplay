#ifndef _THREAD_H
#define _THREAD_H

#include <pthread.h>
#include <sys/types.h>

class Thread {
    public:
        Thread( );
        ~Thread( );
        void start(void);
        void join(void);
        pid_t id(void);
    
    protected:
        virtual void run(void);

    private:
        pthread_t thread;
        static void *start_routine(void *arg);
};

#endif
