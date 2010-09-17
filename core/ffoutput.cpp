#include "output_adapter.h"
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

class PipeBuffer {
    public:
        PipeBuffer(int fd_, size_t block_size_) {
            fd = fd_;
            block_size = block_size_;
            current_read_buf = NULL;
            read_so_far = 0;
            eof = false;
        }


        /* poll all the pipes and update buffers when possible */
        static void update(PipeBuffer **list, int n) {
            /* legal to define inline? */
            struct pollfd *pfds = new struct pollfd[n];
            int i;

            /* poll for pipes with data ready */
            for (i = 0; i < n; ++i) {
                list[i]->fill_pollfd(&pfds[i]);
                pfds[i].events = POLLIN | POLLNVAL | POLLHUP;
            }
            if (poll(pfds, n, POLL_TIMEOUT) < 0) {
                delete [] pfds;
                throw std::runtime_error("poll failed");
            }

            /* for any pipe which is ready to go, read more data */
            for (i = 0; i < n; ++i) {
                if ((pfds[i].revents & POLLIN) && list[i]->want_poll( )) {
                    list[i]->read_more( );
                }

                if (pfds[i].revents & POLLNVAL) {
                    fprintf(stderr, "buffer %d (fd %d) no longer valid\n", i, pfds[i].fd); 
                    list[i]->set_error( );
                }

                if (pfds[i].revents & POLLHUP) {
                    fprintf(stderr, "buffer %d: other end hung up\n", i);
                    list[i]->set_eof( );
                }
            }

            delete [] pfds;

        }

        uint8_t *get_next_block( ) {
            uint8_t *ret;
            if (ready_blocks.empty( )) {
                return NULL;
            } else {
                ret = ready_blocks.front( );
                ready_blocks.pop_front( );
                return ret;
            }
        }

        void done_with_block(uint8_t *block_ptr) {
            av_free(block_ptr);
        }

        bool at_eof( ) {
            return eof;
        }

    protected:
        int fd;
        size_t block_size;
        uint8_t *current_read_buf;
        size_t read_so_far;
        bool eof;
        std::list<uint8_t *> ready_blocks;

        void fill_pollfd(struct pollfd *pfd) {
            pfd->fd = fd;
        }

        bool want_poll( ) {
            return true;
        }

        void set_eof( ) {
            eof = true;
        }

        void set_error( ) {
            /* More sophistication someday. */
            eof = true;
        }

        void read_more( ) {
            ssize_t result;

            /* don't try reading from a broken pipe */
            if (eof) {
                return;
            }
            
            // allocate new block if needed
            if (current_read_buf == NULL) {
                current_read_buf = (uint8_t *)av_malloc(block_size);
                if (current_read_buf == NULL) {
                    throw std::runtime_error("Failed to allocate new block");
                }
            }
        
            // read what we can from the pipe
            result = read(fd, current_read_buf + read_so_far, block_size - read_so_far);

            if (result == -1) {
                perror("read from pipe");
                throw std::runtime_error("failed to read from pipe");
            } else if (result == 0) {
                /* EOF - throw away any partial block for now */
                av_free(current_read_buf);
                current_read_buf = NULL;
                read_so_far = 0;
                eof = true;

                fprintf(stderr, "EOF\n");
            } else {
                read_so_far += result;
                
                /* we never ask for more than we can take so this is true */
                assert(read_so_far <= block_size);

                if (read_so_far == block_size) {
                    /* finished a block so put it on the queue */
                    ready_blocks.push_back(current_read_buf);
                    current_read_buf = NULL;
                    read_so_far = 0;
                }
            }


        }


        static const int POLL_TIMEOUT = 1; /* milliseconds */
};

int main(int argc, char **argv) {
    pid_t ffmpeg_pid;
    int apipe, vpipe;

    uint8_t *audio_block;
    uint8_t *frame;

    AVPicture pict;
    OutputAdapter *out;

    signal(SIGCHLD, SIG_IGN);

    pict.linesize[0] = VIDEO_LINE_SIZE;

    out = new StdoutOutput( );

    ffmpeg_pid = start_ffmpeg(argc - 1, argv + 1, &apipe, &vpipe);

    PipeBuffer v_buffer(vpipe, VIDEO_FRAME_SIZE);
    PipeBuffer a_buffer(apipe, AUDIO_BLOCK_SIZE);
    PipeBuffer *buffer_list[2];

    buffer_list[0] = &v_buffer;
    buffer_list[1] = &a_buffer;

    /* while we've still got stuff to play back... */
    while (!v_buffer.at_eof( ) && !a_buffer.at_eof( )) {
        if (out->ReadyForNextFrame( )) {
            frame = v_buffer.get_next_block( );
            if (frame) {
                fprintf(stderr, "video\n");
                pict.data[0] = frame;
                out->SetNextFrame(&pict);
                v_buffer.done_with_block(frame);

                out->Flip( ); // this is rapidly becoming meaningless
            }
        }
        if (out->ReadyForMoreAudio( )) {
            audio_block = a_buffer.get_next_block( );
            if (audio_block) {
                fprintf(stderr, "audio\n");
                out->SetNextAudio((short *)audio_block, AUDIO_BLOCK_SIZE);
                a_buffer.done_with_block(audio_block);
            }
        }

        /* receive more data if we can */
        PipeBuffer::update(buffer_list, 2);
    }
}
