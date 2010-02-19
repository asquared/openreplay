#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <signal.h>

#include "SDL.h"
#include "SDL_image.h"
#include "SDL_rwops.h"
#include "SDL_ttf.h"

#include "mmap_buffer.h"
#include "mjpeg_config.h"


SDL_Surface *screen, *frame_buf;

MmapBuffer **buffers;
int n_buffers;
int clip_no = 0;


unsigned char frame[MAX_FRAME_SIZE];

int *marks, *replay_ptrs, *replay_ends;

// Preroll frames from mark
int preroll = 300, postroll = 300;
int qreplay_speed = 10;
int qreplay_cam = 0;
int playout_pid = -1;

// Preview frames per frame
#define PVW_FPF 3


enum { PREVIEW, LIVE } display_mode;

void draw_frame(MmapBuffer *buf, int x, int y, int tc) {
    SDL_Rect rect;
    SDL_RWops *io_buf;
    SDL_Surface *img;
    
    int size;

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
        // do JPEG decode using SDL_image...
        io_buf = SDL_RWFromMem(frame, size);
        img = IMG_Load_RW(io_buf, 1);
        if (!img) {
            fprintf(stderr, "JPEG load failed!\n");
        }
        SDL_BlitSurface(img, 0, screen, &rect);
        SDL_FreeSurface(img);
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
        replay_ends[j] = marks[j];
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

void line_of_text(TTF_Font *font, int *x, int *y, const char *fmt, ...) {
    SDL_Color col;
    va_list ap;
    char out_buf[256];
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

    SDL_Surface *surf = TTF_RenderText_Solid(font, out_buf, col);
    SDL_BlitSurface(surf, 0, screen, &dest);
    
    *y += surf->h + 5;
}

int log_clips(void) {
    // who knows what the hell this'll do...
    int child = fork( );
    int j, tc, size = sizeof(frame), dropped;
    int fd;

    char fmt_buf[256];
    if (child < 0) {
        fprintf(stderr, "Fork failed in log_clips\n");
    } else if (child == 0) {
        // log the clips
        for (j = 0; j < n_buffers; ++j) {
            snprintf(fmt_buf, sizeof(fmt_buf) - 1, "replay%03d_feed%02d.mjpg", clip_no, j);
            fmt_buf[sizeof(fmt_buf) - 1] = 0; 

            fd = open(fmt_buf, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

            tc = marks[j] - preroll;
            dropped = 0;
            while (tc < marks[j] + postroll && dropped < 5) { 
                size = sizeof(frame);
                if (!(buffers[j]->get(frame, &size, tc))) {
                    dropped++;
                }
                tc++;
                // write out
                write(fd, frame, size);
            }
        }
        exit(0);
    } else {
        return clip_no++;        
    }
}

void quick_playout(int clip) {
    if (playout_pid != -1) {
       kill(playout_pid, SIGKILL); 
    }

    int child = fork( );
    char name[256], speed[256];
    snprintf(name, sizeof(name) - 1, "replay%03d_feed%02d.mjpg", clip, qreplay_cam);
    name[sizeof(name) - 1] = 0;
    snprintf(speed, sizeof(speed) - 1, "%f", qreplay_speed / 10.0f);
    speed[sizeof(speed) - 1] = 0;

    if (child < 0) {
        perror("quick_playout fork");
    } else if (child == 0) {
        execl("/home/rpitv/bin/playout.sh", "/home/rpitv/bin/playout.sh", name, speed, (char *)NULL);
	perror("quick_playout execl");
	exit(1);
    } else {
        playout_pid = child;
    }
}

int main(int argc, char *argv[])
{
	int frameCount;
        int x, y, j, k;
        int last_logged = -1;
        TTF_Font *font;
        int flag = 0;
        int playout_clip = 0;
        int input = 0;

        SDL_Event evt;

        signal(SIGCHLD, SIG_IGN); // we don't care about our children...

        TTF_Init( );
        font = TTF_OpenFont("Consolas.ttf", 24);

	if (argc < 3) {
		fprintf(stderr, "usage: %s control_file data_file ...\n", argv[0]);
		return 1;
	}


        buffers = (MmapBuffer **)malloc( (argc - 1) / 2 * sizeof(MmapBuffer *));
        marks = (int *)malloc( (argc - 1) / 2 * sizeof(int *));
        replay_ptrs = (int *)malloc( (argc - 1) / 2 * sizeof(int *));
        replay_ends = (int *)malloc( (argc - 1) / 2 * sizeof(int *));

        k = 0;

	for (j = 1; j < argc - 1; j += 2, k++) {
		buffers[k] = new MmapBuffer(argv[j], argv[j+1], MAX_FRAME_SIZE); 
	}
        n_buffers = k;

	fprintf(stderr, "All buffers ready. Initializing SDL...");

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE) != 0) {
            fprintf(stderr, "Failed to initialize SDL!\n");
        }

        screen = SDL_SetVideoMode(720*2, 480*2, 24, SDL_HWSURFACE | SDL_DOUBLEBUF);
        if (!screen) {
            fprintf(stderr, "Failed to set video mode!\n");
            goto dead;
        }

        frame_buf = SDL_CreateRGBSurface(SDL_HWSURFACE, 720, 480, 24, 0, 0, 0, 0);
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

            // x, y points at the top left of the next free square
            x += 10;
            y += 10;

            if (display_mode == LIVE) {
                line_of_text(font, &x, &y, "LIVE PREVIEW");
                line_of_text(font, &x, &y, "%s", timecode_fmt(buffers[0]->get_timecode( )));
            } else if (display_mode == PREVIEW) {
                line_of_text(font, &x, &y, "REPLAY PREVIEW");
                line_of_text(font, &x, &y, "%s", timecode_fmt(replay_ptrs[0]));
            }

            //line_of_text(font, &x, &y, "");
            if (input > 0) {
                line_of_text(font, &x, &y, "%d ", input);
            } else {
                line_of_text(font, &x, &y, " ");
            }
            line_of_text(font, &x, &y, "MARK: %s", timecode_fmt(marks[0]));
            line_of_text(font, &x, &y, "PREROLL:  %s [+qw-, e]", timecode_fmt(preroll));
            line_of_text(font, &x, &y, "POSTROLL: %s [+as-, d]", timecode_fmt(postroll));
            line_of_text(font, &x, &y, "PLAYOUT SPEED: %d [+zx-, +/*-, c]", qreplay_speed);
            line_of_text(font, &x, &y, "PLAYOUT CLIP: %d [keypad .]", playout_clip);
            line_of_text(font, &x, &y, "PLAYOUT SOURCE: %d [0..9, PgUp]", qreplay_cam + 1);
            if (last_logged != -1) {
                line_of_text(font, &x, &y, "LOGGED CLIP: %d", last_logged);
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
                            last_logged = log_clips( );
                            break;
                        case SDLK_p:
                            preview( );
                            break;
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
                            break;
                        case SDLK_x:
                        case SDLK_KP_MULTIPLY:
                            qreplay_speed--;
                            if (qreplay_speed < 0) {
                                qreplay_speed = 0;
                            }
                            break;
                        case SDLK_c:
                            qreplay_speed = input;
                            break;

                        case SDLK_KP_PERIOD:
                            if (input <= last_logged) {
                                playout_clip = input;
                            }
                            input = 0;
                            break;

                        case SDLK_SPACE:
                        case SDLK_KP_ENTER:
                            quick_playout(last_logged);
                            break;

                        case SDLK_BACKSPACE:
                        case SDLK_KP_MINUS:
                            quick_playout(playout_clip);
                            break;

                        case SDLK_PAGEUP:
                            if (input > 0 && input - 1 < n_buffers) {
                                qreplay_cam = input - 1; 
                            }
                            input = 0;
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

                        case SDLK_ESCAPE:
			    flag = 1;
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

