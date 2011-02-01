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
    virtual bool ReadyForNextFrame( ) = 0;
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
          displayMode(0), frame_duration(1001), 
          time_base(30000), frame_counter(0),
          evtq(new_evtq), current_frame(0),
          current_frame_is_stale(true)
    {
        HRESULT result;
        const char *string;

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


        if (deckLinkOutput->SetScheduledFrameCompletionCallback(this) != S_OK) {
            throw std::runtime_error("Failed to set video frame completion callback!\n");
        }


        // enable video output
	if (deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault) != S_OK) {
            throw std::runtime_error("Failed to enable video output!\n");
        }


	// Create frame objects and preroll them
        for (int i = 0; i < DECKLINK_N_FIFO; i++) {
            IDeckLinkMutableVideoFrame *frame;
            if (
                deckLinkOutput->CreateVideoFrame(
                    720, 480, 1440, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame
                ) != S_OK
            ) {
                    throw std::runtime_error("Failed to create frame");
            }

            schedule_next_frame(frame);
        }

        // start the scheduled playback        
        if (deckLinkOutput->StartScheduledPlayback(0, 100, 1.0) != S_OK) {
            throw std::runtime_error("Failed to start scheduled playback!\n");
        }

        // ask client for its first frame
        if (evtq) {
            evtq->post_event(EVT_OUTPUT_NEED_FRAME, NULL);
        }
    }

    bool ReadyForNextFrame( ) {
        return current_frame_is_stale; 
    }

    void SetNextFrame(Picture *in_frame) {
        Picture *new_frame, *old_frame;

        /* 
         * Optimize me! This is a bad place to do this conversion. 
         * A memcpy will be saved if we do it later 
         * (directly into the Decklink buffer)
         */
        if (in_frame->pix_fmt == UYVY8) {
            in_frame->addref( );
            new_frame = in_frame;
        } else {
            new_frame = in_frame->convert_to_format(UYVY8);
        }

        /*
         * swap current_frame and new_frame atomically.
         * This minimizes the time a lock is held for.
         */
        { MutexLock lock(_mut);             
            old_frame = current_frame;
            current_frame = new_frame;
            current_frame_is_stale = false;
        }

        if (old_frame != NULL) {
            Picture::free(old_frame);            
        }
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

        if (deckLinkIterator != NULL) {
            deckLinkIterator->Release();
        }
            
    }

    /* DeckLink delegate functions */
    virtual HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame *completed_frame, BMDOutputFrameCompletionResult result) {
        IDeckLinkMutableVideoFrame *frame
            = (IDeckLinkMutableVideoFrame *) completed_frame;

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

        schedule_next_frame(frame);

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
    BMDTimeScale time_base;
    BMDTimeValue frame_duration;

    Picture *current_frame;
    bool current_frame_is_stale;

    int frame_counter;

    EventHandler *evtq;
    Mutex _mut;

    void schedule_next_frame(IDeckLinkMutableVideoFrame *frame) {
        void *void_data;
        uint8_t *frame_data;
        Picture *in_frame; 
        bool was_stale;
        
        // make sure we get a consistent version of the state.
        // No member access outside this block! (except DeckLink API calls)
        { MutexLock lock(_mut);
            was_stale = current_frame_is_stale; 
            in_frame = current_frame;
            if (in_frame != NULL) {
                in_frame->addref( );
                current_frame_is_stale = true;
                if (!was_stale) {
                    // we have just made the frame stale so let's ask for a new one
                    if (evtq) {
                        evtq->post_event(EVT_OUTPUT_NEED_FRAME, NULL);
                    }
                }
            }
        }

        if (was_stale) {
            // gratuitous?
            fprintf(stderr, "Decklink warning: using a stale frame\n");
        }

        frame->GetBytes(&void_data);
        frame_data = (uint8_t *) void_data;

        if (in_frame != NULL) {
            int in_scanline_size = in_frame->line_pitch;
            int out_scanline_size = 1440; /* size of a 720 pixel UYVY scanline */
            int copy_size;

            if (in_scanline_size < out_scanline_size) {
                copy_size = in_scanline_size;
            } else {
                copy_size = out_scanline_size;
            }

            for (int j = 0; j < 480; j++) {
                memcpy(frame_data, in_frame->scanline(j), copy_size);
                frame_data += out_scanline_size;
            }
            
            Picture::free(in_frame);
        } else {
            // black 
            for (int i = 0; i < 720*480; i++, frame_data += 2) {
                frame_data[0] = 128; // u, v
                frame_data[1] = 16; // y
            }
        }

        deckLinkOutput->ScheduleVideoFrame(
            frame, frame_counter * frame_duration, 
            frame_duration, time_base
        );
        frame_counter++;
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

    bool ReadyForNextFrame( ) {
        bool ready;
        { MutexLock lock(mut);
            ready = !data_ready;
        }

        return ready;
    }


protected:
    void run(void) {
        for (;;) {
            if (evtq) {
                evtq->post_event(EVT_OUTPUT_NEED_FRAME, NULL);
            }
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
