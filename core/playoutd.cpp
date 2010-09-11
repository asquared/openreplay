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

#include "ffwrapper.h"
#include "mmap_buffer.h"
#include "mjpeg_config.h"

#include "playout_ctl.h"

#define OUT_FRAME_W 720
#define OUT_FRAME_H 480

class OutputAdapter {
public:
    virtual void *GetFrameBuffer(void) = 0;
    virtual void SetNextFrame(AVPicture *in_frame) = 0;
    virtual void Flip(void) = 0;
    virtual bool ReadyForNextFrame(void) = 0;
    virtual void SetSpeed(float new_speed) = 0;
    virtual ~OutputAdapter( ) { }
};


#if defined(ENABLE_DECKLINK)

#include "DeckLinkAPI.h"

class DecklinkOutput : public OutputAdapter {
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

    void SetNextFrame(AVPicture *in_frame) {
        unsigned char *data_ptr;
        int i;
        frame->GetBytes(data_ptr);

        for (i = 0; i < 480; ++i) {
            memcpy(frame + 1440*i, in_frame->data[0] + in_frame->linesize[0]*i);
        }
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
        frame_duration = 1000 / ( (float) FRAMES_PER_SEC * speed);
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
#endif

class StdoutOutput : public OutputAdapter {
public:
    StdoutOutput( ) {
        data = (uint8_t *)malloc(2*720*480);
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

    void SetNextFrame(AVPicture *in_frame) {
        int i;

        for (i = 0; i < 480; ++i) {
            memcpy(data + 1440*i, in_frame->data[0] + in_frame->linesize[0]*i, 1440);
        }
    }

    void Flip(void) {
        write(STDOUT_FILENO, data, 2*720*480);    
    }

    bool ReadyForNextFrame(void) {
        return true;
    }

    void SetSpeed(float speed) {
        // stdout has no speed control, so don't do anything
    }

protected:
    uint8_t *data;

};

MmapBuffer *buffers[MAX_CHANNELS];
int marks[MAX_CHANNELS];
int play_offset;

int socket_fd;
struct sockaddr_in timecode_addr;

int playout_source = 0;
bool did_cut = false;
bool paused = false;
bool step = false;
bool step_backward = false;
bool run_backward = false;

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



OutputAdapter *out;

void parse_command(struct playout_command *cmd) {
    switch(cmd->cmd) {
        case PLAYOUT_CMD_CUE:
            did_cut = true;
            paused = true;
            if (cmd->new_speed < 0) {
                cmd->new_speed = -cmd->new_speed;
                run_backward = true;
            } else {
                run_backward = false;
            }
            out->SetSpeed(cmd->new_speed);
            memcpy(marks, cmd->marks, sizeof(marks));
            play_offset = 0;
            playout_source = cmd->source;
            break;

        case PLAYOUT_CMD_ADJUST_SPEED:
            if (cmd->new_speed < 0) {
                cmd->new_speed = -cmd->new_speed;
                run_backward = true;
            } else {
                run_backward = false;
            }
            out->SetSpeed(cmd->new_speed);            
            break;

        case PLAYOUT_CMD_CUT_REWIND:
            play_offset = 0;
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

        case PLAYOUT_CMD_STEP_FORWARD:
            step = true;
            break;

        case PLAYOUT_CMD_STEP_BACKWARD:
            step_backward = true;
            break;
    }

    fprintf(stderr, "source is now... %d\n", playout_source);
}


int main(int argc, char *argv[]) {
    struct playout_command cmd;
    static uint8_t frame_data[MAX_FRAME_SIZE];
    bool next_frame_ready = false;
    int i;
    size_t frame_size;

    AVPicture *decoded_frame, *scaled_frame;

    out = new StdoutOutput( );

    // initialize decoder
    FFwrapper::Decoder mjpeg_decoder( CODEC_ID_MJPEG );

    // initialize scaler
    FFwrapper::Scaler scaler(OUT_FRAME_W, OUT_FRAME_H, PIX_FMT_UYVY422); 

    // initialize buffers
    for (i = 0; i < argc - 1; ++i) {
        buffers[i] = new MmapBuffer(argv[i + 1], MAX_FRAME_SIZE);
    }

    socket_setup( );

    // toss up a black frame until we're ready to go
    memset(out->GetFrameBuffer( ), 0, 2*720*480);
    out->Flip( );

    // now, the interesting bits...
    while (1) {
        if (!next_frame_ready) {
            // Advance all streams one frame. Only decode on the one we care about.
            // (if nothing's open, this fails by design...)
            if (!paused || step || step_backward || did_cut) {
                frame_size = sizeof(frame_data);                
                did_cut = false;
                if (buffers[playout_source]->get(frame_data, &frame_size, marks[playout_source]+play_offset)) {
                    try {
                        decoded_frame = mjpeg_decoder.try_decode(frame_data, frame_size);
                        if (decoded_frame) {
                            scaled_frame = scaler.scale(decoded_frame, mjpeg_decoder.get_ctx( ));
                            out->SetNextFrame(scaled_frame);
                            next_frame_ready = true;
                            if (step_backward || run_backward) {
                                play_offset--;
                            } else {
                                play_offset++;
                            }
                        }
                    } catch (FFwrapper::CodecError e) {
                        fprintf(stderr, "codec error - pausing replay\n");
                        paused = true;
                    } catch (FFwrapper::AllocationError e) {
                        fprintf(stderr, "out of memory - pausing replay\n");
                        paused = true;
                    }
                } else {
                    fprintf(stderr, "off end of available video - pausing\n");
                    paused = true;
                }

                if (step) {
                    step = false;
                }

                if (step_backward) {
                    step_backward = false;
                }
            }

        }

        if (out->ReadyForNextFrame( ) && next_frame_ready) {
            out->Flip( );
            next_frame_ready = false;
        } 
        
        if (try_receive(&cmd)) {
            parse_command(&cmd);
        } else {
            // nothing to do, go to sleep...
            usleep(1000);
        }
    }
}
