#include "mmap_buffer.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#define MAGIC 0xdecafbad

MmapBuffer::MmapBuffer(const char *control_file, const char *data_file, unsigned int record_size, bool reset) {
	struct stat statbuf;

	// assume the files are already there...
	control_fd = open(control_file, O_RDWR);
	if (control_fd < 0) {
		throw std::runtime_error("Failed to open the control file");
	}		

	mmapped_ipc = (struct control_data *)
		mmap(0, sizeof(struct control_data), PROT_READ | PROT_WRITE, MAP_SHARED, control_fd, 0);

	if (mmapped_ipc == 0) {
		throw std::runtime_error("Failed to mmap shared state");
	}

	if (mmapped_ipc->magic != MAGIC || reset) {
		// we're the first one here. we must be the source...
		mmapped_ipc->magic = MAGIC;
		mmapped_ipc->record_size = record_size;
		mmapped_ipc->current_timecode = -1;
		mmapped_ipc->current_offset = 0;
	}

	// now open the data file
	data_fd = open(data_file, O_RDWR);
	if (data_fd < 0) {
		throw std::runtime_error("Failed to open data file");
	}

	if (fstat(data_fd, &statbuf) < 0) {
		throw std::runtime_error("fstat on data file failed");
	}
	
	mmapped_ipc->max_offset = statbuf.st_size;
	n_records = mmapped_ipc->max_offset / mmapped_ipc->record_size;


	mmapped_shit = (char *)
		mmap(0, mmapped_ipc->max_offset, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);

	strcpy((char *)mmapped_shit, "Hello World!");

	if (mmapped_shit == 0) {
		throw std::runtime_error("Failed to mmap data buffer");			
	}
}

int MmapBuffer::put(const void *data, int size) {
	mmapped_ipc->current_timecode++;
	mmapped_ipc->current_offset += mmapped_ipc->record_size;
	if (mmapped_ipc->current_offset > mmapped_ipc->max_offset - mmapped_ipc->record_size) {
		mmapped_ipc->current_offset = 0;
	}
	memcpy((void *)(mmapped_shit + mmapped_ipc->current_offset), data, size);
	return mmapped_ipc->current_timecode;
}

bool MmapBuffer::get(void *data, int size, int timecode) {
	if (
		mmapped_ipc->current_timecode < timecode 
		|| mmapped_ipc->current_timecode - n_records > timecode 
		|| mmapped_ipc->current_timecode == -1
		|| timecode < 0
	) {
		return false;
	}

	long long offset = 
		mmapped_ipc->current_offset 
		- ((long long)(mmapped_ipc->current_timecode - timecode) 
	 	    * mmapped_ipc->record_size);

	if (offset < 0) {
		offset += n_records * mmapped_ipc->record_size;
	}	
	memcpy(data, (void *)(mmapped_shit+offset), size);
	return true;
}

int MmapBuffer::get_timecode(void) {
	return mmapped_ipc->current_timecode - 1;
}
