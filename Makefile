CC=g++
SDK_PATH=../../include
CFLAGS=-Wno-multichar -I $(SDK_PATH) -fno-rtti -g
LDFLAGS=-lm -ldl -lpthread

all: raw_ingest ingest output preview_stream

all_mark: all_mark.cpp mmap_buffer.cpp
	$(CC) -o all_mark all_mark.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

extract_frame: extract_frame.cpp mmap_buffer.cpp
	$(CC) -o extract_frame extract_frame.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

timecode: timecode.cpp mmap_buffer.cpp
	$(CC) -o timecode timecode.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

preview_stream: preview_stream.cpp mmap_buffer.cpp
	$(CC) -o preview_stream preview_stream.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

mjpeg_ingest: mjpeg_ingest.cpp mmap_buffer.cpp
	$(CC) -o mjpeg_ingest mjpeg_ingest.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

raw_ingest: raw_ingest.cpp mmap_buffer.cpp
	$(CC) -o raw_ingest raw_ingest.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

ingest: ingest.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o ingest ingest.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

output: output.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o output output.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

clean:
	rm -f raw_ingest ingest output
