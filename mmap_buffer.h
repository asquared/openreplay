#ifndef _MMAP_BUFFER_H
#define _MMAP_BUFFER_H

class MmapBuffer {
	public:
	MmapBuffer(const char *control_file, const char *data_file, unsigned int record_size, bool reset = false);
	// keyboard suckage, no destructor for now...
	int put(const void *data, int size);
	bool get(void *data, int size, int timecode);
	
	private:
	volatile char *mmapped_shit;
	
	// mmap this thing into the control file so that we have shared data among all processes
	volatile struct control_data {
		int current_timecode;	
		int current_offset;
		int magic;
		unsigned long long max_offset;
		int record_size;
	} *mmapped_ipc;

	int control_fd, data_fd;
	int n_records;
};

#endif
