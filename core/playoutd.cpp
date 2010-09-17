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
#include "output_adapter.h"


MmapBuffer *buffers[MAX_CHANNELS];
int marks[MAX_CHANNELS];
float play_offset;

int socket_fd;
struct sockaddr_in timecode_addr;

int playout_source = 0;
bool did_cut = false;
bool paused = false;
bool step = false;
bool step_backward = false;
bool run_backward = false;
float playout_speed;

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
            playout_speed = cmd->new_speed;
            memcpy(marks, cmd->marks, sizeof(marks));
            play_offset = 0.0f;
            playout_source = cmd->source;
            break;

        case PLAYOUT_CMD_ADJUST_SPEED:
            playout_speed = cmd->new_speed;
            break;

        case PLAYOUT_CMD_CUT_REWIND:
            play_offset = 0.0f;
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
    timecode_t frame_no;

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
                frame_no = marks[playout_source] + play_offset; // round to nearest whole frame
                if (buffers[playout_source]->get(frame_data, &frame_size, frame_no)) {
                    try {
                        decoded_frame = mjpeg_decoder.try_decode(frame_data, frame_size);
                        if (decoded_frame) {
                            scaled_frame = scaler.scale(decoded_frame, mjpeg_decoder.get_ctx( ));
                            out->SetNextFrame(scaled_frame);
                            next_frame_ready = true;
                            if (step) {
                                play_offset++;
                            } else if (step_backward) {
                                play_offset--;
                            } else {
                                play_offset += playout_speed;
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
