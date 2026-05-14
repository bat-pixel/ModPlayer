#pragma once
#include "IMixer.h"
#include "ModFile.h"
#include <array>

class ModMixer : public IMixer {
public:
    // Initialise (or reinitialise) with a loaded MOD file.
    // Called from the UI thread while holding the audio mutex.
    void Init(const ModFile& mod, int sampleRate = 44100);

    void        Mix(float* stereo, int numFrames) override;
    bool        IsPlaying()   const override { return playing_; }
    const char* BackendName() const override { return "Native"; }
    std::string SongTitle()   const override;

private:
    struct Channel {
        int      sampleIdx  = -1;   // index into mod->sampleData; -1 = silent
        double   pos        = 0.0;  // fractional read position within PCM
        double   step       = 0.0;  // pos increment per output frame (incl. vibrato)
        uint8_t  vol        = 0;    // 0..64
        uint16_t period     = 0;    // base period (not modulated by vibrato)

        // Portamento (effects 1xx, 2xx, 3xx, 5xx)
        uint8_t  portaSpeed  = 0;
        uint16_t portaTarget = 0;

        // Vibrato (effects 4xx, 6xx)
        uint8_t  vibSpeed = 0;
        uint8_t  vibDepth = 0;
        uint8_t  vibPhase = 0;
        // bits 0-1: 0=sine 1=ramp-down 2=square 3=random; bit2=no phase retrigger
        uint8_t  vibWave  = 0;

        // Tremolo (effect 7xx)
        uint8_t  tremSpeed = 0;
        uint8_t  tremDepth = 0;
        uint8_t  tremPhase = 0;
        uint8_t  tremWave  = 0;
        int8_t   tremVol   = 0;   // per-tick volume offset applied in Mix()

        // Note delay (effect EDx)
        uint8_t  delayTick = 0;   // 0 = none; else tick# to trigger on
        Note     delayNote = {};
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
