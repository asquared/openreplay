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
	int frameCount;

	if (argc < 3) {
		fprintf(stderr, "usage: %s control_file data_file ...\n", argv[0]);
		return 1;
	}

	for (int j = 1; j < argc - 1; j += 2) {
		buffer = new MmapBuffer(argv[j], argv[j+1], 1<<20 /* put frames on 1meg markers */); 
		frameCount = buffer->get_timecode( );
		printf("%d ", frameCount);
	}
	fprintf(stderr, "\n");
		
	
}

