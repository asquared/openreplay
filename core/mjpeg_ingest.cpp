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
extern "C" {
    #include <getopt.h>
}

#include "mjpeg_config.h"
#include "mmap_buffer.h"
#include "mjpeg_frame.h"

MmapBuffer *buffer;

void usage(const char *name) {
    fprintf(stderr, "usage: %s [-e|-o|--even-dominant|--odd-dominant] buffer_file\n", name);
    fprintf(stderr, "    -e, --even-dominant: assume input is sequential fields in even-dominant order\n");
    fprintf(stderr, "    -o, --odd-dominant: assume input is sequential fields in odd-dominant order\n");
    fprintf(stderr, "default (with no options): assume input is progressive-scan frames\n");
}

int main(int argc, char *argv[])
{
    /* Allocate a buffer big enough for one frame. */    
    struct mjpeg_frame *frame_buf = (struct mjpeg_frame *)malloc(MAX_FRAME_SIZE);
    uint8_t *buf = (uint8_t *)malloc(MAX_FRAME_SIZE);

    int buf_ptr = 0;
    int n_read;
    int j, start_of_frame = -1;
    int move_start = -1;
    enum { PROGRESSIVE, ODD_DOMINANT, EVEN_DOMINANT } 
        interlacing_mode = PROGRESSIVE;

    /* The dominant field is input (and output, and stored) first. */
    bool dominant_field = true;

    /* getopt stuff */
    const struct option options[] = {
        {
            name: "odd-dominant",
            has_arg: 0,
            flag: NULL,
            val: 'o'
        },
        {
            name: "even-dominant",
            has_arg: 0,
            flag: NULL,
            val: 'e'
        }
    };

    int opt;

    while ((opt = getopt_long(argc, argv, "eo", options, NULL)) != EOF) {
        switch (opt) {
            case 'e':
                if (interlacing_mode == PROGRESSIVE) {
                    interlacing_mode = EVEN_DOMINANT;
                } else {
                    usage(argv[0]);
                    fprintf(stderr, "%s: odd and even dominance don't go together\n", argv[0]);
                    exit(1);
                }
                break;
            case 'o':
                if (interlacing_mode == PROGRESSIVE) {
                    interlacing_mode = ODD_DOMINANT;
                } else {
                    usage(argv[0]);
                    fprintf(stderr, "%s: odd and even dominance don't go together\n", argv[0]);
                    exit(1);
                }
                break;
            default:
                usage(argv[0]);
                exit(1);
        }
    }

    if (argc - optind < 1) {
            usage(argv[0]);
            return 1;
    }

    /* Open the ring buffer files. */
    buffer = new MmapBuffer(argv[optind], MAX_FRAME_SIZE); 

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
                    /* 0xff 0xd8 = JPEG start of image */
                    start_of_frame = j;
                } else if (buf[j + 1] == 0xd9 && start_of_frame != -1) {
                    /* 0xff 0xd9 = JPEG end of image, so process it. */
                    if (dominant_field) {
                        frame_buf->f1size = j + 2 - start_of_frame;
                        frame_buf->f2size = 0;
                        memcpy(frame_buf->data, buf + start_of_frame, frame_buf->f1size);
                        if (interlacing_mode != PROGRESSIVE) {
                            dominant_field = false;
                        }
                    } else {
                        frame_buf->f2size = j + 2 - start_of_frame;
                        memcpy(frame_buf->data + frame_buf->f1size, buf + start_of_frame, frame_buf->f2size);
                        dominant_field = true;
                    }

                    if (interlacing_mode == PROGRESSIVE || dominant_field == false) {
                        /* Put data into buffer if a complete frame (2 fields or 1 progressive frame) is done. */    
                        buffer->put(frame_buf, frame_buf->f1size + frame_buf->f2size + sizeof(*frame_buf));
                        start_of_frame = -1;
                        move_start = j + 2;
                    }
                }
            }
        }

        if (move_start != -1) {
            if (move_start < MAX_FRAME_SIZE) {
                /* this could be slowing us down */
                memmove(buf, buf + move_start, MAX_FRAME_SIZE - move_start);
                buf_ptr -= move_start;
                move_start = -1;
            } else {
                buf_ptr = 0; // everything was consumed
            }
        } 
    }
}
