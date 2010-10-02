# This makefile is inefficient.

CC=g++
SDK_PATH=/home/armena/opt/bmd_sdk/Linux/include
CFLAGS=-Wno-multichar -Wall -Wextra -I $(SDK_PATH) -fno-rtti -g -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -DENABLE_DECKLINK
LDFLAGS=-lm -ldl -lpthread
FFCFLAGS=`pkg-config libavcodec libavformat libswscale libavutil --cflags` 
FFLDFLAGS=`pkg-config libavcodec libavformat libswscale libavutil --libs` -lavutil

all: sdl_gui mjpeg_ingest playoutd decklink_capture

sdl_gui: sdl_gui.cpp mmap_buffer.cpp ffwrapper.cpp
	$(CC) $(CFLAGS) $(FFCFLAGS) `sdl-config --cflags` -o sdl_gui sdl_gui.cpp ffwrapper.cpp mmap_buffer.cpp $(LDFLAGS) $(FFLDFLAGS) `sdl-config --libs` -lSDL_image 

mjpeg_ingest: mjpeg_ingest.cpp mmap_buffer.cpp
	$(CC) -o mjpeg_ingest mjpeg_ingest.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

decklink_capture: decklink_capture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o decklink_capture decklink_capture.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

playoutd: playoutd.cpp ffwrapper.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o playoutd playoutd.cpp ffwrapper.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS) $(LAVC_INCLUDES) $(LAVC_LIBS) $(FFCFLAGS) $(FFLDFLAGS)

field_split: field_split.cpp 
	$(CC) -o field_split field_split.cpp $(CFLAGS) $(LDFLAGS) $(FFCFLAGS) $(FFLDFLAGS)

ffoutput: ffoutput.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o ffoutput ffoutput.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS) $(FFCFLAGS) $(FFLDFLAGS)

libjpeg_test: libjpeg_test.cpp mjpeg_frame.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

time_libjpeg: time_libjpeg.cpp mjpeg_frame.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

clean:
	rm -f raw_ingest ingest output
