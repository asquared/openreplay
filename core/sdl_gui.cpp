#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>

#include "ffwrapper.h"

#include "SDL.h"
#include "SDL_image.h"

#include "mmap_buffer.h"
#include "mjpeg_config.h"
#include "playout_ctl.h"


#define PVW_W 720
#define PVW_H 480

SDL_Surface *screen, *frame_buf, *font;

#define FONT_CELL_W 14
#define FONT_CELL_H 20
#define FONT_START ' '

MmapBuffer **buffers;
int n_buffers;
int clip_no = 0;

#define INST_PERIOD 1000
int n_decoded, last_check;

unsigned char frame[MAX_FRAME_SIZE];

// fix an FFmpeg namespace conflict
#define mjpeg_decoder openreplay_mjpeg_decoder

FFwrapper::Decoder mjpeg_decoder( CODEC_ID_MJPEG );
FFwrapper::Scaler scaler(PVW_W, PVW_H, PIX_FMT_BGR24);

int *marks, *replay_ptrs, *replay_ends;

#define MIN_SPEED -20
#define MAX_SPEED 15

// Preroll frames from mark
int preroll = 150;
int postroll = 300; // used for preview only
int qreplay_speed = 10;
int qreplay_cam = 0;
int playout_pid = -1;
int playout_timecode = 0;

// Preview frames per frame
#define PVW_FPF 3


enum _display_mode { PREVIEW, LIVE } display_mode;

int socket_fd;
struct sockaddr_in daemon_addr;

void draw_frame(MmapBuffer *buf, int x, int y, int tc) {
    SDL_Rect rect;
    AVPicture *decoded, *scaled;
    uint8_t *pixels;
    int i;
    
    size_t size;

    rect.x = x;
    rect.y = y;

    // Get the JPEG frame
    size = sizeof(frame);
    if (!buf->get((void *)frame, &size, tc)) {
        // Frame wasn't there. Fill with black.
        fprintf(stderr, "Frame not found!\n");
        SDL_FillRect(frame_buf, 0, 0);
        SDL_BlitSurface(frame_buf, 0, screen, &rect);
    } else {
        try {
            decoded = mjpeg_decoder.try_decode(frame, size);
            if (decoded) {
                scaled = scaler.scale(decoded, mjpeg_decoder.get_ctx( ));
                if (SDL_MUSTLOCK(frame_buf)) {
                    SDL_LockSurface(frame_buf);
                }
                pixels = (uint8_t *)frame_buf->pixels;
                for (i = 0; i < PVW_H; ++i) {
                    memcpy(pixels, scaled->data[0] + scaled->linesize[0] * i, PVW_W * 3);
                    pixels += PVW_W * 3;
                }
                if (SDL_MUSTLOCK(frame_buf)) {
                    SDL_UnlockSurface(frame_buf);
                }
                SDL_BlitSurface(frame_buf, 0, screen, &rect);

                n_decoded++;

                if (n_decoded == INST_PERIOD) {
                    fprintf(stderr, "%f fps\n", (float)INST_PERIOD / (float)(time(NULL) - last_check));
                    last_check = time(NULL);
                    n_decoded = 0;
                }
            }
        } catch (FFwrapper::CodecError e) {
            fprintf(stderr, "error decoding a frame\n");
        } catch (FFwrapper::AllocationError e) {
            fprintf(stderr, "failed to allocate memory in libavcodec\n");
        } 

    }

}

void mark(void) {
    int j;
    for (j = 0; j < n_buffers; ++j) {
        marks[j] = buffers[j]->get_timecode( );
    }
}

void preview(void) {
    int j;
    for (j = 0; j < n_buffers; ++j) {
        replay_ptrs[j] = marks[j] - preroll;
        replay_ends[j] = marks[j] + postroll;
    }

    display_mode = PREVIEW;
}

void return_to_live(void) {
    display_mode = LIVE;
}

const char *timecode_fmt(int timecode) {
    int hr, min, sec, fr;
    static char buf[256];

    fr = timecode % FRAMES_PER_SEC;
    sec = timecode / FRAMES_PER_SEC;
    min = sec / 60;
    sec %= 60;
    hr = min / 60;
    min %= 60;

    snprintf(buf, sizeof(buf) - 1, "%02d:%02d:%02d:%02d", hr, min, sec, fr);
    buf[sizeof(buf) - 1] = 0;

    return buf;
}

void draw_char(char ch, SDL_Surface *dest, SDL_Rect *where) {
    SDL_Rect char_cell;
    int y_offset = 0, x_offset = 0;
    int n_per_row;

    n_per_row = font->w / FONT_CELL_W;

    if (ch >= FONT_START) {
        // compute x and y coordinates
        y_offset = (ch - FONT_START) / n_per_row;
        x_offset = (ch - y_offset * n_per_row - FONT_START);

        y_offset *= FONT_CELL_H;
        x_offset *= FONT_CELL_W;

        char_cell.x = x_offset;
        char_cell.y = y_offset;
        char_cell.w = FONT_CELL_W;
        char_cell.h = FONT_CELL_H;

        if (x_offset < font->w && y_offset < font->h) {
            SDL_BlitSurface(font, &char_cell, dest, where);
        }
    }

    where->x += FONT_CELL_W;
}

void line_of_text(int *x, int *y, const char *fmt, ...) {
    SDL_Color col;
    va_list ap;
    char out_buf[256];
    char *out_ptr;
    SDL_Rect dest;

    dest.x = *x;
    dest.y = *y;

    col.r = 0xff;
    col.g = 0xff;
    col.b = 0xff;

    va_start(ap, fmt);
    vsnprintf(out_buf, sizeof(out_buf) - 1, fmt, ap);
    va_end(ap);
    out_buf[sizeof(out_buf) - 1] = 0;
    out_ptr = out_buf;

    while (*out_ptr) {
        draw_char(*out_ptr, screen, &dest);
        out_ptr++;
    }
    
    *y += FONT_CELL_H + 5;
}


void start_playout(void) {
    int j;

    struct playout_command cmd;
    cmd.source = qreplay_cam;
    cmd.cmd = PLAYOUT_CMD_CUE;
    cmd.new_speed = qreplay_speed/10.0f;

    for (j = 0; j < n_buffers; ++j) {
        cmd.marks[j] = marks[j] - preroll;
    }

    // ready to go... so do it.
    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
}

void live_cut(int new_source) {
    struct playout_command cmd;
    cmd.cmd = PLAYOUT_CMD_CUT;
    cmd.source = new_source;

    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
}

void live_cut_and_rewind(int new_source) {
    struct playout_command cmd;
    cmd.cmd = PLAYOUT_CMD_CUT_REWIND;
    cmd.source = new_source;

    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
}

void adjust_speed(float new_speed) {
    struct playout_command cmd;
    cmd.cmd = PLAYOUT_CMD_ADJUST_SPEED;
    cmd.new_speed = new_speed;

    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
    
}

void do_playout_cmd(int cmd_id) {
    struct playout_command cmd;
    cmd.cmd = cmd_id;

    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
}


void socket_setup(void) {
    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(30002);
    inet_aton("127.0.0.1", &bind_addr.sin_addr);

    // set this to the address of the listening daemon
    memset(&daemon_addr, 0, sizeof(daemon_addr));
    daemon_addr.sin_family = AF_INET;
    daemon_addr.sin_port = htons(30001);
    inet_aton("127.0.0.1", &daemon_addr.sin_addr);

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        exit(1);
    }

    if (bind(socket_fd, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) == -1) {
        perror("bind");
    }

}

void update_playout_timecode(void) {
    int result;
    struct pollfd pfd;

    pfd.fd = socket_fd;
    pfd.events = POLLIN;

    // poll...
    result = poll(&pfd, 1, 1);

    while (pfd.revents & POLLIN) {
        // ready to go!
        recvfrom(socket_fd, &playout_timecode, sizeof(playout_timecode), 0, 0, 0);
        pfd.revents = 0;
        result = poll(&pfd, 1, 1);
    }
}

int main(int argc, char *argv[])
{
        int x, y, j;
        int last_logged = -1;
        int flag = 0;
        int playout_clip = 0;
        int input = 0;

        bool already_selected = false, tally_flag = false;

        SDL_Event evt;
        SDL_Joystick *game_port = 0;

        n_decoded = 0;
        last_check = time(NULL);

        socket_setup( );

        signal(SIGCHLD, SIG_IGN); // we don't care about our children...

        font = IMG_Load("font.bmp");

        if (!font) {
            fprintf(stderr, "Failed to load font!");
        }

	if (argc < 2) {
		fprintf(stderr, "usage: %s buffer_file ...\n", argv[0]);
		return 1;
	}

        n_buffers = argc - 1;
        buffers = (MmapBuffer **)malloc(n_buffers * sizeof(MmapBuffer *));
        marks = (int *)malloc(n_buffers * sizeof(int *));
        replay_ptrs = (int *)malloc(n_buffers * sizeof(int *));
        replay_ends = (int *)malloc(n_buffers * sizeof(int *));

        // initialize buffers from command line args
	for (j = 1; j < argc; j ++) {
	    buffers[j - 1] = new MmapBuffer(argv[j], MAX_FRAME_SIZE); 
	}
        n_buffers = j - 1;

        mark( ); // initialize the mark

	fprintf(stderr, "All buffers ready. Initializing SDL...");

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_NOPARACHUTE) != 0) {
            fprintf(stderr, "Failed to initialize SDL!\n");
        }

        game_port = SDL_JoystickOpen(0);
        if (!game_port) {
            fprintf(stderr, "No joystick found. Auto-control will probably not work.\n");
        }

        screen = SDL_SetVideoMode(1920, 480*2, 24, SDL_HWSURFACE | SDL_DOUBLEBUF);
        if (!screen) {
            fprintf(stderr, "Failed to set video mode!\n");
            goto dead;
        }

        frame_buf = SDL_CreateRGBSurface(SDL_HWSURFACE, PVW_W, PVW_H, 24, 0, 0, 0, 0);
        if (!frame_buf) {
            fprintf(stderr, "Failed to create frame buffer!\n");
            goto dead;
        }


        SDL_EnableUNICODE(1);

        while (!flag) {
            // Video Output
            SDL_FillRect(screen, 0, 0);
            x = 0;
            y = 0;
            for (j = 0; j < n_buffers; j++) {
                if (display_mode == LIVE) {
                    draw_frame(buffers[j], x, y, buffers[j]->get_timecode( ) - 1); 
                } else if (display_mode == PREVIEW) {
                    draw_frame(buffers[j], x, y, replay_ptrs[j]);
                    replay_ptrs[j] += PVW_FPF;
                    if (replay_ptrs[j] >= replay_ends[j]) {
                        display_mode = LIVE;
                    }
                }

                x += frame_buf->w;
                if (x + frame_buf->w > screen->w) {
                    x = 0;
                    y += frame_buf->h;
                }
            }

            // Try to update the timecode
            update_playout_timecode( );

            // x, y points at the top left of the next free square
            x = 720*2 + 10;
            y = 10;

            if (display_mode == LIVE) {
                line_of_text(&x, &y, "LIVE PREVIEW");
                line_of_text(&x, &y, "%s", timecode_fmt(buffers[0]->get_timecode( )));
            } else if (display_mode == PREVIEW) {
                line_of_text(&x, &y, "REPLAY PREVIEW");
                line_of_text(&x, &y, "%s", timecode_fmt(replay_ptrs[0] - (marks[0] - preroll)));
            }
                
            line_of_text(&x, &y, "PLAYOUT: %s", timecode_fmt(playout_timecode));

            //line_of_text(font, &x, &y, "");
            if (input > 0) {
                line_of_text(&x, &y, "%d ", input);
            } else {
                line_of_text(&x, &y, " ");
            }
            line_of_text(&x, &y, "MARK: %s", timecode_fmt(marks[0]));
            line_of_text(&x, &y, "PREROLL:  %s [+qw-, e]", timecode_fmt(preroll));
            line_of_text(&x, &y, "POSTROLL: %s [+as-, d]", timecode_fmt(postroll));
            line_of_text(&x, &y, "PLAYOUT SPEED: %d [+zx-, +/*-, c]", qreplay_speed);
            line_of_text(&x, &y, "PLAYOUT SOURCE: %d [0..9, PgUp]", qreplay_cam + 1);
            line_of_text(&x, &y, "AUTOTAKE [PgDn]");
            line_of_text(&x, &y, "AUTOTAKE + REWIND [Alt+PgDn]");

            // Interface to a video switcher via the game port.
            if (game_port && SDL_JoystickGetButton(game_port, 0) && !already_selected) {
                fprintf(stderr, "button was pushed\n");
                already_selected = true;
                tally_flag = true;
            }

            if (game_port && !SDL_JoystickGetButton(game_port, 0)) {
                fprintf(stderr, "no button pushed\n");
                already_selected = false;
                tally_flag = false;
            }

            if (tally_flag) {
                fprintf(stderr, "Hey! The director took my replay! Let's roll...\n");
                do_playout_cmd(PLAYOUT_CMD_RESUME);
                tally_flag = false;
            }

            // Event Processing
            if (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_KEYDOWN) {
                    switch (evt.key.keysym.sym) {
                        case SDLK_KP0:
                        case SDLK_KP1:
                        case SDLK_KP2:
                        case SDLK_KP3:
                        case SDLK_KP4:
                        case SDLK_KP5:
                        case SDLK_KP6:
                        case SDLK_KP7:
                        case SDLK_KP8:
                        case SDLK_KP9:
                            input *= 10;
                            input += (evt.key.keysym.unicode - L'0');
                            break;
                        case SDLK_m:
                        case SDLK_KP_PLUS:
                            mark( );
                            break;

                        case SDLK_INSERT:
                        case SDLK_p:
                            preview( );
                            break;
                        case SDLK_HOME:
                        case SDLK_l:
                            return_to_live( );
                            break;

                        case SDLK_q:
                            preroll += 5;
                            break;
                        case SDLK_w:
                            preroll -= 5;
                            if (preroll < 0) {
                                preroll = 0;
                            }
                            break;
                        case SDLK_e:
                            preroll = input;
                            input = 0;
                            break;

                        case SDLK_a:
                            postroll += 5;
                            break;
                        case SDLK_s:
                            postroll -= 5;
                            if (postroll < 0) {
                                postroll = 0;
                            }
                            break;
                        case SDLK_d:
                            postroll = input;
                            input = 0;
                            break;

                        case SDLK_z:
                        case SDLK_KP_DIVIDE:
                            qreplay_speed++;
                            if (qreplay_speed > MAX_SPEED) {
                                qreplay_speed = MAX_SPEED;
                            }
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                adjust_speed(qreplay_speed/10.0f);
                            }
                            break;
                        case SDLK_x:
                        case SDLK_KP_MULTIPLY:
                            qreplay_speed--;
                            if (qreplay_speed < MIN_SPEED) {
                                qreplay_speed = MIN_SPEED;
                            }
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                adjust_speed(qreplay_speed/10.0f);
                            }
                            break;
                        case SDLK_c:
                            qreplay_speed = input;
                            input = 0;
                            if (qreplay_speed > MAX_SPEED) {
                                qreplay_speed = MAX_SPEED;
                            }

                            if (qreplay_speed < MIN_SPEED) {
                                qreplay_speed = MIN_SPEED;
                            }
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                adjust_speed(qreplay_speed/10.0f);
                            }
                            break;

                        case SDLK_KP_PERIOD:
                            if (input <= last_logged) {
                                playout_clip = input;
                            }
                            input = 0;
                            break;

                        case SDLK_SPACE:
                        case SDLK_KP_ENTER:
                            start_playout( );
                            break;

                        case SDLK_PAGEUP:
                            if (input > 0 && input - 1 < n_buffers) {
                                qreplay_cam = input - 1; 
                            }
                            input = 0;
                            break;

                        case SDLK_PAGEDOWN:
                            // if alt key was pushed, do a live cut, but also rewind the clips.
                            if (evt.key.keysym.mod & KMOD_ALT) {
                                live_cut_and_rewind(qreplay_cam);
                            } else {
                                live_cut(qreplay_cam);
                            }
                            break;

                        case SDLK_END:
                            live_cut_and_rewind(qreplay_cam);
                            break;

                        case SDLK_1:
                        case SDLK_2:
                        case SDLK_3:
                        case SDLK_4:
                        case SDLK_5:
                        case SDLK_6:
                        case SDLK_7:
                        case SDLK_8:
                        case SDLK_9:
                            qreplay_cam = (evt.key.keysym.unicode - L'0' - 1);
                            if (qreplay_cam >= n_buffers) {
                                qreplay_cam = n_buffers - 1;
                            }
                            

                            break;

                        case SDLK_F9:
                            do_playout_cmd(PLAYOUT_CMD_PAUSE);
                            break;

                        case SDLK_F10:
                            do_playout_cmd(PLAYOUT_CMD_STEP_BACKWARD);
                            break;

                        case SDLK_F11:
                            do_playout_cmd(PLAYOUT_CMD_STEP_FORWARD);
                            break;

                        case SDLK_F12:
                            do_playout_cmd(PLAYOUT_CMD_RESUME);
                            break;

                        case SDLK_ESCAPE:
			    flag = 1;
                            break;
			
                        /* suppress compiler warning */
                        default:
                            break;
                    }
                } 
            }
            // flip pages
            SDL_Flip(screen);
        }
        
dead:
        SDL_Quit( );
}

