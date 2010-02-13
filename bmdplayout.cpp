/*
 * Output shit from the ring buffer on the TV!!
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

#include "DeckLinkAPI.h"
#include "Capture.h"
#include "mmap_buffer.h"

MmapBuffer *buffer;

#define FRAMES_PER_SEC 30

int get_frame_from_stdin(void *dest, int size) {
	unsigned char *ptr = (unsigned char *)dest;
	int n_read, remaining = size;

	while (remaining > 0) {
		n_read = read(STDIN_FILENO, ptr, remaining);
		if (n_read < 0) {
			perror("get_frame_from_stdin");
			return -1;
		} else if (n_read == 0) {
			fprintf(stderr, "End of input");
			return -2;
		} else {	
			remaining -= n_read;
			ptr += n_read;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	IDeckLink *deckLink;
	IDeckLinkOutput *deckLinkOutput;
	IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
	DeckLinkCaptureDelegate	*delegate = 0;
	IDeckLinkDisplayMode *displayMode = 0;
	IDeckLinkConfiguration *deckLinkConfig = 0;
	BMDDisplayMode selectedDisplayMode = bmdModeNTSC;
        BMDVideoConnection input_source;
	IDeckLinkMutableVideoFrame *frame;
	void *data;

	int displayModeCount = 0;
	int exitStatus = 1;
	int ch;
	int cardIndex = 1;
	int timecode;
	int frame_duration = 66; // this should play things back at roughly 1/2 speed...
	HRESULT	result;
	const char *string;

	/* Parse command line arguments */
	if (argc < 2) {
		fprintf(stderr, "usage: %s card [speed]", argv[0]);
		return 1;
	}

	cardIndex = atoi(argv[1]);
	
	if (argc > 2) {
		frame_duration = (1000 / FRAMES_PER_SEC) / atof(argv[2]);
	}
	
	/* Connect to DeckLink card */
	if (!deckLinkIterator)
	{
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
		goto bail;
	}

	while (cardIndex >= 0) {
		/* Connect to the first DeckLink instance */
		result = deckLinkIterator->Next(&deckLink);
		if (result != S_OK)
		{
			fprintf(stderr, "Invalid card index.\n");
			goto bail;
		}
		cardIndex--;
	}

	deckLink->GetModelName(&string);
	fprintf(stderr, "Attached to card: %s\n", string);
	free((void *)string);
    
	if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&deckLinkOutput) != S_OK)
		goto bail;

	if (deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&deckLinkConfig) != S_OK)
		goto bail;


	// configure output
	deckLinkConfig->SetVideoOutputFormat(bmdVideoConnectionComposite);

	// Create frame object
	if (
		deckLinkOutput->CreateVideoFrame(
			720, 480, 1440, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame
		) != S_OK
	) {
		throw std::runtime_error("Failed to create frame");
	}

	// enable video output
	deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault);
	
	// keep replaying until we run out of buffer or we get Ctrl-C'd...

	// this is slightly ugly. it's 5am. i can be forgiven.
	BMDTimeValue frame_start_time, time_now, time_in_frame, ticks_per_frame;
	deckLinkOutput->GetHardwareReferenceClock(1000, &time_now, &time_in_frame, &ticks_per_frame);

	// read first frame from stdin
	frame->GetBytes(&data);
	if (get_frame_from_stdin(data, 1440*480) < 0) {
		return 1;
	}

	for (;;) {
		if (deckLinkOutput->DisplayVideoFrameSync(frame) != S_OK) {
			throw std::runtime_error("Failed to display frame");
		}
		frame_start_time = time_now;
		// start getting the next frame - hopefully this doesn't corrupt things
		frame->GetBytes(&data);
		if (get_frame_from_stdin(data, 1440*480) < 0) {
			fprintf(stderr, "Input error caught, exiting");
			return 1;
		}
		do {
			deckLinkOutput->GetHardwareReferenceClock(1000, &time_now, &time_in_frame, &ticks_per_frame);
			usleep(1000);
		} while (time_now < frame_start_time + frame_duration);
	}
bail:
   	
    if (deckLinkOutput != NULL)
    {
        deckLinkOutput->Release();
        deckLinkOutput = NULL;
    }

    if (deckLink != NULL)
    {
        deckLink->Release();
        deckLink = NULL;
    }


   if (deckLinkConfig != NULL) {
	deckLinkConfig->Release( );
	deckLinkConfig = NULL;
   }

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();

    return exitStatus;
}

