#include "framework.h"
#include "LibOpenMptMixer.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>

bool LibOpenMptMixer::Load(const std::string& path, int sampleRate)
{
    Unload();
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        mod_     = std::make_unique<openmpt::module>(f);
        rate_    = sampleRate;
        playing_ = true;
        vis      = {};
    } catch (...) {
        return false;
    }
    return true;
}

void LibOpenMptMixer::Unload()
{
    mod_.reset();
    playing_ = false;
}

std::string LibOpenMptMixer::SongTitle() const
{
    return mod_ ? mod_->get_metadata("title") : "";
}

void LibOpenMptMixer::Mix(float* stereo, int numFrames)
{
    if (!playing_ || !mod_) {
        std::memset(stereo, 0, numFrames * 2 * sizeof(float));
        return;
    }

    const auto rendered = static_cast<int>(
        mod_->read_interleaved_stereo(rate_, numFrames, stereo));

    if (rendered < numFrames) {
        std::memset(stereo + rendered * 2, 0,
                    (numFrames - rendered) * 2 * sizeof(float));
        playing_ = false;
    }

    UpdateVis(stereo, numFrames);
}

void LibOpenMptMixer::UpdateVis(const float* stereo, int numFrames)
{
    // Fill scope ring buffers from the stereo mix.
    // Mirror Amiga hard-pan layout: L R R L → panels 0 1 2 3
    for (int i = 0; i < numFrames; ++i) {
        const float L = stereo[i * 2];
        const float R = stereo[i * 2 + 1];
        for (int c : { 0, 3 }) {
            auto& cv = vis[c];
            cv.scope[cv.scopePos & (kScopeLen - 1)] = L;
            ++cv.scopePos;
        }
        for (int c : { 1, 2 }) {
            auto& cv = vis[c];
            cv.scope[cv.scopePos & (kScopeLen - 1)] = R;
            ++cv.scopePos;
        }
    }

    // Update peak and activity from per-channel VU meters.
    if (!mod_) return;
    const int nch = mod_->get_num_channels();
    for (int c = 0; c < kVisChannels; ++c) {
        auto& cv = vis[c];
        const float vu = (c < nch) ? mod_->get_current_channel_vu_mono(c) : 0.f;
        cv.peak   = std::max(cv.peak * 0.9998f, vu);
        cv.active = vu > 0.005f;
        cv.vol    = static_cast<uint8_t>(std::clamp(vu * 64.f, 0.f, 64.f));
        cv.period = 0;  // not an Amiga concept for generic formats
    }
}
