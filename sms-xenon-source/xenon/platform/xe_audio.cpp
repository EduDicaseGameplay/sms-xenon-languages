// Audio output on the 360: an XAudio2 source voice (16-bit stereo PCM at the DAC rate, big-endian
// as the CPU writes it) that platform/audio/ai.cpp's FIFO feeds through its SDL callback (port
// patch 07). kBuffers buffers of kFrames circulate: each one XAudio2 has played is filled again
// on its thread and queued behind the others.
#include "../msvc/include/xe_win.h"
#include <xaudio2.h>
#include <stdint.h>
#include <stdio.h>

namespace {

const int kBuffers = 4, kFrames = 512;  // 4 x 16 ms at 32 kHz
void (*g_fill)(void*, uint8_t*, int);
IXAudio2* g_xa;
IXAudio2SourceVoice* g_voice;
int16_t g_pcm[kBuffers][kFrames * 2];

void submit(int i)
{
	g_fill(NULL, (uint8_t*)g_pcm[i], int(sizeof g_pcm[i]));
	XAUDIO2_BUFFER b = {0};
	b.AudioBytes = sizeof g_pcm[i];
	b.pAudioData = (const BYTE*)g_pcm[i];
	b.pContext = (void*)(intptr_t)i;
	g_voice->SubmitSourceBuffer(&b);
}

struct Callback : IXAudio2VoiceCallback {
	void STDMETHODCALLTYPE OnBufferEnd(void* ctx) { submit(int(intptr_t(ctx))); }
	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() {}
	void STDMETHODCALLTYPE OnStreamEnd() {}
	void STDMETHODCALLTYPE OnBufferStart(void*) {}
	void STDMETHODCALLTYPE OnLoopEnd(void*) {}
	void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) {}
} g_callback;

}  // namespace

extern "C" int xe_audio_open(void (*fill)(void*, uint8_t*, int), int rate)
{
	IXAudio2MasteringVoice* master = NULL;
	HRESULT h = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (SUCCEEDED(h)) h = g_xa->CreateMasteringVoice(&master);
	WAVEFORMATEX wf = {0};
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = 2;
	wf.nSamplesPerSec = DWORD(rate);
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = 4;
	wf.nAvgBytesPerSec = DWORD(rate) * 4;
	if (SUCCEEDED(h)) h = g_xa->CreateSourceVoice(&g_voice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_callback);
	if (FAILED(h)) {
		fprintf(stderr, "[audio] XAudio2: %08x\n", unsigned(h));
		return 0;
	}
	g_fill = fill;
	for (int i = 0; i < kBuffers; i++) submit(i);
	g_voice->Start(0);
	fprintf(stderr, "[audio] XAudio2 output: %d Hz, %d buffers of %d frames\n", rate, kBuffers, kFrames);
	return 1;
}
