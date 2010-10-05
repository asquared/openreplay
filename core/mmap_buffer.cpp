/*
 * mmap_buffer.cpp
 *
 * Andrew Armenia
 * 
 * This file is part of openreplay. See the README file for the license
 * terms governing its use.
 */

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
#include <errno.h>

/* cycles of the spinlock loop before we think to write it off as a dead process */
#define DEADLOCK_THRESHOLD 1000000

/*
 * appropriate given the hour of the night at which I wrote most of this...
 */
#define MAGIC 0xdecafbad


/* 
 * Important assumptions which are pervasive in this code: 
 *
 * Only one process ever writes to the ring buffer at a time. (ingest process)
 * Only access to the metadata is locked. If the buffer is large enough, data
 * will never be read and written at the same time.
 */
 

MmapBuffer::MmapBuffer(const char *file, unsigned int record_size, bool reset) {
    struct stat statbuf;

    data_fd = -1;
    mmapped_ipc = NULL;
    mmapped_data = NULL;

    my_pid = getpid( );

    if (record_size % RINGBUF_ALIGN_BOUNDARY != 0) {
        // round up to the next highest aligned size
        record_size = (record_size / RINGBUF_ALIGN_BOUNDARY + 1) * RINGBUF_ALIGN_BOUNDARY;
    }


    // now open the data file
    data_fd = open(file, O_RDWR);
    if (data_fd < 0) {
        throw std::runtime_error("Failed to open data file");
    }

    if (fstat(data_fd, &statbuf) < 0) {
        throw std::runtime_error("fstat on data file failed");
    }

    /* mmap() the control data structure first */
    mmapped_ipc = (struct control_data *)
        mmap(NULL, sizeof(struct control_data), PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);

    if (mmapped_ipc == MAP_FAILED) {\
        perror("mmap");
        throw std::runtime_error("Failed to mmap shared state");
    }


    if (mmapped_ipc->magic != MAGIC || reset) {
        // we're the first one here. we must be the source...
        // note this also will forcibly unlock any old locks... for better or worse.
        mmapped_ipc->lock_pid = my_pid;
        mmapped_ipc->magic = MAGIC;
        mmapped_ipc->record_size = record_size;
        mmapped_ipc->current_timecode = -1;
        mmapped_ipc->current_offset = 0;
        mmapped_ipc->max_offset = (statbuf.st_size - RINGBUF_ALIGN_BOUNDARY);
        mmapped_ipc->lock_pid = 0;
    }

    n_records = mmapped_ipc->max_offset / mmapped_ipc->record_size;

    /* 
     * align data starting at 4k (page boundary) into the file
     */
    mmapped_data = (char *)
        mmap(0, mmapped_ipc->max_offset, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, RINGBUF_ALIGN_BOUNDARY);

    if (mmapped_data == MAP_FAILED) {
        perror("mmap");
        throw std::runtime_error("Failed to mmap data buffer");         
    }

    if (madvise((void *)mmapped_data, mmapped_ipc->max_offset, MADV_SEQUENTIAL) < 0) {
        perror("warning: madvise failed");
    }


}

// Clean up the memory mappings.
MmapBuffer::~MmapBuffer( ) {
    if (0 != mmapped_data) {
        munmap((void *)mmapped_data, mmapped_ipc->max_offset);
    }

    if (0 != mmapped_ipc) {
        munmap((void *)mmapped_ipc, sizeof(struct control_data));
    }

    if (-1 != data_fd) {
        close(data_fd);
    }
}

void MmapBuffer::on_fork( ) {
    my_pid = getpid( );
}

void MmapBuffer::lock( ) {
    int counter = 0;
    while (__sync_bool_compare_and_swap(&(mmapped_ipc->lock_pid), 0, my_pid)) {
        ++counter;
        if (counter == DEADLOCK_THRESHOLD) {
            fprintf(stderr, "maybe deadlocked (held by pid %d)\n", mmapped_ipc->lock_pid);
        }
        if (counter > DEADLOCK_THRESHOLD) {
            check_lock( );
        }
    }
}

void MmapBuffer::check_lock( ) {
    char buffer[256];
    struct stat st;
    int ret;

    pid_t holding_pid = mmapped_ipc->lock_pid;
    snprintf(buffer, sizeof(buffer) - 1, "/proc/%d", holding_pid);
    buffer[sizeof(buffer) - 1] = 0;

    ret = stat(buffer, &st);

    if (ret < 0) {
        // ENOENT = process not alive
        if (errno == ENOENT) {
            fprintf(stderr, "process %d holding lock seems dead, trying to forcibly unlock...\n", holding_pid);
            if (__sync_bool_compare_and_swap(&(mmapped_ipc->lock_pid), holding_pid, 0)) {
                fprintf(stderr, "... and, we're back!\n");
            } else {
                fprintf(stderr, "someone else got there first. Oh well.\n");
            }
        }
    }
}

void MmapBuffer::unlock( ) {
    mmapped_ipc->lock_pid = 0;
}

timecode_t MmapBuffer::put(const void *data, size_t size) {
    timecode_t save_timecode = mmapped_ipc->current_timecode;
    offset_t save_offset = mmapped_ipc->current_offset;
    struct record *rec;

    /* compute the offset and timecode values for the new frame */
    save_timecode++;
    save_offset += mmapped_ipc->record_size;
    if (save_offset > mmapped_ipc->max_offset - mmapped_ipc->record_size) {
        save_offset = 0;
    }

    /* copy the data into the buffer */
    rec = (struct record *)(mmapped_data + save_offset);
    rec->length = size;
    rec->valid = true;
    rec->timecode = save_timecode;
    memcpy(rec->data, data, size);

    /* update the pointer and timecode values */
    lock( );
    mmapped_ipc->current_offset = save_offset;
    mmapped_ipc->current_timecode = save_timecode;
    unlock( );
    return mmapped_ipc->current_timecode;
}

bool MmapBuffer::get(void *data, size_t *size, timecode_t timecode) {
    struct record *rec;
    
    lock( );

    if (
        mmapped_ipc->current_timecode < timecode 
        || mmapped_ipc->current_timecode - n_records > timecode 
        || mmapped_ipc->current_timecode == -1
        || timecode < 0
    ) {
        unlock( );
        return false;
    }


    long long offset = 
        mmapped_ipc->current_offset 
        - ((long long)(mmapped_ipc->current_timecode - timecode) 
            * mmapped_ipc->record_size);

    if (offset < 0) {
        offset += n_records * mmapped_ipc->record_size;
    }       

    rec = (struct record *)(mmapped_data + offset);

    if (rec->length < *size) {
        *size = rec->length;
    }

    // got the wrong data somehow (maybe it was overwritten just now)
    if (rec->timecode != timecode || !rec->valid) {
        unlock( );
        return false;
    }

    unlock( );


    memcpy(data, rec->data, *size);
    return true;
}

int MmapBuffer::get_timecode(void) {
    return mmapped_ipc->current_timecode - 1;
}
