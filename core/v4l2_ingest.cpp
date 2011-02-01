/* 
 * v4l2_ingest.cpp
 * This file is part of openreplay.
 * Copyright (C) 2010 Andrew H. Armenia.
 *
 * You should have received the terms and conditions for copying 
 * along with this file.
 *
 * Significant portions derived from code at:
 * http://v4l2spec.bytesex.org/spec/capture-example.html
 */
#include "mjpeg_config.h"
#include "mjpeg_frame.h"
#include "mmap_buffer.h"
#include "mmap_state.h"
#include "picture.h"
#include "stats.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <asm/types.h>
#include <linux/videodev2.h>

/* 
 * Given two even-dominant frames, construct an odd-dominant frame.
 * We start with a video stream that looks like:
 * [ E O | E O ].
 * We want to get to
 * E [ O E ] O.
 * So we should copy even scanlines from the "second" (in time) frame
 * over even scanlines in the "first" in time frame.
 * The "first" frame is our result.
 *
 * Note that this would transform
 * [ O E | O' E' ]
 * into
 * [ O E' ]
 * which is not a valid frame by any stretch of imagination.
 */
void make_odd_dominant(Picture *first, Picture *second)
{
    int j;
    
    assert(first->line_pitch == second->line_pitch);
    assert(first->h == second->h);

    for (j = 0; j < first->h; j += 2) {
        memcpy(first->scanline(j), second->scanline(j), first->line_pitch);
    }
}

void usage(char *name) {
    fprintf(stderr, "usage: %s [-i input] /dev/videoX buffer", name);
}

struct v4l2_open_device {
    int fd;
    int n_buffers;
    Picture **buffers;
};

void free_open_device(v4l2_open_device *dev) {
    for (int i = 0; i < dev->n_buffers; i++) {
        if (dev->buffers[i]) {
            free(dev->buffers[i]);
        }
    }

    free(dev);
}

v4l2_open_device *open_v4l2(int argc, char * const *argv) {
    int input = 0;
    int opt;
    int fd;

    v4l2_open_device *ret = NULL;

    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req_buf;
    struct v4l2_buffer buffer;

    int i;
    int n_buffers = 16;

    const struct option options[] = {
        {
            name: "input",
            has_arg: 1,
            flag: NULL,
            val: 'i'
        },
    };

    
    while ((opt = getopt_long(argc, argv, "i:", options, NULL)) != EOF) {
        switch (opt) {
            case 'i':
                /* what to do if optarg is non-numeric? */
                input = atoi(optarg);
                break;
            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if (optind != argc - 2) {
        usage(argv[0]);
        return NULL;
    }

    fd = open(argv[optind], O_RDWR);

    if (fd == -1) {
        perror("open v4l2 device");
        return NULL;
    }

    /* figure out if this thing supports memory-mapped capture */
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return NULL; 
    }

    fprintf(stderr, "%s: %s (driver %s)\n", argv[optind], cap.card, cap.driver);

    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        fprintf(stderr, "capture not supported - bailing!\n");
        close(fd);
        return NULL;
    }

    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        fprintf(stderr, "memory-mapped streaming I/O not available. Bailing...\n");
        close(fd);
        return NULL;
    }

    /* try to set the input the user requested */
    if (ioctl(fd, VIDIOC_S_INPUT, &input) == -1) {
        perror("select video input");
        close(fd);
        return NULL;
    }    

    /* set up NTSC (hard code for now) */
    v4l2_std_id standard = V4L2_STD_NTSC_M;
    if (ioctl(fd, VIDIOC_S_STD, &standard) == -1) {
        perror("set video standard");
        close(fd);
        return NULL;
    }

    /* fiddle with VIDIOC_CROPCAP? */

    /* attempt to set video format */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 720;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("set up capture format");
        close(fd);
        return NULL;
    }

    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == -1) {
        perror("query capture format");
        close(fd);
        return NULL;
    }

    fprintf(stderr, "w=%d h=%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
    fprintf(stderr, "field=%d\n", fmt.fmt.pix.field);

    /* set up streaming I/O */
    memset(&req_buf, 0, sizeof(req_buf));
    req_buf.count = 16;
    req_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req_buf.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(fd, VIDIOC_REQBUFS, &req_buf) == -1) {
        perror("cannot set memory-mapped I/O");
        close(fd);
        return NULL;
    }

    ret = (struct v4l2_open_device *) malloc(sizeof(struct v4l2_open_device));
    ret->fd = fd;
    ret->n_buffers = n_buffers;
    ret->buffers = (Picture **)malloc(n_buffers * sizeof(Picture *));
    memset(ret->buffers, 0, n_buffers * sizeof(Picture *));

    for (i = 0; i < req_buf.count; ++i) {
        // FIXME magic numbers
        ret->buffers[i] = Picture::alloc(720, 480, 1440, UYVY8);

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_USERPTR;
        buffer.index = i;
        buffer.m.userptr = (unsigned long) ret->buffers[i]->data;
        buffer.length = 2*720*480; // FIXME hack

        if (ioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
            perror("queue v4l2 buffer");
            close(fd);
            free_open_device(ret);
            return NULL;
        }

    }
    
    if (ioctl(fd, VIDIOC_STREAMON, &buffer.type /* laziness */)) {
        perror("start capture stream");
        close(fd);
        free_open_device(ret);
        return NULL;
    }

    /* done!? */
    return ret;   
}

int main(int argc, char **argv) {
    MJPEGEncoder enc;
    EncodeStats stats(29.97);

    struct v4l2_open_device *dev = open_v4l2(argc, argv);
    struct v4l2_buffer v4lbuf, v4l_lastbuf;

    int current_buf;

    FILE *uyvy_test = fopen("test.uyvy", "w");

    if (!dev) {
        return -1;
    }

    // print out some statistics after every 60 frames we finish
    stats.autoprint(60);

    MmapBuffer buf(argv[argc - 1], MAX_FRAME_SIZE, true);
    MmapState clock_ipc("clock_ipc");

    /* DV = 720x480, capture card = 720x486 */
    int frame_w = 720, frame_h = 480;

    Picture *p_current;
    Picture *p_last = NULL;
    
    for (;;) {
        /* wait for a frame */
        memset(&v4lbuf, 0, sizeof(v4lbuf));
        v4lbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4lbuf.memory = V4L2_MEMORY_USERPTR;

        if (ioctl(dev->fd, VIDIOC_DQBUF, &v4lbuf) == -1) {
            perror("VIDIOC_DQBUF");
            return -1;
        }

        for (current_buf = 0; 
            current_buf < dev->n_buffers; 
            ++current_buf
        ) {            
            if (
                v4lbuf.m.userptr 
                == (unsigned long)dev->buffers[current_buf]->data
            ) {
                break;
            }
        }

        assert(current_buf < dev->n_buffers);
        p_current = dev->buffers[current_buf];

        if (p_last != NULL) {
            make_odd_dominant(p_last, p_current);
            // encode and store the data
            mjpeg_frame *frm = enc.encode_full(p_last, true);

            // (get scoreboard clock info)
            clock_ipc.get(&frm->clock, sizeof(frm->clock));

            frm->odd_dominant = true;
            frm->interlaced = false;

            buf.put(frm, sizeof(struct mjpeg_frame) + frm->f1size);
            stats.output_bytes(frm->f1size);
            stats.finish_frames(1);

            /* pass buffer back to v4l2 driver */
            if (ioctl(dev->fd, VIDIOC_QBUF, &v4l_lastbuf) == -1) {
                perror("VIDIOC_QBUF");
                return -1;
            }
        }

        p_last = p_current;
        memcpy(&v4l_lastbuf, &v4lbuf, sizeof(v4lbuf));
    }

    fclose(uyvy_test);
}
