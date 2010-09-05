/*
 * mjpeg_ingest.cpp
 * 
 * Andrew Armenia
 *
 * This file is part of openreplay. Please read the README file for
 * its license terms.
 *
 * Takes an mjpeg stream (any resolution or frame rate really)
 * on stdin and buffers frames into a ring buffer
 * (buffer control and data files specified on the command line).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <memory.h>

#include "mjpeg_config.h"
#include "mmap_buffer.h"

MmapBuffer *buffer;

int main(int argc, char *argv[])
{
    /* Allocate a buffer big enough for one frame. */
    unsigned char *buf = (unsigned char *)malloc(MAX_FRAME_SIZE);

    int buf_ptr = 0;
    int n_read;
    int scan_start, j, start_of_frame = -1;
    int move_start = -1;
    int frame_count = 0;

    if (argc < 2) {
            fprintf(stderr, "usage: %s buffer_file\n", argv[0]);
            return 1;
    }

    /* Open the ring buffer files. */
    buffer = new MmapBuffer(argv[1], MAX_FRAME_SIZE); 

    /* 
     * Parse stdin, looking for jpeg markers. 
     * 0xffd8 indicates start of frame.
     * 0xffd9 indicates snd of frame.
     */
    for (;;) {
        n_read = read(STDIN_FILENO, buf + buf_ptr, MAX_FRAME_SIZE - buf_ptr);
        
        if (n_read < 0) {
            //fprintf(stderr, "Error!\n");
            perror("read stdin");
            return 1; 
        } else if (n_read == 0) {
            fprintf(stderr, "End of input\n");
            return 0;
        }

        buf_ptr += n_read;

        for (j = 0; j < buf_ptr; ++j) {
            if (buf[j] == 0xff) {
                if (buf[j + 1] == 0xd8) {
                    start_of_frame = j;
                } else if (buf[j + 1] == 0xd9 && start_of_frame != -1) {
                    /* Got a whole frame! Put it into the buffer. */
                    buffer->put(buf + start_of_frame, j + 2 - start_of_frame);
                    start_of_frame = -1;
                    move_start = j + 2;
                }
            }
        }

        if (move_start != -1) {
            if (move_start < MAX_FRAME_SIZE) {
                memmove(buf, buf + move_start, MAX_FRAME_SIZE - move_start);
                buf_ptr -= move_start;
                move_start = -1;
            } else {
                buf_ptr = 0; // everything was consumed
            }
        } 
    }
}
