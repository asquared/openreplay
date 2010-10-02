#ifndef _PICTURE_H
#define _PICTURE_H

#include <list>
#include <stdint.h>

class Picture {
    public:
        uint8_t *data;
        uint16_t w, h, line_pitch;
        /* private */
        uint16_t alloc_size;

        inline uint8_t *scanline(int n) {
            return data + line_pitch * n;
        }

        static Picture *alloc(uint16_t w, uint16_t h, uint16_t line_pitch);
        static void free(Picture *pic);
    protected:
        static std::list<Picture *> free_list;
};

#endif
