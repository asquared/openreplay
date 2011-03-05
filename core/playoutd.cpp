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

#include <getopt.h>
#include <ctype.h>

#include "mmap_buffer.h"
#include "mjpeg_config.h"

#include "playout_ctl.h"
#include "output_adapter.h"

#include "mjpeg_frame.h"

#include "thread.h"

#include <vector>

MmapBuffer *buffers[MAX_CHANNELS];
int marks[MAX_CHANNELS];
int auto_dsk_presets[MAX_CHANNELS];

float play_offset;

int status_socket_fd;

int playout_source = 0;
bool did_cut = false;
bool paused = false;
bool step = false;
bool step_backward = false;
bool run_backward = false;
float playout_speed;
bool overlay_clock = false;

struct DSK {
    Picture *overlay;
    int x, y;
    bool active;
};

#define N_DSK_SLOTS 8
struct DSK dsk_titles[N_DSK_SLOTS];

const char *dsk_files[] = {
    "instantreplay_title.png",
    "reverseangle_title.png",
    "supermotion_title.png",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

#define EVT_PLAYOUT_COMMAND_RECEIVED 0x00000001
class CommandReceiver : public Thread {
    public:
        CommandReceiver(EventHandler *new_dest) : dest(new_dest) {
            socket_setup( );
        }

    protected:
        void socket_setup(void) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(30001);
            inet_aton("127.0.0.1", &addr.sin_addr);

            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd == -1) {
                perror("socket");
                exit(1);
            }

            if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
                perror("bind");
            }
        }

        void run(void) {
            ssize_t ret;
            struct playout_command *cmd;
            for (;;) {
                cmd = new struct playout_command;
                ret = recvfrom(socket_fd, cmd, sizeof(struct playout_command), 0, 0, 0);
                if (ret < 0) {
                    perror("recvfrom");
                    delete cmd;
                } else if (ret < sizeof(struct playout_command)) {
                    fprintf(stderr, "received short command packet\n");
                    delete cmd;
                } else {
                    fprintf(stderr, "Got something\n");
                    dest->post_event(EVT_PLAYOUT_COMMAND_RECEIVED, cmd);
                }
            }
        }

        int socket_fd;
        EventHandler *dest;
};

class StatusSocket {
    public:
        StatusSocket( ) {
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(30002);
            inet_aton("127.0.0.1", &addr.sin_addr);

            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd == -1) {
                perror("socket");
                throw std::runtime_error("StatusSocket socket failed");
            }
        }

        ~StatusSocket( ) {
            close(socket_fd);
        }

        void send_status(const struct playout_status &st) {
            sendto(socket_fd, &st, sizeof(st), 0, 
                (sockaddr *)&addr, sizeof(addr));
        }
    private:
        int socket_fd;
        struct sockaddr_in addr;
};

void update_auto_dsk(int new_source) {
    int dsk = auto_dsk_presets[new_source];
    int j;

    if (dsk != -1) {
        /* turn on the auto DSK and turn off all others */
        for (j = 0; j < N_DSK_SLOTS; j++) {
            if (j == dsk) {
                dsk_titles[j].active = true;
            } else {
                dsk_titles[j].active = false;
            }
        }
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
            update_auto_dsk(playout_source);
            break;

        case PLAYOUT_CMD_CUE_AND_GO:
            did_cut = true;
            paused = false;
            playout_speed = cmd->new_speed;
            memcpy(marks, cmd->marks, sizeof(marks));
            play_offset = 0.0f;
            playout_source = cmd->source;
            update_auto_dsk(playout_source);
            break;

        case PLAYOUT_CMD_ADJUST_SPEED:
            playout_speed = cmd->new_speed;
            break;

        case PLAYOUT_CMD_CUT_REWIND:
            play_offset = 0.0f;
            // fall through to the cut...
        case PLAYOUT_CMD_CUT:
            playout_source = cmd->source;
            update_auto_dsk(playout_source);
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

        case PLAYOUT_CMD_CLOCK_TOGGLE:
            overlay_clock = !overlay_clock;
            break;

        case PLAYOUT_CMD_CLOCK_ON:
            overlay_clock = true;
            break;

        case PLAYOUT_CMD_CLOCK_OFF:
            overlay_clock = false;
            break;

        case PLAYOUT_CMD_DSK_TOGGLE:
            if (cmd->source < N_DSK_SLOTS) {
                dsk_titles[cmd->source].active = !dsk_titles[cmd->source].active;
            }
            break;

        case PLAYOUT_CMD_DSK_ON:
            if (cmd->source < N_DSK_SLOTS) {
                dsk_titles[cmd->source].active = true;
            }
            break;

        case PLAYOUT_CMD_DSK_OFF:
            if (cmd->source < N_DSK_SLOTS) {
                dsk_titles[cmd->source].active = false;
            }
            break;

    }

    fprintf(stderr, "source is now... %d\n", playout_source);
}


class Renderer {
    protected:
        typedef std::vector<struct DSK *> dsk_list_t;
        dsk_list_t dsks;

    public:
        Renderer( ) {
            frame = (struct mjpeg_frame *) malloc(MAX_FRAME_SIZE);
            if (frame == NULL) {
                throw std::runtime_error("Renderer: failed to allocate storage");
            }
            clock_bg = Picture::from_png("hb3_replayclock.png");
        }

        ~Renderer( ) {
            free(frame);
        }

        void add_dsk(struct DSK *dsk) {
            dsks.push_back(dsk);    
        }

        Picture *render_next_frame(void) {
            size_t frame_size;
            timecode_t frame_no;
            uint32_t clock_value;

            Picture *decoded;
            Picture *clock_image;

            // Advance all streams one frame. Only decode on the one we care about.
            // (if nothing's open, this fails by design...)
            frame_size = MAX_FRAME_SIZE;                
            frame_no = marks[playout_source] + play_offset; // round to nearest whole frame

            if (buffers[playout_source]->get(frame, &frame_size, frame_no)) {
                try {
                    if (playout_speed <= 0.8 || paused) {
                        // decode and scan double a field if we can get it
                        // (should get better temporal resolution on slow motion playout)
                        if (play_offset - floorf(play_offset) < 0.5) {
                            decoded = mjpeg_decoder.decode_first_doubled(frame);
                        } else {
                            decoded = mjpeg_decoder.decode_second_doubled(frame);
                        }
                    } else {
                        decoded = mjpeg_decoder.decode_full(frame);
                    }

                    if (step) {
                        play_offset++;
                        step = false;
                    } else if (step_backward) {
                        play_offset--;
                        step_backward = false;
                    } else if (!paused) {
                        play_offset += playout_speed;
                    }

                    if (overlay_clock) {
                        clock_value = frame->clock;
                        clock_image = Picture::copy(clock_bg);
                        clock_image->set_font("Gotham FWN Narrow Bold", 35);
                        if (clock_value >= 600) {
                            clock_image->render_text(
                                20, 4, "%d:%02d\n",
                                clock_value / 600, (clock_value / 10) % 60
                            );
                        } else {
                            clock_image->render_text(
                                20, 4, ":%02d.%d\n",
                                clock_value / 10, clock_value % 10
                            );
                        }

                        decoded->draw(clock_image, 25, 25, 0, 0, 0);

                        Picture::free(clock_image);
                    }
                    
                    /* DSK rendering */
                    dsk_list_t::iterator i;
                    for (i = dsks.begin( ); i != dsks.end( ); i++) {
                        struct DSK *dsk = *i;
                        if (dsk->active && dsk->overlay != NULL) {
                            decoded->draw(dsk->overlay, dsk->x, dsk->y, 0, 0, 0);
                        }
                    }
                
                    return decoded;
                } catch (std::runtime_error e) {
                    fprintf(stderr, "Cannot decode frame\n");
                    return NULL;
                }
            } else {
                fprintf(stderr, "off end of available video\n");
                return NULL;
            }

        }

    protected:
        struct mjpeg_frame *frame;
        MJPEGDecoder mjpeg_decoder;
        Picture *clock_bg;

};

void usage(char *name) {
    fprintf(stderr, "usage: %s [options] buffers\n", name);
    fprintf(stderr, "allowed options: \n");
    fprintf(stderr, "-a, --auto-dsk <number>: assign automatic DSK\n");
    fprintf(stderr, "    This option may be specified multiple times.\n");
    fprintf(stderr, "    the first -a option applies to the first buffer\n");
    fprintf(stderr, "    and so on and so forth\n");
    fprintf(stderr, "-d, --dsk <filename>: specify DSK PNG files\n");
    fprintf(stderr, "    This option may be specified multiple times:\n");
    fprintf(stderr, "    DSKs will be numbered starting from zero.\n");
}

int main(int argc, char *argv[]) {
    struct playout_command cmd;
    struct playout_status status;
    int i;

    const struct option options[] = {
        {
            name: "dsk",
            has_arg: 1,
            flag: NULL,
            val: 'd'
        },
        {
            name: "auto-dsk",
            has_arg: 1,
            flag: NULL,
            val: 'a'
        }
    };

    int opt;
    int dsk_number = 0;
    int auto_dsk_number = 0;

    Renderer r;

    memset(dsk_titles, 0, sizeof(dsk_titles));

    /* start off with auto-DSK off for all cameras */
    for (i = 0; i < MAX_CHANNELS; i++) {
        auto_dsk_presets[i] = -1;
    }
    
    /* parse options, load DSKs, set up auto-DSK */
    while ((opt = getopt_long(argc, argv, "d:a:", options, NULL)) != EOF) {
        switch (opt) {
            case 'd':
                /* load DSK */
                if (dsk_number < N_DSK_SLOTS) {
                    dsk_titles[dsk_number].overlay = Picture::from_png(optarg);
                    dsk_titles[dsk_number].x = 0;
                    dsk_titles[dsk_number].y = 400;
                    dsk_titles[dsk_number].active = false;
                    r.add_dsk(&dsk_titles[dsk_number]);
                    dsk_number++;
                } else {
                    fprintf(stderr, "too many DSKs: can't load %s", optarg);
                }
                break;
            case 'a':
                if (auto_dsk_number < MAX_CHANNELS) {
                    if (isdigit(optarg[0])) {
                        auto_dsk_presets[auto_dsk_number] = atoi(optarg);
                        auto_dsk_number++;
                    } else {
                        fprintf(stderr, "--auto-dsk argument must be numeric\n");
                    }
                } else {
                    fprintf(stderr, "more auto-DSKs than allowed channels");
                }
                break;
            default:
                fprintf(stderr, "invalid argument\n");
                usage(argv[0]);
                exit(1);
        }
    }



    /* generate blank picture */
    Picture *blank = Picture::alloc(720, 480, 1440, UYVY8);
    memset(blank->data, 0, 1440*480);

    Picture *current_decoded, *last_decoded = blank;

    EventHandler evtq;
    CommandReceiver recv(&evtq);
    StatusSocket statsock;
    recv.start( );

    out = new DecklinkOutput(&evtq, 0);


    // initialize buffers
    for (i = 0; optind < argc; ++i, ++optind) {
        buffers[i] = new MmapBuffer(argv[optind], MAX_FRAME_SIZE);
    }

    // now, the interesting bits...
    uint32_t event;
    void *argptr;
    while (1) {
        event = evtq.wait_event(argptr);
        switch (event) {
            case EVT_PLAYOUT_COMMAND_RECEIVED:
                parse_command( (struct playout_command *) argptr );             
                break;
            case EVT_OUTPUT_NEED_FRAME:
                /* try to decode another frame */
                current_decoded = r.render_next_frame( );
                if (current_decoded != NULL) {
                    /* get rid of the old frame if we got a new one */
                    if (last_decoded != blank) {
                        Picture::free(last_decoded);
                    }

                    last_decoded = current_decoded;
                } else {
                    fprintf(stderr, "Frame rendering failed!\n");
                }

                /* if the decode failed just show the last frame decoded instead */
                out->SetNextFrame(last_decoded);
                break;
        }

        // (try to) send status update
        status.valid = 1;
        // timecode is always relative to stream 0
        status.timecode = marks[0] + play_offset;
        status.active_source = playout_source;
        status.clock_on = overlay_clock;

        // TODO: fix the DSK reporting
        status.dsk_on = dsk_titles[0].active;

        /* 
         * note this could block the process! 
         * Remove it and see if issues go away??
         */
        statsock.send_status(status); 
    }
}
