#ifndef _MMAP_BUFFER_H
#define _MMAP_BUFFER_H

#include <semaphore.h>

#define RINGBUF_ALIGN_BOUNDARY 4096 /* pages on x86 */

class MmapBuffer {
    public:
    MmapBuffer(const char *file, unsigned int record_size, bool reset = false);
    ~MmapBuffer( ); 
    int put(const void *data, int size);
    bool get(void *data, int *size, int timecode);
    int get_timecode(void);
    
    private:
    volatile char *mmapped_data;

    struct record {
        unsigned long length;
        int timecode;
        int valid; // should read as zero if the sparse file hasn't been filled yet
        unsigned char data[0]; // seemingly legal only in gcc
    };
    
    // mmap this thing into the buffer file so that we have shared data among all processes
    volatile struct control_data {
            int current_timecode;   
            long long current_offset;
            int magic;
            unsigned long long max_offset;
            int record_size;
            sem_t sem;
    } *mmapped_ipc;

    int data_fd;
    int n_records;
};

#endif
