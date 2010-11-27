#ifndef _MMAP_STATE_H
#define _MMAP_STATE_H

#include <semaphore.h>
#include <stdint.h>

class MmapState {
    public:
        MmapState(const char *file);
        ~MmapState( ); 
        void put(const void *data, size_t size);
        void get(void *data, size_t size);
        void on_fork(void);
    
    private:
        volatile char *mmapped_data;

        // mmap this thing into the buffer file so that we have shared data among all processes
        volatile struct control_data {
            uint32_t magic;
            pid_t lock_pid;
        } *mmapped_ipc;

        void lock( );
        void unlock( );
        void check_lock( );

        int data_fd;
        size_t max_data;

        pid_t my_pid; // fork( ) unsafe
};

#endif
