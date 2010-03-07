CC=g++
SDK_PATH=../../include
CFLAGS=-Wno-multichar -I $(SDK_PATH) -fno-rtti -g
LDFLAGS=-lm -ldl -lpthread

LAVC_INCLUDES=-I/usr/include/libavcodec -I/usr/include/libavformat -I/usr/include/libavutil -I/usr/include/libswscale
LAVC_LIBS=-lavutil -lavformat -lavcodec -lz -lavutil -lm -lswscale -lavcodec

all: raw_ingest ingest output preview_stream

sdl_gui: sdl_gui.cpp mmap_buffer.cpp
	$(CC) -o sdl_gui sdl_gui.cpp mmap_buffer.cpp `sdl-config --cflags` `sdl-config --libs` -lSDL_image -lSDL_ttf $(CFLAGS) $(LDFLAGS)

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

decklink_capture: decklink_capture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o decklink_capture decklink_capture.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

ingest: ingest.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o ingest ingest.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

bmdplayoutd: bmdplayoutd.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp 
	$(CC) -o bmdplayoutd bmdplayoutd.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS) $(LAVC_INCLUDES) $(LAVC_LIBS)

bmdplayout: bmdplayout.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o bmdplayout bmdplayout.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

output: output.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o output output.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

clean:
	rm -f raw_ingest ingest output
