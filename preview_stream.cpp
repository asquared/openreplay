#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>

#include "mmap_buffer.h"

MmapBuffer *buffer;

#define UYVY_FRAME_SIZE (2*720*480)
#define FRAMES_PER_SEC 30

int main(int argc, char *argv[])
{
	int frameCount, frames, sec, min, hr;
	size_t bytes_in_buffer = 0;
	ssize_t result;

	char *data = (char *)malloc(UYVY_FRAME_SIZE);
	if (!data) {
		fprintf(stderr, "Failed to allocate memory!\n");
	}

	if (argc < 3) {
		fprintf(stderr, "usage: %s control_file data_file\n", argv[0]);
		return 1;
	}

	buffer = new MmapBuffer(argv[1], argv[2], 1<<20 /* put frames on 1meg markers */); 
	
	for (;;) {
		buffer->get(data, UYVY_FRAME_SIZE, buffer->get_timecode( ));
		write(STDOUT_FILENO, data, UYVY_FRAME_SIZE);
		usleep(100000);
	}	
	
	free(data);
	fprintf(stderr, "Exiting \"normally\"");
}

