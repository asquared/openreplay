#ifndef __CAPTURE_H__
#define __CAPTURE_H__

#include "DeckLinkAPI.h"

class DeckLinkCaptureDelegate : public IDeckLinkInputCallback
{
public:
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
	virtual ULONG STDMETHODCALLTYPE AddRef(void) { return 1; }
	virtual ULONG STDMETHODCALLTYPE  Release(void) { return 1; }
	virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
	virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);
};

#endif
