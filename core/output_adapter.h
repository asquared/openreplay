#ifndef _OUTPUT_ADAPTER_H
#define _OUTPUT_ADAPTER_H

#define OUT_FRAME_W 720
#define OUT_FRAME_H 480

#include "picture.h"

#include <stdlib.h>
#include <stdexcept>
#include <string.h>

#include "mutex.h"
#include "event_handler.h"
#include "condition.h"
#include "thread.h"

#define EVT_OUTPUT_NEED_FRAME 0x80000001

/* 
 * For now: input format to these things:
 * pixel data as Picture object pointer
 * signed 16-bit 48khz stereo audio
 */

class OutputAdapter {
public:
    virtual void SetNextFrame(Picture *in_frame) = 0;
    virtual ~OutputAdapter( ) { }
};


#if defined(ENABLE_DECKLINK)

#include "DeckLinkAPI.h"

#define SCHEDULE_MORE_AUDIO_THRESHOLD 12000
#define N_CHANNELS 2

#include <stdio.h>
#include <math.h>

#define DECKLINK_N_FIFO 8

/* Generate a 48khz sampled sine wave at frequency f */
int GenerateSine(int16_t *samples, int len, int n_ch, int offset, float f) {
    int i, j;
    for (i = 0; i < len / n_ch; i++) {
        for (j = i * n_ch; j < i * (n_ch + 1); j++) {
            samples[j] = 16000.0 * sinf((offset + i) * f / 48000.0f);
        }
    }

    return offset + i;
}

class DecklinkOutput : public OutputAdapter, 
    public IDeckLinkVideoOutputCallback {
public:
    // Magic framerate divisors for NTSC: timebase = 30000 ticks per second, frame duration = 1001 ticks
    // Magic framerate divisors for PAL 25FPS: timebase = 25 ticks per second, frame duration =  1 tick
    // (I wish I was in Europe. This API would seem so much more elegant over there.)
    DecklinkOutput(EventHandler *new_evtq, int cardIndex = 0) 
        : deckLink(0), deckLinkOutput(0), deckLinkIterator(0),
          displayMode(0), deckLinkConfig(0), frame_duration(1001), time_base(30000), 
          frame_counter(0), sine_offset(0), evtq(new_evtq)
    {
        deckLinkIterator = CreateDeckLinkIteratorInstance( ); 
	/* Connect to DeckLink card */
	if (!deckLinkIterator) {
            throw std::runtime_error("This application requires the DeckLink drivers installed.");
	}

	while (cardIndex >= 0) {
            /* Connect to the first DeckLink instance */
            result = deckLinkIterator->Next(&deckLink);
            if (result != S_OK) {
                throw std::runtime_error("Invalid card index.");
            }
            cardIndex--;
	}

	deckLink->GetModelName(&string);
	fprintf(stderr, "Attached to card: %s\n", string);
	free((void *)string);
    
	if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&deckLinkOutput) != S_OK) {
            throw std::runtime_error("Failed to get IDeckLinkOutput interface"); 
        }

	// Create frame object
        for (int i = 0; i < DECKLINK_N_FIFO; i++) {
            IDeckLinkMutableVideoFrame *frame;
            if (
                deckLinkOutput->CreateVideoFrame(
                    720, 480, 1440, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame
                ) != S_OK
            ) {
                    throw std::runtime_error("Failed to create frame");
            }
            ready_frames.push_back(frame);
        }

        if (deckLinkOutput->SetScheduledFrameCompletionCallback(this) != S_OK) {
            throw std::runtime_error("Failed to set video frame completion callback!\n");
        }


        // enable video output
	if (deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault) != S_OK) {
            throw std::runtime_error("Failed to enable video output!\n");
        }


        // Preroll a few black frames(??)
        for (int i = 0; i < DECKLINK_N_FIFO; ++i) {
            schedule_next_frame( );
        }

        if (deckLinkOutput->StartScheduledPlayback(0, 100, 1.0) != S_OK) {
            throw std::runtime_error("Failed to start scheduled playback!\n");
        }

    }


    void SetNextFrame(Picture *in_frame) {
        MutexLock lock(_mut);
        // INTERESTING QUESTION: Does modifying a frame that has been 
        // scheduled have an effect on the output? The API documentation
        // is bad enough, that I don't really know.
        unsigned char *data_ptr;
        int i;

        Picture *convert;

        if (in_frame->pix_fmt != UYVY8) {
            convert = in_frame->convert_to_format(UYVY8);
        } else {
            convert = in_frame;
        }

        if (free_frames.empty( )) {
            throw std::runtime_error("Can't set next frame when no frames free!");
        }

        IDeckLinkMutableVideoFrame *frame = free_frames.front( );
        free_frames.pop_front( );
        frame->GetBytes((void **) &data_ptr);

        int blit_max_w = (in_frame->w < 720) ? in_frame->w : 720;
        int blit_max_h = (in_frame->h < 480) ? in_frame->h : 480;

        for (i = 0; i < blit_max_h; ++i) {
            memcpy(data_ptr + 1440*i, convert->data + convert->line_pitch*i, 2*blit_max_w);
        }

        ready_frames.push_back(frame);

        if (convert != in_frame) {
            Picture::free(convert);
        }

    }

    bool ReadyForNextFrame(void) {
        MutexLock lock(_mut);
        return !free_frames.empty( );
    }


    ~DecklinkOutput(void) {
        if (deckLinkOutput != NULL) {
            deckLinkOutput->Release();
            deckLinkOutput = NULL;
        }

        if (deckLink != NULL) {
            deckLink->Release();
            deckLink = NULL;
        }


        if (deckLinkConfig != NULL) {
            deckLinkConfig->Release( );
            deckLinkConfig = NULL;
        }

        if (deckLinkIterator != NULL) {
            deckLinkIterator->Release();
        }
            
    }

    /* DeckLink delegate functions */
    virtual HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame *completed_frame, BMDOutputFrameCompletionResult result) {
        IDeckLinkMutableVideoFrame *frame
            = (IDeckLinkMutableVideoFrame *) completed_frame;
        MutexLock lock(_mut);

        free_frames.push_back(frame);
        evtq->post_event(EVT_OUTPUT_NEED_FRAME, NULL);
        switch (result) {
            case bmdOutputFrameDisplayedLate:
                fprintf(stderr, "WARNING: Decklink displayed frame late (running too slow!)\r\n");
                break;
            case bmdOutputFrameDropped:
                fprintf(stderr, "WARNING: Decklink dropped frame\r\n");
                break;
            case bmdOutputFrameFlushed:
                fprintf(stderr, "WARNING: Decklink flushed frame\r\n");
                break;
            default:
                break;
        }

        schedule_next_frame( );

        return S_OK;
    }

    virtual HRESULT ScheduledPlaybackHasStopped(void) {
        fprintf(stderr, "WARNING: Scheduled playback stopped unexpectedly!\n");
        return S_OK;
    }


    /* stubs */
    HRESULT QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
    ULONG AddRef() { return 1; }
    ULONG Release() { return 1; }

protected:
    // lots of variables...
    IDeckLink *deckLink;
    IDeckLinkOutput *deckLinkOutput;
    IDeckLinkIterator *deckLinkIterator;
    IDeckLinkDisplayMode *displayMode;
    IDeckLinkConfiguration *deckLinkConfig;
    IDeckLinkMutableVideoFrame *last_good_frame;
    BMDTimeValue frame_start, time_now;

    BMDTimeValue frame_duration;
    BMDTimeScale time_base;
    int frame_counter;
    int sine_offset;


    HRESULT result;
    const char *string;

    std::list<IDeckLinkMutableVideoFrame *> free_frames;
    std::list<IDeckLinkMutableVideoFrame *> ready_frames;

    EventHandler *evtq;
    Mutex _mut;

    void schedule_next_frame(void) {
        if (ready_frames.empty( )) {
            // do nothing - no frames available
            fprintf(stderr, "Decklink: dropped frame");
        } else {
            IDeckLinkMutableVideoFrame *frame = ready_frames.front( );
            ready_frames.pop_front( );
            deckLinkOutput->ScheduleVideoFrame(frame, frame_counter * frame_duration, frame_duration, time_base);
            frame_counter++;
        }
    }

};
#endif

class StdoutOutput : public OutputAdapter, public Thread {
public:
    StdoutOutput(EventHandler *evtq_) : evtq(evtq_) {
        data = (uint8_t *)malloc(2*720*480);
        if (!data) {
            throw std::runtime_error("allocation failure");
        }

        start( );
    }

    ~StdoutOutput( ) {
        free(data);
    }

    void *GetFrameBuffer(void) {
        return data;
    }

    void SetNextFrame(Picture *in_frame) {
        int i;

        Picture *convert;
        if (in_frame->pix_fmt == UYVY8) {
            convert = in_frame;
        } else {
            convert = in_frame->convert_to_format(UYVY8);
        }

        int blit_max_w = (in_frame->w < 720) ? in_frame->w : 720;
        int blit_max_h = (in_frame->h < 480) ? in_frame->h : 480;

        { MutexLock lock(mut);
            for (i = 0; i < blit_max_h; ++i) {
                memcpy(data + 1440*i, convert->scanline(i), 2*blit_max_w);
            }
            data_ready = true;
            data_ready_cond.signal( );
        }

        if (convert != in_frame) {
            Picture::free(convert);
        }
    }


protected:
    void run(void) {
        for (;;) {
            evtq->post_event(EVT_OUTPUT_NEED_FRAME, NULL);
            { MutexLock lock(mut);
                if (!data_ready) {
                    data_ready_cond.wait(mut);
                }
        
                write(STDOUT_FILENO, data, 2*720*480);    
                data_ready = false;
            }
            usleep(33667); /* crude NTSC approximation */
        }
    }

    uint8_t *data;
    Condition data_ready_cond;
    Mutex mut;
    bool data_ready;
    EventHandler *evtq;
};

#endif
