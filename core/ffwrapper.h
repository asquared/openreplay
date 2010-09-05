#ifndef _FFWRAPPER_H
#define _FFWRAPPER_H

#include <stdint.h>

extern "C" {
    #include "libavformat/avformat.h"
    #include "libswscale/swscale.h"
    #include "libavcodec/avcodec.h"
}

#include <stdexcept>

namespace FFwrapper {
    class Decoder {
        public:
            Decoder(enum CodecID id);
            ~Decoder( );
            AVPicture *try_decode(uint8_t *buf, int len);
            const AVCodecContext *get_ctx( ) { return ctx; }
        protected:
            AVFrame *frame;
            AVCodecContext *ctx;
            AVCodec *codec;
            static bool avcodec_initialized;
    };

    class Scaler {
        public:
            Scaler(int dstW, int dstH, enum PixelFormat dstFormat);
            ~Scaler( );
            AVPicture *scale(AVPicture *input, const AVCodecContext *src_ctx);
            uint16_t stride( );
            uint16_t pitch( );
        protected:
            SwsContext *ctx;
            uint8_t *out_buf;
            int width, height;
            enum PixelFormat pixfmt;
            AVPicture out;
    };

    class FFwrapperError : public std::runtime_error {
        public:
            FFwrapperError(const std::string &s) : std::runtime_error(s) { }
    };

    class NoCodecError : public FFwrapperError {
        public:
            NoCodecError(const std::string& s) : FFwrapperError(s) { }
    };

    class AllocationError : public FFwrapperError {
        public:
            AllocationError(const std::string& s) : FFwrapperError(s) { }
    };

    class CodecError : public FFwrapperError {
        public:
            CodecError(const std::string& s) : FFwrapperError(s) { }
    };
};

#endif
