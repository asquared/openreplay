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

MJPEGDecoder::MJPEGDecoder( ) {
    memset(&cinfo, 0, sizeof(cinfo));

    cinfo.err = jpeg_std_error(&jerr);
    // hack this up to throw a C++ exception? will that work?

    jpeg_create_decompress(&cinfo);

}

struct picture *MJPEGDecoder::decode_full(mjpeg_frame *frame) {
    struct picture *f1, *f2, *out;
    if (frame->interlaced) {
        f1 = decode_first(frame);
        f2 = decode_second(frame);
        
        free_picture(f1);
        free_picture(f2);
        if (frame->odd_dominant) {
            out = weave(f2, f1);
        } else {
            out = weave(f1, f2);
        }
    } else {
        out = decode(frame->data, frame->f1size);
    }
    return out;
}

struct picture *MJPEGDecoder::weave(struct picture *even, struct picture *odd) {
    assert(even->w == odd->w);
    assert(even->h == odd->h);
    assert(even->line_pitch == odd->line_pitch);
    
    int i;

    struct picture *out = alloc_picture(even->w, 2*even->h, even->line_pitch);
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

struct picture *MJPEGDecoder::decode_first(mjpeg_frame *frame) {
    return decode(frame->data, frame->f1size);
}

struct picture *MJPEGDecoder::decode_second(mjpeg_frame *frame) {
    return decode(frame->data + frame->f1size, frame->f2size);
}

struct picture *MJPEGDecoder::decode(void *data, size_t len) {
    struct picture *output;
        
    
    jpeg_mem_src(&cinfo, data, len);
    jpeg_read_header(&cinfo, TRUE);

    jpeg_start_decompress(&cinfo);

    /* picture dimensions are calculated, so allocate */
    output = alloc_picture(cinfo.output_width, cinfo.output_height,
        cinfo.output_width * cinfo.output_components);

    uint8_t *data_ptr = output->data;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &data_ptr, 1);        
        data_ptr += output->line_pitch;
    }

    jpeg_finish_decompress(&cinfo);

    return output;
}

struct picture *MJPEGDecoder::alloc_picture(uint16_t w, uint16_t h, 
        uint16_t line_pitch) {

    struct picture *candidate;
    size_t pic_size = h * line_pitch;

    // See if we can get something off the free list.
    if (!free_pictures.empty( )) {
        candidate = free_pictures.front( );
        free_pictures.pop_front( );

        candidate->w = w;
        candidate->h = h;
        candidate->line_pitch = line_pitch;
        if (candidate->alloc_size >= pic_size) {
            return candidate;
        } else {
            candidate->data = 
                (uint8_t *)align_realloc(candidate->data, pic_size);
            if (!candidate->data) {
                throw std::runtime_error("Allocation failed in MJPEGDecoder");
            }
            candidate->alloc_size = pic_size;
            return candidate;
        }
    } else {
        candidate = (struct picture *)malloc(sizeof(struct picture));
        candidate->w = w;
        candidate->h = h;
        candidate->line_pitch = line_pitch;
        candidate->data = 
            (uint8_t *)malloc(pic_size);        
        if (!candidate->data) {
            throw std::runtime_error("Allocation failed in MJPEGDecoder");
        }
        candidate->alloc_size = pic_size;
        return candidate;
    }
}

void MJPEGDecoder::free_picture(struct picture *pic) {
    if (free_pictures.size( ) >= FREELIST_MAX) {
        // free the thing
        free(pic->data);
        free(pic);
    } else {
        free_pictures.push_back(pic);
    }
}

MJPEGDecoder::~MJPEGDecoder( ) {
    /* really free all the free pictures */
    struct picture *pic;
    while (!free_pictures.empty( )) {
        pic = free_pictures.front( );
        free_pictures.pop_front( );

        free(pic->data);
        free(pic);
    }


    jpeg_destroy_decompress(&cinfo);
}
