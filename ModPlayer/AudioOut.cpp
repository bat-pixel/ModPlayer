#include "framework.h"
#include "AudioOut.h"
#include <algorithm>
#include <cstring>

// ── FillBuffer ────────────────────────────────────────────────────────────────
// Asks the mixer for one buffer's worth of float audio, converts to int16.

void AudioOut::SetPaused(bool paused)
{
    std::lock_guard<std::mutex> lk(fillMtx_);
    paused_ = paused;
}

void AudioOut::FillBuffer(int idx)
{
    float floatBuf[kAudioBufFrames * kAudioChannels];
    {
        std::lock_guard<std::mutex> lk(fillMtx_);
        if (paused_)
            std::memset(floatBuf, 0, sizeof(floatBuf));
        else
            mixer_->Mix(floatBuf, kAudioBufFrames);

        int16_t* dst = bufData_[idx];
        for (int i = 0; i < kAudioBufFrames * kAudioChannels; ++i) {
            float s = floatBuf[i];
            if (s >  1.f) s =  1.f;
            if (s < -1.f) s = -1.f;
            dst[i] = static_cast<int16_t>(s * 32767.f);
        }
        RecordSamples(dst, kAudioBufFrames * kAudioChannels);
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

// ── Recording ─────────────────────────────────────────────────────────────────

void AudioOut::StartRecording(const std::string& path)
{
    std::lock_guard<std::mutex> lk(fillMtx_);
    if (recFile_) { fclose(recFile_); recFile_ = nullptr; }

    fopen_s(&recFile_, path.c_str(), "wb");
    if (!recFile_) return;
    recBytes_ = 0;

    // Write a placeholder WAV header (44 bytes); finalized in StopRecording.
    uint8_t hdr[44]{};
    fwrite(hdr, 1, sizeof(hdr), recFile_);
}

void AudioOut::StopRecording()
{
    std::lock_guard<std::mutex> lk(fillMtx_);
    if (!recFile_) return;

    // Seek back and write the real RIFF/WAVE/fmt/data header.
    fseek(recFile_, 0, SEEK_SET);

    const uint32_t dataBytes = recBytes_;
    const uint32_t riffSize  = 36 + dataBytes;
    const uint32_t rate      = kAudioSampleRate;
    const uint16_t channels  = kAudioChannels;
    const uint16_t bits      = 16;
    const uint32_t byteRate  = rate * channels * bits / 8;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);

    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, recFile_); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, recFile_); };

    fwrite("RIFF", 1, 4, recFile_); w32(riffSize);
    fwrite("WAVE", 1, 4, recFile_);
    fwrite("fmt ", 1, 4, recFile_); w32(16);
    w16(1); w16(channels); w32(rate); w32(byteRate); w16(blockAlign); w16(bits);
    fwrite("data", 1, 4, recFile_); w32(dataBytes);

    fclose(recFile_);
    recFile_  = nullptr;
    recBytes_ = 0;
}

void AudioOut::RecordSamples(const int16_t* samples, int count)
{
    if (!recFile_) return;
    const size_t bytes = static_cast<size_t>(count) * sizeof(int16_t);
    fwrite(samples, 1, bytes, recFile_);
    recBytes_ += static_cast<uint32_t>(bytes);
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
