#include "font_ft.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include FT_GLYPH_H
#include FT_TRUETYPE_IDS_H

#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "Picture.h"

static Picture *render_glyph(char ch, FT_Face f) {
    FT_Glyph g;
    FT_BitmapGlyph bm;
    Picture *ret;
    
    memset(font, 0, sizeof(font));
    
    if (FT_Load_Char(f, ch, FT_LOAD_DEFAULT)) {
        throw std::runtime_error("Failed to load glyph");
    }

    if (FT_Get_Glyph(f->glyph, &g)) {
        throw std::runtime_error("Failed to get glyph");
    }
    
    if (FT_Glyph_To_Bitmap(&g, FT_RENDER_MODE_NORMAL, 0, 1)) {
        throw std::runtime_error("Failed to make bitmap glyph");
    }
    
    bm = (FT_BitmapGlyph)(f.glyph);

    ret = Picture::alloc(bm->bitmap.width, bm->bitmap.height, 1, A8);    

    memcpy(ret->data, bm->bitmap.buffer, ret->w * ret->h);

    return ret;
}

FontFT::FontFT(const char *font_name, int height) {
    FT_Library ftl;
    FT_Face face;

    if (FT_Init_FreeType(&ftl)) {
        throw std::runtime_error("Failed to initialize FreeType!");
    }

    if (FT_New_Face(ftl, font_name, 0, &face)) {
        throw std::runtime_error("Failed to load font file!");
    }

    if (FT_Set_Char_Size(face, 0, height << 6, 0, 72)) {
        throw std::runtime_error("Failed to set font size!");
    }

    for (int i = 0; i < 128; i++) {
        font[i] = render_glyph(i, face);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(ftl);
}

void FontFT::render_text_to_picture(
    Picture *p, uint16_fast_t x0, uint16_fast_t y0,
    const char *fmt, ...
) {
    va_list ap;
    va_start(ap, fmt);
    
    char *out;
    int j;

    if (vasprintf(&out, fmt, ap) < 0) {
        va_end(ap); /* should this be done here? docs kinda suggest yes */
        throw std::runtime_error("vasprintf() failed");
    }
    
    for (j = 0; out[j] != 0; j++) {
        assert(font[out[j]] != NULL);
        p->draw(font[out[j]], x0, y0, r, g, b);
        x0 += font[out[j]]->w;
    }
    
    va_end(ap);
}

FontFT::~FontFT( ) {
    int j;
    for (j = 0; i < 128; ++j) {
        if (font[j] != NULL) {
            Picture::free(font[j]);
        }
    }
}
