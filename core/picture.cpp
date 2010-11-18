#include "picture.h"

#include <stdlib.h>
#include <stdexcept>
#include <string.h>
#include <assert.h>
#include <malloc.h> // memalign

#define align_malloc malloc
#define align_realloc realloc

/* 16 works out nicely on scanline boundaries but we can go higher*/
#define ALIGN_ON 64

#define FREELIST_MAX 16

Picture::Picture( ) {
    data = NULL;
}

void Picture::alloc_data(size_t size) {
    if (data) {
        ::free(data);
    }
    data = (uint8_t *)memalign(ALIGN_ON, size);
    alloc_size = size;
}

Picture *Picture::alloc(uint16_t w, uint16_t h, uint16_t line_pitch,
        enum pixel_format pix_fmt) {
    Picture *candidate;
    size_t pic_size = h * line_pitch;

    // See if we can get something off the free list.
    if (!free_list.empty( )) {
        candidate = free_list.front( );
        free_list.pop_front( );
    } else {
        candidate = new Picture;
    }

    candidate->w = w;
    candidate->h = h;
    candidate->line_pitch = line_pitch;
    candidate->pix_fmt = pix_fmt;
    candidate->alloc_data(pic_size);
    return candidate;
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

        case YUV8:
            return to_yuv8( );

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

        case YUV8:
            return yuv8_to_uyvy8( );

        default:
            throw std::runtime_error("Cannot convert this format to UYVY8");
    }
}

Picture *Picture::to_yuv8(void) {
    switch (this->pix_fmt) {
        case YUV8:
            return Picture::copy(this);

        case UYVY8:
            return uyvy8_to_yuv8( );

        default:
            throw std::runtime_error("Cannot convert this format to UYVY8");
    }
}

#define CLAMP(x) ( (x < 256) ? x : 0 )
#define SCLAMP(x) ( (x > 0) ? CLAMP(x) : 0 )

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

/* THIS IS BROKEN and results in video artifacts */
/* (maybe not anymore??) */
Picture *Picture::uyvy8_to_rgb8(void) {
    int i, j;
    int16_t r, g, b;
    uint8_t u, y1, v, y2;
    Picture *out = Picture::alloc(this->w, this->h, 3*this->w, RGB8);
    uint8_t *in_ptr, *out_ptr;

    for (i = 0; i < this->h; i++) {
        in_ptr = this->scanline(i);
        out_ptr = out->scanline(i);

        for (j = 0; j < this->w; j += 2) {
            u = *in_ptr++;
            y1 = *in_ptr++;
            v = *in_ptr++;
            y2 = *in_ptr++;

            r = (298 * y1 + 409 * v) / 256 - 223;
            g = (298 * y1 - 100 * u - 208 * v) / 256 + 135;
            b = (298 * y1 + 516 * u) / 256 - 277;

            *out_ptr++ = SCLAMP(r);
            *out_ptr++ = SCLAMP(g);
            *out_ptr++ = SCLAMP(b);

            r = (298 * y2 + 409 * v) / 256 - 223;
            g = (298 * y2 - 100 * u - 208 * v) / 256 + 135;
            b = (298 * y2 + 516 * u) / 256 - 277;

            *out_ptr++ = SCLAMP(r);
            *out_ptr++ = SCLAMP(g);
            *out_ptr++ = SCLAMP(b);
        }
    }

    return out;
}

Picture *Picture::uyvy8_to_yuv8(void) {
    int i, j;
    int16_t r, g, b;
    uint8_t u, y1, v, y2;
    Picture *out = Picture::alloc(this->w, this->h, 3*this->w, YUV8);
    uint8_t *in_ptr, *out_ptr;

    for (i = 0; i < this->h; i++) {
        in_ptr = this->scanline(i);
        out_ptr = out->scanline(i);

        for (j = 0; j < this->w; j+=2) {
            u = *in_ptr++;
            y1 = *in_ptr++;
            v = *in_ptr++;
            y2 = *in_ptr++;

            *out_ptr++ = y1;
            *out_ptr++ = u;
            *out_ptr++ = v;

            *out_ptr++ = y2;
            *out_ptr++ = u;
            *out_ptr++ = v;
        }
    }

    return out;
}

Picture *Picture::yuv8_to_uyvy8(void) {
    int i, j;
    uint16_t u, v;
    uint8_t y1, y2;

    Picture *out = Picture::alloc(this->w, this->h, 2*this->w, UYVY8);
    uint8_t *in_ptr, *out_ptr;

    for (i = 0; i < this->h; i++) {
        in_ptr = this->scanline(i);
        out_ptr = out->scanline(i);

        for (j = 0; j < this->w; j++) {
            y1 = *in_ptr++;
            u = *in_ptr++;
            v = *in_ptr++;
            y2 = *in_ptr++;
            u += *in_ptr++;
            v += *in_ptr++;

            u /= 2;
            v /= 2;
    
            *out_ptr++ = u;
            *out_ptr++ = y1;
            *out_ptr++ = v;
            *out_ptr++ = y2;
        }
    }

    return out;
}


std::list<Picture *> Picture::free_list;
