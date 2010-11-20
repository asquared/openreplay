# This makefile is inefficient.

CC=g++
SDK_PATH=/home/armena/opt/bmd_sdk/Linux/include
CFLAGS=-Wno-multichar -Wall -Wextra -I $(SDK_PATH) -fno-rtti -g -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -DENABLE_DECKLINK -O3
LDFLAGS=-lm -ldl -lpthread

all: sdl_gui mjpeg_ingest playoutd decklink_capture field_split ffoutput libjpeg_test time_libjpeg v4l2_ingest

sdl_gui: sdl_gui.cpp mmap_buffer.cpp mjpeg_frame.cpp picture.cpp
	$(CC) $(CFLAGS) `sdl-config --cflags` -o $@ $^ $(LDFLAGS) `sdl-config --libs` -lSDL_image -ljpeg

mjpeg_ingest: mjpeg_ingest.cpp mmap_buffer.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

uyvy_ingest: uyvy_ingest.cpp mmap_buffer.cpp picture.cpp mjpeg_frame.cpp stats.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

v4l2_ingest: v4l2_ingest.cpp mmap_buffer.cpp picture.cpp mjpeg_frame.cpp stats.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

decklink_capture: decklink_capture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

playoutd: playoutd.cpp mjpeg_frame.cpp picture.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

field_split: field_split.cpp 
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

ffoutput: ffoutput.cpp picture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

libjpeg_test: libjpeg_test.cpp mjpeg_frame.cpp picture.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

time_libjpeg: time_libjpeg.cpp mjpeg_frame.cpp picture.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

clean:
	rm -f sdl_gui mjpeg_ingest decklink_capture playoutd field_split ffoutput libjpeg_test time_libjpeg
