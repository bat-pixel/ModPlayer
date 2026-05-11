#pragma once
#include "framework.h"
#include <mmsystem.h>
#include <cstdint>
#include <mutex>

class ModMixer;

inline constexpr int kAudioSampleRate = 44100;
inline constexpr int kAudioChannels   = 2;      // stereo
inline constexpr int kAudioBufFrames  = 1024;   // frames per buffer (~23 ms)
inline constexpr int kAudioNumBufs    = 2;      // double-buffering

class AudioOut {
public:
    bool Open(ModMixer* mixer);
    void Close();
    ~AudioOut() { Close(); }

    // Lock this before reinitialising the mixer from the UI thread.
    // FillBuffer holds it while calling Mix(), so the swap is race-free.
    std::mutex& GetMutex() { return fillMtx_; }

private:
    static DWORD WINAPI FillThread(LPVOID param);
    void FillLoop();
    void FillBuffer(int idx);

    std::mutex fillMtx_;
    ModMixer*  mixer_      = nullptr;
    HWAVEOUT   hWave_      = nullptr;
    HANDLE     hEvent_     = nullptr;  // signalled by waveOut when a buffer completes
    HANDLE     hStopEvent_ = nullptr;  // signalled by Close() to shut down the thread
    HANDLE     hThread_    = nullptr;
    bool       open_       = false;

    static constexpr int kBufSamples = kAudioBufFrames * kAudioChannels;
    int16_t  bufData_[kAudioNumBufs][kBufSamples]{};
    WAVEHDR  headers_[kAudioNumBufs]{};
};
