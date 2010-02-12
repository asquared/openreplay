#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <jpeglib.h>
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

// Preview frames per frame
#define PVW_FPF 3

struct jpeg_decompress_struct jdecomp;
struct jpeg_error_mgr jerror;

void libjpeg_init(void) {
    jdecomp.err = jpeg_std_error(&jerror);
    jpeg_create_decompress(&jdecomp);
}

enum { PREVIEW, LIVE } display_mode;

#if COCAINE
    if (SDL_MUSTLOCK(frame_buf)) {
        SDL_LockSurface(frame_buf);
    }

    pixels = (unsigned char *)frame_buf->pixels;
    stride = 720*3;


    // JPEG decoding leetnees here
    // ohmygodcocaine... can IJG please write better documentation?
    jpeg_mem_src(&jdecomp, (JOCTET *)buf, size, 0, 0);
    jpeg_read_header(&jdecomp, TRUE);
    jpeg_start_decompress(&jdecomp);
    
    while (jdecomp.output_scanline < jdecomp.output_height && jdecomp.output_scanline < frame_buf->h) {
        // cocaine... we're relying on SDL using the same pixel format as JPEG.
        jpeg_read_scanlines(&jdecomp, (JSAMPLE **)(pixels + jdecomp.output_scanline * stride), 1);
    }

    jpeg_finish_decompress(&jdecomp);

    if (SDL_MUSTLOCK(frame_buf)) {
        SDL_UnlockSurface(frame_buf);
    }
#endif

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

            fd = open(fmt_buf, O_CREAT | O_TRUNC | O_WRONLY);

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

int main(int argc, char *argv[])
{
	int frameCount;
        int x, y, j, k;
        int last_logged = -1;
        TTF_Font *font;

        SDL_Event evt;

        signal(SIGCHLD, SIG_IGN); // we don't care about our children...

        libjpeg_init( );
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

        screen = SDL_SetVideoMode(720*3, 480*2, 24, SDL_HWSURFACE | SDL_DOUBLEBUF);
        if (!screen) {
            fprintf(stderr, "Failed to set video mode!\n");
            goto dead;
        }

        frame_buf = SDL_CreateRGBSurface(SDL_HWSURFACE, 720, 480, 24, 0, 0, 0, 0);
        if (!frame_buf) {
            fprintf(stderr, "Failed to create frame buffer!\n");
            goto dead;
        }

        for (;;) {
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
            line_of_text(font, &x, &y, "PREROLL:  %s", timecode_fmt(preroll));
            line_of_text(font, &x, &y, "POSTROLL: %s", timecode_fmt(postroll));
            if (last_logged != -1) {
                line_of_text(font, &x, &y, "LOGGED CLIP: %d", last_logged);
            }


            // Event Processing
            if (SDL_PollEvent(&evt)) {
                if (evt.type == SDL_KEYDOWN) {
                    switch (evt.key.keysym.sym) {
                        case SDLK_m:
                            mark( );
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
                        case SDLK_a:
                            postroll += 5;
                            break;
                        case SDLK_s:
                            postroll -= 5;
                            if (postroll < 0) {
                                postroll = 0;
                            }
                            break;
                        case SDLK_RETURN:
                            last_logged = log_clips( );
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

