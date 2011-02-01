#ifndef _MUTEX_H
#define _MUTEX_H

/* <bp> */
/* </bp> */

#include <pthread.h>

class Condition;

class Mutex {
    public:
        Mutex( );
        ~Mutex( );
        void lock( );
        void unlock( );
    protected:
        pthread_mutex_t mut;
        friend class Condition;
};

class MutexLock {
    public:
        MutexLock(Mutex &mut);
        void force_unlock( );
        ~MutexLock( );
    protected:
        Mutex *_mut;
        bool locked;
};

#endif
