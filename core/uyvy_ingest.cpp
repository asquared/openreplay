#include "mjpeg_config.h"
#include "mjpeg_frame.h"
#include "mmap_buffer.h"
#include "picture.h"
#include "stats.h"
#include "mmap_state.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
    MJPEGEncoder enc;
    EncodeStats stats(29.97);

    if (argc < 2) {
        fprintf(stderr, "usage: %s buffer\n", argv[0]);
    }

    // print out some statistics after every 60 frames we finish
    stats.autoprint(60);

    MmapBuffer buf(argv[1], MAX_FRAME_SIZE, true);
    MmapState clock_ipc("clock_ipc");

    /* DV = 720x480, capture card = 720x486 */
    int frame_w = 720, frame_h = 480;

    Picture *input = Picture::alloc(frame_w, frame_h, 2*frame_w, UYVY8);
    
    size_t n_bytes = frame_w * frame_h * 2;
    size_t read_so_far = 0;
    ssize_t n_read = 0;

    for (;;) {
        if (read_so_far < n_bytes) {
            // read more data
            n_read = read(STDIN_FILENO, input->data + read_so_far, 
                n_bytes - read_so_far);

            if (n_read < 0) {
                perror("read");
                if (errno != EAGAIN && errno != EINTR) {
                    exit(1);
                }
            } else if (n_read == 0) {
                fprintf(stderr, "EOF?");
                exit(0);
            } else {
                stats.input_bytes(n_read);
                read_so_far += n_read;
            }
        } else {
            // encode and store the data
            mjpeg_frame *frm = enc.encode_full(input, true);

            // scoreboard clock input
            clock_ipc.get(&frm->clock, sizeof(frm->clock));

            buf.put(frm, sizeof(struct mjpeg_frame) + frm->f1size);
            stats.output_bytes(frm->f1size);
            stats.finish_frames(1);
            read_so_far = 0;
        }
    }
}
