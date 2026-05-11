#pragma once
#include "ModFile.h"
#include <array>

class ModMixer {
public:
    void Init(const ModFile& mod, int sampleRate = 44100);

    // Fills stereo interleaved float buffer [-1, 1].
    // Called exclusively from the audio thread.
    void Mix(float* stereo, int numFrames);

    bool IsPlaying() const { return playing_; }

    // ── Visualization ─────────────────────────────────────────────────────────
    // Written by the audio thread; read by the UI. Minor tearing is fine.
    static constexpr int kScopeLen = 256;
    struct ChannelVis {
        std::array<float, kScopeLen> scope{};
        int      scopePos = 0;   // next write index (mod kScopeLen)
        float    peak     = 0.f; // smoothed amplitude 0..1
        uint8_t  vol      = 0;
        uint16_t period   = 0;
        bool     active   = false;
    };
    std::array<ChannelVis, MOD_CHANNELS> vis{};

private:
    struct Channel {
        int      sampleIdx  = -1;   // index into mod->sampleData; -1 = silent
        double   pos        = 0.0;  // fractional read position within PCM
        double   step       = 0.0;  // pos increment per output frame (effective, incl. vibrato)
        uint8_t  vol        = 0;    // 0..64
        uint16_t period     = 0;    // base period (not modulated by vibrato)

        // Portamento state (effects 1xx, 2xx, 3xx)
        uint8_t  portaSpeed  = 0;
        uint16_t portaTarget = 0;   // 3xx target period

        // Vibrato state (effects 4xx, 6xx)
        uint8_t  vibSpeed = 0;
        uint8_t  vibDepth = 0;
        uint8_t  vibPhase = 0;
    };

    void UpdateSamplesPerTick();
    void ProcessTick();
    void ProcessRow();
    void AdvanceRow();
    void TriggerNote(int ch, const Note& n);
    void ApplyTickEffects();
    uint16_t FinetunedPeriod(uint16_t period, int sampleIdx) const;

    const ModFile* mod_  = nullptr;
    int            rate_ = 44100;

    std::array<Channel, MOD_CHANNELS> ch_{};

    int  speed_           = 6;
    int  bpm_             = 125;
    int  tick_            = 0;
    int  row_             = 0;
    int  order_           = 0;
    int  samplesPerTick_  = 882;
    int  sampleCountdown_ = 0;
    bool playing_         = false;

    bool  jumpToOrder_ = false;
    int   jumpOrder_   = 0;
    bool  breakToRow_  = false;
    int   breakRow_    = 0;

    float lpL_ = 0.f;   // Amiga low-pass filter state (left)
    float lpR_ = 0.f;   // Amiga low-pass filter state (right)
};
