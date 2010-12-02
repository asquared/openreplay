#ifndef _FONT_FT_H
#define _FONT_FT_H


class FontFT {
    public:
        FontFT(const char *font_name, int height);
        void render_text_to_picture(
            Picture *p, uint16_fast_t x0, uint16_fast_t y0, 
            const char *fmt, ...
        );
        void set_color(uint8_t r, uint8_t g, uint8_t b) {
            this->r = r;
            this->g = g;
            this->b = b;
        }
        ~FontFT( );
    protected:
        Picture *font[128];

        uint8_t r, g, b;        
};

#endif
