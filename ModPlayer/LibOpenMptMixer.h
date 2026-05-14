#pragma once
#include "IMixer.h"
#include <libopenmpt/libopenmpt.hpp>
#include <memory>
#include <string>

// Playback backend backed by libopenmpt.
// Supports all formats libopenmpt handles (MOD, XM, S3M, IT, …).
class LibOpenMptMixer : public IMixer {
public:
    // Load a module from a file path. Returns false if the format is not supported.
    // Safe to call while NOT on the audio thread (caller must hold the audio mutex).
    bool Load(const std::string& path, int sampleRate = 44100);
    void Unload();

    void        Mix(float* stereo, int numFrames) override;
    bool        IsPlaying()   const override { return playing_; }
    const char* BackendName() const override { return "libopenmpt"; }
    std::string SongTitle()   const override;

private:
    void UpdateVis(const float* stereo, int numFrames);

    std::unique_ptr<openmpt::module> mod_;
    int  rate_    = 44100;
    bool playing_ = false;
};
