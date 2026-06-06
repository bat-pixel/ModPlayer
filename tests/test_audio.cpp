// test_audio.cpp — ModPlayer audio output tests
//
// Build as a Windows console application alongside:
//   ../ModPlayer/ModParser.cpp
//   ../ModPlayer/ModMixer.cpp
//
// Additional includes: ../ModPlayer (for headers)
// Additional libs: none (no AudioOut / Win32 UI)
//
// Run from the repo root so the default MOD path resolves:
//   tests\Release\test_audio.exe
// Or pass a path explicitly:
//   tests\Release\test_audio.exe "MODS\1987\mod.Ackerlight"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

#include "../ModPlayer/framework.h"
#include "../ModPlayer/ModFile.h"
#include "../ModPlayer/ModMixer.h"

// Pull in DumpEffectUsage (defined in ModParser.cpp, declared in ModFile.h)
// No extra include needed — ModFile.h already declares it.

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;
static const char* g_suite = "";

static void suite(const char* name) {
    g_suite = name;
    printf("\n[%s]\n", name);
}

static void check(bool cond, const char* msg, int line) {
    if (cond) {
        ++g_pass;
        printf("  PASS  %s\n", msg);
    } else {
        ++g_fail;
        printf("  FAIL  %s  (line %d)\n", msg, line);
    }
}

#define EXPECT(cond, msg)   check((cond), (msg), __LINE__)
#define EXPECT_FALSE(cond, msg) check(!(cond), (msg), __LINE__)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float rms(const float* buf, int n) {
    if (n == 0) return 0.f;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += static_cast<double>(buf[i]) * buf[i];
    return static_cast<float>(std::sqrt(sum / n));
}

static float peak(const float* buf, int n) {
    float p = 0.f;
    for (int i = 0; i < n; ++i) p = std::max(p, std::abs(buf[i]));
    return p;
}

// Deinterleave stereo buffer into separate left/right vectors.
static void deinterleave(const float* stereo, int frames,
                         std::vector<float>& L, std::vector<float>& R) {
    L.resize(frames); R.resize(frames);
    for (int i = 0; i < frames; ++i) {
        L[i] = stereo[i * 2 + 0];
        R[i] = stereo[i * 2 + 1];
    }
}

// ---------------------------------------------------------------------------
// Effect-usage diagnostics
// ---------------------------------------------------------------------------

static void print_effect_usage(const std::string& modPath) {
    ModFile mod;
    if (!LoadMod(modPath, mod)) {
        printf("  (cannot load %s for effect scan)\n", modPath.c_str());
        return;
    }
    DumpEffectUsage(mod);
}

// ---------------------------------------------------------------------------
// Parser tests
// ---------------------------------------------------------------------------

static void test_parser(const std::string& validPath) {
    suite("Parser");

    // Invalid path
    {
        ModFile mod;
        EXPECT_FALSE(LoadMod("nonexistent_file.mod", mod),
                     "LoadMod returns false for missing file");
    }

    // Corrupt/empty content — write a temp file
    {
        const std::string tmp = "tmp_corrupt.mod";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f) {
            uint8_t junk[16] = {};
            fwrite(junk, 1, sizeof(junk), f);
            fclose(f);
            ModFile mod;
            EXPECT_FALSE(LoadMod(tmp, mod),
                         "LoadMod returns false for corrupt (too-short) file");
            remove(tmp.c_str());
        }
    }

    // Valid file
    {
        ModFile mod;
        bool ok = LoadMod(validPath, mod);
        EXPECT(ok, "LoadMod returns true for a valid 4-channel MOD");
        if (!ok) { printf("  (skipping dependent parser tests — file not found)\n"); return; }

        EXPECT(mod.songLength > 0 && mod.songLength <= 128,
               "Song length is in valid range [1, 128]");

        EXPECT(!mod.patterns.empty(),
               "At least one pattern was parsed");

        EXPECT(static_cast<int>(mod.samples.size()) == MOD_SAMPLES,
               "Exactly 31 sample slots were parsed");

        // Magic must be one of the accepted 4-channel tags
        const bool validMagic =
            std::strcmp(mod.magic, "M.K.") == 0 ||
            std::strcmp(mod.magic, "M!K!") == 0 ||
            std::strcmp(mod.magic, "FLT4") == 0 ||
            std::strcmp(mod.magic, "4CHN") == 0;
        EXPECT(validMagic, "Magic tag is a recognised 4-channel ProTracker variant");

        // At least one sample has PCM data
        bool hasPcm = false;
        for (const auto& sd : mod.sampleData)
            if (!sd.empty()) { hasPcm = true; break; }
        EXPECT(hasPcm, "At least one sample has PCM data");

        // Order table entries all reference existing patterns
        bool orderOk = true;
        for (int i = 0; i < mod.songLength; ++i) {
            if (mod.orderTable[i] >= static_cast<int>(mod.patterns.size()))
                { orderOk = false; break; }
        }
        EXPECT(orderOk, "All order-table entries reference existing patterns");
    }
}

// ---------------------------------------------------------------------------
// Mixer tests
// ---------------------------------------------------------------------------

static void test_mixer(const std::string& validPath) {
    suite("Mixer");

    ModFile mod;
    if (!LoadMod(validPath, mod)) {
        printf("  SKIP  (could not load test MOD: %s)\n", validPath.c_str());
        return;
    }

    constexpr int kRate   = 44100;
    constexpr int kFrames = kRate;          // 1 second of audio
    std::vector<float> buf(kFrames * 2, 0.f);

    // --- Basic state after Init ---
    {
        ModMixer mx;
        mx.Init(mod, kRate);
        EXPECT(mx.IsPlaying(), "IsPlaying() is true immediately after Init");
        EXPECT(!mx.SongTitle().empty() || mx.SongTitle().empty(),
               "SongTitle() does not crash");
        EXPECT(std::strcmp(mx.BackendName(), "Native") == 0,
               "BackendName() returns \"Native\"");
    }

    // --- Output is non-silent ---
    {
        ModMixer mx;
        mx.Init(mod, kRate);
        mx.Mix(buf.data(), kFrames);

        std::vector<float> L, R;
        deinterleave(buf.data(), kFrames, L, R);

        EXPECT(rms(L.data(), kFrames) > 0.001f,
               "Left channel RMS is non-zero (music is audible)");
        EXPECT(rms(R.data(), kFrames) > 0.001f,
               "Right channel RMS is non-zero (music is audible)");
    }

    // --- Samples stay within plausible amplitude ---
    // Two channels sum into each side; each channel is at most ±1.0 so the
    // combined output can be up to ±2.0 before the Amiga LP filter.
    {
        ModMixer mx;
        mx.Init(mod, kRate);
        mx.Mix(buf.data(), kFrames);

        EXPECT(peak(buf.data(), kFrames * 2) <= 2.0f,
               "All output samples are within [-2.0, 2.0]");

        bool hasNaN = false;
        for (int i = 0; i < kFrames * 2; ++i)
            if (!std::isfinite(buf[i])) { hasNaN = true; break; }
        EXPECT_FALSE(hasNaN, "Output contains no NaN or Inf values");
    }

    // --- Determinism: identical state → identical output ---
    {
        std::vector<float> buf2(kFrames * 2, 0.f);
        ModMixer mx1, mx2;
        mx1.Init(mod, kRate);
        mx2.Init(mod, kRate);
        mx1.Mix(buf.data(),  kFrames);
        mx2.Mix(buf2.data(), kFrames);

        bool identical = (std::memcmp(buf.data(), buf2.data(),
                                      kFrames * 2 * sizeof(float)) == 0);
        EXPECT(identical,
               "Two mixers with identical initial state produce bit-identical output");
    }

    // --- Amiga stereo panning: verify channel assignment ---
    // Channels 0 and 3 pan hard left; channels 1 and 2 pan hard right.
    // After 1 s of a typical 4-channel MOD, both sides should carry signal,
    // and neither side should be completely silent.
    {
        ModMixer mx;
        mx.Init(mod, kRate);
        mx.Mix(buf.data(), kFrames);
        std::vector<float> L, R;
        deinterleave(buf.data(), kFrames, L, R);

        // Stereo correlation: left and right should not be identical
        // (they carry independent channels due to hard panning).
        double dotLR = 0.0, normL = 0.0, normR = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            dotLR += L[i] * R[i];
            normL += L[i] * L[i];
            normR += R[i] * R[i];
        }
        const double denom = std::sqrt(normL * normR);
        const double corr  = (denom > 1e-9) ? dotLR / denom : 1.0;
        EXPECT(corr < 0.99,
               "Left and right channels are not identical (hard-pan stereo is active)");
    }

    // --- Silent output when not playing ---
    {
        ModMixer mx;
        // Never call Init — mixer starts in non-playing state.
        std::vector<float> silentBuf(256 * 2, 0.f);
        mx.Mix(silentBuf.data(), 256);
        EXPECT(peak(silentBuf.data(), 256 * 2) == 0.f,
               "Uninitialised mixer outputs silence");
    }

    // --- Incremental mixing: chunked == single-call for same total frames ---
    {
        constexpr int kChunk = 512;
        constexpr int kTotal = kChunk * 4;
        std::vector<float> chunked(kTotal * 2, 0.f);
        std::vector<float> single(kTotal * 2, 0.f);

        ModMixer mxA, mxB;
        mxA.Init(mod, kRate);
        mxB.Init(mod, kRate);

        for (int off = 0; off < kTotal; off += kChunk)
            mxA.Mix(chunked.data() + off * 2, kChunk);
        mxB.Mix(single.data(), kTotal);

        bool identical = (std::memcmp(chunked.data(), single.data(),
                                      kTotal * 2 * sizeof(float)) == 0);
        EXPECT(identical,
               "Chunked mixing produces the same output as a single large call");
    }

    // --- Visualization data is updated after mixing ---
    {
        ModMixer mx;
        mx.Init(mod, kRate);
        mx.Mix(buf.data(), kFrames);

        bool anyActive = false;
        for (int c = 0; c < IMixer::kVisChannels; ++c)
            if (mx.vis[c].active) { anyActive = true; break; }
        EXPECT(anyActive, "At least one visualiser channel is marked active after mixing");

        bool anyPeak = false;
        for (int c = 0; c < IMixer::kVisChannels; ++c)
            if (mx.vis[c].peak > 0.f) { anyPeak = true; break; }
        EXPECT(anyPeak, "At least one visualiser channel has a non-zero peak after mixing");
    }
}

// ---------------------------------------------------------------------------
// WAV rendering
// ---------------------------------------------------------------------------

static bool write_wav(const std::string& path, const float* stereo,
                      int frames, int rate = 44100)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("  ERROR: cannot open %s for writing\n", path.c_str()); return false; }

    const int channels   = 2;
    const int bitsPerSmp = 16;
    const int byteRate   = rate * channels * bitsPerSmp / 8;
    const int dataBytes  = frames * channels * (bitsPerSmp / 8);
    const int riffSize   = 36 + dataBytes;

    // RIFF header
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);  w32(riffSize);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);  w32(16);
    w16(1);                    // PCM
    w16(static_cast<uint16_t>(channels));
    w32(static_cast<uint32_t>(rate));
    w32(static_cast<uint32_t>(byteRate));
    w16(static_cast<uint16_t>(channels * bitsPerSmp / 8));
    w16(static_cast<uint16_t>(bitsPerSmp));
    fwrite("data", 1, 4, f);  w32(static_cast<uint32_t>(dataBytes));

    // Samples (float → int16, clamped)
    for (int i = 0; i < frames * channels; ++i) {
        float s = std::max(-1.f, std::min(1.f, stereo[i]));
        int16_t v = static_cast<int16_t>(s * 32767.f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return true;
}

static void render_wav(const std::string& modPath, const std::string& wavPath,
                       int seconds = 10)
{
    ModFile mod;
    if (!LoadMod(modPath, mod)) {
        printf("  ERROR: cannot load %s\n", modPath.c_str());
        return;
    }
    constexpr int kRate = 44100;
    const int frames = kRate * seconds;
    std::vector<float> buf(frames * 2);

    ModMixer mx;
    mx.Init(mod, kRate);
    mx.Mix(buf.data(), frames);

    if (write_wav(wavPath, buf.data(), frames, kRate))
        printf("  Rendered %d s → %s\n", seconds, wavPath.c_str());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Usage:
    //   test_audio.exe [modpath]
    //   test_audio.exe --wav <out.wav> [modpath] [seconds]
    bool wavMode = false;
    std::string wavPath;
    std::string modPath = "MODS\\1987\\mod.Ackerlight";
    int wavSeconds = 10;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--wav" && i + 1 < argc) {
            wavMode = true;
            wavPath = argv[++i];
        } else if (wavMode && wavPath.size() && i == (wavMode ? 3 : 1)) {
            modPath = a;
        } else if (wavMode && std::isdigit(a[0])) {
            wavSeconds = std::stoi(a);
        } else if (a[0] != '-') {
            modPath = a;
        }
    }

    if (wavMode) {
        printf("Rendering %d s of native mixer output...\n", wavSeconds);
        render_wav(modPath, wavPath, wavSeconds);
        return 0;
    }

    printf("ModPlayer Audio Tests\n");
    printf("MOD file: %s\n\n", modPath.c_str());

    print_effect_usage(modPath);
    test_parser(modPath);
    test_mixer(modPath);

    printf("\n----------------------------------------\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
