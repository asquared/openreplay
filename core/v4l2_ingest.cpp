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
#include "picture.h"
#include "stats.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <asm/types.h>
#include <linux/videodev2.h>

void usage(char *name) {
    fprintf(stderr, "usage: %s [-i input] /dev/videoX buffer", name);
}

struct v4l2_open_device {
    int fd;
    int n_buffers;
    void **buffers; /* size should be essentially determined by pixel format */
    int *buffer_lengths;
};

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

    /* set up mmap()-based I/O */
    req_buf.count = 4;
    req_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req_buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req_buf) == -1) {
        perror("cannot set memory-mapped I/O");
        close(fd);
        return NULL;
    }

    if (req_buf.count < 2) {
        fprintf(stderr, "couldn't allocate at least 2 capture buffers...\n");
        close(fd);
        return NULL;
    }

    ret = (struct v4l2_open_device *) malloc(sizeof(struct v4l2_open_device));
    ret->fd = fd;
    ret->n_buffers = req_buf.count;
    ret->buffers = (void **)malloc(req_buf.count * sizeof(void *));
    ret->buffer_lengths = (int *)malloc(req_buf.count * sizeof(int));

    for (i = 0; i < req_buf.count; ++i) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            perror("could not query buffer information");
            close(fd);
            free(ret->buffers);
            free(ret->buffer_lengths);
            free(ret);
            return NULL;
        }

        ret->buffer_lengths[i] = buffer.length;
        ret->buffers[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, 
            MAP_SHARED, fd, buffer.m.offset);

        if (ret->buffers[i] == MAP_FAILED) {
            perror("mmap v4l2 buffer");
            close(fd);
            free(ret->buffers);
            free(ret->buffer_lengths);
            free(ret);
            return NULL;
        }

    }
    
    /* queue the capture buffers and start the stream */
    for (i = 0; i < ret->n_buffers; i++) {
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
            perror("queue v4l2 buffer");
            close(fd);
            free(ret->buffers);
            free(ret->buffer_lengths);
            free(ret);
            return NULL;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &buffer.type /* laziness */)) {
        perror("start capture stream");
        close(fd);
        free(ret->buffers);
        free(ret->buffer_lengths);
        free(ret);
        return NULL;
    }

    /* done!? */
    return ret;   
}

int main(int argc, char **argv) {
    MJPEGEncoder enc;
    EncodeStats stats(29.97);

    struct v4l2_open_device *dev = open_v4l2(argc, argv);
    struct v4l2_buffer v4lbuf;

    FILE *uyvy_test = fopen("test.uyvy", "w");

    if (!dev) {
        return -1;
    }

    // print out some statistics after every 60 frames we finish
    stats.autoprint(60);

    MmapBuffer buf(argv[argc - 1], MAX_FRAME_SIZE, true);

    /* DV = 720x480, capture card = 720x486 */
    int frame_w = 720, frame_h = 480;

    Picture *input = Picture::alloc(frame_w, frame_h, 2*frame_w, UYVY8);
    
    for (;;) {
        /* wait for a frame */
        memset(&v4lbuf, 0, sizeof(v4lbuf));
        v4lbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4lbuf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(dev->fd, VIDIOC_DQBUF, &v4lbuf) == -1) {
            perror("VIDIOC_DQBUF");
            return -1;
        }

        /* copy uyvy422 data */
        memcpy(input->data, dev->buffers[v4lbuf.index], input->line_pitch * input->h);

        //fwrite(dev->buffers[v4lbuf.index], 2*frame_w*frame_h, 1, stdout);


        // encode and store the data
        mjpeg_frame *frm = enc.encode_full(input, true);
        buf.put(frm, sizeof(struct mjpeg_frame) + frm->f1size);
        stats.output_bytes(frm->f1size);
        stats.finish_frames(1);

        /* pass buffer back to v4l2 driver */
        if (ioctl(dev->fd, VIDIOC_QBUF, &v4lbuf) == -1) {
            perror("VIDIOC_QBUF");
            return -1;
        }

    }

    fclose(uyvy_test);
}
