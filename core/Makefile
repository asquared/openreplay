# This makefile is inefficient.

-include local.mk

CC=g++
SDK_PATH ?= /home/armena/opt/bmd_sdk/Linux/include
CFLAGS += -Wno-multichar -Wall -Wextra -I $(SDK_PATH) -fno-rtti -g -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -DENABLE_DECKLINK `pkg-config --cflags freetype2`

LDFLAGS=-lm -ldl -lpthread `pkg-config --libs freetype2`

CFLAGS += -DHAVE_PANGOCAIRO
CFLAGS += `pkg-config --cflags cairo`
CFLAGS += `pkg-config --cflags pango`
CFLAGS += `pkg-config --cflags pangocairo`

LDFLAGS += `pkg-config --libs cairo`
LDFLAGS += `pkg-config --libs pango`
LDFLAGS += `pkg-config --libs pangocairo`

all: sdl_gui mjpeg_ingest playoutd decklink_capture field_split ffoutput \
		libjpeg_test time_libjpeg v4l2_ingest \
		decklink_ingest

sdl_gui: sdl_gui.cpp mmap_buffer.cpp mjpeg_frame.cpp picture.cpp
	$(CC) $(CFLAGS) `sdl-config --cflags` -o $@ $^ $(LDFLAGS) `sdl-config --libs` -lSDL_image -ljpeg

mjpeg_ingest: mjpeg_ingest.cpp mmap_buffer.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

uyvy_ingest: uyvy_ingest.cpp mmap_buffer.cpp picture.cpp \
		mjpeg_frame.cpp stats.cpp mmap_state.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

v4l2_ingest: v4l2_ingest.cpp mmap_buffer.cpp picture.cpp \
		mjpeg_frame.cpp stats.cpp mmap_state.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

decklink_capture: decklink_capture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

decklink_ingest: decklink_ingest.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp \
		mmap_buffer.cpp picture.cpp mjpeg_frame.cpp \
		stats.cpp mmap_state.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

playoutd: playoutd.cpp mjpeg_frame.cpp picture.cpp mmap_buffer.cpp \
		$(SDK_PATH)/DeckLinkAPIDispatch.cpp thread.cpp \
		mutex.cpp condition.cpp event_handler.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

clockd: clockd.cpp mmap_state.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

field_split: field_split.cpp 
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

ffoutput: ffoutput.cpp picture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp \
		thread.cpp mutex.cpp condition.cpp event_handler.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

libjpeg_test: libjpeg_test.cpp mjpeg_frame.cpp picture.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

time_libjpeg: time_libjpeg.cpp mjpeg_frame.cpp picture.cpp
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -ljpeg

clean:
	rm -f sdl_gui mjpeg_ingest playoutd decklink_capture \
	field_split ffoutput libjpeg_test time_libjpeg v4l2_ingest \
	decklink_ingest
