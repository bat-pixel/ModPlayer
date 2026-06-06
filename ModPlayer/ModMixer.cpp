#include "framework.h"
#include "ModMixer.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

// Standard ProTracker vibrato/tremolo sine table (64 entries, range -255..255)
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

// Waveform lookup for vibrato/tremolo: 0=sine, 1=ramp-down, 2=square, 3=random
static int WaveValue(uint8_t wave, uint8_t phase)
{
    phase &= 63;
    switch (wave & 3) {
    case 0: return kVibSine[phase];
    case 1: return 255 - (phase * 8);
    case 2: return (phase < 32) ? 255 : -255;
    default: return (std::rand() & 0x1FF) - 255;
    }
}

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

uint16_t ModMixer::FinetunedPeriod(uint16_t period, int8_t finetune) const
{
    if (period == 0 || finetune == 0) return period;
    const int ftRaw = (finetune >= 0) ? static_cast<int>(finetune)
                                      : static_cast<int>(finetune) + 16;
    const int noteIdx = PeriodToNoteIndex(period, 0);
    if (noteIdx < 0) return period;
    return kPeriodTable[ftRaw & 0xF][noteIdx];
}

uint16_t ModMixer::FinetunedPeriod(uint16_t period, int sampleIdx) const
{
    if (sampleIdx < 0 || period == 0) return period;
    return FinetunedPeriod(period, mod_->samples[sampleIdx].finetune);
}

std::string ModMixer::SongTitle() const
{
    return mod_ ? mod_->songName : "";
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
    // Amiga hard-pan defaults: channels 0,3 → left; channels 1,2 → right
    ch_[0].panL = 1.f; ch_[0].panR = 0.f;
    ch_[1].panL = 0.f; ch_[1].panR = 1.f;
    ch_[2].panL = 0.f; ch_[2].panR = 1.f;
    ch_[3].panL = 1.f; ch_[3].panR = 0.f;
    jumpToOrder_ = false;
    breakToRow_  = false;
    loopRow_     = 0;
    loopCount_   = 0;
    loopBack_    = false;
    patternDelay_ = 0;
    rowTriggered_ = false;
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
            const float v = std::clamp(static_cast<int>(ch.vol) + ch.tremVol, 0, 64) / 64.f;

            L += s * v * ch.panL;
            R += s * v * ch.panR;

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

        // Amiga-style one-pole low-pass (~4.7 kHz at 44100 Hz, always-on RC filter)
        static constexpr float kLpAlpha = 0.49f;
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

    if (tick_ == 0 && !rowTriggered_) {
        rowTriggered_ = true;
        ProcessRow();
    } else if (tick_ != 0) {
        ApplyTickEffects();
    }

    if (++tick_ >= speed_) {
        tick_ = 0;
        if (patternDelay_ > 0) {
            --patternDelay_;
            // rowTriggered_ stays true; ProcessRow won't refire on tick 0
        } else {
            rowTriggered_ = false;
            AdvanceRow();
        }
    }
}

void ModMixer::AdvanceRow()
{
    if (loopBack_) {
        row_      = loopRow_;
        loopBack_ = false;
        return;
    }
    if (jumpToOrder_) {
        loopRow_ = 0; loopCount_ = 0;
        order_       = jumpOrder_;
        row_         = 0;
        jumpToOrder_ = false;
        breakToRow_  = false;
    } else if (breakToRow_) {
        loopRow_ = 0; loopCount_ = 0;
        ++order_;
        row_        = breakRow_;
        breakToRow_ = false;
        if (order_ >= static_cast<int>(mod_->songLength))
            order_ = 0;   // loop back to start
    } else {
        if (++row_ >= MOD_PATTERN_ROWS) {
            loopRow_ = 0; loopCount_ = 0;
            row_ = 0;
            if (++order_ >= static_cast<int>(mod_->songLength))
                order_ = 0;   // loop back to start
        }
    }
}

void ModMixer::ProcessRow()
{
    if (order_ >= static_cast<int>(mod_->songLength)) { playing_ = false; return; }
    const int pat = mod_->orderTable[order_];
    if (pat >= static_cast<int>(mod_->patterns.size())) return;

    for (int c = 0; c < MOD_CHANNELS; ++c) {
        ch_[c].tremVol = 0;  // tremolo doesn't apply on tick 0
        const Note& n = mod_->patterns[pat][row_][c];

        // EDx: delay note trigger until tick x; apply instrument volume now
        if (n.effect == 0xE && (n.param >> 4) == 0xD && (n.param & 0xF) > 0) {
            if (n.sample > 0 && n.sample <= MOD_SAMPLES)
                ch_[c].vol = mod_->samples[n.sample - 1].volume;
            ch_[c].delayTick = n.param & 0xF;
            ch_[c].delayNote = n;
        } else {
            ch_[c].delayTick = 0;
            TriggerNote(c, n);
        }
    }
}

// ── TriggerNote ───────────────────────────────────────────────────────────────

void ModMixer::TriggerNote(int c, const Note& n)
{
    Channel& ch = ch_[c];

    // Load instrument: copy default volume and sample finetune.
    if (n.sample > 0 && n.sample <= MOD_SAMPLES) {
        ch.sampleIdx = n.sample - 1;
        ch.vol       = mod_->samples[ch.sampleIdx].volume;
        ch.finetune  = mod_->samples[ch.sampleIdx].finetune;
    }

    // E5x pre-scan: override finetune before period is applied.
    if (n.effect == 0xE && (n.param >> 4) == 0x5) {
        const uint8_t val = n.param & 0xF;
        ch.finetune = (val >= 8) ? static_cast<int8_t>(val - 16)
                                 : static_cast<int8_t>(val);
    }

    // Effects 3xx/5xx: portamento — store target but suppress retrigger.
    if (n.effect == 0x3 || n.effect == 0x5) {
        if (n.period > 0) ch.portaTarget = FinetunedPeriod(n.period, ch.finetune);
        if (n.effect == 0x3 && n.param > 0) ch.portaSpeed = n.param;
        if (ch.period == 0 && ch.portaTarget > 0) {
            ch.period = ch.portaTarget;
            ch.step   = StepForPeriod(ch.portaTarget, rate_);
            ch.pos    = 0.0;
        }
    } else if (n.period > 0) {
        const uint16_t adjPeriod = FinetunedPeriod(n.period, ch.finetune);
        ch.period = adjPeriod;
        ch.step   = StepForPeriod(adjPeriod, rate_);
        ch.pos    = 0.0;
        if (!(ch.vibWave  & 4)) ch.vibPhase  = 0;
        if (!(ch.tremWave & 4)) ch.tremPhase = 0;
    }

    // Row-0 effect side-effects
    switch (n.effect) {
    case 0x1:   // Portamento Up
        if (n.param) ch.portaSpeed = n.param;
        break;
    case 0x2:   // Portamento Down
        if (n.param) ch.portaSpeed = n.param;
        break;
    case 0x4:   // Vibrato
        if (n.param >> 4)   ch.vibSpeed = n.param >> 4;
        if (n.param & 0x0F) ch.vibDepth = n.param & 0x0F;
        break;
    case 0x5:   // Portamento to note + Volume Slide
        break;
    case 0x7:   // Tremolo
        if (n.param >> 4)   ch.tremSpeed = n.param >> 4;
        if (n.param & 0x0F) ch.tremDepth = n.param & 0x0F;
        break;
    case 0x8:   // Set Panning (non-standard, 0x00=full-left, 0x80=centre, 0xFF=full-right)
        ch.panL = (255 - n.param) / 255.f;
        ch.panR = n.param / 255.f;
        break;
    case 0x9:   // Sample Offset
        if (n.period > 0) {
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
    case 0xE: { // Extended effects (tick-0 subset)
        const uint8_t sub = n.param >> 4;
        const uint8_t val = n.param & 0x0F;
        switch (sub) {
        case 0x1: // E1x: Fine portamento up
            if (ch.period > 0) {
                ch.period = static_cast<uint16_t>(
                    std::max(113, static_cast<int>(ch.period) - val));
                ch.step = StepForPeriod(ch.period, rate_);
            }
            break;
        case 0x2: // E2x: Fine portamento down
            if (ch.period > 0) {
                ch.period = static_cast<uint16_t>(
                    std::min(856, static_cast<int>(ch.period) + val));
                ch.step = StepForPeriod(ch.period, rate_);
            }
            break;
        case 0x3: // E3x: Glissando control (0=off, else=on)
            ch.glissando = (val != 0);
            break;
        case 0x4: // E4x: Set vibrato waveform
            ch.vibWave = val;
            break;
        case 0x5: // E5x: Set finetune (handled early above; update period if note sounding)
            if (n.period == 0 && ch.period > 0) {
                // No new note — re-pitch the currently playing note
                ch.period = FinetunedPeriod(ch.period, ch.finetune);
                ch.step   = StepForPeriod(ch.period, rate_);
            }
            break;
        case 0x6: // E6x: Pattern Loop
            if (val == 0) {
                loopRow_ = row_;
            } else {
                if (loopCount_ == 0) {
                    loopCount_ = val;
                    loopBack_  = true;
                } else {
                    --loopCount_;
                    if (loopCount_ > 0) loopBack_ = true;
                }
            }
            break;
        case 0x7: // E7x: Set tremolo waveform
            ch.tremWave = val;
            break;
        case 0xA: // EAx: Fine volume slide up
            ch.vol = static_cast<uint8_t>(std::min(64, static_cast<int>(ch.vol) + val));
            break;
        case 0xB: // EBx: Fine volume slide down
            ch.vol = static_cast<uint8_t>(std::max(0, static_cast<int>(ch.vol) - val));
            break;
        case 0xC: // ECx: Note Cut — fires per-tick in ApplyTickEffects
        case 0xD: // EDx: Note Delay — handled in ProcessRow
            break;
        case 0xE: // EEx: Pattern Delay
            if (val > 0) patternDelay_ = val;
            break;
        default:
            break;
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

        // EDx: fire delayed note trigger
        if (ch.delayTick > 0 && tick_ == static_cast<int>(ch.delayTick)) {
            ch.delayTick = 0;
            TriggerNote(c, ch.delayNote);  // EDx sub-case in TriggerNote is a no-op
            continue;
        }

        switch (n.effect) {
        case 0x0:   // Arpeggio (0xy: cycle base / +x / +y semitones)
            if (n.param != 0 && ch.period > 0) {
                const int x = n.param >> 4;
                const int y = n.param & 0xF;
                const int t = tick_ % 3;
                const int semis = (t == 1) ? x : (t == 2) ? y : 0;
                if (semis == 0) {
                    ch.step = StepForPeriod(ch.period, rate_);
                } else {
                    int ftRaw = 0;
                    if (ch.sampleIdx >= 0) {
                        const int8_t ft = mod_->samples[ch.sampleIdx].finetune;
                        ftRaw = (ft >= 0) ? static_cast<int>(ft) : static_cast<int>(ft) + 16;
                    }
                    const int ni = PeriodToNoteIndex(ch.period, ftRaw);
                    if (ni >= 0) {
                        const int ni2 = std::clamp(ni + semis, 0, 35);
                        ch.step = StepForPeriod(kPeriodTable[ftRaw & 0xF][ni2], rate_);
                    }
                }
            }
            break;

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

                // E3x glissando: snap to nearest semitone in the period table
                if (ch.glissando) {
                    const int ftRaw = (ch.finetune >= 0) ? ch.finetune : ch.finetune + 16;
                    int bestNi = 0, bestDist = INT_MAX;
                    for (int ni = 0; ni < 36; ++ni) {
                        int d = std::abs(static_cast<int>(ch.period)
                                       - static_cast<int>(kPeriodTable[ftRaw & 0xF][ni]));
                        if (d < bestDist) { bestDist = d; bestNi = ni; }
                    }
                    ch.period = kPeriodTable[ftRaw & 0xF][bestNi];
                }
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
            // Apply at current phase, then advance (ProTracker order)
            const int vdelta = (WaveValue(ch.vibWave, ch.vibPhase) * ch.vibDepth) / 128;
            ch.vibPhase = (ch.vibPhase + ch.vibSpeed) & 63;
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

        case 0x7: { // Tremolo — modulate volume without changing ch.vol
            const int tdelta = (WaveValue(ch.tremWave, ch.tremPhase) * ch.tremDepth) >> 6;
            ch.tremPhase = (ch.tremPhase + ch.tremSpeed) & 63;
            ch.tremVol   = static_cast<int8_t>(std::clamp(tdelta, -64, 63));
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
            if (sub == 0x9 && val > 0 && (tick_ % val) == 0)
                ch.pos = 0.0;                              // E9x: Retrigger
            else if (sub == 0xC && tick_ == static_cast<int>(val))
                ch.vol = 0;                                // ECx: Note Cut
            break;
        }
        }
    }
}
