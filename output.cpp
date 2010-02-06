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

bool send_next_frame(IDeckLinkOutput *output, int timecode) {
	IDeckLinkMutableVideoFrame *frame;
	void *data;

	// Create frame object
	if (
		output->CreateVideoFrame(
			720, 480, 1440, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame
		) != S_OK
	) {
		throw std::runtime_error("Failed to create frame");
	}

	// copy data
	frame->GetBytes(&data);
	if (!buffer->get(data, 1440*480, timecode)) {
		frame->Release( );
		return false;
	} 

	if (output->DisplayVideoFrameSync(frame) != S_OK) {
		throw std::runtime_error("Failed to display frame");
	}
	return true;	
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
	int displayModeCount = 0;
	int exitStatus = 1;
	int ch;
	int cardIndex = 1;
	int timecode;
	int frame_duration = 66; // this should play things back at roughly 1/2 speed...
	HRESULT	result;
	const char *string;


	if (argc < 6) {
		fprintf(stderr, "usage: %s card control_file data_file timecode speed", argv[0]);
		return 1;
	}

	cardIndex = atoi(argv[1]);

	buffer = new MmapBuffer(argv[2], argv[3], 1<<20 /* put frames on 1meg markers */); 

	int hours = 0, minutes = 0, seconds = 0, frames = 0;
	sscanf(argv[4], "%d:%d:%d:%d", &hours, &minutes, &seconds, &frames);
	timecode = (((hours * 60 + minutes) * 60 + seconds) * FRAMES_PER_SEC + frames);

	frame_duration = (1000 / FRAMES_PER_SEC) / atof(argv[5]);
	
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

	// enable video output
	deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault);
	
	// keep replaying until we run out of buffer or we get Ctrl-C'd...

	// this is slightly ugly. it's 5am. i can be forgiven.
	BMDTimeValue frame_start_time, time_now, time_in_frame, ticks_per_frame;
	deckLinkOutput->GetHardwareReferenceClock(1000, &time_now, &time_in_frame, &ticks_per_frame);

	while (send_next_frame(deckLinkOutput, timecode++)) {
		frame_start_time = time_now;
		do {
			deckLinkOutput->GetHardwareReferenceClock(1000, &time_now, &time_in_frame, &ticks_per_frame);
			usleep(1000);
		} while (time_now < frame_start_time + frame_duration);
		fprintf(stderr, "outputting\n");
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

