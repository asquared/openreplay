#ifndef _EVENT_HANDLER_H
#define _EVENT_HANDLER_H

#include "mutex.h"
#include "condition.h"
#include <list>
#include <stdint.h>

class EventHandler {
    public:
        EventHandler( );
        ~EventHandler( );
        void post_event(uint32_t event, void *arg, uint32_t priority = 0);
        uint32_t wait_event(void *&arg);

    private:
        struct event_data {
            uint32_t event;
            void *arg;
        };
        std::list<struct event_data> queued_events;
        Condition event_ready;
        Mutex mut;
};

#endif
