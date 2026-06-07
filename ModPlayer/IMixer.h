#pragma once
#include <array>
#include <cstdint>
#include <string>

// Abstract playback backend interface.
// Implementations: ModMixer (native ProTracker engine), LibOpenMptMixer (libopenmpt).
class IMixer {
public:
    virtual ~IMixer() = default;

    // Fill stereo interleaved float buffer [-1, 1].
    // Called exclusively from the audio thread.
    virtual void Mix(float* stereo, int numFrames) = 0;
    virtual bool IsPlaying() const = 0;

    // Short label for the title bar ("Native", "libopenmpt", …).
    virtual const char* BackendName() const = 0;
    // Song title as UTF-8 (may be empty).
    virtual std::string SongTitle() const = 0;

    // Playback position — read from UI thread (minor tearing is acceptable).
    virtual int    CurrentOrder()       const { return 0; }
    virtual int    CurrentRow()         const { return 0; }
    virtual int    SongOrders()         const { return 0; }
    virtual double GetDurationSeconds() const { return 0.0; }
    virtual double GetPositionSeconds() const { return 0.0; }
    virtual void   SeekSeconds(double)        {}

    // ── Visualization ──────────────────────────────────────────────────────────
    // Written by the audio thread inside Mix(); read by the UI thread.
    // Minor tearing is acceptable — no lock needed.
    static constexpr int kScopeLen    = 256;
    static constexpr int kVisChannels = 4;   // always 4 display panels

    struct ChannelVis {
        std::array<float, kScopeLen> scope{};
        int      scopePos  = 0;      // next write index (wraps mod kScopeLen)
        float    peak      = 0.f;    // slow-decay max amplitude [0, 1]
        uint8_t  vol       = 0;      // volume [0, 64]
        uint16_t period    = 0;      // Amiga period (0 for non-native backends)
        bool     active    = false;
        uint8_t  sampleNum = 0;      // instrument/sample number 1–31; 0 = none
        char     sampleName[23]{};   // MOD sample name (null-terminated)
        uint8_t  effect    = 0;      // effect command nibble (0x0–0xF)
        uint8_t  param     = 0;      // effect parameter byte
    };
    std::array<ChannelVis, kVisChannels> vis{};
};
