// render_libopenmpt.cpp — render a MOD using LibOpenMptMixer (same engine as ffplay)
// and save to a WAV file for comparison.
//
// Build (from repo root):
//   cl.exe /nologo /std:c++20 /EHsc /MD /O2 /I"ModPlayer" /I"vcpkg_installed\x64-windows\include"
//          tests\render_libopenmpt.cpp ModPlayer\LibOpenMptMixer.cpp
//          /Fe"tests\bin\render_libopenmpt.exe" /Fo"tests\bin\\"
//          /link /LIBPATH:"vcpkg_installed\x64-windows\lib" libopenmpt.lib kernel32.lib

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

#include "LibOpenMptMixer.h"

static bool write_wav(const char* path, const float* stereo, int frames, int rate)
{
    FILE* f{};
    fopen_s(&f, path, "wb");
    if (!f) { printf("Cannot open %s\n", path); return false; }

    const int ch = 2, bits = 16;
    const int dataBytes = frames * ch * (bits / 8);

    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };

    fwrite("RIFF", 1, 4, f); w32(36 + dataBytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16);
    w16(1); w16(ch); w32(rate); w32(rate * ch * bits / 8);
    w16(ch * bits / 8); w16(bits);
    fwrite("data", 1, 4, f); w32(dataBytes);

    for (int i = 0; i < frames * ch; ++i) {
        float s = std::max(-1.f, std::min(1.f, stereo[i]));
        int16_t v = static_cast<int16_t>(s * 32767.f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char* argv[])
{
    std::string modPath = "MODS\\Demos\\Mod.Ackerlight 1.Mod";
    std::string outPath = "tests\\bin\\libopenmpt_ackerlight.wav";
    int seconds = 30;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)      outPath = argv[++i];
        else if (a == "--sec" && i + 1 < argc) seconds = std::stoi(argv[++i]);
        else if (a[0] != '-')                  modPath = a;
    }

    printf("Loading: %s\n", modPath.c_str());

    LibOpenMptMixer mixer;
    if (!mixer.Load(modPath, 44100)) {
        printf("ERROR: LibOpenMptMixer failed to load (openmpt.dll missing?)\n");
        return 1;
    }
    printf("Rendering %d s via libopenmpt (same engine as ffplay)...\n", seconds);

    const int totalFrames = 44100 * seconds;
    std::vector<float> buf(totalFrames * 2, 0.f);

    // Use the same 1024-frame chunk size as AudioOut to be maximally faithful.
    constexpr int kChunk = 1024;
    for (int off = 0; off < totalFrames && mixer.IsPlaying(); off += kChunk) {
        const int n = std::min(kChunk, totalFrames - off);
        mixer.Mix(buf.data() + off * 2, n);
    }

    if (write_wav(outPath.c_str(), buf.data(), totalFrames, 44100))
        printf("Saved -> %s\n", outPath.c_str());

    return 0;
}
