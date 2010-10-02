#include <stdio.h>
#include <stdlib.h>

#include "mjpeg_frame.h"

#include <sys/time.h>

/* decode a jpeg file (<1Meg) to RGB888 packed data */
#define MAX_DATA_SIZE 1048576
int main(int argc, char **argv) {
    MJPEGDecoder dec;
    struct timeval start, finish;
    int i;
    
    int n_decodes = 100;

    struct mjpeg_frame *contrived_frame 
        = (struct mjpeg_frame *)malloc(sizeof(struct mjpeg_frame) + MAX_DATA_SIZE);

    /* input data is not two interlaced JPEG images but one whole image */
    contrived_frame->interlaced = false;

    /* read input data from file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Failed to open input!\n");
        return 1;
    }

    contrived_frame->f1size = fread(contrived_frame->data, 1, MAX_DATA_SIZE, f);

    fprintf(stderr, "Timing a few thousand decodes of this thing\n");

    gettimeofday(&start, NULL);
    for (i = 0; i < n_decodes; i++) {
        /* decode it to a progressive frame */
        struct picture *pic = dec.decode_full(contrived_frame);

        /* do something useful here */
        fwrite(pic->data, 1, pic->h * pic->line_pitch, stdout);

        dec.free_picture(pic);
    }
    gettimeofday(&finish, NULL);

    fprintf(stderr, "finish time: %d/%d\n", finish.tv_sec, finish.tv_usec);
    fprintf(stderr, "start time: %d/%d\n", start.tv_sec, start.tv_usec);

    return 0;
}
