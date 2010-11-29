/*
 * mmap_state.cpp
 *
 * Andrew Armenia
 * 
 * This file is part of openreplay. See the README file for the license
 * terms governing its use.
 */

#include "mmap_state.h"
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
#include <assert.h>

/* cycles of the spinlock loop before we think to write it off as a dead process */
#define DEADLOCK_THRESHOLD 1000000

/*
 * appropriate given the hour of the night at which I wrote most of this...
 */
#define MAGIC 0xdecafbad



MmapState::MmapState(const char *file) {
    struct stat statbuf;

    data_fd = -1;
    mmapped_ipc = NULL;

    my_pid = getpid( );

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
        mmap(
            NULL, statbuf.st_size, 
            PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0
        );

    if (mmapped_ipc == MAP_FAILED) {\
        perror("mmap");
        throw std::runtime_error("Failed to mmap shared state");
    }


    if (mmapped_ipc->magic != MAGIC) {
        // we're the first one here. we must be the source...
        // note this also will forcibly unlock any old locks... for better or worse.
        mmapped_ipc->lock_pid = my_pid;
        mmapped_ipc->magic = MAGIC;
    }

    max_data = statbuf.st_size - sizeof(struct control_data);
}

// Clean up the memory mappings.
MmapState::~MmapState( ) {
    if (0 != mmapped_ipc) {
        munmap((void *)mmapped_ipc, max_data + sizeof(struct control_data));
    }

    if (-1 != data_fd) {
        close(data_fd);
    }
}

void MmapState::on_fork( ) {
    my_pid = getpid( );
}

void MmapState::lock( ) {
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

void MmapState::check_lock( ) {
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

void MmapState::unlock( ) {
    mmapped_ipc->lock_pid = 0;
}

void MmapState::put(const void *data, size_t size) {
    assert(size < max_data);

    lock( );
    /* to cast away volatile is OK since we're locked here */
    memcpy((void *)mmapped_ipc->data, data, size);
    unlock( );
}

void MmapState::get(void *data, size_t size) {
    assert(size < max_data);

    lock( );
    memcpy(data, (void *)mmapped_ipc->data, size);
    unlock( );
}

