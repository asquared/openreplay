#include "output_adapter.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>

#include <list>

char *ff_video_args[] = {
  /* decode to 720x480 raw UYVY422 format */
  "-f", "rawvideo", 
  "-s", "720x480", /* FIXME magic resolution number */
  "-pix_fmt", "uyvy422", 
  NULL 
};

char *ff_audio_args[] = { 
  /* decode to 16-bit little-endian 48khz signed data */
  "-f", "s16le", 
  "-ar", "48000", /* FIXME magic number */
  NULL 
};

/*
 * start_ffmpeg( )
 * Start a FFmpeg child process which will decode audio or video.
 * This process will send its output to a pipe.
 *
 * argc, argv: initial arguments to ffmpeg. These will be provided from
 * the command line and used to configure the ffmpeg input.
 *
 * preset_args: more arguments to ffmpeg. These will be provided internally
 * and determine the type and format of data to be sent via the pipe.
 *
 * pipe: A pointer to a variable that will receive the pipe descriptor.
 *
 * Return Value:
 * Process ID of the FFmpeg process.
 */
pid_t start_ffmpeg(int argc, char **argv, char **preset_args, int *pipe) {

    char *pipe_arg = 0;
    char **args;
    char **argp;
    pid_t child_pid;

    int i;

    /* pipe[0] = read, pipe[1] = write */
    int opipe[2];

    if (pipe(opipe) < 0) {
        perror("ffmpeg pipe");
        throw std::runtime_error("pipe failed");
    }

    for (i = 0; preset_args[i]; ++i) { /* just count using i */ }

    args = new char *[
        i + argc + 1 /* ffmpeg */ + 1 /* null terminator */
        + 1 /* pipe number */
    ];

    args[0] = "ffmpeg"; /* name of program to run */
    argp = &args[1];

    /* copy input arguments */
    for (i = 0; i < argc; ++i) {
        *argp = argv[i];
        argp++;
    }

    /* copy preset output args */
    for (i = 0; preset_args[i]; ++i) {
        *argp = preset_args[i];
        argp++;
    }

    /* set output pipe */
    asprintf(&pipe_arg, "pipe:%d", opipe[1]);
    *argp = pipe_arg;
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
        close(opipe[0]);
        execvp("ffmpeg", args);
        free(pipe_arg);
        delete [] args; 
        /* only reached if execvp fails */
        perror("execvp");
        exit(-1);
    } else {
        /* parent process */
        close(opipe[1]);

        free(pipe_arg);
        delete [] args;

        *pipe = opipe[0];
        return child_pid;
    }
}

/*
 * start_aplay
 * Start the "aplay" program to play an audio stream.
 *
 * in_pipe_fd: pipe from which aplay should read audio data
 *
 * return value: process ID of the aplay process forked
 */
pid_t start_aplay(int in_pipefd) {
    pid_t child_pid;

    child_pid = fork( );
    if (child_pid == -1) {
        perror("aplay fork");
        throw std::runtime_error("Forking aplay process failed");
    } else if (child_pid == 0) {
        /* exec aplay with the necessary options, and with the pipe as stdin */
        dup2(in_pipefd, STDIN_FILENO);
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
        return child_pid;
    }
}

/*
 * alloc_picture
 * Allocate a picture buffer of the correct size for one video frame.
 *
 * return value: a newly allocated picture of the correct size
 */
Picture *alloc_picture(void) {
    return Picture::alloc(720, 480, 1440, UYVY8); /* FIXME magic numbers */
}

/*
 * read_all
 * Read all requested bytes from the FD. Work around read( )'s idiosyncrasies.
 *
 * fd: file descriptor to read from
 * buf: pointer to location where data should be stored
 * size: number of bytes to read
 *
 * return value: 
 *      total number of bytes on success
 *      0 on end of file
 *      -1 on error
 */
ssize_t read_all(int fd, void *buf, size_t size) {
    size_t n_done;
    ssize_t result;
    uint8_t bufp = (uint8_t *) buf;

    while (n_done < size) {
        result = read(fd, bufp + n_done, size - n_done);
        if (result > 0) {
            n_done += result;
        } else if (result == 0) {
            return 0;   
        } else {
            if (errno != EAGAIN && errno != EINTR) {
                return -1;
            } 
            /* if it *is* one of those two we will try again */
        }
    }   
    return size;
}

/* Main entry point */
int main(int argc, char **argv) {
    pid_t ffmpeg_video_pid, ffmpeg_audio_pid, aplay_pid;
    int apipe, vpipe;

    uint8_t *audio_block;
    uint8_t *frame;

    uint32_t event;
    void *argptr;

    /* These are the picture size values for NTSC UYVY8. For now, them's the breaks. */
    Picture *uyvy_frame = alloc_picture( );
    OutputAdapter *out;

    signal(SIGCHLD, SIG_IGN);


    /* set up audio */
    ffmpeg_audio_pid = start_ffmpeg(argc, argv, ff_audio_args, &apipe); 
    aplay_pid = start_aplay(apipe);

    /* set up video - audio will take care of itself from here */
    ffmpeg_video_pid = start_ffmpeg(argc, argv, ff_video_args, &vpipe);
    out = new DecklinkOutput(&evtq, 0);

    for (;;) {
        event = evtq.wait_event(argptr);
        switch (event) {
            case EVT_OUTPUT_NEED_FRAME:
                if (read_all(vpipe, uyvy_frame->data, 
                        uyvy_frame->line_pitch * uyvy_frame->h) == 0) {
                    goto done;
                } else {
                    out->SetNextFrame(uyvy_frame);
                    Picture::free(uyvy_frame);
                    uyvy_frame = alloc_picture( );
                }            
                break;
        }
    }
    
done:
    delete out;
    Picture::free(uyvy_frame);
}
