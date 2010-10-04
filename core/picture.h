#ifndef _PICTURE_H
#define _PICTURE_H

#include <list>
#include <stdint.h>

enum pixel_format {
    RGB8, UYVY8
};

class Picture {
    public:
        uint8_t *data;
        uint16_t w, h, line_pitch;
        /* private */
        uint16_t alloc_size;
        enum pixel_format pix_fmt;

        inline uint8_t *scanline(int n) {
            return data + line_pitch * n;
        }

        static Picture *alloc(uint16_t w, uint16_t h, uint16_t line_pitch,
            enum pixel_format pix_fmt = RGB8);
        static Picture *copy(Picture *src);
        static void free(Picture *pic);
        
        Picture *convert_to_format(enum pixel_format pix_fmt);
    protected:
        Picture *to_rgb8(void);
        Picture *to_uyvy8(void);

        Picture *rgb8_to_uyvy8(void);
        Picture *uyvy8_to_rgb8(void);

        static std::list<Picture *> free_list;
};

#endif
