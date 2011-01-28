#ifndef _PICTURE_H
#define _PICTURE_H

#include <list>
#include <stdint.h>

enum pixel_format {
    RGB8, UYVY8, YUV8, BGRA8, YUVA8, A8
};

#ifdef HAVE_PANGOCAIRO
#include <cairo.h>
#include <pango/pangocairo.h>
#endif

class Picture {
    public:
        uint8_t *data;
        uint16_t w, h, line_pitch;

        virtual ~Picture( );

        enum pixel_format pix_fmt;

        inline uint8_t *scanline(int n) {
            return data + line_pitch * n;
        }

        static Picture *alloc(uint16_t w, uint16_t h, uint16_t line_pitch,
            enum pixel_format pix_fmt = RGB8);
        static Picture *copy(Picture *src);
        static void free(Picture *pic);

        int pixel_pitch(void);
        
        Picture *convert_to_format(enum pixel_format pix_fmt);

        /* approximate some sort of fast blit (from A8 surface, color fill) */
        void draw(Picture *src, uint_fast16_t x, uint_fast16_t y,
            uint_fast8_t r, uint_fast8_t g, uint_fast8_t b);

        void addref( );

#ifdef HAVE_PANGOCAIRO
        cairo_surface_t *get_cairo(void);
        void render_text(uint_fast16_t x, uint_fast16_t y, const char *fmt, ...);
        static Picture *from_png(const char *filename);
        void set_font(const char *family, int height);
#endif
    protected:
        int rcount;

        Picture( );
        Picture *to_rgb8(void);
        Picture *to_uyvy8(void);
        Picture *to_yuv8(void);
        Picture *to_yuva8(void);

        Picture *rgb8_to_uyvy8(void);
        Picture *uyvy8_to_rgb8(void);
        Picture *yuv8_to_uyvy8(void);
        Picture *uyvy8_to_yuv8(void);
        Picture *bgra8_to_yuva8(void);

        void alloc_data(size_t size);

        void drawA8(Picture *src, uint_fast16_t x, uint_fast16_t y,
            uint_fast8_t r, uint_fast8_t g, uint_fast8_t b);

        uint16_t alloc_size;

        
        
#ifdef HAVE_PANGOCAIRO
        PangoFontDescription *font_description;
#endif
};

#endif
