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


#include "SDL.h"
#include "SDL_image.h"

#include "mmap_buffer.h"
#include "mjpeg_config.h"
#include "playout_ctl.h"
#include "mjpeg_frame.h"

#include <vector>
#include <list>

#include <time.h>


#define PVW_W 720
#define PVW_H 480

SDL_Surface *screen, *frame_buf, *font;
SDL_Surface *vscope_bg;

#define FONT_CELL_W 14
#define FONT_CELL_H 20
#define FONT_START ' '

#define JOYSTICK_SPEED 2.0f

MmapBuffer **buffers;
int n_buffers;
int clip_no = 0;
int hypermotion_source;

#define INST_PERIOD 1000
int n_decoded, last_check;

struct mjpeg_frame *frame = NULL;

MJPEGDecoder mjpeg_decoder;

int *marks, *replay_ptrs, *replay_ends;
int *hypermarks;
std::vector<int *> saved_marks;

#define MIN_SPEED -20
#define MAX_SPEED 15
#define SEEK_STEP 15 // 1/2 second steps
#define TALLY_MARGIN 3

// Preroll frames from mark
int preroll = 150;
int postroll = 300; // used for preview only
int qreplay_speed = 10;
int qreplay_cam = 0;
int playout_pid = -1;

struct playout_status playout_status;

// Preview frames per frame
#define PVW_FPF 2


enum _display_mode { PREVIEW, LIVE, PLAYOUT, SEEK_START, SEEK_HYPER, LIVE_VECTOR, LIVE_WAVEFORM } display_mode;

enum analyze { PICTURE, VECTOR, WAVEFORM };

int socket_fd;
struct sockaddr_in daemon_addr;

void log_message(const char *fmt, ...);

static void putpixel(SDL_Surface *output, int16_t x, int16_t y,
    uint8_t r, uint8_t g, uint8_t b) {
    
    if (SDL_MUSTLOCK(output)) {
        if (SDL_LockSurface(output) != 0) {
            throw std::runtime_error("putpixel could not lock surface");
        }
    }

    uint8_t *pixel_ptr = (uint8_t *) output->pixels;
    pixel_ptr += (output->pitch * y);
    pixel_ptr += (output->format->BytesPerPixel * x);

    /* for now just hope it's RGB... */
    pixel_ptr[0] = r;
    pixel_ptr[1] = g;
    pixel_ptr[2] = b;

    if (SDL_MUSTLOCK(output)) {
        SDL_UnlockSurface(output);
    }
}

void render_vectorscope(SDL_Surface *output, Picture *p) {
    Picture *p_use;

    if (p->pix_fmt == YUV8) {
        p_use = p;
    } else {
        fprintf(stderr, "vectorscope: warning: converting to YUV8 (slow)\n");
        p_use = p->convert_to_format(YUV8);
    }

    /*
     * draw vectorscope graticule (assume an image centered on U=0 V=0)
     */

    SDL_BlitSurface(vscope_bg, NULL, output, NULL);
    
    /* 
     * downsample by 1/4 (every other pixel, every other line) 
     * to cut down drawing time
     */
    uint8_t *pixel_ptr;
    int i, j;
    int8_t u, v;
    int16_t u_ofs, v_ofs, u_scale, v_scale;
    int32_t x, y;

    u_ofs = output->w / 2;
    v_ofs = output->h / 2;
    u_scale = output->w / 2;
    v_scale = output->h / 2;

    /* assume graticule is square and centered on image */
    if (v_scale > u_scale) {
        v_scale = u_scale;
    } else {
        u_scale = v_scale;
    }

    for (i = 0; i < p_use->h; i++) {
        pixel_ptr = p_use->scanline(i);

        for (j = 0; j < p_use->w; j++) {
            u = pixel_ptr[1] - 128;
            v = 128 - pixel_ptr[2];

            x = u;
            x = x * u_scale / 128 + u_ofs;
            y = v;
            y = y * v_scale / 128 + v_ofs;
            

            putpixel(output, x, y, 255, 255, 255);
            pixel_ptr += 3;
        }
    }

    if (p_use != p) {
        Picture::free(p_use);
    }
}

void render_waveform(SDL_Surface *output, Picture *p) {
    Picture *p_use;

    if (p->pix_fmt == YUV8) {
        p_use = p;
    } else {
        fprintf(stderr, "vectorscope: warning: converting to YUV8 (slow)\n");
        p_use = p->convert_to_format(YUV8);
    }

    /*
     * draw graticule 
     */

    /*SDL_BlitSurface(wfm_bg, NULL, output, NULL);*/
    SDL_FillRect(output, NULL, 0); /* erase to black */
    
    /* 
     * downsample by 1/4 (every other pixel, every other line) 
     * to cut down drawing time
     */
    uint8_t *pixel_ptr;
    int i, j;
    int32_t x, y;
    int16_t y_scale = 480;


    for (i = 0; i < p_use->h; i++) {
        pixel_ptr = p_use->scanline(i);

        for (j = 0; j < p_use->w; j++) {
            x = j;
            y = (256 - pixel_ptr[0]) * y_scale / 256;

            putpixel(output, x, y, 255, 255, 255);
            pixel_ptr += 3;
        }
    }

    if (p_use != p) {
        Picture::free(p_use);
    }
}

void draw_frame(MmapBuffer *buf, int x, int y, int tc, enum analyze analyze, uint32_t *scoreboard_clock = 0) {
    SDL_Rect rect;
    Picture *decoded;
    uint8_t *pixels;
    int i;
    int blit_w;
    
    size_t size;

    rect.x = x;
    rect.y = y;

    // Get the JPEG frame
    if (frame == NULL) {
        frame = (struct mjpeg_frame *)malloc(MAX_FRAME_SIZE);
    }

    size = MAX_FRAME_SIZE;

    if (!buf->get((void *)frame, &size, tc)) {
        // Frame wasn't there. Fill with black.
        fprintf(stderr, "Frame not found!\n");
        SDL_FillRect(frame_buf, 0, 0);
        SDL_BlitSurface(frame_buf, 0, screen, &rect);
    } else {
        if (scoreboard_clock != NULL) {
            *scoreboard_clock = frame->clock;
        }
        try {
            if (analyze == PICTURE) {
                decoded = mjpeg_decoder.decode_full(frame, RGB8);
            } else {
                decoded = mjpeg_decoder.decode_full(frame, YUV8);
            }

            if (decoded) {
                /* transfer decoded data to SDL_Surface and blit onto screen */
                /* (does it make more sense just to lock the screen surface?) */
                
                if (analyze == PICTURE) {
                    /* draw the picture */
                    if (SDL_MUSTLOCK(frame_buf)) {
                        SDL_LockSurface(frame_buf);
                    }

                    pixels = (uint8_t *)frame_buf->pixels;
                    if (decoded->w > PVW_W) {
                        blit_w = PVW_W;
                    } else {
                        blit_w = decoded->w;
                    }

                    for (i = 0; i < PVW_H && i < decoded->h; ++i) {
                        memcpy(pixels, decoded->data + decoded->line_pitch * i, blit_w * 3);
                        pixels += PVW_W * 3;
                    }

                    if (SDL_MUSTLOCK(frame_buf)) {
                        SDL_UnlockSurface(frame_buf);
                    }
                } else if (analyze == VECTOR) {
                    /* render vectorscope display of image */
                    render_vectorscope(frame_buf, decoded);
                } else if (analyze == WAVEFORM) {
                    render_waveform(frame_buf, decoded);
                }

                Picture::free(decoded);


                SDL_BlitSurface(frame_buf, 0, screen, &rect);

                n_decoded++;

                if (n_decoded == INST_PERIOD) {
                    fprintf(stderr, "%f fps\n", (float)INST_PERIOD / (float)(time(NULL) - last_check));
                    last_check = time(NULL);
                    n_decoded = 0;
                }
            } else {
                fprintf(stderr, "decode failed!\n");
            }
        } catch (...) {
            fprintf(stderr, "unexpected decode error\n");
        }

    }

}

void mark(void) {
    int j;
    for (j = 0; j < n_buffers; ++j) {
        marks[j] = buffers[j]->get_timecode( ) - preroll;
    }
}

void mark_playout(void) {
    int j;
    timecode_t displacement = playout_status.timecode - marks[0];
    for (j = 0; j < n_buffers; ++j) {
        marks[j] += displacement;
    }
}

void hypermark(void) {
    int j;
    for (j = 0; j < n_buffers; ++j) {
        hypermarks[j] = buffers[j]->get_timecode( );
    }
}

void save_mark(void) {
    int *saved_mark = new int[n_buffers];

    // alignment safe??
    memcpy(saved_mark, marks, sizeof(int) * n_buffers);

    saved_marks.push_back(saved_mark);

    log_message("saved mark: number %d", saved_marks.size( ) - 1);
}

void save_hypermark(void) {
    int *saved_mark = new int[n_buffers];

    // alignment safe??
    memcpy(saved_mark, hypermarks, sizeof(int) * n_buffers);

    saved_marks.push_back(saved_mark);

    log_message("saved hypermotion: number %d", saved_marks.size( ) - 1);

}

void recall_mark(int n) {
    if (n < 0) {
        log_message("recall: (negative) mark number %d not found", n);
        return;
    }

    if (n >= saved_marks.size( )) { 
        log_message("recall: mark number %d not found", n);
        return;
    }
    memcpy(marks, saved_marks[n], sizeof(int) * n_buffers);
}

void write_file_from_mark(int n = -1) {
    int *mark_to_write = marks;
    char time_str[256];
    time_t tv;
    struct tm *tm;
    char *fn;
    int i;
    struct mjpeg_frame *frame;
    int output_fd;
    size_t frame_size;
    timecode_t j;
    ssize_t written;

    pid_t child_pid;

    if (n >= 0) {
        if (n < saved_marks.size( )) {
            mark_to_write = saved_marks[n];
        } else {
            log_message("failed to write\n");
            log_message("no mark #%d\n", n);
            return;
        }
    }


    child_pid = fork( );
    if (child_pid == -1) {
        log_message("fork failed: can't write\n");
        return;
    }

    if (child_pid == 0) {
        /* child process */
        tv = time(NULL);
        tm = localtime(&tv);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d-%H_%M_%S", tm);
        time_str[sizeof(time_str) - 1] = 0;

        frame = (struct mjpeg_frame *) malloc(MAX_FRAME_SIZE);
        if (!frame) {
            fprintf(stderr, "write child process: allocate failed\n");
            exit(1);
        }

        /* for each camera feed */
        for (i = 0; i < n_buffers; ++i) {
            /* tell buffer we forked */
            buffers[i]->on_fork( );

            /* construct filename */
            asprintf(&fn, "replay_save_%s_cam%d.mjpg", time_str, i + 1);

            /* open output file */
            output_fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (output_fd < 0) {
                perror("open output");
                exit(1);
            }

            /* for each frame */
            for (j = mark_to_write[i];
                   j < mark_to_write[i] + postroll; j++) {
                /* get frame from buffer */
                frame_size = MAX_FRAME_SIZE;
                if (buffers[i]->get(frame, &frame_size, j)) {
                    /* write frame to output file */
                    written = write(output_fd, frame->data, frame->f1size);
                    if (written < frame->f1size) {
                        perror("write");
                        exit(1);
                    }
                    if (frame->f2size > 0) {
                        written = write(output_fd, frame->data + frame->f1size, frame->f2size);
                        if (written < frame->f2size) {
                            perror("write");
                            exit(1);
                        }
                    }
                } else {
                    fprintf(stderr, "write: camera %d: could not get frame %d\n", i, j);
                }
            }
            /* close output file */
            close(output_fd);

            /* free filename */
            free(fn);
        }

        exit(0);
    } else {
        /* parent process - just return and let child do its thing */
        /* maybe someday we'll check for signals */
    }
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

typedef std::list<char *> message_log_t;
message_log_t message_log;

void log_message(const char *fmt, ...) {
    va_list ap;
    char *sptr;

    va_start(ap, fmt);
    vasprintf(&sptr, fmt, ap);
    va_end(ap);

    message_log.push_front(sptr);    
}

void draw_message_log(int x, int y, int h) {
    /* most recent log messages go at the top and we'll work our way down */
    message_log_t::iterator i;
    int yf = y + h;

    for (i = message_log.begin( ); i != message_log.end( ); ++i) {
        line_of_text(&x, &y, "%s", *i);
        // stop if we're off the end of the screen
        if (y >= yf) {
            break;
        }
    }
}


void cue_playout(void) {
    int j;

    struct playout_command cmd;
    cmd.source = qreplay_cam;
    cmd.cmd = PLAYOUT_CMD_CUE;
    cmd.new_speed = qreplay_speed/10.0f;

    for (j = 0; j < n_buffers; ++j) {
        cmd.marks[j] = marks[j];
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
    int j;
    struct playout_command cmd;
    cmd.cmd = PLAYOUT_CMD_CUE_AND_GO;
    cmd.source = new_source;
    cmd.new_speed = qreplay_speed/10.0f;

    for (j = 0; j < n_buffers; ++j) {
        cmd.marks[j] = marks[j];
    }

    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
}

void live_cut_to_hypermark(void) {
    /* 
     * Is it just me or is it getting rather damp in here? 
     * Anyway... this macro-kludge cues RPITV Hyper-Motion(tm).
     * To do so we should set the speed back to 10 (since 5 is really slow).
     * Also, we need to cut to the "hyper-motion" mark. This is marked by
     * the hypermark( ) function, which marks the current frame (no preroll).
     * Workflow: Hit `h` to mark the in-point of a hyper-motion clip.
     * Hit `j` to roll it! 
     */

    int j;
    struct playout_command cmd;
    cmd.cmd = PLAYOUT_CMD_CUE_AND_GO;
    cmd.source = hypermotion_source;
    cmd.new_speed = 10;

    for (j = 0; j < n_buffers; ++j) {
        cmd.marks[j] = hypermarks[j];
    }

    sendto(socket_fd, &cmd, sizeof(cmd), 0, (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));
    
}

void set_hypermotion_source(int source) {
    if (source >= 0 && source < n_buffers) {
        hypermotion_source = source;
    }
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


void toggle_dsk(int dsk_number) {
    struct playout_command cmd;
    cmd.cmd = PLAYOUT_CMD_DSK_TOGGLE;
    cmd.source = dsk_number;

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

void update_playout_status(void) {
    int result;
    struct pollfd pfd;

    pfd.fd = socket_fd;
    pfd.events = POLLIN;

    // poll...
    result = poll(&pfd, 1, 1);

    while (pfd.revents & POLLIN) {
        // ready to go!
        recvfrom(socket_fd, &playout_status, sizeof(playout_status), 0, 0, 0);
        pfd.revents = 0;
        result = poll(&pfd, 1, 1);
    }
}

void seek_mark_by(int n_frames) {
    for (int j = 0; j < n_buffers; ++j) {
        marks[j] += n_frames;
    }
}

void seek_hypermark_by(int n_frames) {
    for (int j = 0; j < n_buffers; ++j) {
        hypermarks[j] += n_frames;
    }
}


void preroll_up(int amount) {
    preroll += amount;
}

void preroll_down(int amount) {
    preroll -= amount;
    if (preroll < 0) {
        preroll = 0;
    }
}

void preroll_set(int value) {
    if (value > 0) {
        preroll = value;
    } else {
        preroll = 0;
    }
}

void postroll_set(int value) {
    if (value > 0) {
        postroll = value;
    } else {
        postroll = 0;
    }
}

void postroll_up(int amount) {
    postroll += amount;
}

void postroll_down(int amount) {
    postroll -= amount;
    if (postroll < 0) {
        postroll = 0;
    }
}

void playout_speed_up(int amount) {
    qreplay_speed += amount;
    if (qreplay_speed > MAX_SPEED) {
        qreplay_speed = MAX_SPEED;
    }
}

void playout_speed_down(int amount) {
    qreplay_speed -= amount;
    if (qreplay_speed < MIN_SPEED) {
        qreplay_speed = MIN_SPEED;
    }
}

void playout_speed_set(int new_speed) {
    qreplay_speed = new_speed;
    if (qreplay_speed > MAX_SPEED) {
        qreplay_speed = MAX_SPEED;
    }
    if (qreplay_speed < MIN_SPEED) {
        qreplay_speed = MIN_SPEED;
    }
}

void playout_speed_live_change(void) {
    adjust_speed(qreplay_speed/10.0f);
}

void playout_speed_reverse(void) {
    playout_speed_set(-qreplay_speed);
}

/* note that this takes the 1-based camera number */
void camera_set(int camera) {
    if (camera > 0 && camera <= n_buffers) {
        qreplay_cam = camera - 1;
    }
}

int camera_get(void) {
    return qreplay_cam;
}

void display_mode_seek_start(void) {
    display_mode = SEEK_START;
}

void display_mode_seek_hypermotion(void) {
    display_mode = SEEK_HYPER;
}

void display_mode_preview(void) {
    int j;
    for (j = 0; j < n_buffers; ++j) {
        replay_ptrs[j] = marks[j];
        replay_ends[j] = marks[j] + postroll;
    }

    display_mode = PREVIEW;
}

void display_mode_live(void) {
    display_mode = LIVE;
}

void display_mode_playout(void) {
    display_mode = PLAYOUT;
}

int input;

int consume_numeric_input(void) {
    int temp = input;
    input = 0;
    return temp;
}


void draw_tally(int x, int y, int r, int g, int b) {
    SDL_Rect rc;

    rc.x = x - TALLY_MARGIN;
    rc.y = y - TALLY_MARGIN;
    rc.w = frame_buf->w + 2*TALLY_MARGIN;
    rc.h = frame_buf->h + 2*TALLY_MARGIN;

    SDL_FillRect(screen, &rc, 
        SDL_MapRGB(screen->format, r, g, b)
    );
}

class Seeker {
    public:
        Seeker(float start_rate_, float max_rate_, float growth_rate_, int growth_frames_) 
                : start_rate(start_rate_), max_rate(max_rate_),
                  growth_rate(growth_rate_), growth_frames(growth_frames_), dir(0) { }

        int update_and_get_offset( ) {
            int ret;

            if (dir == 0) {
                return 0;
            } else {
                acc += rate;
                
                /* subtract out integer part and return it */
                ret = floorf(acc);
                acc -= ret;

                frames++;
                if (frames == growth_frames) {
                    frames = 0;
                    rate = rate * growth_rate;
                    if (rate > max_rate) {
                        rate = max_rate;
                    }
                }

                return rate * dir;
            }
        }

        void start_forward( ) {
            if (dir == 0) {
                rate = start_rate;
                frames = 0;
                dir = 1;
                acc = 0;
            }
        }

        void start_back( ) {
            if (dir == 0) {
                rate = start_rate;
                frames = 0;
                dir = 1;
                acc = 0;
            }
        }

        void stop( ) {
            dir = 0;
        }

    private:
        float start_rate;
        float max_rate;
        float growth_rate;
        int growth_frames;
        float rate;
        int frames;
        int dir;
        float acc;
};

int main(int argc, char *argv[])
{
        int x, y, j;
        int xt, yt;
        // this kludge works if the video frame size isn't an even multiple
        // of the whole screen size... :-/
        int text_start_x;
        int flag = 0;
        int display_cam;
        enum analyze analyze_mode = PICTURE;

        uint32_t sbc; /* score board clock */

        input = 0;

        memset((void *)&playout_status, 0, sizeof(playout_status));

        SDL_Event evt;

        Seeker mark_seeker(2.0f, 60.0f, 1.2f, 30);
        Seeker hypermark_seeker(2.0f, 60.0f, 1.2f, 30);

        n_decoded = 0;
        last_check = time(NULL);

        socket_setup( );

        signal(SIGCHLD, SIG_IGN); // we don't care about our children...

        font = IMG_Load("font.bmp");

        if (!font) {
            fprintf(stderr, "Failed to load font!");
            return 1;
        }

        vscope_bg = IMG_Load("vgraticule.bmp");

        if (!vscope_bg) {
            fprintf(stderr, "Could not load vectorscope graticule image. Vectorscope not available\n");
        }

        if (argc < 2) {
            fprintf(stderr, "usage: %s buffer_file ...\n", argv[0]);
            return 1;
        }

        n_buffers = argc - 1;
        buffers = (MmapBuffer **)malloc(n_buffers * sizeof(MmapBuffer *));
        marks = (int *)malloc(n_buffers * sizeof(int *));
        hypermarks = (int *)malloc(n_buffers * sizeof(int *));
        replay_ptrs = (int *)malloc(n_buffers * sizeof(int *));
        replay_ends = (int *)malloc(n_buffers * sizeof(int *));

        // initialize buffers from command line args
	for (j = 1; j < argc; j ++) {
	    buffers[j - 1] = new MmapBuffer(argv[j], MAX_FRAME_SIZE); 
	}

    n_buffers = j - 1;

    mark( ); // initialize the mark
    hypermark( ); // initialize hyper-motion mark

	fprintf(stderr, "All buffers ready. Initializing SDL...");

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_NOPARACHUTE) != 0) {
            fprintf(stderr, "Failed to initialize SDL!\n");
        }


        screen = SDL_SetVideoMode(1920, 480*2, 24, SDL_HWSURFACE | SDL_DOUBLEBUF);
        if (!screen) {
            fprintf(stderr, "Failed to set video mode!\n");
            goto dead;
        }

        frame_buf = SDL_CreateRGBSurface(SDL_HWSURFACE, PVW_W, PVW_H, 24, 0xff, 0xff00, 0xff0000, 0);
        if (!frame_buf) {
            fprintf(stderr, "Failed to create frame buffer!\n");
            goto dead;
        }


        SDL_EnableUNICODE(1);

        while (!flag) {
            // Video Output
            SDL_FillRect(screen, 0, 0);
            x = TALLY_MARGIN;
            y = TALLY_MARGIN;
            text_start_x = 0;
            for (j = 0; j < n_buffers && j < 4; j++) {
                if (j == 0 && camera_get( ) >= 4) {
                    display_cam = camera_get( );
                } else {
                    display_cam = j;
                }

                /* Draw tally indicators */
                if (display_cam == playout_status.active_source 
                        && playout_status.valid) {
                    /* red "live" tally */
                    draw_tally(x, y, 255, 0, 0);
                } else if (display_cam == camera_get( )) {
                    /* green "preview" tally */
                    draw_tally(x, y, 0, 255, 0);
                }

                /* Draw frames as appropriate for the current mode. */
                if (display_mode == LIVE || display_mode == PLAYOUT) {
                    int d_timecode;

                    if (display_mode == LIVE) {
                        d_timecode = buffers[display_cam]->get_timecode( ) - 1;
                    } else if (display_mode == PLAYOUT) {
                        /* playout_status.timecode is relative to camera 1 */
                        d_timecode = playout_status.timecode
                            + marks[display_cam]
                            - marks[0];
                    }

                    draw_frame(buffers[display_cam], x, y, 
                        d_timecode, PICTURE, &sbc); 
                    if (sbc > 60) {
                        line_of_text(&xt, &yt, "scoreboard: %02d:%02d", 
                            sbc / 600, (sbc / 10) % 60);
                    } else {
                        line_of_text(&xt, &yt, "scoreboard: :%02d.%02d", 
                            (sbc / 10) % 60, sbc % 10);
                    }
                } else if (display_mode == LIVE_VECTOR) {
                    draw_frame(buffers[display_cam], x, y, 
                        buffers[display_cam]->get_timecode( ) - 1, VECTOR);
                } else if (display_mode == LIVE_WAVEFORM) {
                    draw_frame(buffers[display_cam], x, y, 
                        buffers[display_cam]->get_timecode( ) - 1, WAVEFORM);
                } else if (display_mode == PREVIEW) {
                    draw_frame(buffers[display_cam], x, y, 
                        replay_ptrs[display_cam], PICTURE, &sbc);
                    if (sbc > 60) {
                        line_of_text(&xt, &yt, "scoreboard: %02d:%02d", 
                            sbc / 600, (sbc / 10) % 60);
                    } else {
                        line_of_text(&xt, &yt, "scoreboard: :%02d.%02d", 
                            (sbc / 10) % 60, sbc % 10);
                    }
                    replay_ptrs[display_cam] += PVW_FPF;
                    if (replay_ptrs[display_cam] >= replay_ends[display_cam]) {
                        display_mode = LIVE;
                    }
                } else if (display_mode == SEEK_START) {
                    draw_frame(buffers[display_cam], x, y, 
                        marks[display_cam], PICTURE, &sbc);
                    if (sbc > 60) {
                        line_of_text(&xt, &yt, "scoreboard: %02d:%02d", sbc / 600, (sbc / 10) % 60);
                    } else {
                        line_of_text(&xt, &yt, "scoreboard: :%02d.%02d", (sbc / 10) % 60, sbc % 10);
                    }
                } else if (display_mode == SEEK_HYPER) {
                    draw_frame(buffers[display_cam], x, y, 
                        hypermarks[display_cam], PICTURE, &sbc);
                    if (sbc > 60) {
                        line_of_text(&xt, &yt, "scoreboard: %02d:%02d", sbc / 600, (sbc / 10) % 60);
                    } else {
                        line_of_text(&xt, &yt, "scoreboard: :%02d.%02d", (sbc / 10) % 60, sbc % 10);
                    }
                }

                xt = x;
                yt = y;
                line_of_text(&xt, &yt, "CAM %d", display_cam + 1);

                x += frame_buf->w + 2*TALLY_MARGIN;
                if (x + frame_buf->w > screen->w) {
                    text_start_x = x;
                    x = 0;
                    y += frame_buf->h + 2*TALLY_MARGIN;
                }
            }

            if (text_start_x == 0) {
                text_start_x = x;
            }


            // Try to update the playout information
            update_playout_status( );

            // x, y points at the top left of the big empty column 
            // at the right of the screen...
            x = text_start_x;
            y = 10;

            if (display_mode == LIVE) {
                line_of_text(&x, &y, "LIVE PREVIEW");
                line_of_text(&x, &y, "%s", timecode_fmt(buffers[0]->get_timecode( )));
            } else if (display_mode == LIVE_VECTOR) {
                line_of_text(&x, &y, "LIVE VECTORSCOPE");
                line_of_text(&x, &y, "%s", timecode_fmt(buffers[0]->get_timecode( )));
            } else if (display_mode == LIVE_WAVEFORM) {
                line_of_text(&x, &y, "LIVE WAVEFORM");
                line_of_text(&x, &y, "%s", timecode_fmt(buffers[0]->get_timecode( )));
            } else if (display_mode == PREVIEW) {
                line_of_text(&x, &y, "REPLAY PREVIEW");
                line_of_text(&x, &y, "%s", timecode_fmt(replay_ptrs[0] - marks[0]));
            } else if (display_mode == SEEK_START) {
                line_of_text(&x, &y, "PLAYOUT START FRAME");
                line_of_text(&x, &y, "");
            }
                
            if (playout_status.valid) {
                line_of_text(&x, &y, "PLAYOUT: %s", timecode_fmt(playout_status.timecode));
                line_of_text(
                    &x, &y, "PLAYOUT SOURCE: [CAM %d%s%s]", 
                    playout_status.active_source + 1, 
                    playout_status.clock_on? " CLOCK" : "",
                    playout_status.dsk_on? " DSK" : ""
                );
            } else {
                line_of_text(&x, &y, "PLAYOUT SERVER NOT RUNNING??");
                line_of_text(&x, &y, "STATUS UNKNOWN");
            }

            //line_of_text(font, &x, &y, "");
            if (input > 0) {
                line_of_text(&x, &y, "%d ", input);
            } else {
                line_of_text(&x, &y, " ");
            }
            line_of_text(&x, &y, "MARK: %s", timecode_fmt(marks[0]));
            line_of_text(&x, &y, "HYPERMOTION MARK: %s", timecode_fmt(hypermarks[0]));
            line_of_text(&x, &y, "HYPERMOTION SOURCE: CAM %d", hypermotion_source + 1);
            line_of_text(&x, &y, "PREROLL:  %s [+qw-, e]", timecode_fmt(preroll));
            line_of_text(&x, &y, "POSTROLL: %s [+as-, d]", timecode_fmt(postroll));
            line_of_text(&x, &y, "PLAYOUT SPEED: %d [+zx-, +/*-, c]", qreplay_speed);
            line_of_text(&x, &y, "PLAYOUT SOURCE: %d [0..9, PgUp]", qreplay_cam + 1);
            line_of_text(&x, &y, "AUTOTAKE [PgDn]");
            line_of_text(&x, &y, "AUTOTAKE + REWIND [Alt+PgDn]");
            draw_message_log(x, y, screen->h - y);

            // Seeking
            seek_mark_by(mark_seeker.update_and_get_offset( ));
            seek_hypermark_by(hypermark_seeker.update_and_get_offset( ));

            // Event Processing
            if (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_KEYUP) {
                    switch (evt.key.keysym.sym) {
                        case SDLK_F5:
                        case SDLK_F6:
                            mark_seeker.stop( );
                            break;

                        case SDLK_COMMA:
                        case SDLK_PERIOD:
                            hypermark_seeker.stop( );
                            break;
                    }
                } else if (evt.type == SDL_KEYDOWN) {
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
                            mark( );
                            save_mark( );
                            break;

                        case SDLK_g:
                            set_hypermotion_source(consume_numeric_input( ) - 1);
                            break;

                        case SDLK_h:
                            hypermark( );
                            save_hypermark( );
                            break;

                        case SDLK_j:
                            live_cut_to_hypermark( );
                            break;

                        case SDLK_i:
                            mark_playout( );
                            break;

                        case SDLK_KP_PLUS:
                            mark( );
                            save_mark( );
                            cue_playout( );
                            break;

                        case SDLK_INSERT:
                        case SDLK_p:
                            display_mode_preview( );
                            break;

                        case SDLK_HOME:
                        case SDLK_l:
                            display_mode_live( );
                            break;

                        case SDLK_DELETE:
                            display_mode_playout( );
                            break;

                        case SDLK_q:
                            preroll_up(5);
                            break;
                        case SDLK_w:
                            preroll_down(5);
                            break;

                        case SDLK_e:
                            preroll_set(consume_numeric_input( ));
                            input = 0;
                            break;

                        case SDLK_a:
                            postroll_up(5);
                            break;

                        case SDLK_s:
                            postroll_down(5);
                            break;

                        case SDLK_d:
                            postroll_set(consume_numeric_input( ));
                            break;

                        case SDLK_z:
                        case SDLK_KP_DIVIDE:
                            playout_speed_up(1);
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                playout_speed_live_change( );
                            }
                            break;

                        case SDLK_r:
                            write_file_from_mark( );
                            break;

                        case SDLK_t:
                            write_file_from_mark(consume_numeric_input( ));
                            break;

                        case SDLK_x:
                        case SDLK_KP_MULTIPLY:
                            playout_speed_down(1);
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                playout_speed_live_change( );
                            }

                            break;
                        case SDLK_c:
                            playout_speed_set(consume_numeric_input( ));
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                playout_speed_live_change( );
                            }
                            break;

                        case SDLK_SPACE:
                            cue_playout( );
                            do_playout_cmd(PLAYOUT_CMD_RESUME);
                            break;

                        case SDLK_KP_ENTER:
                            cue_playout( );
                            break;

                        case SDLK_PAGEUP:
                            camera_set(consume_numeric_input( ));
                            break;

                        case SDLK_PAGEDOWN:
                            // if alt key was pushed, do a live cut, but also rewind the clips.
                            if (evt.key.keysym.mod & KMOD_ALT) {
                                live_cut_and_rewind(camera_get( ));
                            } else {
                                live_cut(camera_get( ));
                            }
                            break;

                        case SDLK_END:
                            /* by default, return from slow-mo on rewind */
                            if (evt.key.keysym.mod & KMOD_CTRL) {
                                playout_speed_set(10);   
                                playout_speed_live_change( );
                            }
                            live_cut_and_rewind(camera_get( ));
                            break;

                        case SDLK_BACKSPACE:
                            save_mark( );
                            break;

                        case SDLK_BACKSLASH:
                            recall_mark(consume_numeric_input( ));
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
                            camera_set(evt.key.keysym.unicode - L'0');
                            break;

                        case SDLK_F1:
                            playout_speed_set(2);
                            playout_speed_live_change( );
                            break;

                        case SDLK_F2:
                            playout_speed_set(5);
                            playout_speed_live_change( );
                            break;

                        case SDLK_F3:
                            playout_speed_set(10);
                            playout_speed_live_change( );
                            break;

                        case SDLK_F4:
                            playout_speed_reverse( );
                            if (!(evt.key.keysym.mod & KMOD_CTRL)) {
                                playout_speed_live_change( );
                            }
                            break;

                        case SDLK_F5:
                            mark_seeker.start_back( );
                            break;

                        case SDLK_F6:
                            mark_seeker.start_forward( );
                            break;


                        case SDLK_COMMA:
                            hypermark_seeker.start_back( );
                            break;

                        case SDLK_PERIOD:
                            hypermark_seeker.start_forward( );
                            break;

                        case SDLK_F7:
                            display_mode_seek_start( );
                            break;

                        case SDLK_F8:
                            display_mode_seek_hypermotion( );
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
                        
                        case SDLK_b:
                            do_playout_cmd(PLAYOUT_CMD_CLOCK_TOGGLE);
                            break;

                        case SDLK_k:
                            toggle_dsk(consume_numeric_input( ));
                            break;

                        case SDLK_ESCAPE:
                            flag = 1;
                            break;

                        case SDLK_v: /* Vectorscope */
                            if (vscope_bg) {
                                display_mode = LIVE_VECTOR;
                            }
                            break;

                        case SDLK_f: /* waveForm monitor */
                            display_mode = LIVE_WAVEFORM;
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

