/* <bp> */
/* </bp> */

#include "mutex.h"
#include <pthread.h>
#include <stdexcept>

Mutex::Mutex( ) {
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0) {
        throw std::runtime_error("Failed to initialize mutex attribute");
    }

    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        throw std::runtime_error("Failed to set mutex as recursive");
    }

    if (pthread_mutex_init(&mut, &attr) != 0) {
        throw std::runtime_error("Failed to initialize mutex");
    }

    pthread_mutexattr_destroy(&attr);
}

Mutex::~Mutex( ) {
    if (pthread_mutex_destroy(&mut) != 0) {
        throw std::runtime_error("Failed to destroy mutex");
    }
}

void Mutex::lock( ) {
    if (pthread_mutex_lock(&mut) != 0) {
        throw std::runtime_error("Failed to lock mutex");
    }
}

void Mutex::unlock( ) {
    if (pthread_mutex_unlock(&mut) != 0) {
        throw std::runtime_error("Failed to unlock mutex");
    }
}

MutexLock::MutexLock(Mutex &mut) {
    _mut = &mut;
    _mut->lock( );
    locked = true;
}

void MutexLock::force_unlock( ) {
    _mut->unlock( );
    locked = false;
}

MutexLock::~MutexLock( ) {
    if (locked) {
        _mut->unlock( );
    }
}
