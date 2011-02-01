#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "DeckLinkAPI.h"
#include "Capture.h"

#include "mmap_buffer.h"
#include "mmap_state.h"
#include "mjpeg_config.h"
#include "mjpeg_frame.h"
#include "stats.h"

MmapBuffer *buffer;
MmapState *clock_ipc;
MJPEGEncoder enc;
EncodeStats stats(29.97);

#define FRAMES_PER_SEC 30

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
    void *frameBytes;
    void *audioFrameBytes;
    
    BMDTimeValue time, duration;
    int frameCount, frames, sec, min, hr;
    int size;
    void *data;
    Picture *p;

    // Handle Video Frame
    if(videoFrame)
    {
        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
        {
            fprintf(stderr, "!!! NO SIGNAL !!!       \n");
        }
        else
        {
            size = videoFrame->GetRowBytes( ) * videoFrame->GetHeight( );
            videoFrame->GetBytes(&data);

            // make a 720x480 frame
            p = Picture::alloc(720, 480, 1440, UYVY8);
            memcpy(p->data, ((uint8_t *) data) + 6*1440, 480*1440); 

            // encode frame
            mjpeg_frame *frm = enc.encode_full(p, true);
            Picture::free(p);

            clock_ipc->get(&frm->clock, sizeof(frm->clock));
            frm->odd_dominant = true;
            frm->interlaced = false;
            buffer->put(frm, sizeof(struct mjpeg_frame) + frm->f1size);
            
            stats.output_bytes(frm->f1size);
            stats.finish_frames(1);
        }

    }

    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

int main(int argc, char *argv[])
{
    IDeckLink *deckLink;
    IDeckLinkInput *deckLinkInput;
    IDeckLinkDisplayModeIterator *displayModeIterator;
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    DeckLinkCaptureDelegate *delegate = 0;
    IDeckLinkDisplayMode *displayMode = 0;
    IDeckLinkConfiguration *deckLinkConfig = 0;
    BMDDisplayMode selectedDisplayMode = bmdModeNTSC;
    BMDVideoConnection input_source;
    int displayModeCount = 0;
    int exitStatus = 1;
    int ch;
    int cardIndex = 2;
    HRESULT result;
    const char *string;

    if (argc < 3) {
        fprintf(stderr, "usage: %s card_index buffer\n", argv[0]);
        return 1;
    }

    cardIndex = atoi(argv[1]);

    buffer = new MmapBuffer(argv[2], MAX_FRAME_SIZE, true);
    clock_ipc = new MmapState("clock_ipc");

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
    fprintf(stderr, "Found a card: %s\n", string);
    free((void *)string);
    
    if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput) != S_OK)
        goto bail;

    if (deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&deckLinkConfig) != S_OK)
        goto bail;

    /* set composite in */
    if (deckLinkConfig->SetInt(bmdDeckLinkConfigVideoInputConnection, 
            bmdVideoConnectionComposite) != S_OK) {
        fprintf(stderr, "failed to set composite input\n");
        goto bail;
    }

    /* set 7.5 IRE setup level*/
    if (deckLinkConfig->SetInt(bmdDeckLinkConfigAnalogVideoInputFlags,
            bmdAnalogVideoFlagCompositeSetup75) != S_OK) {
        fprintf(stderr, "failed to set analog input flags\n");
        goto bail;
    }

    delegate = new DeckLinkCaptureDelegate();
    deckLinkInput->SetCallback(delegate);
    fprintf(stderr, "Callback set\n");
   

    result = deckLinkInput->EnableVideoInput(selectedDisplayMode, bmdFormat8BitYUV, 0);
    if(result != S_OK)
    {
        fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
        goto bail;
    }


    result = deckLinkInput->StartStreams();
    if(result != S_OK)
    {
        goto bail;
    }
    

    fprintf(stderr, "Attempting to start capture...\n");
    // All Okay.
    exitStatus = 0;
    
    // Block main thread until signal occurs
    while (1) { sleep(1); }
    fprintf(stderr, "Stopping Capture\n");

bail:
    
    if (deckLinkInput != NULL)
    {
        deckLinkInput->Release();
        deckLinkInput = NULL;
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

