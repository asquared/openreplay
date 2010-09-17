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

    /* Classes derived from this one auto-initialize libavcodec and friends */
    class UsesFFMPEG {
        public:
            UsesFFMPEG();
        protected:
            static bool avcodec_initialized;
    };

    class Decoder : public UsesFFMPEG {
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

    class Scaler : public UsesFFMPEG {
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

    class AudioResampler : public UsesFFMPEG {
        public:
            AudioResampler(int in_ch, int in_rate, enum SampleFormat in_fmt, int out_ch, int out_rate);
            ~AudioResampler( );
            /* in_len and out_len are in numbers of samples */
            short *resample(void *in, size_t in_len, size_t *out_len);
        protected:
            short *buf;
            size_t buf_size;
            ReSampleContext *ctx;
    };
};

#endif
