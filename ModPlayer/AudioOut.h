#pragma once
#include "framework.h"
#include "IMixer.h"
#include <mmsystem.h>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include <string>

inline constexpr int kAudioSampleRate = 44100;
inline constexpr int kAudioChannels   = 2;      // stereo
inline constexpr int kAudioBufFrames  = 1024;   // frames per buffer (~23 ms)
inline constexpr int kAudioNumBufs    = 4;      // quad-buffering for stability

class AudioOut {
public:
    bool Open(IMixer* mixer);
    void Close();
    ~AudioOut() { Close(); }

    // Swap the active mixer. Acquires the fill mutex so the swap is race-free.
    void SetMixer(IMixer* mixer);

    // Pause/resume: when paused, FillBuffer outputs silence without advancing the mixer.
    void SetPaused(bool paused);
    bool IsPaused() const { return paused_; }

    // Hold this lock before reinitialising a mixer from the UI thread.
    // FillBuffer holds it while calling Mix(), so the swap is race-free.
    std::mutex& GetMutex() { return fillMtx_; }

    // Debug recording: captures exactly what is sent to waveOut.
    // Call StartRecording before audio events of interest; StopRecording finalises the WAV.
    void StartRecording(const std::string& path);
    void StopRecording();
    bool IsRecording() const { return recFile_ != nullptr; }

private:
    static DWORD WINAPI FillThread(LPVOID param);
    void FillLoop();
    void FillBuffer(int idx);
    void RecordSamples(const int16_t* samples, int count);

    std::mutex fillMtx_;
    IMixer*    mixer_      = nullptr;
    bool       paused_     = false;
    HWAVEOUT   hWave_      = nullptr;
    HANDLE     hEvent_     = nullptr;
    HANDLE     hStopEvent_ = nullptr;
    HANDLE     hThread_    = nullptr;
    bool       open_       = false;

    static constexpr int kBufSamples = kAudioBufFrames * kAudioChannels;
    int16_t  bufData_[kAudioNumBufs][kBufSamples]{};
    WAVEHDR  headers_[kAudioNumBufs]{};

    // Recording state (written from audio thread, controlled from UI thread via fillMtx_)
    FILE*    recFile_      = nullptr;
    uint32_t recBytes_     = 0;   // total PCM bytes written so far
};
