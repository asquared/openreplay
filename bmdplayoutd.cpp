#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>
#include <string.h>

extern "C" {
    #include <avcodec.h>
    #include <avformat.h>
    #include <swscale.h>
}

#include "DeckLinkAPI.h"
//#include "Capture.h"

#include "playout_ctl.h"

enum decode_status { SUCCESS, FAILURE, END_OF_FILE };
#define FRAMES_PER_SEC 30

// ohmygodcocaine... this amounts to magic incantations found throughout the internets...
class lavc_decoder {
public:
    lavc_decoder(const char *filename) {
        fprintf(stderr, "trying to open %s\n", filename);
        if (av_open_input_file(&format_ctx, filename, 0, 0, 0) != 0) {
            throw std::runtime_error("av_open_input_file failed");
        }

        if (av_find_stream_info(format_ctx) < 0) {
            throw std::runtime_error("av_find_stream_info failed");
        }

        video_stream = -1;
        for (int i = 0; i < format_ctx->nb_streams; ++i) {
            if (format_ctx->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO) {
                video_stream = i;
                break;
            }
        }

        if (video_stream == -1) {
            throw std::runtime_error("no video stream");
        }

        codec_ctx = format_ctx->streams[video_stream]->codec;
        codec = avcodec_find_decoder(codec_ctx->codec_id);
        
        if (!codec) {
            throw std::runtime_error("no codec found");
        }

        if (avcodec_open(codec_ctx, codec) < 0) {
            throw std::runtime_error("codec failure");
        }

        frame = avcodec_alloc_frame( );
        if (!frame) {
            throw std::runtime_error("frame allocation error (1)");
        }

        frameUYVY = avcodec_alloc_frame( );
        if (!frameUYVY) {
            throw std::runtime_error("frame allocation error (1)");
        }

        int numBytes = avpicture_get_size(PIX_FMT_UYVY422, codec_ctx->width, codec_ctx->height);

        buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

        avpicture_fill((AVPicture *)frameUYVY, buffer, PIX_FMT_UYVY422, codec_ctx->width, codec_ctx->height);

        sws_ctx = sws_getContext(
            codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt, 
            codec_ctx->width, codec_ctx->height, PIX_FMT_UYVY422,
            SWS_FAST_BILINEAR | SWS_PRINT_INFO, 0, 0, 0
        );

    }

    decode_status read_frame(bool want_decode) {
        int frameFinished;
        if (av_read_frame(format_ctx, &packet) >= 0) {
            if (packet.stream_index == video_stream) {
                if (want_decode) {
                    avcodec_decode_video(codec_ctx, frame, &frameFinished, packet.data, packet.size);

                    if (frameFinished) {
                        av_free_packet(&packet);
                        return SUCCESS;
                    } else {
                        return FAILURE; // decode error??
                    }
                } else {
                    // skip the frame
                    return SUCCESS;
                }
            }
            av_free_packet(&packet);
        }

        return END_OF_FILE; // we were unable to read another frame...
    }

    void copy_frame(void *dest, int width, int height) {
        int y;
        int maxh, maxw;

        uint8_t *conv_dest[3] = {(uint8_t *) dest, 0, 0};
        int conv_stride[3] = { 2*width, 0, 0 };
        
        if (height < codec_ctx->height) {
            maxh = height;
        } else {
            maxh = codec_ctx->height;
        }

        if (width < codec_ctx->width) {
            maxw = width;
        } else {
            maxw = codec_ctx->width;
        }

        // this is probably cocaine.
        sws_scale(sws_ctx, frame->data, frame->linesize, 0, maxh, conv_dest, conv_stride);

    }

    void rewind(void) {
        // hopefully this does the job...
        av_seek_frame(format_ctx, -1, 0, AVSEEK_FLAG_BYTE);
    }

    ~lavc_decoder( ) {
        av_free(buffer);
        av_free(frameUYVY);
        av_free(frame);
        avcodec_close(codec_ctx);
        av_close_input_file(format_ctx);
    }


protected:
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    AVCodec *codec;
    AVFrame *frame, *frameUYVY;
    struct SwsContext *sws_ctx;
    AVPacket packet;
    int video_stream;
    uint8_t *buffer;
};

class DecklinkOutput {
public:
    DecklinkOutput(int cardIndex = 0) 
        : deckLink(0), deckLinkOutput(0), deckLinkIterator(0),
          displayMode(0), deckLinkConfig(0), frame_duration(66)
    {
        deckLinkIterator = CreateDeckLinkIteratorInstance( ); 
	/* Connect to DeckLink card */
	if (!deckLinkIterator) {
            throw std::runtime_error("This application requires the DeckLink drivers installed.");
	}

	while (cardIndex >= 0) {
            /* Connect to the first DeckLink instance */
            result = deckLinkIterator->Next(&deckLink);
            if (result != S_OK) {
                throw std::runtime_error("Invalid card index.");
            }
            cardIndex--;
	}

	deckLink->GetModelName(&string);
	fprintf(stderr, "Attached to card: %s\n", string);
	free((void *)string);
    
	if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&deckLinkOutput) != S_OK) {
            throw std::runtime_error("Failed to get IDeckLinkOutput interface"); 
        }

	if (deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&deckLinkConfig) != S_OK) {
            throw std::runtime_error("Failed to get IDeckLinkConfiguration interface");
        }

	// configure output
	deckLinkConfig->SetVideoOutputFormat(bmdVideoConnectionComposite);

	// Create frame object
	if (
            deckLinkOutput->CreateVideoFrame(
                720, 480, 1440, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame
            ) != S_OK
	) {
		throw std::runtime_error("Failed to create frame");
	}

	// enable video output
	deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault);
    }

    void *GetFrameBuffer(void) {
        void *data;
        frame->GetBytes(&data);
        return data;
    }

    void Flip(void) {
        update_time( );
        frame_start = time_now;
        if (deckLinkOutput->DisplayVideoFrameSync(frame) != S_OK) {
            fprintf(stderr, "warning: frame display failed\n");
        }
    }

    bool ReadyForNextFrame(void) {
        update_time( );
        return (time_now > frame_start + frame_duration);
    }

    ~DecklinkOutput(void) {
        if (deckLinkOutput != NULL) {
            deckLinkOutput->Release();
            deckLinkOutput = NULL;
        }

        if (deckLink != NULL) {
            deckLink->Release();
            deckLink = NULL;
        }


        if (deckLinkConfig != NULL) {
            deckLinkConfig->Release( );
            deckLinkConfig = NULL;
        }

        if (deckLinkIterator != NULL) {
            deckLinkIterator->Release();
        }
            
    }

    void SetSpeed(float speed) {
        frame_duration = 1000 * speed / (float) FRAMES_PER_SEC;
    }


protected:
    void update_time(void) {
        BMDTimeValue time_in_frame, ticks_per_frame; // don't give a damn about these
        deckLinkOutput->GetHardwareReferenceClock(1000, &time_now, &time_in_frame, &ticks_per_frame);
    }

    // lots of variables...
    IDeckLink *deckLink;
    IDeckLinkOutput *deckLinkOutput;
    IDeckLinkIterator *deckLinkIterator;
    IDeckLinkDisplayMode *displayMode;
    IDeckLinkConfiguration *deckLinkConfig;
    IDeckLinkMutableVideoFrame *frame;
    BMDTimeValue frame_start, time_now;

    int frame_duration; 
    HRESULT	result;
    const char *string;

};

class StdoutOutput {
public:
    StdoutOutput(int cardIndex = 0) {
        data = malloc(2*720*480);
        if (!data) {
            throw std::runtime_error("allocation failure");
        }
    }

    ~StdoutOutput( ) {
        free(data);
    }

    void *GetFrameBuffer(void) {
        return data;
    }

    void Flip(void) {
        write(STDOUT_FILENO, data, 2*720*480);    
    }

    bool ReadyForNextFrame(void) {
        return true;
    }

    void SetSpeed(float speed) {
        // we can't change the damn speed...
    }

protected:
    void *data;

};

#define MAX_OPEN_FILES 32
lavc_decoder *open_files[MAX_OPEN_FILES] = {0};

int socket_fd;
struct sockaddr_in timecode_addr;

int playout_source = 0;
int timecode = 0;
bool did_cut = false;
bool paused = false;
bool step = false;

// send timecode update on UDP socket
void update_timecode(void) {
    sendto(socket_fd, &timecode, sizeof(timecode), 0, 
        (struct sockaddr *)&timecode_addr, sizeof(timecode_addr));
}

void socket_setup(void) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(30001);
    inet_aton("127.0.0.1", &addr.sin_addr);

    timecode_addr.sin_family = AF_INET;
    timecode_addr.sin_port = htons(30002);
    inet_aton("127.0.0.1", &timecode_addr.sin_addr);

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        exit(1);
    }

    if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
    }
}

int try_receive(struct playout_command *cmd) {
    int result;
    struct pollfd pfd;

    pfd.fd = socket_fd;
    pfd.events = POLLIN;

    // poll...
    result = poll(&pfd, 1, 1);

    if (pfd.revents & POLLIN) {
        // ready to go!
        fprintf(stderr, "Got something");
        recvfrom(socket_fd, cmd, sizeof(struct playout_command), 0, 0, 0);
        return 1;
    } else {
        return 0;
    }
}

void close_open_files(void) {
    for (int j = 0; j < MAX_OPEN_FILES && open_files[j]; ++j) {
        delete open_files[j];
        open_files[j] = 0;
    }
}

void open_new_files(char *filenames) {
    int j = 0;
    // terminates on double null...
    while (strlen(filenames) > 0) {
        fprintf(stderr, "opening file... %s on slot %d\n", filenames, j);
        open_files[j] = new lavc_decoder(filenames);
        filenames += strlen(filenames) + 1;
        ++j;
    }
}

DecklinkOutput out;

void parse_command(struct playout_command *cmd) {
    switch(cmd->cmd) {
        case PLAYOUT_CMD_START_FILES:
            close_open_files( );
            open_new_files(cmd->filenames);
            playout_source = cmd->source;
            did_cut = true;
            timecode = 0;
            out.SetSpeed(cmd->new_speed);
            break;

        case PLAYOUT_CMD_ADJUST_SPEED:
            out.SetSpeed(cmd->new_speed);            
            break;

        case PLAYOUT_CMD_CUT_REWIND:
            // rewind all the sources
            for (int j = 0; j < MAX_OPEN_FILES && open_files[j]; ++j) {
                open_files[j]->rewind( );
            }
            timecode = 0;
            // fall through to the cut...
        case PLAYOUT_CMD_CUT:
            playout_source = cmd->source;
            did_cut = true;
            break;
        
        case PLAYOUT_CMD_PAUSE:
            paused = true;
            break;

        case PLAYOUT_CMD_RESUME:
            paused = false;
            break;

        case PLAYOUT_CMD_STEP:
            step = true;
            break;
    }

    fprintf(stderr, "source is now... %d\n", playout_source);
}

decode_status advance_all_and_read_from(int source) {
    decode_status result = FAILURE;
    for (int j = 0; j < MAX_OPEN_FILES && open_files[j]; ++j) {
        if (j == source) {
            result = open_files[j]->read_frame(true);
        } else {
            // just skip the frame
            open_files[j]->read_frame(false);
        }
    }
    return result;
}


int main(int argc, char *argv[]) {
    struct playout_command cmd;
    unsigned char *buf = (unsigned char *)malloc(2*720*480);
    bool next_frame_ready = false;
    decode_status result;

    socket_setup( );
    av_register_all( );

    // toss up a black frame until we're ready to go
    memset(out.GetFrameBuffer( ), 0, 2*720*480);
    out.Flip( );

    // now, the interesting bits...
    while (1) {
        if (!next_frame_ready) {
            // Advance all streams one frame. Only decode on the one we care about.
            // (if nothing's open, this fails by design...)
            if (!paused || step || did_cut) {
                result = advance_all_and_read_from(playout_source);
                if (result == SUCCESS) {
                    open_files[playout_source]->copy_frame(out.GetFrameBuffer( ), 720, 480); 
                    next_frame_ready = true;
                }
                step = false;
                did_cut = false;

                if (result != END_OF_FILE) {
                    ++timecode;
                    update_timecode( );
                }

            }

        }

        /* else */ if (out.ReadyForNextFrame( ) && next_frame_ready) {
            out.Flip( );
            next_frame_ready = false;
        } 
        
        if (try_receive(&cmd)) {
            fprintf(stderr, "receivin shits...\n");
            parse_command(&cmd);
        } else {
            // nothing to do, go to sleep...
            usleep(1000);
        }
    }
}
