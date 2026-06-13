#pragma once
#include "IMixer.h"
#include <libopenmpt/libopenmpt.hpp>
#include <memory>
#include <string>
#include <vector>

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
    int         CurrentOrder()       const override { return mod_ ? static_cast<int>(mod_->get_current_order())    : 0; }
    int         CurrentRow()         const override { return mod_ ? static_cast<int>(mod_->get_current_row())      : 0; }
    int         SongOrders()         const override { return mod_ ? static_cast<int>(mod_->get_num_orders())       : 0; }
    double      GetDurationSeconds() const override { return mod_ ? mod_->get_duration_seconds()                   : 0.0; }
    double      GetPositionSeconds() const override { return mod_ ? mod_->get_position_seconds()                   : 0.0; }
    void        SeekSeconds(double t)      override { if (mod_) mod_->set_position_seconds(t); }

private:
    void UpdateVis(const float* stereo, int numFrames);

    std::unique_ptr<openmpt::module> mod_;
    int  rate_    = 44100;
    bool playing_ = false;

    // Per-panel synthetic-scope state. libopenmpt doesn't expose per-channel PCM,
    // so each panel's oscilloscope is synthesised from its dominant channel's VU
    // level and latched note pitch — keeping the waveform in sync with the label.
    // Modules with >4 channels are folded onto the 4 panels (contiguous groups).
    float visPhase_[kVisChannels] = {};   // running oscillator phase
    float visAmp_  [kVisChannels] = {};   // smoothed amplitude envelope
    std::vector<int> chNote_;             // latched semitone per module channel (-1 = none)
};
