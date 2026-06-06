#include "framework.h"
#include "ModFile.h"

#include <fstream>
#include <algorithm>
#include <cstring>
#include <format>
#include <cstdio>
#include <array>

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint16_t ReadBE16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// ── LoadMod ───────────────────────────────────────────────────────────────────

bool LoadMod(const std::string& path, ModFile& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    const auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0);

    std::vector<uint8_t> data(fileSize);
    f.read(reinterpret_cast<char*>(data.data()), fileSize);
    if (!f) return false;

    if (fileSize < static_cast<size_t>(MOD_OFFSET_PATTERN_DATA))
        return false;

    const uint8_t* d = data.data();

    // Validate magic — only accept 4-channel ProTracker variants
    {
        char magic[5]{};
        std::memcpy(magic, d + MOD_OFFSET_MAGIC, 4);
        static constexpr const char* k4ch[] = { "M.K.", "M!K!", "FLT4", "4CHN" };
        bool ok = false;
        for (auto m : k4ch) if (std::memcmp(magic, m, 4) == 0) { ok = true; break; }
        if (!ok) return false;
    }

    // Song name (20 bytes, may not be null-terminated in file)
    std::memcpy(out.songName, d + MOD_OFFSET_SONG_NAME, 20);
    out.songName[20] = '\0';

    // Magic tag
    std::memcpy(out.magic, d + MOD_OFFSET_MAGIC, 4);
    out.magic[4] = '\0';

    // 31 sample headers, 30 bytes each
    out.samples.resize(MOD_SAMPLES);
    for (int i = 0; i < MOD_SAMPLES; ++i) {
        const uint8_t* h = d + MOD_OFFSET_SAMPLE_HDRS + i * 30;
        SampleInfo& s = out.samples[i];

        std::memcpy(s.name, h, 22);
        s.name[22] = '\0';

        s.lengthBytes  = ReadBE16(h + 22) * 2u;

        // finetune is a signed 4-bit value stored in the low nibble
        uint8_t rawFine = h[24] & 0x0F;
        s.finetune = (rawFine >= 8) ? static_cast<int8_t>(rawFine - 16)
                                    : static_cast<int8_t>(rawFine);

        s.volume       = std::min<uint8_t>(h[25], 64);
        s.repeatOffset = ReadBE16(h + 26) * 2u;
        s.repeatLength = ReadBE16(h + 28) * 2u;
    }

    // Song length, restart position, and pattern order
    out.songLength  = d[MOD_OFFSET_SONG_LEN];
    out.restartPos  = d[MOD_OFFSET_RESTART_POS];
    // Clamp to a valid order; 0x7F means "no defined restart" — default to 0
    if (out.restartPos >= out.songLength) out.restartPos = 0;
    std::memcpy(out.orderTable, d + MOD_OFFSET_ORDER_TABLE, 128);

    // Number of patterns = highest pattern index referenced + 1
    int numPatterns = 0;
    for (int i = 0; i < 128; ++i)
        numPatterns = std::max(numPatterns, static_cast<int>(out.orderTable[i]) + 1);

    const size_t patternDataSize = static_cast<size_t>(numPatterns)
                                   * MOD_PATTERN_ROWS * MOD_CHANNELS * 4;
    if (fileSize < MOD_OFFSET_PATTERN_DATA + patternDataSize)
        return false;

    // Parse patterns
    out.patterns.resize(numPatterns);
    const uint8_t* p = d + MOD_OFFSET_PATTERN_DATA;

    for (int pat = 0; pat < numPatterns; ++pat) {
        Pattern& pattern = out.patterns[pat];
        for (int row = 0; row < MOD_PATTERN_ROWS; ++row) {
            for (int ch = 0; ch < MOD_CHANNELS; ++ch) {
                // 4-byte encoding:
                //  byte0: [sHi(4) | periodHi(4)]
                //  byte1: [periodLo(8)]
                //  byte2: [sLo(4)  | effectCmd(4)]
                //  byte3: [effectParam(8)]
                const uint8_t b0 = *p++, b1 = *p++, b2 = *p++, b3 = *p++;

                Note& n   = pattern[row][ch];
                n.sample  = static_cast<uint8_t>((b0 & 0xF0) | (b2 >> 4));
                n.period  = static_cast<uint16_t>(((b0 & 0x0F) << 8) | b1);
                n.effect  = b2 & 0x0F;
                n.param   = b3;
            }
        }
    }

    // Sample PCM data — follows patterns, concatenated in order
    const uint8_t* pcm = d + MOD_OFFSET_PATTERN_DATA + patternDataSize;
    out.sampleData.resize(MOD_SAMPLES);

    for (int i = 0; i < MOD_SAMPLES; ++i) {
        const uint32_t len = out.samples[i].lengthBytes;
        if (len == 0) {
            out.sampleData[i].clear();
            continue;
        }
        const size_t offset = static_cast<size_t>(pcm - d);
        if (offset + len > fileSize) {
            out.sampleData[i].clear();
            pcm += std::min<size_t>(len, fileSize - offset);
            continue;
        }
        out.sampleData[i].assign(
            reinterpret_cast<const int8_t*>(pcm),
            reinterpret_cast<const int8_t*>(pcm) + len);
        pcm += len;
    }

    return true;
}

// ── DumpModInfo ───────────────────────────────────────────────────────────────

void DumpModInfo(const ModFile& mod)
{
    auto dbg = [](const std::string& s) {
        OutputDebugStringA(s.c_str());
    };

    dbg(std::format("=== MOD: \"{}\"  magic={}  positions={}\n",
        mod.songName, mod.magic, mod.songLength));

    dbg(std::format("    Patterns in file: {}  Order: ",
        static_cast<int>(mod.patterns.size())));
    {
        std::string order;
        for (int i = 0; i < mod.songLength; ++i)
            order += std::format("{}{}", mod.orderTable[i], i + 1 < mod.songLength ? "," : "");
        dbg(order + "\n");
    }

    dbg("--- Samples ---\n");
    for (int i = 0; i < MOD_SAMPLES; ++i) {
        const SampleInfo& s = mod.samples[i];
        if (s.lengthBytes == 0) continue;
        dbg(std::format("  [{:2d}] \"{:<22}\"  len={:5}  vol={:2}  fine={:+2}  "
                        "loop off={} len={}\n",
            i + 1, s.name, s.lengthBytes, s.volume, static_cast<int>(s.finetune),
            s.repeatOffset, s.repeatLength));
    }

    dbg("--- Pattern 0 (first 8 rows) ---\n");
    if (!mod.patterns.empty()) {
        const Pattern& pat = mod.patterns[mod.orderTable[0]];
        for (int row = 0; row < 8; ++row) {
            std::string line = std::format("  row {:2d} | ", row);
            for (int ch = 0; ch < MOD_CHANNELS; ++ch) {
                const Note& n = pat[row][ch];
                int noteIdx = (n.period > 0) ? PeriodToNoteIndex(n.period, 0) : -1;
                line += std::format("{} s{:02d} e{:X}{:02X} | ",
                    NoteName(noteIdx), n.sample, n.effect, n.param);
            }
            dbg(line + "\n");
        }
    }
}

// ── DumpEffectUsage ──────────────────────────────────────────────────────────
// Prints a sorted table of every effect code used in the module.
// Rows list: effect command, total count, and for extended effects (E) the
// sub-command counts. Call after LoadMod() to see which effects a file uses
// and cross-check against the implemented set.

void DumpEffectUsage(const ModFile& mod)
{
    // [effect 0-F] -> count
    std::array<int, 16> effectCount{};
    effectCount.fill(0);
    // Extended effect sub-commands [E0x..EFx] -> count
    std::array<int, 16> extCount{};
    extCount.fill(0);

    for (const auto& pat : mod.patterns) {
        for (const auto& row : pat) {
            for (const auto& n : row) {
                if (n.effect == 0 && n.param == 0) continue; // silent slot
                effectCount[n.effect & 0xF]++;
                if (n.effect == 0xE)
                    extCount[(n.param >> 4) & 0xF]++;
            }
        }
    }

    static constexpr const char* kEffectName[16] = {
        "0xx Arpeggio",        "1xx Portamento Up",    "2xx Portamento Down",
        "3xx Porta-to-Note",   "4xx Vibrato",          "5xx Porta+VolSlide",
        "6xx Vib+VolSlide",    "7xx Tremolo",          "8xx Set Panning",
        "9xx Sample Offset",   "Axx Volume Slide",     "Bxx Jump to Order",
        "Cxx Set Volume",      "Dxx Pattern Break",    "Exx Extended",
        "Fxx Speed/BPM",
    };
    static constexpr const char* kExtName[16] = {
        "E0x Filter",          "E1x Fine Porta Up",    "E2x Fine Porta Down",
        "E3x Glissando",       "E4x Vib Waveform",     "E5x Set Finetune",
        "E6x Pattern Loop",    "E7x Trem Waveform",    "E8x Set Panning(E)",
        "E9x Retrigger",       "EAx Fine Vol Up",      "EBx Fine Vol Down",
        "ECx Note Cut",        "EDx Note Delay",       "EEx Pattern Delay",
        "EFx Funk Repeat",
    };

    auto out = [](const std::string& s) {
        printf("%s", s.c_str());
        OutputDebugStringA(s.c_str());
    };

    out(std::format("=== Effect usage: \"{}\" ===\n", mod.songName));
    for (int e = 0; e < 16; ++e) {
        if (effectCount[e] == 0) continue;
        out(std::format("  {:5d}  {}\n", effectCount[e], kEffectName[e]));
        if (e == 0xE) {
            for (int s = 0; s < 16; ++s) {
                if (extCount[s] == 0) continue;
                out(std::format("         {:5d}  {}\n", extCount[s], kExtName[s]));
            }
        }
    }
    out("\n");
}
