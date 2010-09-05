#ifndef _MMAP_BUFFER_H
#define _MMAP_BUFFER_H

#include <semaphore.h>
#include <stdint.h>

#define RINGBUF_ALIGN_BOUNDARY 4096 /* pages on x86 */

typedef int timecode_t;

class MmapBuffer {
    public:
    MmapBuffer(const char *file, unsigned int record_size, bool reset = false);
    ~MmapBuffer( ); 
    timecode_t put(const void *data, size_t size);
    bool get(void *data, size_t *size, timecode_t timecode);
    timecode_t get_timecode(void);
    
    private:
    volatile char *mmapped_data;

    typedef uint64_t offset_t;
    typedef uint64_t recsize_t;

    struct record {
        size_t length;
        timecode_t timecode;
        bool valid; // should read as zero if the sparse file hasn't been filled yet
        unsigned char data[0]; // seemingly legal only in gcc
    };
    
    // mmap this thing into the buffer file so that we have shared data among all processes
    volatile struct control_data {
            int current_timecode;   
            offset_t current_offset;
            uint32_t magic;
            offset_t max_offset;
            recsize_t record_size;
            sem_t sem;
    } *mmapped_ipc;

    int data_fd;
    int n_records;
};

#endif
