#include "picture.h"

#include <stdlib.h>
#include <stdexcept>
#include <string.h>
#include <assert.h>
#include <malloc.h> // memalign
#include <stdarg.h>

#define align_malloc malloc
#define align_realloc realloc

/* 16 works out nicely on scanline boundaries but we can go higher*/
#define ALIGN_ON 64

#define FREELIST_MAX 16

Picture::Picture( ) {
    data = NULL;
    fprintf(stderr, "NEW PICTURE %p\n", this);
    rcount = 1;
#ifdef HAVE_PANGOCAIRO
    font_description = NULL;
#endif
}

void Picture::addref( ) {
    fprintf(stderr, "ADDREF %p\n", this);
    __sync_add_and_fetch(&rcount, 1);
}

void Picture::alloc_data(size_t size) {
    if (data) {
        fprintf(stderr, "FREE DATA %p\n", this);
        ::free(data);
    }
    fprintf(stderr, "ALLOC DATA %p\n", this);
    data = (uint8_t *)memalign(ALIGN_ON, size);
    alloc_size = size;
}

Picture *Picture::alloc(uint16_t w, uint16_t h, uint16_t line_pitch,
        enum pixel_format pix_fmt) {
    Picture *candidate;
    size_t pic_size = h * line_pitch;

    candidate = new Picture;
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

Picture::~Picture( ) {
    if (data) {
        fprintf(stderr, "FREE DATA %p\n", this);
        ::free(data);
    }

#ifdef HAVE_PANGOCAIRO
    if (font_description) {
        pango_font_description_free(font_description);
    }
#endif
}

void Picture::free(Picture *pic) {
    int new_rcount;
    fprintf(stderr, "DECREF %p\n", pic);
    new_rcount = __sync_sub_and_fetch(&pic->rcount, 1);
    assert(new_rcount >= 0);

    if (new_rcount > 0) {
        return; /* there are still references */
    }

    fprintf(stderr, "FINALIZE %p\n", pic);
    delete pic;
}

Picture *Picture::convert_to_format(enum pixel_format pix_fmt) {
    switch (pix_fmt) {
        case RGB8:
            return to_rgb8( );
        
        case UYVY8:
            return to_uyvy8( );

        case YUV8:
            return to_yuv8( );

        case YUVA8:
            return to_yuva8( );

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

Picture *Picture::to_yuva8(void) {
    switch (this->pix_fmt) {
        case BGRA8:
            return bgra8_to_yuva8( );
        case YUVA8:
            return Picture::copy(this);

        default:
            throw std::runtime_error("Cannot convert this format to YUVA8");
    }

    return NULL; /* suppress a meaningless warning - the switch either returns or throws */
}

#undef CLAMP
#undef SCLAMP
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

Picture *Picture::bgra8_to_yuva8(void) {
    int i, j;
    uint8_t r, g, b, a;
    uint16_t y, u, v;

    Picture *out = Picture::alloc(this->w, this->h, 4*this->w, YUVA8);
    uint8_t *pix_ptr, *out_ptr;

    for (i = 0; i < this->h; i++) {
        pix_ptr = this->scanline(i);
        out_ptr = out->scanline(i);
        for (j = 0; j < this->w; j++) {            
            b = *pix_ptr++;
            g = *pix_ptr++;
            r = *pix_ptr++;
            a = *pix_ptr++;

            y = 16 + (r * 66 + g * 129 + b * 25) / 256;
            u = 128 + (b * 112 - g * 74 - r * 37) / 256;
            v = 128 + (r * 112 - g * 94 - b * 18) / 256;

            *out_ptr++ = y;
            *out_ptr++ = u;
            *out_ptr++ = v;
            *out_ptr++ = a;
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

        for (j = 0; j < this->w; j += 2) {
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

int Picture::pixel_pitch(void) {
    switch (pix_fmt) {
        case A8:
            return 1;

        case UYVY8:
            return 2;

        case RGB8:
        case YUV8:
            return 3;

        default:
            throw std::runtime_error("cannot deal with that pixel format");
            break;
    }
}

/* god awful slow blit routine */
void Picture::draw(Picture *src, uint_fast16_t x, uint_fast16_t y,
        uint_fast8_t r, uint_fast8_t g, uint_fast8_t b) {
    
    uint_fast16_t blit_w, blit_h;
    uint_fast16_t blit_x, blit_y;
    
    uint_fast32_t ad, bd, cd;
    uint_fast8_t as, bs, cs;
    uint_fast8_t alpha;

    uint8_t *dst_start_ptr, *src_start_ptr;

    Picture *src_conv;

    if (src->pix_fmt == A8) {
        drawA8(src, x, y, r, g, b);
        return;
    } else if (src->pix_fmt == pix_fmt) {
        src_conv = src;
    } else if (src->pix_fmt == BGRA8 && pix_fmt == RGB8) {
        src_conv = src;
    } else if (src->pix_fmt == YUVA8 && pix_fmt == YUV8) {
        src_conv = src;
    } else if (src->pix_fmt == YUVA8 && pix_fmt == RGB8) {
        src_conv = src->convert_to_format(BGRA8);
    } else if (src->pix_fmt == BGRA8 && pix_fmt == YUV8) {
        src_conv = src->convert_to_format(YUVA8);
    } else if (src->pix_fmt == BGRA8 || src->pix_fmt == YUVA8) {
        throw std::runtime_error("unsupported pix fmt");
    } else {
        src_conv = src->convert_to_format(pix_fmt);
    }


    blit_w = src->w;
    blit_h = src->h;

    if (x >= w || y >= h) {
        return;
    }

    if (x + blit_w >= w) {
        blit_w = w - x;
    }

    if (y + blit_h >= h) {
        blit_h = h - y;
    }

    if (src->pix_fmt != BGRA8 && src->pix_fmt != YUVA8) {
        for (blit_y = 0; blit_y < blit_h; ++blit_y) {
            dst_start_ptr = data + line_pitch * blit_y + pixel_pitch( ) * x;
            memcpy(dst_start_ptr, src->scanline(blit_y), pixel_pitch( ) * blit_w);
        }
    } else if (src->pix_fmt == BGRA8 && pix_fmt == RGB8) {
        for (blit_y = 0; blit_y < blit_h; ++blit_y) {
            dst_start_ptr = scanline(y + blit_y) + pixel_pitch( ) * x;
            src_start_ptr = src->scanline(blit_y);
            for (blit_x = 0; blit_x < blit_w; ++blit_x) {
                ad = dst_start_ptr[0];
                bd = dst_start_ptr[1];
                cd = dst_start_ptr[2];

                cs = *src_start_ptr++;
                bs = *src_start_ptr++;
                as = *src_start_ptr++;
                alpha = *src_start_ptr++;

                ad = as * alpha + ad * (256 - alpha);
                bd = bs * alpha + bd * (256 - alpha);
                cd = cs * alpha + cd * (256 - alpha);

                *dst_start_ptr++ = ad / 256;
                *dst_start_ptr++ = bd / 256;
                *dst_start_ptr++ = cd / 256;
            }
        }
    } else if (src->pix_fmt == YUVA8 && pix_fmt == YUV8) {
        for (blit_y = 0; blit_y < blit_h; ++blit_y) {
            dst_start_ptr = scanline(y + blit_y) + pixel_pitch( ) * x;
            src_start_ptr = src->scanline(blit_y);
            for (blit_x = 0; blit_x < blit_w; ++blit_x) {
                ad = dst_start_ptr[0];
                bd = dst_start_ptr[1];
                cd = dst_start_ptr[2];

                as = *src_start_ptr++;
                bs = *src_start_ptr++;
                cs = *src_start_ptr++;
                alpha = *src_start_ptr++;

                ad = as * alpha + ad * (256 - alpha);
                bd = bs * alpha + bd * (256 - alpha);
                cd = cs * alpha + cd * (256 - alpha);

                *dst_start_ptr++ = ad / 256;
                *dst_start_ptr++ = bd / 256;
                *dst_start_ptr++ = cd / 256;
            }
        }

    } else {
        throw std::runtime_error("can't handle that yet");
    }

}


void Picture::drawA8(Picture *src, uint_fast16_t x, uint_fast16_t y,
        uint_fast8_t r, uint_fast8_t g, uint_fast8_t b) {

    uint_fast8_t blend_src[4];
    int blend_pitch;
    uint_fast16_t y1, u, v;
    uint_fast16_t blit_w, blit_h, blit_x, blit_y;
    uint_fast16_t blend;
    uint8_t *src_ptr, *my_ptr;
    uint8_t alpha;
    int j;

    /* use Y'CbCr format in 2 cases below */
    y1 = 16 + (r * 66 + g * 129 + b * 25) / 256;
    u = 128 + (b * 112 - g * 74 - r * 37) / 256;
    v = 128 + (r * 112 - g * 94 - b * 18) / 256;
    switch (pix_fmt) {
        case RGB8:
            blend_src[0] = r;
            blend_src[1] = g;
            blend_src[2] = b;
            blend_pitch = 3;
            break;

        case YUV8:
            blend_src[0] = y1;
            blend_src[1] = u;
            blend_src[2] = v;
            blend_pitch = 3;
            break;

        case UYVY8:
            throw std::runtime_error("this doesn't quite work yet");
            blend_src[0] = u;
            blend_src[1] = y;
            blend_src[2] = v;
            blend_src[3] = y;
            blend_pitch = 4;
            break;
    }

    blit_w = src->w;
    blit_h = src->h;

    if (x >= w || y >= h) {
        return;
    }

    if (x + blit_w >= w) {
        blit_w = w - x;
    }

    if (y + blit_h >= h) {
        blit_h = h - y;
    }

    for (blit_y = 0; blit_y < blit_h; ++blit_y) {
        src_ptr = src->scanline(blit_y);
        my_ptr = scanline(blit_y) + pixel_pitch( ) * x;
        for (blit_x = 0; blit_x < blit_w; ++blit_x) {
            alpha = src_ptr[blit_x];
            for (j = 0; j < blend_pitch; ++j) {
                /* alpha blending each pixel */
                blend = (alpha * *my_ptr + (256 - alpha) * blend_src[j]) / 256;
                *my_ptr = blend;
                ++my_ptr;
            }
        }
    }
    
}
#ifdef HAVE_PANGOCAIRO

cairo_surface_t *Picture::get_cairo(void) {
    if (pix_fmt != BGRA8) {
        throw std::runtime_error("cairo only supported with BGRA8 pictures");
    }

    return cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, w, h, line_pitch);
}

void Picture::render_text(uint_fast16_t x, uint_fast16_t y, 
        const char *fmt, ...) {

    va_list ap;
    char *str;
    int n;
    cairo_surface_t *surf = get_cairo( );
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = pango_cairo_create_layout(cr);

    pango_layout_set_font_description(layout, font_description);
    
    va_start(ap, fmt);
    n = vasprintf(&str, fmt, ap);
    va_end(ap);

    if (n < 0) {
        throw std::runtime_error("vasprintf failed");
    }

    pango_layout_set_text(layout, str, -1);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    /* ditch cairo stuff */
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

void Picture::set_font(const char *family, int height) {
    if (font_description == NULL) {
        font_description = pango_font_description_new( );
    }

    pango_font_description_set_family(font_description, family);
    pango_font_description_set_weight(font_description, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(font_description, height * PANGO_SCALE);

}

Picture *Picture::from_png(const char *filename) {
    cairo_surface_t *pngs = cairo_image_surface_create_from_png(filename);

    if (cairo_surface_status(pngs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(pngs);
        throw std::runtime_error("Failed to load PNG to Cairo surface");
    }

    cairo_format_t nf = cairo_image_surface_get_format(pngs);

    if (nf != CAIRO_FORMAT_ARGB32 && nf != CAIRO_FORMAT_RGB24) {
        cairo_surface_destroy(pngs);
        throw std::runtime_error("PNG uses unsupported pixel format");
    } 

    Picture *ret = Picture::alloc(
        cairo_image_surface_get_width(pngs),
        cairo_image_surface_get_height(pngs),
        4*cairo_image_surface_get_width(pngs),
        BGRA8
    );
        
    int xcopy = 4*ret->w;
    int stride = cairo_image_surface_get_stride(pngs);
    uint8_t *data = (uint8_t *)cairo_image_surface_get_data(pngs);

    /* copy data */
    for (int ycopy = 0; ycopy < ret->h; ++ycopy) {
        memcpy(ret->scanline(ycopy), data + stride * ycopy, xcopy);
    }
    
    cairo_surface_destroy(pngs);
    return ret;
}
#endif
