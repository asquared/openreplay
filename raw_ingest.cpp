#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "mmap_buffer.h"

MmapBuffer *buffer;

#define UYVY_FRAME_SIZE (2*720*480)
#define FRAMES_PER_SEC 30

int main(int argc, char *argv[])
{
	int frameCount, frames, sec, min, hr;

	void *data = malloc(2*UYVY_FRAME_SIZE);
	if (!data) {
		fprintf(stderr, "Failed to allocate memory!\n");
	}

	buffer = new MmapBuffer("control1", "data1", 1<<20 /* put frames on 1meg markers */, true); 
	
	while (read(STDIN_FILENO, data, UYVY_FRAME_SIZE) == UYVY_FRAME_SIZE) {
		frameCount = buffer->put(data, UYVY_FRAME_SIZE);

		// split absolute time into hh:mm:ss:ff
		frames = frameCount % FRAMES_PER_SEC;
		sec = frameCount / FRAMES_PER_SEC;
		min = sec / 60;
		sec = sec % 60;
		hr = min / 60;
		min = min % 60;
		fprintf(stderr, "%02d:%02d:%02d:%02d         \r", hr, min, sec, frames);
	}	
	
	free(data);
	fprintf(stderr, "Exiting \"normally\"");
}

