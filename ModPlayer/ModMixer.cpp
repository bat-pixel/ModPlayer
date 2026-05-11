#include "framework.h"
#include "ModMixer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// Standard ProTracker vibrato sine table (64 entries, range -255..255)
static constexpr int kVibSine[64] = {
      0,  24,  49,  74,  97, 120, 141, 161,
    180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120,  97,  74,  49,  24,
      0, -24, -49, -74, -97,-120,-141,-161,
   -180,-197,-212,-224,-235,-244,-250,-253,
   -255,-253,-250,-244,-235,-224,-212,-197,
   -180,-161,-141,-120, -97, -74, -49, -24,
};

// ── Helpers ───────────────────────────────────────────────────────────────────

void ModMixer::UpdateSamplesPerTick()
{
    // samplesPerTick = sampleRate × 60 / (bpm × 24)
    // e.g. 44100 × 60 / (125 × 24) = 882
    samplesPerTick_ = (rate_ * 60) / (bpm_ * 24);
    if (samplesPerTick_ < 1) samplesPerTick_ = 1;
}

static inline double StepForPeriod(uint16_t period, int rate)
{
    if (period == 0) return 0.0;
    return MOD_PAL_CLOCK / (static_cast<double>(period) * 2.0 * rate);
}

// Returns the finetune-adjusted period for a given note period and sample.
// ProTracker stores finetune as signed –8..+7 in the sample header.
uint16_t ModMixer::FinetunedPeriod(uint16_t period, int sampleIdx) const
{
    if (sampleIdx < 0 || period == 0) return period;
    const int8_t ft = mod_->samples[sampleIdx].finetune;
    if (ft == 0) return period;
    const int ftRaw = (ft >= 0) ? static_cast<int>(ft) : (static_cast<int>(ft) + 16);
    const int noteIdx = PeriodToNoteIndex(period, 0);
    if (noteIdx < 0) return period;   // period not in table, use as-is
    return kPeriodTable[ftRaw & 0xF][noteIdx];
}

// ── Init ──────────────────────────────────────────────────────────────────────

void ModMixer::Init(const ModFile& mod, int sampleRate)
{
    mod_  = &mod;
    rate_ = sampleRate;
    speed_ = 6;
    bpm_   = 125;
    tick_  = 0;
    row_   = 0;
    order_ = 0;
    ch_    = {};
    jumpToOrder_ = false;
    breakToRow_  = false;
    playing_     = true;
    lpL_ = lpR_  = 0.f;
    vis          = {};
    UpdateSamplesPerTick();
    sampleCountdown_ = 0;   // fire ProcessTick immediately on the first Mix() call
}

// ── Mix ───────────────────────────────────────────────────────────────────────

void ModMixer::Mix(float* stereo, int numFrames)
{
    if (!playing_) {
        std::memset(stereo, 0, numFrames * 2 * sizeof(float));
        return;
    }

    // Classic Amiga hard panning: L R R L
    static constexpr float kPanL[MOD_CHANNELS] = { 1.f, 0.f, 0.f, 1.f };
    static constexpr float kPanR[MOD_CHANNELS] = { 0.f, 1.f, 1.f, 0.f };

    for (int i = 0; i < numFrames; ++i) {
        if (sampleCountdown_ == 0) {
            ProcessTick();
            sampleCountdown_ = samplesPerTick_;
        }
        --sampleCountdown_;

        float L = 0.f, R = 0.f;

        for (int c = 0; c < MOD_CHANNELS; ++c) {
            Channel& ch = ch_[c];
            auto&    cv = vis[c];

            if (ch.sampleIdx < 0) {
                cv.peak  *= 0.9998f;
                cv.active = false;
                cv.scope[cv.scopePos & (kScopeLen - 1)] = 0.f;
                ++cv.scopePos;
                continue;
            }

            const auto& pcm = mod_->sampleData[ch.sampleIdx];
            if (pcm.empty()) { ch.sampleIdx = -1; continue; }

            const SampleInfo& si = mod_->samples[ch.sampleIdx];

            // Loop-aware linear interpolation: s1 wraps to loop start at boundary
            const auto  p0  = static_cast<size_t>(ch.pos);
            const float frc = static_cast<float>(ch.pos - static_cast<double>(p0));
            const float s0  = (p0 < pcm.size()) ? pcm[p0] / 128.f : 0.f;
            float s1;
            if (si.repeatLength > 2) {
                const size_t loopEnd = si.repeatOffset + si.repeatLength;
                s1 = ((p0 + 1 < loopEnd) && (p0 + 1 < pcm.size()))
                   ? pcm[p0 + 1] / 128.f
                   : (si.repeatOffset < pcm.size() ? pcm[si.repeatOffset] / 128.f : s0);
            } else {
                s1 = (p0 + 1 < pcm.size()) ? pcm[p0 + 1] / 128.f : s0;
            }
            const float s = s0 + (s1 - s0) * frc;
            const float v = ch.vol / 64.f;

            L += s * v * kPanL[c];
            R += s * v * kPanR[c];

            // Visualization
            const float chSamp = s * v;
            cv.scope[cv.scopePos & (kScopeLen - 1)] = chSamp;
            ++cv.scopePos;
            cv.peak   = std::max(cv.peak * 0.9998f, std::abs(chSamp));
            cv.vol    = ch.vol;
            cv.period = ch.period;
            cv.active = true;

            ch.pos += ch.step;

            if (si.repeatLength > 2) {
                const double loopEnd = static_cast<double>(si.repeatOffset + si.repeatLength);
                if (ch.pos >= loopEnd)
                    ch.pos = si.repeatOffset
                           + std::fmod(ch.pos - si.repeatOffset,
                                       static_cast<double>(si.repeatLength));
            } else if (ch.pos >= static_cast<double>(si.lengthBytes)) {
                ch.sampleIdx = -1;
            }
        }

        // Amiga-style one-pole low-pass (~3.3 kHz at 44100 Hz)
        static constexpr float kLpAlpha = 0.37f;
        lpL_ += kLpAlpha * (L * 0.5f - lpL_);
        lpR_ += kLpAlpha * (R * 0.5f - lpR_);
        stereo[i * 2]     = lpL_;
        stereo[i * 2 + 1] = lpR_;
    }
}

// ── Tick engine ───────────────────────────────────────────────────────────────

void ModMixer::ProcessTick()
{
    if (!playing_) return;

    if (tick_ == 0)
        ProcessRow();
    else
        ApplyTickEffects();

    if (++tick_ >= speed_) {
        tick_ = 0;
        AdvanceRow();
    }
}

void ModMixer::AdvanceRow()
{
    if (jumpToOrder_) {
        order_       = jumpOrder_;
        row_         = 0;
        jumpToOrder_ = false;
        breakToRow_  = false;
    } else if (breakToRow_) {
        ++order_;
        row_        = breakRow_;
        breakToRow_ = false;
        if (order_ >= static_cast<int>(mod_->songLength))
            playing_ = false;
    } else {
        if (++row_ >= MOD_PATTERN_ROWS) {
            row_ = 0;
            if (++order_ >= static_cast<int>(mod_->songLength))
                playing_ = false;
        }
    }
}

void ModMixer::ProcessRow()
{
    if (order_ >= static_cast<int>(mod_->songLength)) { playing_ = false; return; }
    const int pat = mod_->orderTable[order_];
    if (pat >= static_cast<int>(mod_->patterns.size())) return;

    for (int c = 0; c < MOD_CHANNELS; ++c)
        TriggerNote(c, mod_->patterns[pat][row_][c]);
}

// ── TriggerNote ───────────────────────────────────────────────────────────────

void ModMixer::TriggerNote(int c, const Note& n)
{
    Channel& ch = ch_[c];

    // Load instrument (always sets default volume when sample number present)
    if (n.sample > 0 && n.sample <= MOD_SAMPLES) {
        ch.sampleIdx = n.sample - 1;
        ch.vol       = mod_->samples[ch.sampleIdx].volume;
    }

    // Effects 3xx/5xx: portamento — store target but suppress retrigger
    if (n.effect == 0x3 || n.effect == 0x5) {
        if (n.period > 0) ch.portaTarget = FinetunedPeriod(n.period, ch.sampleIdx);
        if (n.effect == 0x3 && n.param > 0) ch.portaSpeed = n.param;
        // Bootstrap: if no prior period, start at target so step is non-zero
        if (ch.period == 0 && ch.portaTarget > 0) {
            ch.period = ch.portaTarget;
            ch.step   = StepForPeriod(ch.portaTarget, rate_);
            ch.pos    = 0.0;
        }
    } else if (n.period > 0) {
        // All other effects: normal retrigger with finetune applied
        const uint16_t adjPeriod = FinetunedPeriod(n.period, ch.sampleIdx);
        ch.period = adjPeriod;
        ch.step   = StepForPeriod(adjPeriod, rate_);
        ch.pos    = 0.0;
        ch.vibPhase = 0;    // reset vibrato phase on new note
    }

    // Row-0 effect side-effects
    switch (n.effect) {
    case 0x1:   // Portamento Up — store speed for per-tick use
        if (n.param) ch.portaSpeed = n.param;
        break;
    case 0x2:   // Portamento Down
        if (n.param) ch.portaSpeed = n.param;
        break;
    case 0x4:   // Vibrato — (re)configure speed/depth
        if (n.param >> 4)   ch.vibSpeed = n.param >> 4;
        if (n.param & 0x0F) ch.vibDepth = n.param & 0x0F;
        break;
    case 0x5:   // Portamento to note + Volume Slide — portaTarget handled above; param is vol slide
        break;
    case 0x9:   // Sample Offset — reposition within the just-triggered sample
        if (n.period > 0) {     // only meaningful when a note triggered
            ch.pos = static_cast<double>(n.param) * 256.0;
            if (ch.sampleIdx >= 0) {
                double limit = static_cast<double>(mod_->samples[ch.sampleIdx].lengthBytes);
                if (ch.pos >= limit) ch.pos = limit > 0.0 ? limit - 1.0 : 0.0;
            }
        }
        break;
    case 0xC:   // Set Volume
        ch.vol = std::min<uint8_t>(n.param, 64);
        break;
    case 0xF:   // Set Speed / BPM
        if (n.param > 0) {
            if (n.param < 0x20) speed_ = n.param;
            else                { bpm_ = n.param; UpdateSamplesPerTick(); }
        }
        break;
    case 0xB:   // Jump to Order
        jumpToOrder_ = true;
        jumpOrder_   = std::min<int>(n.param, static_cast<int>(mod_->songLength) - 1);
        break;
    case 0xD:   // Pattern Break (param is BCD row)
        breakToRow_ = true;
        breakRow_   = std::min((n.param >> 4) * 10 + (n.param & 0xF), 63);
        break;
    case 0xE: { // Extended effects
        const uint8_t sub = n.param >> 4;
        const uint8_t val = n.param & 0x0F;
        if (sub == 0xC) {   // ECx: Note Cut — silence after val ticks (handled per-tick)
            (void)val;      // stored in param, re-read in ApplyTickEffects
        }
        break;
    }
    }
}

// ── ApplyTickEffects ─────────────────────────────────────────────────────────

void ModMixer::ApplyTickEffects()
{
    if (order_ >= static_cast<int>(mod_->songLength)) return;
    const int pat = mod_->orderTable[order_];
    if (pat >= static_cast<int>(mod_->patterns.size())) return;

    for (int c = 0; c < MOD_CHANNELS; ++c) {
        Channel&    ch = ch_[c];
        const Note& n  = mod_->patterns[pat][row_][c];

        switch (n.effect) {
        case 0x1:   // Portamento Up (lower period = higher pitch)
            if (ch.period > 0) {
                ch.period = static_cast<uint16_t>(
                    std::max(113, static_cast<int>(ch.period) - ch.portaSpeed));
                ch.step = StepForPeriod(ch.period, rate_);
            }
            break;

        case 0x2:   // Portamento Down (higher period = lower pitch)
            if (ch.period > 0) {
                ch.period = static_cast<uint16_t>(
                    std::min(856, static_cast<int>(ch.period) + ch.portaSpeed));
                ch.step = StepForPeriod(ch.period, rate_);
            }
            break;

        case 0x3:   // Portamento to Note
        case 0x5:   // Portamento to Note + Volume Slide
            if (ch.portaTarget > 0 && ch.period > 0) {
                const int delta = ch.portaSpeed;
                if (ch.period < ch.portaTarget)
                    ch.period = static_cast<uint16_t>(
                        std::min(static_cast<int>(ch.portaTarget),
                                 static_cast<int>(ch.period) + delta));
                else
                    ch.period = static_cast<uint16_t>(
                        std::max(static_cast<int>(ch.portaTarget),
                                 static_cast<int>(ch.period) - delta));
                ch.step = StepForPeriod(ch.period, rate_);
            }
            if (n.effect == 0x5) {
                // Volume slide component
                const int up   = n.param >> 4;
                const int down = n.param & 0xF;
                if (up > 0)
                    ch.vol = static_cast<uint8_t>(std::min(64, static_cast<int>(ch.vol) + up));
                else if (down > 0)
                    ch.vol = static_cast<uint8_t>(std::max(0,  static_cast<int>(ch.vol) - down));
            }
            break;

        case 0x4:   // Vibrato
        case 0x6: { // Vibrato + Volume Slide
            if (ch.period == 0) break;
            ch.vibPhase = (ch.vibPhase + ch.vibSpeed) & 63;
            const int vdelta = (kVibSine[ch.vibPhase] * ch.vibDepth) / 128;
            const int effP   = std::max(1, std::min(9999,
                                   static_cast<int>(ch.period) + vdelta));
            ch.step = StepForPeriod(static_cast<uint16_t>(effP), rate_);

            if (n.effect == 0x6) {
                const int up   = n.param >> 4;
                const int down = n.param & 0xF;
                if (up > 0)
                    ch.vol = static_cast<uint8_t>(std::min(64, static_cast<int>(ch.vol) + up));
                else if (down > 0)
                    ch.vol = static_cast<uint8_t>(std::max(0,  static_cast<int>(ch.vol) - down));
            }
            break;
        }

        case 0xA:   // Volume Slide
            if (n.param != 0) {
                const int up   = n.param >> 4;
                const int down = n.param & 0xF;
                if (up > 0)
                    ch.vol = static_cast<uint8_t>(std::min(64, static_cast<int>(ch.vol) + up));
                else
                    ch.vol = static_cast<uint8_t>(std::max(0,  static_cast<int>(ch.vol) - down));
            }
            break;

        case 0xE: { // Extended effects (per-tick subset)
            const uint8_t sub = n.param >> 4;
            const uint8_t val = n.param & 0x0F;
            if (sub == 0xC && tick_ == val)   // ECx: Note Cut at tick x
                ch.vol = 0;
            break;
        }
        }
    }
}
