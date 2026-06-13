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
    (void)stereo;  // per-channel scopes are synthesised, not taken from the mix

    if (!mod_) {
        // No module: flat-line every panel.
        for (auto& cv : vis) {
            for (int i = 0; i < numFrames; ++i) {
                cv.scope[cv.scopePos & (kScopeLen - 1)] = 0.f;
                ++cv.scopePos;
            }
        }
        return;
    }

    const int nch   = std::max(1, mod_->get_num_channels());
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

    // Parse a libopenmpt note cell ("C-5", "A#4", "...", "===") to an absolute
    // semitone index, or -1 if the cell holds no fresh note.
    auto parseNote = [](const std::string& s) -> int {
        if (s.size() < 3) return -1;
        static const int kSemi[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A B C D E F G
        char letter = (char)toupper((unsigned char)s[0]);
        if (letter < 'A' || letter > 'G') return -1;            // skip ... === ^^^ ~~~
        int semi = kSemi[letter - 'A'];
        if (s[1] == '#') ++semi;
        else if (s[1] == 'b') --semi;
        int octave = (s[2] >= '0' && s[2] <= '9') ? (s[2] - '0') : 4;
        return octave * 12 + semi;
    };

    // Fold module channels onto the 4 display panels in contiguous groups, e.g.
    // an 8-channel module shows channels {0,1} {2,3} {4,5} {6,7} on panels 0–3.
    auto panelOf = [nch](int ch) {
        return std::min(kVisChannels - 1, ch * kVisChannels / nch);
    };

    // Latch the current note pitch for every module channel (held until retrigger).
    if ((int)chNote_.size() != nch) chNote_.assign(nch, -1);
    for (int ch = 0; ch < nch; ++ch) {
        int p = parseNote(mod_->format_pattern_row_channel_command(
                    pat, row, ch, openmpt::module::command_note));
        if (p >= 0) chNote_[ch] = p;
    }

    // Aggregate VU per panel; track each panel's loudest (dominant) channel and a
    // representative channel (first in the group) to use when the panel is silent.
    float panelVu [kVisChannels] = {};
    float loudVu  [kVisChannels] = {};
    int   loudCh  [kVisChannels] = { -1, -1, -1, -1 };
    int   firstCh [kVisChannels] = { -1, -1, -1, -1 };
    for (int ch = 0; ch < nch; ++ch) {
        const int   p  = panelOf(ch);
        const float vu = mod_->get_current_channel_vu_mono(ch);
        panelVu[p] += vu;
        if (firstCh[p] < 0)   firstCh[p] = ch;
        if (vu > loudVu[p]) { loudVu[p] = vu; loudCh[p] = ch; }
    }

    constexpr float kTwoPi = 6.28318530718f;

    for (int c = 0; c < kVisChannels; ++c) {
        auto& cv = vis[c];

        const float vu = std::min(1.f, panelVu[c]);
        cv.peak   = std::max(cv.peak * 0.9998f, vu);
        cv.active = vu > 0.005f;
        cv.vol    = static_cast<uint8_t>(std::clamp(vu * 64.f, 0.f, 64.f));
        cv.period = 0;

        // Label/pitch source: the panel's loudest channel, or its first channel
        // when nothing is sounding in the group.
        const int src = (loudCh[c] >= 0) ? loudCh[c] : firstCh[c];

        cv.sampleNum     = 0;
        cv.sampleName[0] = '\0';
        cv.effect        = 0;
        cv.param         = 0;
        if (src >= 0) {
            // Instrument / sample number
            std::string inst = mod_->format_pattern_row_channel_command(
                pat, row, src, openmpt::module::command_instrument);
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
                pat, row, src, openmpt::module::command_effect);
            std::string prm = mod_->format_pattern_row_channel_command(
                pat, row, src, openmpt::module::command_parameter);
            if (!eff.empty() && eff[0] != ' ' && eff[0] != '.') {
                char ec = (char)toupper((unsigned char)eff[0]);
                cv.effect = (ec >= 'A') ? (uint8_t)(ec - 'A' + 10) : (uint8_t)(ec - '0');
            }
            if (prm.size() >= 2 && prm[0] != '.' && prm[0] != ' ') {
                cv.param = (hexNibble(prm[0]) << 4) | hexNibble(prm[1]);
            }
        }

        // ── Synthesise the oscilloscope trace ──────────────────────────────────
        // Amplitude tracks the (smoothed) panel VU; the number of visible cycles
        // tracks the dominant channel's latched note. Phase persists across calls
        // so the trace stays continuous.
        const float target = std::min(1.f, vu * 1.3f);
        visAmp_[c] += (target - visAmp_[c]) * 0.35f;

        const int   note   = (src >= 0 && src < (int)chNote_.size()) ? chNote_[src] : -1;
        const float cycles = std::clamp(2.f + (note >= 0 ? note : 18) * 0.42f, 2.f, 48.f);
        const float inc    = cycles * kTwoPi / (float)kScopeLen;

        float ph = visPhase_[c];
        for (int i = 0; i < numFrames; ++i) {
            ph += inc;
            const float w = sinf(ph) + 0.35f * sinf(ph * 2.f + 0.5f) + 0.18f * sinf(ph * 3.f);
            cv.scope[cv.scopePos & (kScopeLen - 1)] = visAmp_[c] * w * 0.6f;
            ++cv.scopePos;
        }
        visPhase_[c] = fmodf(ph, kTwoPi);
    }
}
