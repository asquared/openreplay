#ifndef _OUTPUT_ADAPTER_H
#define _OUTPUT_ADAPTER_H

#define OUT_FRAME_W 720
#define OUT_FRAME_H 480

#include "ffwrapper.h" /* for some convenient FFmpeg structures */

/* 
 * For now: input format to these things:
 * 720x480 packed UYVY422
 * signed 16-bit 48khz stereo audio
 */

class OutputAdapter {
public:
    virtual void *GetFrameBuffer(void) = 0;
    virtual void SetNextFrame(AVPicture *in_frame) = 0;
    virtual void SetNextAudio(short *samples, size_t len) = 0;
    virtual void Flip(void) = 0;
    virtual bool ReadyForNextFrame(void) = 0;
    virtual bool ReadyForMoreAudio(void) = 0;
    virtual ~OutputAdapter( ) { }
};


#if defined(ENABLE_DECKLINK)

#include "DeckLinkAPI.h"

#define SCHEDULE_MORE_AUDIO_THRESHOLD 2*(48000/50)
#define N_CHANNELS 2

class DecklinkOutput : public OutputAdapter {
public:
    // Magic framerate divisors for NTSC: timebase = 30000 ticks per second, frame duration = 1001 ticks
    // Magic framerate divisors for PAL 25FPS: timebase = 25 ticks per second, frame duration =  1 tick
    // (I wish I was in Europe. This API would seem so much more elegant over there.)
    DecklinkOutput(int cardIndex = 0) 
        : deckLink(0), deckLinkOutput(0), deckLinkIterator(0),
          displayMode(0), deckLinkConfig(0), frame_duration(1001), time_base(30000), 
          frame_counter(0), ready_frame(true), callback_obj(this), aud_callback_obj(this)
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
	if (
            deckLinkOutput->CreateVideoFrame(
                720, 480, 1440, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame
            ) != S_OK
	) {
		throw std::runtime_error("Failed to create frame");
	}

        audio_buf = NULL;
        audio_len = 0;
        audio_actual_len = 0;


        // enable audio output
        if (deckLinkOutput->SetAudioCallback(&aud_callback_obj) != S_OK) {
            throw std::runtime_error("Failed to set audio callback!\n");
        }

        if (deckLinkOutput->EnableAudioOutput(
            bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 
            N_CHANNELS, bmdAudioOutputStreamContinuous) != S_OK) {

            throw std::runtime_error("Failed to open audio output!\n");

        }

	// enable video output
        if (deckLinkOutput->SetScheduledFrameCompletionCallback(&callback_obj) != S_OK) {
            throw std::runtime_error("Failed to set video frame completion callback!\n");
        }

	if (deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault) != S_OK) {
            throw std::runtime_error("Failed to enable video output!\n");
        }


        // Preroll a few black frames(??)
        schedule_next_frame( );
        deckLinkOutput->StartScheduledPlayback(0, time_base, 1.0);
    }


    void *GetFrameBuffer(void) {
        void *data;
        frame->GetBytes(&data);
        return data;
    }

    void SetNextFrame(AVPicture *in_frame) {
        // INTERESTING QUESTION: Does modifying a frame that has been 
        // scheduled have an effect on the output? The API documentation
        // is bad enough, that I don't really know.
        unsigned char *data_ptr;
        int i;
        frame->GetBytes((void **) &data_ptr);

        for (i = 0; i < 480; ++i) {
            memcpy(frame + 1440*i, in_frame->data[0] + in_frame->linesize[0]*i, 1440);
        }
    }

    void SetNextAudio(short *sample_buf, size_t len) {
        if (audio_len < len) {
            audio_buf = (short *)av_realloc(audio_buf, len);
            if (!audio_buf) {
                audio_len = 0;
                audio_actual_len = 0;
                throw std::runtime_error("Failed to allocate more audio buffer");
            }
            audio_len = len;
        }

        memcpy(audio_buf, sample_buf, len);
        audio_actual_len = len;
        audio_ptr = 0;
    }

    void Flip(void) {
        // just let the callback do the work??
        //schedule_next_frame( );
        ready_frame = false;
    }

    bool ReadyForNextFrame(void) {
        return ready_frame;
    }

    bool ReadyForMoreAudio(void) {
        return (audio_actual_len >= audio_ptr);
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


protected:
    // lots of variables...
    IDeckLink *deckLink;
    IDeckLinkOutput *deckLinkOutput;
    IDeckLinkIterator *deckLinkIterator;
    IDeckLinkDisplayMode *displayMode;
    IDeckLinkConfiguration *deckLinkConfig;
    IDeckLinkMutableVideoFrame *frame;
    BMDTimeValue frame_start, time_now;

    BMDTimeValue frame_duration;
    BMDTimeScale time_base;
    int frame_counter;


    HRESULT result;
    const char *string;
    bool ready_frame;

    short *audio_buf;
    size_t audio_len, audio_actual_len, audio_ptr;

    void schedule_next_frame(void) {
        deckLinkOutput->ScheduleVideoFrame(frame, frame_counter * frame_duration, frame_duration, time_base);
        frame_counter++;
    }

    class MyVideoCallback : public IDeckLinkVideoOutputCallback {
        public:
            MyVideoCallback(DecklinkOutput *_owner) : owner(_owner) { }
            HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame *completed_frame, BMDOutputFrameCompletionResult result) {
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
                owner->ScheduledFrameCompleted( );
                return S_OK;
            }
            HRESULT ScheduledPlaybackHasStopped(void) {
                owner->ScheduledPlaybackHasStopped( );
                return S_OK;
            }

            HRESULT QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
            ULONG AddRef() { return 1; }
            ULONG Release() { return 1; }
        protected:
            DecklinkOutput *owner;
    } callback_obj;

    class MyAudioCallback : public IDeckLinkAudioOutputCallback {
        public:
            MyAudioCallback(DecklinkOutput *_owner) : owner(_owner) { }
            HRESULT RenderAudioSamples(bool preroll) { 
                return owner->RenderAudioSamples(preroll);
            }
            HRESULT QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
            ULONG AddRef() { return 1; }
            ULONG Release() { return 1; }
        protected:
            DecklinkOutput *owner;
    } aud_callback_obj;

    void ScheduledFrameCompleted(void) {
        schedule_next_frame( );
        ready_frame = true;
    }

    void ScheduledPlaybackHasStopped(void) {
        fprintf(stderr, "WARNING: Scheduled playback stopped unexpectedly!\n");
    }

    HRESULT RenderAudioSamples(bool preroll) {
        uint32_t number_buffered = 0;
        uint32_t number_consumed = 0;
        // 1/50 sec worth of zeros @ 48khz stereo = 960 samples. Leave some extra...
        short zeros[3840];
        if (deckLinkOutput->GetBufferedAudioSampleFrameCount(&number_buffered) != S_OK) {
            // error handling
        }

        if (number_buffered < SCHEDULE_MORE_AUDIO_THRESHOLD) {
            // consume what will be consumed...
            if (audio_ptr >= audio_actual_len) {
                // zero out buffer and print warning
                fprintf(stderr, "Ran out of audio! Scheduling some zeros instead...");
                memset(zeros, 0, sizeof(zeros));
                if (deckLinkOutput->ScheduleAudioSamples(
                    (void *) zeros, sizeof(zeros) / sizeof(short) / N_CHANNELS,
                    0, 0, &number_consumed) != S_OK) {

                    fprintf(stderr, "warning: scheduling of (null) audio samples failed\n");
                }
                
            } else {
                // Schedule what we've got.
                if (deckLinkOutput->ScheduleAudioSamples(
                    (void *) (audio_buf + audio_ptr), 
                    (audio_actual_len - audio_ptr) / N_CHANNELS,
                    0, 0, &number_consumed) != S_OK) {
                
                    fprintf(stderr, "warning: scheduling of audio samples failed\n");

                } else {
                    audio_ptr += (number_consumed * N_CHANNELS);
                }
            }
        }

        return S_OK;
    }


};
#endif

class StdoutOutput : public OutputAdapter {
public:
    StdoutOutput( ) {
        data = (uint8_t *)malloc(2*720*480);
        if (!data) {
            throw std::runtime_error("allocation failure");
        }
    }

    ~StdoutOutput( ) {
        free(data);
    }

    void *GetFrameBuffer(void) {
        return data;
    }

    void SetNextFrame(AVPicture *in_frame) {
        int i;

        for (i = 0; i < 480; ++i) {
            memcpy(data + 1440*i, in_frame->data[0] + in_frame->linesize[0]*i, 1440);
        }
    }

    void SetNextAudio(short *samples, size_t len) {
        fprintf(stderr, "StdoutOutput::SetNextAudio( ): stub\n");
    }

    void Flip(void) {
        write(STDOUT_FILENO, data, 2*720*480);    
    }

    bool ReadyForNextFrame(void) {
        return true;
    }

    bool ReadyForMoreAudio(void) {
        return true;
    }

protected:
    uint8_t *data;

};

#endif
