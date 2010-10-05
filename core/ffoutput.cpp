#include "output_adapter.h"
#include "pipe_buffer.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>

#include <list>

int count_args(char **args) {
    int count = 0;

    while (*args) {
        args++;
        count++;
    }

    return count;
}

#define VIDEO_LINE_SIZE 2*720
#define VIDEO_FRAME_SIZE VIDEO_LINE_SIZE*480
#define AUDIO_BLOCK_SIZE 4

/* start FFmpeg in a child process. Audio and video fds come out. */
pid_t start_ffmpeg(int argc, char **argv, int *audio_pipe, int *video_pipe) {
    /* output args: "-f rawvideo -s 720x480 -pix_fmt uyvy422 pipe:xxx -f s16le -ar 48000 */
    char *ff_video_args[] = { "-f", "rawvideo", "-s", "720x480", "-pix_fmt", "uyvy422", NULL };
    char *ff_audio_args[] = { "-f", "s16le", "-ar", "48000", NULL };
    char *vpipe_arg = 0, *apipe_arg = 0;
    char **args;
    char **argp;
    pid_t child_pid;

    int i;

    /* pipe[0] = read, pipe[1] = write */
    int vpipe[2], apipe[2];

    if (pipe(vpipe) < 0) {
        perror("video pipe");
        throw std::runtime_error("pipe failed");
    }

    if (pipe(apipe) < 0) {
        perror("audio pipe");
        throw std::runtime_error("pipe failed");
    }

    args = new char *[
        count_args(ff_video_args) + count_args(ff_audio_args)
        + argc + 1 /* ffmpeg */ + 1 /* null terminator */
        + 1 /* video pipe# */ + 1 /* audio pipe# */
    ];

    args[0] = "ffmpeg"; /* name of program to run */
    argp = &args[1];

    /* copy input arguments */
    for (i = 0; i < argc; ++i) {
        *argp = argv[i];
        argp++;
    }

    /* copy ff_video_args */
    for (i = 0; i < count_args(ff_video_args); ++i) {
        *argp = ff_video_args[i];
        argp++;
    }

    /* set video pipe */
    asprintf(&vpipe_arg, "pipe:%d", vpipe[1]);
    *argp = vpipe_arg;
    argp++;

    /* copy ff_audio_args */
    for (i = 0; i < count_args(ff_audio_args); ++i) {
        *argp = ff_audio_args[i];
        argp++;
    }

    /* set audio pipe */
    asprintf(&apipe_arg, "pipe:%d", apipe[1]);
    *argp = apipe_arg;
    argp++;

    /* terminate argument list */
    *argp = NULL;

    /* fork it */
    child_pid = fork( );
    if (child_pid == -1) {
        perror("ffmpeg fork");
        throw std::runtime_error("Forking child process failed");
    } else if (child_pid == 0) {
        /* child process */
        close(apipe[0]);
        close(vpipe[0]);
        execvp("ffmpeg", args);
        
        /* only reached if execvp fails */
        perror("execvp");
        exit(-1);
    } else {
        /* parent process */
        close(apipe[1]);
        close(vpipe[1]);

        *audio_pipe = apipe[0];
        *video_pipe = vpipe[0];
        return child_pid;
    }
}

pid_t start_aplay(int *pipefd) {
    pid_t child_pid;
    int apipe[2];
    
    if (pipe(apipe) < 0) {
        perror("aplay pipe");
        throw std::runtime_error("pipe creation failed");
    }

    child_pid = fork( );
    if (child_pid == -1) {
        perror("aplay fork");
        throw std::runtime_error("Forking aplay process failed");
    } else if (child_pid == 0) {
        /* exec aplay with the necessary options, and with the pipe as stdin */
        close(apipe[1]);
        dup2(apipe[0], STDIN_FILENO);
        execlp("aplay", "aplay",
            "--format=S16_LE",
            "--channels=2",
            "--rate=48000",
            NULL /* terminator */
        );
        perror("execlp");
        fprintf(stderr, "Failed to start aplay\n");
        exit(-1);
    } else {
        *pipefd = apipe[1];
        close(apipe[0]);
        return child_pid;
    }
}


class PipeAudioOutput {
    public:
        PipeAudioOutput(int pipefd) {
            fd = pipefd;
            buf_size = 0;
            data_buf = NULL;

        }
        bool ReadyForMoreAudio( ) {
            do_poll( );
            if (buf_avail == 0) {
                return true;
            } else {
                return false;
            }
        }
        void SetNextAudio(short *samples, size_t len) {
            if (buf_size < len) {
                data_buf = (uint8_t *)av_realloc(data_buf, len);
                if (!data_buf) {
                    throw std::runtime_error("allocation failure");
                }

                buf_size = len;
            }

            buf_avail = len;
            buf_ptr = 0;
            memcpy(data_buf, samples, len);
        }
    protected:
        int fd;

        void do_poll( ) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, 0) < 0) {
                perror("poll");
                throw std::runtime_error("poll failed");
            }

            if (pfd.revents & POLLOUT) {
                do_write( );
            }
        }

        void do_write( ) {
            ssize_t n_written;
            if (buf_avail > 0) {
                n_written = write(fd, data_buf + buf_ptr, buf_avail);
                if (n_written < 0) {
                    perror("write");
                    throw std::runtime_error("write failed");
                } else {
                    buf_ptr += n_written;
                    buf_avail -= n_written;
                }
            }
        }

        uint8_t *data_buf;
        size_t buf_size;
        size_t buf_avail;
        size_t buf_ptr;

        bool ready;
};

int main(int argc, char **argv) {
    pid_t ffmpeg_pid, aplay_pid;
    int apipe, vpipe, aout_pipe;

    uint8_t *audio_block;
    uint8_t *frame;

    /* These are the picture size values for NTSC UYVY8. For now, them's the breaks. */
    Picture *uyvy_frame = Picture::alloc(720, 480, 1440, UYVY8);
    OutputAdapter *out;

    signal(SIGCHLD, SIG_IGN);

    out = new DecklinkOutput(0);

    ffmpeg_pid = start_ffmpeg(argc - 1, argv + 1, &apipe, &vpipe);
    aplay_pid = start_aplay(&aout_pipe);

    PipeAudioOutput *aout = new PipeAudioOutput(aout_pipe);

    PipeBuffer v_buffer(vpipe, VIDEO_FRAME_SIZE);
    PipeBuffer a_buffer(apipe, AUDIO_BLOCK_SIZE);
    PipeBuffer *buffer_list[2];

    buffer_list[0] = &v_buffer;
    buffer_list[1] = &a_buffer;

    /* while we've still got stuff to play back... */
    while (!v_buffer.stream_finished( ) || !a_buffer.stream_finished( )) {
        if (out->ReadyForNextFrame( )) {
            frame = v_buffer.get_next_block( );
            if (frame) {
                memcpy(uyvy_frame->data, frame, VIDEO_FRAME_SIZE);

                out->SetNextFrame(uyvy_frame);
                v_buffer.done_with_block(frame);
            }
        }
        if (aout->ReadyForMoreAudio( )) {
            audio_block = a_buffer.get_next_block( );
            if (audio_block) {
                aout->SetNextAudio((short *)audio_block, AUDIO_BLOCK_SIZE);
                a_buffer.done_with_block(audio_block);
            }
        }

        /* receive more data if we can */
        PipeBuffer::update(buffer_list, 2);
    }

    usleep(5000000); // wait until Decklink output finishes?
}
