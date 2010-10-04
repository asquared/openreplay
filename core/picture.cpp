#include "picture.h"

#include <stdlib.h>
#include <stdexcept>
#include <string.h>
#include <assert.h>

#define align_malloc malloc
#define align_realloc realloc

#define FREELIST_MAX 16

Picture *Picture::alloc(uint16_t w, uint16_t h, uint16_t line_pitch,
        enum pixel_format pix_fmt) {
    Picture *candidate;
    size_t pic_size = h * line_pitch;

    // See if we can get something off the free list.
    if (!free_list.empty( )) {
        candidate = free_list.front( );
        free_list.pop_front( );

        candidate->w = w;
        candidate->h = h;
        candidate->line_pitch = line_pitch;
        candidate->pix_fmt = pix_fmt;

        if (candidate->alloc_size >= pic_size) {
            return candidate;
        } else {
            candidate->data = 
                (uint8_t *)align_realloc(candidate->data, pic_size);
            if (!candidate->data) {
                throw std::runtime_error("Picture resizing failed!\n");
            }
            candidate->alloc_size = pic_size;
            return candidate;
        }
    } else {
        candidate = new Picture;
        candidate->w = w;
        candidate->h = h;
        candidate->line_pitch = line_pitch;
        candidate->pix_fmt = pix_fmt;
        candidate->data = 
            (uint8_t *)align_malloc(pic_size);        
        if (!candidate->data) {
            throw std::runtime_error("New picture allocation failed!\n");
        }
        candidate->alloc_size = pic_size;
        return candidate;
    }
}

Picture *Picture::copy(Picture *src) {
    Picture *dest = Picture::alloc(src->w, src->h, src->line_pitch, src->pix_fmt);
    memcpy(dest->data, src->data, src->h * src->line_pitch);
    return dest;
}

void Picture::free(Picture *pic) {
    if (free_list.size( ) >= FREELIST_MAX) {
        ::free(pic->data);
        delete pic;
    } else {
        free_list.push_back(pic);
    }
}

Picture *Picture::convert_to_format(enum pixel_format pix_fmt) {
    switch (pix_fmt) {
        case RGB8:
            return to_rgb8( );
        
        case UYVY8:
            return to_uyvy8( );

        default:
            throw std::runtime_error("Unknown pixel format requested");
    }
}

Picture *Picture::to_rgb8(void) {
    switch (this->pix_fmt) {
        case RGB8:
            return Picture::copy(this);

        case UYVY8:
            return uyvy8_to_rgb8( );

        default:
            throw std::runtime_error("Cannot convert this format to RGB8");
    }
}

Picture *Picture::to_uyvy8(void) {
    switch (this->pix_fmt) {
        case RGB8:
            return rgb8_to_uyvy8( );

        case UYVY8:
            return Picture::copy(this);

        default:
            throw std::runtime_error("Cannot convert this format to UYVY8");
    }
}

/* used for pixel format conversions */
static uint8_t vector_multiply(uint8_t *src, const float vector[]) {
    /* Apply the Optimization Process(tm) here first. */
    float result;
    result = src[0] * vector[0] + src[1] * vector[1] + src[2] * vector[2];
    if (result < 0) {
        return 0;
    } else if (result > 255) {
        return 255;
    } else {
        return (uint8_t) result;
    }

}

Picture *Picture::rgb8_to_uyvy8(void) {
    int i, j;
    uint8_t r, g, b;
    uint16_t y1, y2, u, v;

    /* UYVY8 = 4 bytes/2 pixels (w must be even) */
    assert(this->w % 2 == 0);
    Picture *out = Picture::alloc(this->w, this->h, 2*this->w, UYVY8);
    uint8_t *pix_ptr, *out_ptr;

    for (i = 0; i < this->h; i++) {
        pix_ptr = this->scanline(i);
        out_ptr = out->scanline(i);
        for (j = 0; j < this->w; j += 2) {            
            r = *pix_ptr++;
            g = *pix_ptr++;
            b = *pix_ptr++;

            y1 = 16 + (r * 66 + g * 129 + b * 25) / 256;
            u = 128 + (b * 112 - g * 74 - r * 37) / 256;
            v = 128 + (r * 112 - g * 94 - b * 18) / 256;

            r = *pix_ptr++;
            g = *pix_ptr++;
            b = *pix_ptr++;

            y2 = 16 + (r * 66 + g * 129 + b * 25) / 256;
            u += 128 + (b * 112 - g * 74 - r * 37) / 256;
            v += 128 + (r * 112 - g * 94 - b * 18) / 256;
            
            u >>= 1;
            v >>= 1;

            *out_ptr++ = u;
            *out_ptr++ = y1;
            *out_ptr++ = v;
            *out_ptr++ = y2;
        }
    }
    
    return out;
}

Picture *Picture::uyvy8_to_rgb8(void) {
    int i, j;
    uint16_t r, g, b;
    uint8_t u, y1, v, y2;
    Picture *out = Picture::alloc(this->w, this->h, 3*this->w, RGB8);
    uint8_t *in_ptr, *out_ptr;

    for (i = 0; i < this->h; i++) {
        u = *in_ptr++;
        y1 = *in_ptr++;
        v = *in_ptr++;
        y2 = *in_ptr++;

        r = (298 * y1 + 409 * v) / 256 - 223;
        g = (298 * y1 - 100 * u - 208 * v) / 256 + 135;
        b = (298 * y1 + 516 * v) / 256 - 277;

        *out_ptr++ = r;
        *out_ptr++ = g;
        *out_ptr++ = b;

        r = (298 * y2 + 409 * v) / 256 - 223;
        g = (298 * y2 - 100 * u - 208 * v) / 256 + 135;
        b = (298 * y2 + 516 * v) / 256 - 277;

        *out_ptr++ = r;
        *out_ptr++ = g;
        *out_ptr++ = b;
    }

    return out;
}


std::list<Picture *> Picture::free_list;
