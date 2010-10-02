#include <stdio.h>
#include <stdlib.h>

#include "mjpeg_frame.h"

/* decode a jpeg file (<1Meg) to RGB888 packed data */
#define MAX_DATA_SIZE 1048576
int main(int argc, char **argv) {
    MJPEGDecoder dec;

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

    /* decode it to a progressive frame */
    struct picture *pic = dec.decode_full(contrived_frame);

    fprintf(stderr, "decoded image: %dx%d\n", pic->w, pic->h);
    /* write decoded image to stdout */
    fwrite(pic->data, 1, pic->h * pic->line_pitch, stdout);

    struct picture *pic2 = dec.decode_full(contrived_frame);
    fprintf(stderr, "decoded again: %dx%d\n", pic->w, pic->h);
    fwrite(pic->data, 1, pic->h * pic->line_pitch, stdout);

    return 0;
}
