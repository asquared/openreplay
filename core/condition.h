#ifndef _CONDITION_H
#define _CONDITION_H

/* <bp> */
/* </bp> */

#include "mutex.h"
#include <pthread.h>

class Condition {
    public:
        Condition( );
        ~Condition( );

        void wait(Mutex &mut);
        void signal( );
        void broadcast( );

    protected:
        pthread_cond_t cond;
};

#endif
