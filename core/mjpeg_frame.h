#ifndef _MJPEG_FRAME_H
#define _MJPEG_FRAME_H

#include <stdio.h>
#include <stdlib.h>
#include "jpeglib.h"

#include <stdint.h>
#include <list>

struct picture {
    uint8_t *data;
    uint16_t w, h, line_pitch;
    /* private */
    uint16_t alloc_size;
};

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
        struct picture *decode_full(struct mjpeg_frame *frame);
        struct picture *decode_first(struct mjpeg_frame *frame);
        struct picture *decode_second(struct mjpeg_frame *frame);
        /* Scan doubling */
        struct picture *scan_double(struct picture *in) { return 0; }
        struct picture *decode_first_doubled(struct mjpeg_frame *frame) { return 0; }
        struct picture *decode_second_doubled(struct mjpeg_frame *frame) { return 0; }
        void free_picture(struct picture *pic);
    protected:
        struct picture *decode(void *data, size_t len);
        struct picture *weave(struct picture *even, struct picture *odd);
        struct picture *alloc_picture(uint16_t w, uint16_t h, uint16_t line_pitch);
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;

        std::list<struct picture *> free_pictures;
};

#endif
