/* <bp> */
/* </bp> */

#include "condition.h"
#include <stdexcept>

Condition::Condition( ) {
    if (pthread_cond_init(&cond, NULL) != 0) {
        throw std::runtime_error("Failed to create pthread condition variable");
    }
}

Condition::~Condition( ) {
    if (pthread_cond_destroy(&cond) != 0) {
        throw std::runtime_error("Failed to destroy condition variable");
    }
}

void Condition::wait(Mutex &mut) {
    if (pthread_cond_wait(&cond, &mut.mut) != 0) {
        throw std::runtime_error("Failed to wait on condition variable");
    }
}

void Condition::signal( ) {
    if (pthread_cond_signal(&cond) != 0) {
        throw std::runtime_error("Failed to signal condition variable");
    }
}

void Condition::broadcast( ) {
    if (pthread_cond_broadcast(&cond) != 0) {
        throw std::runtime_error("Failed to broadcast condition variable");
    }
}

