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

    if (!mod_) return;

    // Update peak and activity from per-channel VU meters.
    const int nch = mod_->get_num_channels();
    for (int c = 0; c < kVisChannels; ++c) {
        auto& cv = vis[c];
        const float vu = (c < nch) ? mod_->get_current_channel_vu_mono(c) : 0.f;
        cv.peak   = std::max(cv.peak * 0.9998f, vu);
        cv.active = vu > 0.005f;
        cv.vol    = static_cast<uint8_t>(std::clamp(vu * 64.f, 0.f, 64.f));
        cv.period = 0;
    }

    // Per-channel instrument number, name, and effect from the current pattern row.
    const int order = mod_->get_current_order();
    const int row   = mod_->get_current_row();
    const int pat   = mod_->get_order_pattern(order);

    const bool hasInstr = mod_->get_num_instruments() > 0;
    const auto instrNames  = hasInstr  ? mod_->get_instrument_names() : std::vector<std::string>{};
    const auto sampleNames = !hasInstr ? mod_->get_sample_names()     : std::vector<std::string>{};

    auto hexNibble = [](char c) -> uint8_t {
        c = (char)toupper((unsigned char)c);
        return (c >= 'A') ? (uint8_t)(c - 'A' + 10) : (uint8_t)(c - '0');
    };

    for (int c = 0; c < kVisChannels; ++c) {
        auto& cv = vis[c];

        // Instrument / sample number
        std::string inst = mod_->format_pattern_row_channel_command(
            pat, row, c, openmpt::module::command_instrument);
        cv.sampleNum  = 0;
        cv.sampleName[0] = '\0';
        if (inst.size() >= 2 && inst[0] != '-' && inst[0] != ' ') {
            try {
                int n = std::stoi(inst);
                if (n > 0 && n <= 255) {
                    cv.sampleNum = static_cast<uint8_t>(n);
                    const auto& names = hasInstr ? instrNames : sampleNames;
                    if (n - 1 < (int)names.size() && !names[n - 1].empty()) {
                        size_t len = std::min(names[n - 1].size(), (size_t)22);
                        std::memcpy(cv.sampleName, names[n - 1].c_str(), len);
                        cv.sampleName[len] = '\0';
                    }
                }
            } catch (...) {}
        }

        // Effect command and parameter
        std::string eff = mod_->format_pattern_row_channel_command(
            pat, row, c, openmpt::module::command_effect);
        std::string prm = mod_->format_pattern_row_channel_command(
            pat, row, c, openmpt::module::command_parameter);
        cv.effect = 0;
        cv.param  = 0;
        if (!eff.empty() && eff[0] != ' ' && eff[0] != '.') {
            char ec = (char)toupper((unsigned char)eff[0]);
            cv.effect = (ec >= 'A') ? (uint8_t)(ec - 'A' + 10) : (uint8_t)(ec - '0');
        }
        if (prm.size() >= 2 && prm[0] != '.' && prm[0] != ' ') {
            cv.param = (hexNibble(prm[0]) << 4) | hexNibble(prm[1]);
        }
    }
}
