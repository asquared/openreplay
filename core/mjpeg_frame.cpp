#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "mjpeg_frame.h"
#include "jerror.h"

#include <stdexcept>

#define align_malloc malloc
#define align_realloc realloc
#define FREELIST_MAX 20

typedef struct {
    struct jpeg_source_mgr pub;
} mem_source_mgr;

// jpeglib seems to be crudely implementing C++ using C...
// and it doesn't get along with my coding style well.
METHODDEF(void) init_source(j_decompress_ptr cinfo) {
    /* do nothing - it's ready to go already */
}

METHODDEF(boolean) fill_input_buffer(j_decompress_ptr cinfo) {
    /* if we need more data we're doing it wrong */
    ERREXIT(cinfo, JERR_INPUT_EMPTY);
    return false;
}

METHODDEF(void) skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
    if (cinfo->src->bytes_in_buffer < num_bytes) {
        ERREXIT(cinfo, JERR_INPUT_EMPTY);
    } else {
        cinfo->src->next_input_byte += num_bytes;
        cinfo->src->bytes_in_buffer -= num_bytes;
    }
}

METHODDEF(void) term_source(j_decompress_ptr cinfo) {
    /* caller responsible for the memory so don't worry about it */
}

GLOBAL(void) jpeg_mem_src(j_decompress_ptr cinfo, void *data, size_t len) {
    if (cinfo->src == NULL) {
        cinfo->src = (struct jpeg_source_mgr *)
            malloc(sizeof(struct jpeg_source_mgr));

    }
        
    /* just give it the damn pointer */
    cinfo->src->next_input_byte = (JOCTET *)data;
    cinfo->src->bytes_in_buffer = len;

    /* fill in the (quasi-dummy) functions */
    cinfo->src->init_source = init_source;
    cinfo->src->fill_input_buffer = fill_input_buffer;
    cinfo->src->skip_input_data = skip_input_data;
    cinfo->src->resync_to_restart = jpeg_resync_to_restart;
    cinfo->src->term_source = term_source;
}

METHODDEF(void) my_error_exit(j_common_ptr cinfo) {
    (*cinfo->err->output_message)(cinfo);
    /* return control to exception handler (or maybe crash spectacularly) */
    throw std::runtime_error("JPEG decode error");
}

MJPEGDecoder::MJPEGDecoder( ) {
    memset(&cinfo, 0, sizeof(cinfo));

    cinfo.err = jpeg_std_error(&jerr);
    // this handler throws a C++ exception.
    jerr.error_exit = my_error_exit;

    jpeg_create_decompress(&cinfo);

}

Picture *MJPEGDecoder::decode_full(mjpeg_frame *frame) {
    Picture *f1, *f2, *out;
    if (frame->interlaced) {
        f1 = decode_first(frame);
        f2 = decode_second(frame);
        
        if (frame->odd_dominant) {
            out = weave(f2, f1);
        } else {
            out = weave(f1, f2);
        }

        Picture::free(f1);
        Picture::free(f2);
    } else {
        out = decode(frame->data, frame->f1size);
    }
    return out;
}

Picture *MJPEGDecoder::weave(Picture *even, Picture *odd) {
    assert(even->w == odd->w);
    assert(even->h == odd->h);
    assert(even->line_pitch == odd->line_pitch);
    
    int i;

    Picture *out = Picture::alloc(even->w, 2*even->h, even->line_pitch);
    uint8_t *out_ptr = out->data;
    uint8_t *even_ptr = even->data;
    uint8_t *odd_ptr = odd->data;

    for (i = 0; i < even->h; ++i) {
        memcpy(out_ptr, even_ptr, even->line_pitch);
        even_ptr += even->line_pitch;
        out_ptr += even->line_pitch;

        memcpy(out_ptr, odd_ptr, even->line_pitch);
        odd_ptr += even->line_pitch;
        out_ptr += even->line_pitch;
    }

    return out;
}

Picture *MJPEGDecoder::decode_first(mjpeg_frame *frame) {
    if (frame->interlaced) {
        return decode(frame->data, frame->f1size);
    } else {
        return decode_full(frame);
    }
}

Picture *MJPEGDecoder::decode_second(mjpeg_frame *frame) {
    if (frame->interlaced) {
        return decode(frame->data + frame->f1size, frame->f2size);
    } else {
        return decode_full(frame);
    }
}

Picture *MJPEGDecoder::decode_first_doubled(mjpeg_frame *frame) {
    Picture *field, *ret;
    if (frame->interlaced) {
        field = decode_first(frame);
        if (frame->odd_dominant) {
            ret = scan_double_up(field);
        } else {
            ret = scan_double_down(field);
        }
        Picture::free(field);
        return ret;
    } else {
        // scan double the appropriate scanlines from the full frame
        Picture *out = decode_full(frame);
        if (frame->odd_dominant) {
            scan_double_full_frame_odd(out);
        } else {
            scan_double_full_frame_even(out);
        }
        return out;
    }
}

Picture *MJPEGDecoder::decode_second_doubled(mjpeg_frame *frame) {
    Picture *field, *ret;
    if (frame->interlaced) {
        field = decode_second(frame);
        if (frame->odd_dominant) {
            ret = scan_double_down(field);
        } else {
            ret = scan_double_up(field);
        }
        Picture::free(field);
        return ret;
    } else {
        // scan double the appropriate scanlines from the full frame
        Picture *out = decode_full(frame);
        if (frame->odd_dominant) {
            scan_double_full_frame_even(out);
        } else {
            scan_double_full_frame_odd(out);
        }
        return out;
    }
}

static void interpolate_scanline(uint8_t *out, uint8_t *in1, uint8_t *in2, size_t len) {
    uint16_t temp;
    while (len > 0) {
        temp = *in1;
        temp += *in2;
        temp >>= 1;
        *out = temp;
        in1++;
        in2++;
        out++;
        len--;
    }
}

/* in = odd scanlines. Generate the even ones by interpolation. */
Picture *MJPEGDecoder::scan_double_up(Picture *in) {
    int i, j;
    Picture *out = Picture::alloc(in->w, 2*in->h, in->line_pitch);

    // i counts even scanlines in the output, j counts input scanlines
    for (i = 0, j = 0; i < out->h; i += 2, j++) {        
        /* if we have two surrounding odd scanlines interpolate.
         * otherwise, copy the odd scanline to the even one. */
        if (j - 1 >= 0) {
            interpolate_scanline(out->scanline(i), in->scanline(j - 1), in->scanline(j), in->line_pitch);
        } else {
            memcpy(out->scanline(i), in->scanline(j), in->line_pitch);
        }

        /* copy odd scanline from input (always) */
        memcpy(out->scanline(i + 1), in->scanline(j), in->line_pitch);
    }

    return out;
}

/* in = even scanlines (0, 2, 4, 6, ...). Generate the odd ones by interpolation. */
Picture *MJPEGDecoder::scan_double_down(Picture *in) {
    int i, j;
    Picture *out = Picture::alloc(in->w, 2*in->h, in->line_pitch);

    // i counts even output scanlines. j counts input scanlines.
    for (i = 0, j = 0; i < out->h; i += 2, j++) {
        // Copy the even scanline.
        memcpy(out->scanline(i), in->scanline(j), in->line_pitch);
        
        // Generate the following odd scanline by interpolation or copying.
        if (j + 1 < in->h) {
            // Generate the odd scanline from the two surrounding even ones
            interpolate_scanline(out->scanline(i + 1), in->scanline(j), in->scanline(j + 1), in->line_pitch);
        } else {
            // last scanline doesn't have any below it so just copy it
            memcpy(out->scanline(i + 1), in->scanline(j), in->line_pitch);
        }
    }

    return out;
}

void MJPEGDecoder::scan_double_full_frame_even(Picture *p) {
    int i;    
    for (i = 0; i < p->h; i += 2) {
        if (i + 2 < p->h) {
            /* have 2 scanlines, so interpolate */
            interpolate_scanline(p->scanline(i + 1), p->scanline(i), p->scanline(i + 2), p->line_pitch);
        } else {
            memcpy(p->scanline(i + 1), p->scanline(i), p->line_pitch);
        }
    }
}

void MJPEGDecoder::scan_double_full_frame_odd(Picture *p) {
    int i;
    for (i = 1; i < p->h; i += 2) {
        if (i - 2 >= 0) {
            /* have 2 surrounding scanlines, so interpolate it */
            interpolate_scanline(p->scanline(i - 1), p->scanline(i - 2), p->scanline(i), p->line_pitch);
        } else {
            /* copy the scanline up a row */
            memcpy(p->scanline(i - 1), p->scanline(i), p->line_pitch);
        }
    }
}

Picture *MJPEGDecoder::decode(void *data, size_t len) {
    Picture *output;
        
    
    jpeg_mem_src(&cinfo, data, len);
    jpeg_read_header(&cinfo, TRUE);

    jpeg_start_decompress(&cinfo);

    /* picture dimensions are calculated, so allocate */
    output = Picture::alloc(cinfo.output_width, cinfo.output_height,
        cinfo.output_width * cinfo.output_components);

    uint8_t *data_ptr = output->data;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &data_ptr, 1);        
        data_ptr += output->line_pitch;
    }

    jpeg_finish_decompress(&cinfo);

    return output;
}

MJPEGDecoder::~MJPEGDecoder( ) {
    jpeg_destroy_decompress(&cinfo);
}
