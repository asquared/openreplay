#ifndef _MJPEG_FRAME_H
#define _MJPEG_FRAME_H

#include <stdio.h>
#include <stdlib.h>
#include "jpeglib.h"

#include <stdint.h>
#include <list>

#include "picture.h"

#include <stdexcept>

struct mjpeg_frame {
    bool interlaced;
    bool odd_dominant;
    size_t f1size;
    size_t f2size;
    uint8_t data[0];
};

class MJPEGEncoder {
    public:
        MJPEGEncoder( );
        mjpeg_frame *encode_full(Picture *pict, bool odd_dominant);
        mjpeg_frame *encode_fields(Picture *f1, Picture *f2, bool odd_dominant);
        
        void set_quality(int quality) {
            if (quality < 0 || quality > 100) {
                throw std::runtime_error("Invalid quality value supplied");
            }
            this->quality = quality;
        }

        ~MJPEGEncoder( );
    protected:
        mjpeg_frame *out_frame;
        struct jpeg_compress_struct cinfo;
        struct jpeg_error_mgr jerr;
        size_t alloc_size;
        int quality;

};

class MJPEGDecoder {
    public:
        MJPEGDecoder( );
        ~MJPEGDecoder( );
        Picture *decode_full(struct mjpeg_frame *frame, enum pixel_format fmt = RGB8);
        Picture *decode_first(struct mjpeg_frame *frame, enum pixel_format fmt = RGB8);
        Picture *decode_second(struct mjpeg_frame *frame, enum pixel_format fmt = RGB8);
        /* Scan doubling - e.g. for smooth slow motion */
        Picture *decode_first_doubled(struct mjpeg_frame *frame, enum pixel_format fmt = RGB8);
        Picture *decode_second_doubled(struct mjpeg_frame *frame, enum pixel_format fmt = RGB8);
    protected:
        /* These return a full-frame Picture given one field. */
        Picture *scan_double_up(Picture *in);
        Picture *scan_double_down(Picture *in);
        /* These operate in place on a Picture that already has both fields. */
        /* Use even scanlines to compute scan doubled picture. Discard the odd ones. */
        void scan_double_full_frame_even(Picture *p); 
        /* Use odd scanlines to compute scan doubled picture. Discard the even ones. */
        void scan_double_full_frame_odd(Picture *p); 

        Picture *decode(void *data, size_t len, enum pixel_format fmt = RGB8);
        Picture *weave(Picture *even, Picture *odd);
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
};

#endif
