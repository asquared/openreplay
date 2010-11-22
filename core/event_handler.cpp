#include "event_handler.h"

EventHandler::EventHandler( ) {

}

EventHandler::~EventHandler( ) {

}

void EventHandler::post_event(uint32_t event, void *arg, uint32_t priority = 0) {
    struct event_data evt;
    evt.event = event;
    evt.arg = arg;

    { MutexLock lock(mut);        
        queued_events.push_back(evt);
        event_ready.signal( );
    }
}

uint32_t EventHandler::wait_event(void *&arg) {
    struct event_data ret;

    { MutexLock lock(mut);
        if (queued_events.empty( )) {
            /* wait until something is ready */
            event_ready.wait(mut); 
        }

        ret = queued_events.front( );
        queued_events.pop_front( );
        arg = event.arg;
        return ret.event;
    }
}
