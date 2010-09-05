#include "ffwrapper.h"


FFwrapper::Decoder::Decoder(enum CodecID id) {
    if (!avcodec_initialized) {
        avcodec_init( );
        avcodec_register_all( );
        avcodec_initialized = true;
    }

    codec = avcodec_find_decoder(id);
    if (!codec) {
        throw NoCodecError("could not find codec");
    }

    ctx = avcodec_alloc_context( );
    if (!ctx) {
        throw AllocationError("allocating codec context");
    }

    frame = avcodec_alloc_frame( );
    if (!frame) {
        throw AllocationError("allocating frame");
    }

    if (avcodec_open(ctx, codec) < 0) {
        throw CodecError("failed to open codec");
    }
}

FFwrapper::Decoder::~Decoder( ) {
    avcodec_close(ctx);
    av_free(ctx);
    av_free(frame);
}

AVPicture *FFwrapper::Decoder::try_decode(uint8_t *buf, int len) {
    int ret, got_frame;

    ret = avcodec_decode_video(ctx, frame, &got_frame, buf, len);

    if (ret < 0) {
        throw CodecError("failed to decode frame");
    } else if (ret == 0 || got_frame == 0) {
        return 0;
    } else {
        return (AVPicture *)frame;
    }
}

FFwrapper::Scaler::Scaler(int dstW, int dstH, enum PixelFormat dstFormat) {
    ctx = NULL;
    out_buf = NULL;
    width = dstW;
    height = dstH;
    pixfmt = dstFormat;
}

FFwrapper::Scaler::~Scaler( ) {
    if (out_buf) {
        delete [] out_buf;
    }

    if (ctx) {
        sws_freeContext(ctx);
    }
}
            
AVPicture *FFwrapper::Scaler::scale(AVPicture *input, const AVCodecContext *src_ctx) {
    size_t picture_size;

    if (!out_buf) {
        picture_size = avpicture_get_size(pixfmt, width, height);
        out_buf = new uint8_t[picture_size];
        if (!out_buf) {
            throw AllocationError("frame buffer");
        }

        ctx = sws_getContext(
            src_ctx->width, src_ctx->height, src_ctx->pix_fmt,
            width, height, pixfmt,
            SWS_FAST_BILINEAR | SWS_PRINT_INFO, 0, 0, 0
        );
        if (!ctx) {
            throw AllocationError("swscale context");
        }

        avpicture_fill(&out, out_buf, pixfmt, width, height);
    }

    sws_scale(ctx, input->data, input->linesize, 0, src_ctx->height, out.data, out.linesize);

    return &out;
}

bool FFwrapper::Decoder::avcodec_initialized = false;
