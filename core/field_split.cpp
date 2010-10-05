#include "pipe_buffer.h"
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

void usage(void) {
    fprintf(stderr, "usage: \n");
    fprintf(stderr, "-l, --linesize: specify size of one scanline "
        "in bytes\n");
    fprintf(stderr, "-h, --height: specify height of image in scanlines"
        " (must be even)\n");
    fprintf(stderr, "-o, --odd-dominance: assume odd field dominance"
        " instead of even");

}


void write_all_to_stdout(uint8_t *data, int len) {
    int ret;
    while (len > 0) {
        ret = write(STDOUT_FILENO, data, len);

        if (ret > 0) {
            data += ret;
            len -= ret;
        } else if (ret == 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                perror("write wouldblock?");
                exit(2);
            }
        } else {
            if (errno == EINTR) {
                continue;
            } else {
                perror("write");
                exit(2);
            }
        }
    }
}

void write_interlaced_lines(uint8_t *frame, int start, int linesize, int height) {
    int i;
    for (i = start; i < height; i += 2) {
        write_all_to_stdout(frame + linesize * i, linesize);
    }
}


int main(int argc, char **argv) {
    int ret;

    /* frame size information (assumes packed pixel format!) */
    int linesize = 1440, height = 480, odd_dominance = 0;

    struct option opts[] = {
        {
            name: "linesize",
            has_arg: 1,
            flag: NULL,
            val: 'l'
        },
        {
            name: "height",
            has_arg: 1,
            flag: NULL,
            val: 'h'
        },
        {
            name: "odd-dominance",
            has_arg: 0,
            flag: NULL,
            val: 'o'
        },
        {0}
    };

    while ((ret = getopt_long(argc, argv, "l:h:o", opts, NULL)) != EOF) {
        switch (ret) {
            case '?':
                usage( );
                break;
            case ':':
                fprintf(stderr, "missing a required argument option\n");
                break;
            case 'l':
                linesize = atoi(optarg);
                if (linesize <= 0) {
                    fprintf(stderr, "linesize requires positive integer argument\n");
                    exit(1);
                }
                break;
            case 'h':
                height = atoi(optarg); 
                if (height <= 0) {
                    fprintf(stderr, "height requires positive even integer argument\n");
                    exit(1);
                }
                if (height % 2 != 0) {
                    fprintf(stderr, "height must be even\n");
                    exit(1);
                }
                break;
            case 'o':
                odd_dominance = 1;
                break;
        }
    }

    PipeBuffer input(STDIN_FILENO, linesize * height);
    PipeBuffer *buffers[] = { &input };
    uint8_t *frame;
    int start, ptr, wptr;
    
    while (!input.stream_finished( )) {
        PipeBuffer::update(buffers, 1);

        frame = input.get_next_block( );

        if (frame) {
            if (odd_dominance) {
                write_interlaced_lines(frame, 1, linesize, height);
                write_interlaced_lines(frame, 0, linesize, height);
            } else {
                write_interlaced_lines(frame, 0, linesize, height);
                write_interlaced_lines(frame, 1, linesize, height);
            }
           
            input.done_with_block(frame);
        }
    }
}
