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
    unsigned char *buf = (unsigned char *)malloc(MAX_FRAME_SIZE);
    int buf_ptr = 0;
    int n_read;
    int scan_start, j, start_of_frame = -1;
    int move_start = -1;
    int frame_count = 0;

    if (argc < 3) {
            fprintf(stderr, "usage: %s control_file data_file\n", argv[0]);
            return 1;
    }

    buffer = new MmapBuffer(argv[1], argv[2], MAX_FRAME_SIZE /* put frames on 1meg markers */); 

    for (;;) {
        n_read = read(STDIN_FILENO, buf + buf_ptr, MAX_FRAME_SIZE - buf_ptr);
        
        if (n_read < 0) {
            //fprintf(stderr, "Error!\n");
            perror("shit's on cocaine");
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
