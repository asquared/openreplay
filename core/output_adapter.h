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

#define SCHEDULE_MORE_AUDIO_THRESHOLD 12000
#define N_CHANNELS 2

#include <stdio.h>
#include <math.h>

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
    public IDeckLinkVideoOutputCallback, 
    public IDeckLinkAudioOutputCallback {
public:
    // Magic framerate divisors for NTSC: timebase = 30000 ticks per second, frame duration = 1001 ticks
    // Magic framerate divisors for PAL 25FPS: timebase = 25 ticks per second, frame duration =  1 tick
    // (I wish I was in Europe. This API would seem so much more elegant over there.)
    DecklinkOutput(int cardIndex = 0) 
        : deckLink(0), deckLinkOutput(0), deckLinkIterator(0),
          displayMode(0), deckLinkConfig(0), frame_duration(1001), time_base(30000), 
          frame_counter(0), sine_offset(0), ready_frame(true), rawaudio(NULL)
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

        rawaudio=fopen("ffoutput_audio.raw", "w");
        if (!rawaudio) {
            throw std::runtime_error("failed to open raw audio output file");
        }


        if (deckLinkOutput->SetScheduledFrameCompletionCallback(this) != S_OK) {
            throw std::runtime_error("Failed to set video frame completion callback!\n");
        }

        // don't want, don't need audio callback
        //if (deckLinkOutput->SetAudioCallback(this) != S_OK) {
        //    throw std::runtime_error("Failed to set audio callback!\n");
        //}

        // enable audio and video output
	if (deckLinkOutput->EnableVideoOutput(bmdModeNTSC, bmdVideoOutputFlagDefault) != S_OK) {
            throw std::runtime_error("Failed to enable video output!\n");
        }

        if (deckLinkOutput->EnableAudioOutput(
            bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 
            N_CHANNELS, bmdAudioOutputStreamContinuous) != S_OK) {

            throw std::runtime_error("Failed to open audio output!\n");

        }

        //deckLinkOutput->BeginAudioPreroll( );

        short one_frame_zero_audio[3200] = {0};

        // Preroll a few black frames(??)
        for (int i = 0; i < 10; ++i) {
            schedule_next_frame( );
            SetNextAudio(one_frame_zero_audio, 6400);
        }

        if (deckLinkOutput->StartScheduledPlayback(0, 100, 1.0) != S_OK) {
            throw std::runtime_error("Failed to start scheduled playback!\n");
        }

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
            memcpy(data_ptr + 1440*i, in_frame->data[0] + in_frame->linesize[0]*i, 1440);
        }
    }

    void SetNextAudio(short *sample_buf, size_t len) {
        uint32_t actual_frames;

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

        /* schedule it and hope for the best */
        deckLinkOutput->ScheduleAudioSamples(sample_buf, len / sizeof(short) / N_CHANNELS,
            0, 0, &actual_frames);

        if (actual_frames < (len / sizeof(short) / N_CHANNELS)) {
            fprintf(stderr, "sent too large an audio block");
        }
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
        uint32_t number_buffered;

        if (deckLinkOutput->GetBufferedAudioSampleFrameCount(&number_buffered) != S_OK) {
            fprintf(stderr, "don't know how many samples we got\n");
            return false; 
        }

        fprintf(stderr, "audio left: %u\n", number_buffered);

        if (number_buffered < SCHEDULE_MORE_AUDIO_THRESHOLD) {
            return true;
        } else {
            return false;
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
        ready_frame = true;

        return S_OK;
    }

    virtual HRESULT ScheduledPlaybackHasStopped(void) {
        fprintf(stderr, "WARNING: Scheduled playback stopped unexpectedly!\n");
        return S_OK;
    }

    virtual HRESULT RenderAudioSamples(bool preroll) {
#if 0
        uint32_t number_buffered = 0;
        uint32_t number_consumed = 0;
        // 1/50 sec worth of zeros @ 48khz stereo = 960 samples. Leave some extra...
        short zeros[64000];

        fprintf(stderr, "RenderAudioSamples(%d)\n", preroll);

        /* Generate 1000Hz sine */
        sine_offset = GenerateSine(zeros, 64000/2, 2, sine_offset, 1000.0f);

        if (deckLinkOutput->GetBufferedAudioSampleFrameCount(&number_buffered) != S_OK) {
            fprintf(stderr, "don't know how many samples we got\n");
            return S_OK; /* return something else? */
        }

        if (number_buffered < SCHEDULE_MORE_AUDIO_THRESHOLD) {
            // consume what will be consumed...
            //if (audio_ptr >= audio_actual_len) {
                // zero out buffer and print warning
                fwrite(zeros, 1, sizeof(zeros), rawaudio);
                if (deckLinkOutput->ScheduleAudioSamples(
                    (void *) zeros, sizeof(zeros) / sizeof(short) / N_CHANNELS,
                    0, 0, &number_consumed) != S_OK) {

                    fprintf(stderr, "warning: scheduling of (null) audio samples failed\n");
                }
            
            #if 0
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
            #endif
        } else {
            fprintf(stderr, "Not yet below water mark - not scheduling more audio\n");
        }

        if (preroll) {
//            if (deckLinkOutput->StartScheduledPlayback(0, 100, 1.0) != S_OK) {
//                throw std::runtime_error("Failed to start scheduled playback!\n");
//            }
        }
#endif
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
    IDeckLinkMutableVideoFrame *frame;
    BMDTimeValue frame_start, time_now;

    BMDTimeValue frame_duration;
    BMDTimeScale time_base;
    int frame_counter;
    int sine_offset;


    HRESULT result;
    const char *string;
    bool ready_frame;

    short *audio_buf;
    size_t audio_len, audio_actual_len, audio_ptr;

    FILE *rawaudio;

    void schedule_next_frame(void) {
        deckLinkOutput->ScheduleVideoFrame(frame, frame_counter * frame_duration, frame_duration, time_base);
        frame_counter++;
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