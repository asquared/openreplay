#include "picture.h"

#include <stdlib.h>
#include <stdexcept>

#define align_malloc malloc
#define align_realloc realloc

#define FREELIST_MAX 16

Picture *Picture::alloc(uint16_t w, uint16_t h, uint16_t line_pitch) {
    Picture *candidate;
    size_t pic_size = h * line_pitch;

    // See if we can get something off the free list.
    if (!free_list.empty( )) {
        candidate = free_list.front( );
        free_list.pop_front( );

        candidate->w = w;
        candidate->h = h;
        candidate->line_pitch = line_pitch;
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
        candidate->data = 
            (uint8_t *)malloc(pic_size);        
        if (!candidate->data) {
            throw std::runtime_error("New picture allocation failed!\n");
        }
        candidate->alloc_size = pic_size;
        return candidate;
    }
}

void Picture::free(Picture *pic) {
    if (free_list.size( ) >= FREELIST_MAX) {
        ::free(pic->data);
        delete pic;
    } else {
        free_list.push_back(pic);
    }
}

std::list<Picture *> Picture::free_list;
