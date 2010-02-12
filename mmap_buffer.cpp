#include "mmap_buffer.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#define MAGIC 0xdecafbad

MmapBuffer::MmapBuffer(const char *control_file, const char *data_file, unsigned int record_size, bool reset) {
    struct stat statbuf;

    control_fd = -1;
    data_fd = -1;
    mmapped_ipc = 0;
    mmapped_shit = 0;

    // assume the files are already there...
    control_fd = open(control_file, O_RDWR);
    if (control_fd < 0) {
        throw std::runtime_error("Failed to open the control file");
    }           

    mmapped_ipc = (struct control_data *)
        mmap(0, sizeof(struct control_data), PROT_READ | PROT_WRITE, MAP_SHARED, control_fd, 0);

    if (mmapped_ipc == 0) {
        throw std::runtime_error("Failed to mmap shared state");
    }

    if (mmapped_ipc->magic != MAGIC || reset) {
        // we're the first one here. we must be the source...
        mmapped_ipc->magic = MAGIC;
        mmapped_ipc->record_size = record_size;
        mmapped_ipc->current_timecode = -1;
        mmapped_ipc->current_offset = 0;
        // initialize the semaphore
        sem_init((sem_t *)&mmapped_ipc->sem, 1, 1);
    }

    // now open the data file
    data_fd = open(data_file, O_RDWR);
    if (data_fd < 0) {
        throw std::runtime_error("Failed to open data file");
    }

    if (fstat(data_fd, &statbuf) < 0) {
        throw std::runtime_error("fstat on data file failed");
    }
    
    mmapped_ipc->max_offset = statbuf.st_size;
    n_records = mmapped_ipc->max_offset / mmapped_ipc->record_size;


    mmapped_shit = (char *)
        mmap(0, mmapped_ipc->max_offset, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);

    strcpy((char *)mmapped_shit, "Hello World!");

    if (mmapped_shit == 0) {
        throw std::runtime_error("Failed to mmap data buffer");         
    }
}

// Clean up the memory mappings.
MmapBuffer::~MmapBuffer( ) {
    if (0 != mmapped_shit) {
        munmap((void *)mmapped_shit, mmapped_ipc->max_offset);
    }

    if (0 != mmapped_ipc) {
        munmap((void *)mmapped_ipc, sizeof(struct control_data));
    }

    if (-1 != control_fd) {
        close(control_fd);
    }

    if (-1 != data_fd) {
        close(data_fd);
    }
}

int MmapBuffer::put(const void *data, int size) {
    int save_timecode = mmapped_ipc->current_timecode;
    long long save_offset = mmapped_ipc->current_offset;

    save_timecode++;
    save_offset += mmapped_ipc->record_size;
    if (save_offset > mmapped_ipc->max_offset - mmapped_ipc->record_size) {
        save_offset = 0;
    }

    *(int *)(mmapped_shit + save_offset) = size;
    memcpy((void *)(mmapped_shit + save_offset + sizeof(int)), data, size);

    if (sem_wait((sem_t *)&mmapped_ipc->sem) < 0) {
        perror("Locking semaphore");
    }
    mmapped_ipc->current_offset = save_offset;
    mmapped_ipc->current_timecode = save_timecode;
    if (sem_post((sem_t *)&mmapped_ipc->sem) < 0) {
        perror("Unlocking semaphore");
    }
    return mmapped_ipc->current_timecode;
}

bool MmapBuffer::get(void *data, int *size, int timecode) {
    int actual_size; 
    
    if (sem_wait((sem_t *)&mmapped_ipc->sem) < 0) {
        perror("Locking semaphore");
    }

    if (
        mmapped_ipc->current_timecode < timecode 
        || mmapped_ipc->current_timecode - n_records > timecode 
        || mmapped_ipc->current_timecode == -1
        || timecode < 0
    ) {
        if (sem_post((sem_t *)&mmapped_ipc->sem) < 0) {
            perror("Unlocking semaphore");
        }
        return false;
    }

    long long offset = 
        mmapped_ipc->current_offset 
        - ((long long)(mmapped_ipc->current_timecode - timecode) 
            * mmapped_ipc->record_size);

    if (offset < 0) {
        offset += n_records * mmapped_ipc->record_size;
    }       

    actual_size = *(int *)(mmapped_shit + offset);

    if (actual_size < *size) {
        *size = actual_size;
    }

    if (sem_post((sem_t *)&mmapped_ipc->sem) < 0) {
        perror("Unlocking semaphore");
    }

    memcpy(data, (void *)(mmapped_shit+offset + sizeof(int)), *size);
    return true;
}

int MmapBuffer::get_timecode(void) {
    return mmapped_ipc->current_timecode - 1;
}
