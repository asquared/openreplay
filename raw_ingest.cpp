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

	char *data = (char *)malloc(2*UYVY_FRAME_SIZE);
	if (!data) {
		fprintf(stderr, "Failed to allocate memory!\n");
	}

	if (argc < 3) {
		fprintf(stderr, "usage: %s control_file data_file\n", argv[0]);
		return 1;
	}

	buffer = new MmapBuffer(argv[1], argv[2], 1<<20 /* put frames on 1meg markers */); 
	
	for (;;) {
		result = read(STDIN_FILENO, data + bytes_in_buffer, UYVY_FRAME_SIZE);
		if (result < 0) {
			fprintf(stderr, "Error exit\n");
			return 1;
		} else if (result == 0) {
			break;		
		} else {
			bytes_in_buffer += result;
		}
	
		if (bytes_in_buffer >= UYVY_FRAME_SIZE) {
			frameCount = buffer->put(data, UYVY_FRAME_SIZE);

			// split absolute time into hh:mm:ss:ff
			frames = frameCount % FRAMES_PER_SEC;
			sec = frameCount / FRAMES_PER_SEC;
			min = sec / 60;
			sec = sec % 60;
			hr = min / 60;
			min = min % 60;
			fprintf(stderr, "%02d:%02d:%02d:%02d         \r", hr, min, sec, frames);

			// shift old data to front of buffer
			memmove(data, data + UYVY_FRAME_SIZE, bytes_in_buffer - UYVY_FRAME_SIZE);
			bytes_in_buffer -= UYVY_FRAME_SIZE;
		}
	}	
	
	free(data);
	fprintf(stderr, "Exiting \"normally\"");
}

