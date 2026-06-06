#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// ── Layout constants ─────────────────────────────────────────────────────────
inline constexpr int MOD_CHANNELS     = 4;
inline constexpr int MOD_SAMPLES      = 31;
inline constexpr int MOD_PATTERN_ROWS = 64;

// ── Raw file offsets ──────────────────────────────────────────────────────────
inline constexpr int MOD_OFFSET_SONG_NAME    = 0;
inline constexpr int MOD_OFFSET_SAMPLE_HDRS  = 20;   // 31 × 30 bytes = 930
inline constexpr int MOD_OFFSET_SONG_LEN     = 950;
inline constexpr int MOD_OFFSET_RESTART_POS  = 951;
inline constexpr int MOD_OFFSET_ORDER_TABLE  = 952;  // 128 bytes
inline constexpr int MOD_OFFSET_MAGIC        = 1080; // 4 bytes ("M.K." etc.)
inline constexpr int MOD_OFFSET_PATTERN_DATA = 1084;

// ── Data structures ───────────────────────────────────────────────────────────

struct SampleInfo {
    char     name[23]{};       // null-terminated (22 usable chars)
    uint32_t lengthBytes{};    // PCM bytes (file stores words → ×2)
    int8_t   finetune{};       // signed –8..+7
    uint8_t  volume{};         // 0..64
    uint32_t repeatOffset{};   // bytes from start of this sample's PCM
    uint32_t repeatLength{};   // bytes; ≤2 means no loop
};

struct Note {
    uint8_t  sample{};  // 1–31; 0 = no sample trigger
    uint16_t period{};  // Amiga period; 0 = no note
    uint8_t  effect{};  // 0x0–0xF
    uint8_t  param{};   // effect parameter byte
};

// A pattern: 64 rows, each row has MOD_CHANNELS notes.
using Pattern = std::array<std::array<Note, MOD_CHANNELS>, MOD_PATTERN_ROWS>;

struct ModFile {
    char    songName[21]{};      // null-terminated
    char    magic[5]{};          // e.g. "M.K."
    uint8_t songLength{};        // number of entries used in orderTable
    uint8_t restartPos{};        // order to loop back to when song ends (0x7F = no loop)
    uint8_t orderTable[128]{};   // pattern play order

    std::vector<Pattern>              patterns;
    std::vector<SampleInfo>           samples;    // [0] = instrument 1
    std::vector<std::vector<int8_t>>  sampleData; // [0] = instrument 1 PCM
};

// ── Period → frequency ────────────────────────────────────────────────────────
// PAL Amiga clock: 7,093,789.2 Hz.  frequency = clock / (period × 2)
inline constexpr double MOD_PAL_CLOCK = 7093789.2;

inline double PeriodToHz(uint16_t period)
{
    if (period == 0) return 0.0;
    return MOD_PAL_CLOCK / (static_cast<double>(period) * 2.0);
}

// ── Standard ProTracker period table ─────────────────────────────────────────
// Indexed [finetune_raw][note], finetune_raw 0–15, note 0–35 (C-1 .. B-3).
// Raw finetune bits:  0 = +0, 1 = +1 … 7 = +7, 8 = -8, 9 = -7 … 15 = -1.
inline constexpr uint16_t kPeriodTable[16][36] = {
    // finetune  0
    { 856,808,762,720,678,640,604,570,538,508,480,453,
      428,404,381,360,339,320,302,285,269,254,240,226,
      214,202,190,180,170,160,151,143,135,127,120,113 },
    // finetune +1
    { 850,802,757,715,674,637,601,567,535,505,477,450,
      425,401,379,357,337,318,300,284,268,253,239,225,
      212,200,189,178,168,159,150,142,134,126,119,112 },
    // finetune +2
    { 844,796,752,709,670,632,597,563,532,502,474,447,
      422,398,376,355,335,316,298,282,266,251,237,224,
      211,199,188,177,167,158,149,141,133,125,118,112 },
    // finetune +3
    { 838,791,746,704,665,628,592,559,528,498,470,444,
      419,395,373,352,332,314,296,280,264,249,235,222,
      209,198,187,176,166,157,148,140,132,125,118,111 },
    // finetune +4
    { 832,785,741,699,660,623,588,555,524,495,467,441,
      416,392,370,350,330,312,294,278,262,247,233,220,
      208,196,185,175,165,156,147,139,131,124,117,110 },
    // finetune +5
    { 826,779,736,694,655,619,584,551,520,491,463,437,
      413,390,368,347,328,309,292,276,260,245,232,219,
      206,195,184,174,164,155,146,138,130,123,116,110 },
    // finetune +6
    { 820,774,730,689,651,614,580,547,516,487,460,434,
      410,387,365,345,325,307,290,274,258,244,230,217,
      205,193,183,172,163,154,145,137,129,122,115,109 },
    // finetune +7
    { 814,768,725,684,646,610,575,543,513,484,457,431,
      407,384,363,342,323,305,288,272,256,242,228,216,
      204,192,181,171,161,152,144,136,128,121,114,108 },
    // finetune -8
    { 907,856,808,762,720,678,640,604,570,538,508,480,
      453,428,404,381,360,339,320,302,285,269,254,240,
      226,214,202,190,180,170,160,151,143,135,127,120 },
    // finetune -7
    { 900,850,802,757,715,675,636,601,567,535,505,477,
      450,425,401,379,357,337,318,300,284,268,253,238,
      225,212,200,189,179,169,159,150,142,134,126,119 },
    // finetune -6
    { 894,844,796,752,709,670,632,597,563,532,502,474,
      447,422,398,376,355,335,316,298,282,266,251,237,
      223,211,199,188,177,167,158,149,141,133,125,118 },
    // finetune -5
    { 887,838,791,746,704,665,628,592,559,528,498,470,
      444,419,395,373,352,332,314,296,280,264,249,235,
      222,209,198,187,176,166,157,148,140,132,125,118 },
    // finetune -4
    { 881,832,785,741,699,660,623,588,555,524,494,467,
      441,416,392,370,350,330,312,294,278,262,247,233,
      220,208,196,185,175,165,156,147,139,131,124,117 },
    // finetune -3
    { 875,826,779,736,694,655,619,584,551,520,491,463,
      437,413,390,368,347,328,309,292,276,260,245,232,
      219,206,195,184,174,164,155,146,138,130,123,116 },
    // finetune -2
    { 868,820,774,730,689,651,614,580,547,516,487,460,
      434,410,387,365,345,325,307,290,274,258,244,230,
      217,205,193,183,172,163,154,145,137,129,122,115 },
    // finetune -1
    { 862,814,768,725,684,646,610,575,543,513,484,457,
      431,407,384,363,342,323,305,288,272,256,242,228,
      216,203,192,181,171,161,152,144,136,128,121,114 },
};

// Returns the note index (0–35) for a period, or –1 if not found.
// Uses finetune_raw 0–15 as stored in the sample header.
inline int PeriodToNoteIndex(uint16_t period, int finetune_raw)
{
    for (int n = 0; n < 36; ++n)
        if (kPeriodTable[finetune_raw & 0xF][n] == period)
            return n;
    return -1;
}

inline const char* NoteName(int note_index)
{
    static constexpr const char* names[] = {
        "C-1","C#1","D-1","D#1","E-1","F-1","F#1","G-1","G#1","A-1","A#1","B-1",
        "C-2","C#2","D-2","D#2","E-2","F-2","F#2","G-2","G#2","A-2","A#2","B-2",
        "C-3","C#3","D-3","D#3","E-3","F-3","F#3","G-3","G#3","A-3","A#3","B-3",
    };
    if (note_index < 0 || note_index >= 36) return "---";
    return names[note_index];
}

// ── Parser ────────────────────────────────────────────────────────────────────
bool    LoadMod(const std::string& path, ModFile& out);
void    DumpModInfo(const ModFile& mod);     // VS Output via OutputDebugStringA
void    DumpEffectUsage(const ModFile& mod); // prints effect frequency table to stdout + VS Output
