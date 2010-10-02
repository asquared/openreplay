#ifndef _MJPEG_FRAME_H
#define _MJPEG_FRAME_H

#include <stdio.h>
#include <stdlib.h>
#include "jpeglib.h"

#include <stdint.h>
#include <list>

#include "picture.h"


struct mjpeg_frame {
    bool interlaced;
    bool odd_dominant;
    size_t f1size;
    size_t f2size;
    uint8_t data[0];
};

class MJPEGDecoder {
    public:
        MJPEGDecoder( );
        ~MJPEGDecoder( );
        Picture *decode_full(struct mjpeg_frame *frame);
        Picture *decode_first(struct mjpeg_frame *frame);
        Picture *decode_second(struct mjpeg_frame *frame);
        /* Scan doubling */
        Picture *scan_double_up(Picture *in);
        Picture *scan_double_down(Picture *in);
        Picture *decode_first_doubled(struct mjpeg_frame *frame);
        Picture *decode_second_doubled(struct mjpeg_frame *frame);
    protected:
        Picture *decode(void *data, size_t len);
        Picture *weave(Picture *even, Picture *odd);
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
};

#endif
