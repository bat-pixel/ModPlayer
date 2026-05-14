#include "framework.h"
#include "AudioOut.h"
#include <algorithm>

// ── FillBuffer ────────────────────────────────────────────────────────────────
// Asks the mixer for one buffer's worth of float audio, converts to int16.

void AudioOut::FillBuffer(int idx)
{
    float floatBuf[kAudioBufFrames * kAudioChannels];
    {
        std::lock_guard<std::mutex> lk(fillMtx_);
        mixer_->Mix(floatBuf, kAudioBufFrames);
    }

    int16_t* dst = bufData_[idx];
    for (int i = 0; i < kAudioBufFrames * kAudioChannels; ++i) {
        float s = floatBuf[i];
        if (s >  1.f) s =  1.f;
        if (s < -1.f) s = -1.f;
        dst[i] = static_cast<int16_t>(s * 32767.f);
    }
}

// ── FillLoop ──────────────────────────────────────────────────────────────────
// Runs on a dedicated thread. Pre-fills both buffers, then waits for waveOut
// to signal that a buffer completed, refills it, and re-queues it.

void AudioOut::FillLoop()
{
    for (int i = 0; i < kAudioNumBufs; ++i) {
        FillBuffer(i);
        waveOutPrepareHeader(hWave_, &headers_[i], sizeof(WAVEHDR));
        waveOutWrite(hWave_, &headers_[i], sizeof(WAVEHDR));
    }

    HANDLE events[2] = { hEvent_, hStopEvent_ };

    while (true) {
        DWORD r = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0) break;  // stop event or error

        for (int i = 0; i < kAudioNumBufs; ++i) {
            if (headers_[i].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(hWave_, &headers_[i], sizeof(WAVEHDR));
                FillBuffer(i);
                waveOutPrepareHeader(hWave_, &headers_[i], sizeof(WAVEHDR));
                waveOutWrite(hWave_, &headers_[i], sizeof(WAVEHDR));
            }
        }
    }
}

DWORD WINAPI AudioOut::FillThread(LPVOID param)
{
    static_cast<AudioOut*>(param)->FillLoop();
    return 0;
}

// ── Open / Close ──────────────────────────────────────────────────────────────

void AudioOut::SetMixer(IMixer* mixer)
{
    std::lock_guard<std::mutex> lk(fillMtx_);
    mixer_ = mixer;
}

bool AudioOut::Open(IMixer* mixer)
{
    mixer_ = mixer;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = kAudioChannels;
    fmt.nSamplesPerSec  = kAudioSampleRate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    hEvent_     = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hStopEvent_ = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    if (!hEvent_ || !hStopEvent_) return false;

    if (waveOutOpen(&hWave_, WAVE_MAPPER, &fmt,
            reinterpret_cast<DWORD_PTR>(hEvent_), 0,
            CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return false;

    for (int i = 0; i < kAudioNumBufs; ++i) {
        headers_[i]               = {};
        headers_[i].lpData        = reinterpret_cast<LPSTR>(bufData_[i]);
        headers_[i].dwBufferLength = kAudioBufFrames * kAudioChannels * sizeof(int16_t);
    }

    hThread_ = CreateThread(nullptr, 0, FillThread, this, 0, nullptr);
    if (!hThread_) return false;

    open_ = true;
    return true;
}

void AudioOut::Close()
{
    if (!open_) return;
    open_ = false;

    SetEvent(hStopEvent_);
    WaitForSingleObject(hThread_, 5000);

    waveOutReset(hWave_);
    for (int i = 0; i < kAudioNumBufs; ++i) {
        if (headers_[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(hWave_, &headers_[i], sizeof(WAVEHDR));
    }
    waveOutClose(hWave_);

    CloseHandle(hThread_);
    CloseHandle(hEvent_);
    CloseHandle(hStopEvent_);

    hThread_    = nullptr;
    hWave_      = nullptr;
    hEvent_     = nullptr;
    hStopEvent_ = nullptr;
}
