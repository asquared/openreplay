#include "ffwrapper.h"

// just initializes FFmpeg when derived classes are instantiated
FFwrapper::UsesFFMPEG::UsesFFMPEG( ) {
    if (!avcodec_initialized) {
        fprintf(stderr, "Initializing FFmpeg libraries...\n");
        avcodec_init( );
        av_register_all( );
        avcodec_initialized = true;
    }

}

FFwrapper::Decoder::Decoder(enum CodecID id) {

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
        av_free(out_buf);
    }

    if (ctx) {
        sws_freeContext(ctx);
    }
}
            
AVPicture *FFwrapper::Scaler::scale(AVPicture *input, const AVCodecContext *src_ctx) {
    size_t picture_size;

    if (!out_buf) {
        picture_size = avpicture_get_size(pixfmt, width, height);
        out_buf = (uint8_t *)av_malloc(picture_size);
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

FFwrapper::AudioResampler::AudioResampler(int in_ch, int in_rate, enum SampleFormat in_fmt, int out_ch, int out_rate) 
    : buf(NULL), buf_size(0), ctx(NULL) {

    ctx = av_audio_resample_init(
        out_ch, in_ch, out_rate, in_rate,       /* in, out parameters */
        SAMPLE_FMT_S16, in_fmt,         /* output, input formats */
        16, 10, 0, 0.8                          /* filter length, log2 phase count, linear?, cutoff */
    );

    if (!ctx) {
        throw AllocationError("audio resampling context");
    }
}

FFwrapper::AudioResampler::~AudioResampler( ) {
    if (ctx) {
        audio_resample_close(ctx);
    }

    if (buf) {
        av_free(buf);
    }
}

short *FFwrapper::AudioResampler::resample(void *in, size_t in_len, size_t *out_len) {
    *out_len = 2 * in_len; /* FFmpeg needs a way to find this out */
    if (*out_len > buf_size) {
        fprintf(stderr, "warning: resample buffer being reallocated\n");
        buf = (short *) av_realloc(buf, *out_len * sizeof(short));
        if (!buf) {
            buf_size = 0;
            throw AllocationError("audio resample buffer");
        }
        buf_size = *out_len;
    }

    *out_len = audio_resample(ctx, buf, (short *) in, in_len);
    if (*out_len == 0) {
        return NULL;
    } else {
        return buf;
    }
}

bool FFwrapper::UsesFFMPEG::avcodec_initialized = false;
