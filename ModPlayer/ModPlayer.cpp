// ModPlayer.cpp

#include "framework.h"
#include "ModPlayer.h"
#include "ModFile.h"
#include "ModMixer.h"
#include "LibOpenMptMixer.h"
#include "AudioOut.h"
#include <format>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>
#include <cmath>
#include <cstdlib>
#include <ctime>

#define MAX_LOADSTRING 100

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

static ModFile          g_mod;
static ModMixer         g_nativeMixer;
static LibOpenMptMixer  g_omptMixer;
static IMixer*          g_activeMixer = &g_nativeMixer;
static AudioOut         g_audio;

static std::vector<std::filesystem::path> g_modPaths;
static int  g_modIndex = 0;

// ── Browser state ─────────────────────────────────────────────────────────────

namespace fs = std::filesystem;

struct BrowserItem { fs::path path; bool isDir; bool isParent; };

static fs::path g_browserRoot;
static fs::path g_browserDir;
static std::vector<BrowserItem> g_browserItems;
static HWND g_hBrowser  = nullptr;
static HWND g_hLog      = nullptr;
static bool g_showBrowser = true;
static WNDPROC g_browserOrigProc = nullptr;

// ── Fullscreen ────────────────────────────────────────────────────────────────

static bool g_fullscreen = false;
static WINDOWPLACEMENT g_wpPrev{ sizeof(WINDOWPLACEMENT) };

// ── Amiga effects ─────────────────────────────────────────────────────────────

static int  g_effectMode  = 0;   // 0=scopes, 1=raster, 2=starfield, 3=plasma, 4=scroller, 5=spectrum
static int  g_effectTick  = 0;

struct Star { float x, y, z; };
static std::vector<Star> g_stars;

// ── Layout constants ──────────────────────────────────────────────────────────

static constexpr int kBrowserW  = 280;  // browser panel width
static constexpr int kBannerH   = 56;   // top banner + seek bar
static constexpr int kLogH      = 52;   // log panel height

// ── UTF-8 helpers ─────────────────────────────────────────────────────────────

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end()); // fallback
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ── File scanning ─────────────────────────────────────────────────────────────

static bool IsModFilename(const fs::path& p)
{
    std::string lo = p.filename().string();
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    if (lo.size() > 4 && lo.substr(lo.size() - 4) == ".mod") return true;
    if (lo.size() > 4 && lo.substr(0, 4) == "mod.")          return true;
    return false;
}

static fs::path FindModsDir()
{
    WCHAR buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path dir = fs::path(buf).parent_path();
    for (int d = 0; d <= 4; ++d) {
        std::error_code ec;
        fs::path c = dir / L"MODS";
        if (fs::exists(c, ec) && fs::is_directory(c, ec)) return c;
        dir = dir.parent_path();
    }
    return {};
}

static void ScanModFiles()
{
    try {
        for (auto& e : fs::recursive_directory_iterator(g_browserRoot)) {
            if (e.is_regular_file() && IsModFilename(e.path()))
                g_modPaths.push_back(e.path());
        }
        std::sort(g_modPaths.begin(), g_modPaths.end());
    } catch (...) {}
}

// Forward declarations
static void LogUnimplementedEffects(const ModFile&);
static void UpdateWindowTitle(HWND);
static void BrowserPopulate();
static bool PathsEqual(const fs::path&, const fs::path&);

// ── Browser population ────────────────────────────────────────────────────────

static void BrowserPopulate()
{
    if (!g_hBrowser) return;
    g_browserItems.clear();
    SendMessageW(g_hBrowser, LB_RESETCONTENT, 0, 0);

    std::error_code ec;
    if (!fs::exists(g_browserDir, ec)) g_browserDir = g_browserRoot;

    // Parent dir entry
    if (g_browserDir != g_browserRoot) {
        g_browserItems.push_back({ g_browserDir.parent_path(), true, true });
        SendMessageW(g_hBrowser, LB_ADDSTRING, 0, (LPARAM)L"↑ ..");
    }

    std::vector<BrowserItem> dirs, files;
    try {
        for (auto& e : fs::directory_iterator(g_browserDir)) {
            BrowserItem bi{ e.path(), e.is_directory(), false };
            if (bi.isDir) dirs.push_back(bi);
            else if (IsModFilename(bi.path)) files.push_back(bi);
        }
    } catch (...) {}

    auto byPath = [](const BrowserItem& a, const BrowserItem& b){
        std::wstring an = a.path.filename().wstring();
        std::wstring bn = b.path.filename().wstring();
        std::transform(an.begin(), an.end(), an.begin(), ::towlower);
        std::transform(bn.begin(), bn.end(), bn.begin(), ::towlower);
        return an < bn;
    };
    std::sort(dirs.begin(),  dirs.end(),  byPath);
    std::sort(files.begin(), files.end(), byPath);

    for (auto& d : dirs) {
        g_browserItems.push_back(d);
        auto name = L"▶ " + d.path.filename().wstring();
        SendMessageW(g_hBrowser, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
    for (auto& f : files) {
        g_browserItems.push_back(f);
        auto name = L"  " + f.path.filename().wstring();
        SendMessageW(g_hBrowser, LB_ADDSTRING, 0, (LPARAM)name.c_str());
    }

    // Highlight the currently playing file (case-insensitive)
    if (g_modIndex < (int)g_modPaths.size()) {
        for (int i = 0; i < (int)g_browserItems.size(); ++i) {
            if (!g_browserItems[i].isDir &&
                PathsEqual(g_browserItems[i].path, g_modPaths[g_modIndex])) {
                SendMessageW(g_hBrowser, LB_SETCURSEL, i, 0);
                SendMessageW(g_hBrowser, LB_SETTOPINDEX, std::max(0, i - 3), 0);
                break;
            }
        }
    }
}

// Case-insensitive path comparison (Windows filesystem is case-insensitive)
static bool PathsEqual(const fs::path& a, const fs::path& b)
{
    std::wstring wa = a.wstring(), wb = b.wstring();
    std::transform(wa.begin(), wa.end(), wa.begin(), ::towlower);
    std::transform(wb.begin(), wb.end(), wb.begin(), ::towlower);
    return wa == wb;
}

static void SyncModIndexToPath(const fs::path& p)
{
    for (int i = 0; i < (int)g_modPaths.size(); ++i)
        if (PathsEqual(g_modPaths[i], p)) { g_modIndex = i; return; }
}

// Play a file directly by path — does not require g_modPaths to contain it.
static void PlayFileByPath(HWND hWnd, const fs::path& filePath)
{
    SyncModIndexToPath(filePath);   // update g_modIndex if found (best-effort)
    const std::string spath = filePath.string();
    g_audio.SetPaused(false);

    if (g_activeMixer == &g_omptMixer) {
        ModFile newMod;
        const bool nok = LoadMod(spath, newMod);
        bool loaded = false;
        {
            std::lock_guard<std::mutex> lk(g_audio.GetMutex());
            loaded = g_omptMixer.Load(spath, kAudioSampleRate);
            if (loaded && nok) g_mod = std::move(newMod);
        }
        if (loaded) {
            if (nok) LogUnimplementedEffects(g_mod);
            UpdateWindowTitle(hWnd);
            BrowserPopulate();
        }
    } else {
        ModFile newMod;
        if (LoadMod(spath, newMod)) {
            std::lock_guard<std::mutex> lk(g_audio.GetMutex());
            g_mod = std::move(newMod);
            g_nativeMixer.Init(g_mod, kAudioSampleRate);
            LogUnimplementedEffects(g_mod);
            UpdateWindowTitle(hWnd);
            BrowserPopulate();
        }
    }
}

// ── Log helpers ───────────────────────────────────────────────────────────────

static void LogClear()
{
    if (g_hLog) SetWindowTextW(g_hLog, L"");
}

static void LogAppend(const std::string& s)
{
    if (!g_hLog) return;
    std::wstring w = Utf8ToWide(s);
    // Replace \n with \r\n for EDIT control
    std::wstring out;
    for (wchar_t c : w) {
        if (c == L'\n') out += L'\r';
        out += c;
    }
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, len, len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)out.c_str());
}

static void LogUnimplementedEffects(const ModFile& mod)
{
    static constexpr struct { uint8_t effect, subMask, subVal; const char* name; } kU[] = {
        { 0xE, 0xF0, 0x00, "E0x Filter On/Off" },
        { 0xE, 0xF0, 0xF0, "EFx Funk Repeat"   },
    };
    std::string w;
    for (auto& u : kU) {
        int n = 0;
        for (auto& pat : mod.patterns)
            for (auto& row : pat)
                for (auto& note : row)
                    if (note.effect == u.effect && (note.param & u.subMask) == u.subVal) ++n;
        if (n) w += std::format("  [!] {} \xe2\x80\x94 {} uses (not implemented)\n", u.name, n);
    }
    LogClear();
    if (w.empty()) LogAppend("All effects in this MOD are implemented.\n");
    else           LogAppend(std::format("Unimplemented in \"{}\":\n{}", mod.songName, w));
}

// ── Window title ──────────────────────────────────────────────────────────────

static void UpdateWindowTitle(HWND hWnd)
{
    const int total = (int)g_modPaths.size();
    std::wstring wbn  = Utf8ToWide(g_activeMixer->BackendName());
    std::wstring wtit = Utf8ToWide(g_activeMixer->SongTitle());
    const wchar_t* rec = g_audio.IsRecording() ? L" [REC]" : L"";
    SetWindowTextW(hWnd, std::format(L"ModPlayer [{}/{}] [{}]{} — {}",
        g_modIndex + 1, total, wbn, rec, wtit).c_str());
}

// ── Load MOD ──────────────────────────────────────────────────────────────────

static void LoadModAtIndex(HWND hWnd)
{
    if (g_modPaths.empty()) return;
    const int total = (int)g_modPaths.size();
    for (int tries = 0; tries < total; ++tries) {
        const std::string path = g_modPaths[g_modIndex].string();
        if (g_activeMixer == &g_nativeMixer) {
            ModFile newMod;
            if (LoadMod(path, newMod)) {
                std::lock_guard<std::mutex> lk(g_audio.GetMutex());
                g_mod = std::move(newMod);
                g_nativeMixer.Init(g_mod, kAudioSampleRate);
                LogUnimplementedEffects(g_mod);
                UpdateWindowTitle(hWnd);
                BrowserPopulate();
                return;
            }
        } else {
            ModFile newMod;
            const bool nok = LoadMod(path, newMod);
            bool loaded = false;
            {
                std::lock_guard<std::mutex> lk(g_audio.GetMutex());
                loaded = g_omptMixer.Load(path, kAudioSampleRate);
                if (loaded && nok) g_mod = std::move(newMod);
            }
            if (loaded) {
                if (nok) LogUnimplementedEffects(g_mod);
                else     LogAppend("(libopenmpt-only format)\n");
                UpdateWindowTitle(hWnd);
                BrowserPopulate();
                return;
            }
        }
        g_modIndex = (g_modIndex + 1) % total;
    }
}

// ── Backend switch ────────────────────────────────────────────────────────────

static void SwitchBackend(HWND hWnd)
{
    if (g_modPaths.empty()) return;
    const std::string path = g_modPaths[g_modIndex].string();
    if (g_activeMixer == &g_nativeMixer) {
        bool ok = false;
        { std::lock_guard<std::mutex> lk(g_audio.GetMutex());
          ok = g_omptMixer.Load(path, kAudioSampleRate);
          if (ok) g_activeMixer = &g_omptMixer; }
        if (ok) g_audio.SetMixer(&g_omptMixer);
        else MessageBoxA(hWnd, ("libopenmpt failed:\n" + path).c_str(), "Backend", MB_ICONWARNING|MB_OK);
    } else {
        { std::lock_guard<std::mutex> lk(g_audio.GetMutex());
          g_nativeMixer.Init(g_mod, kAudioSampleRate);
          g_activeMixer = &g_nativeMixer; }
        g_audio.SetMixer(&g_nativeMixer);
    }
    UpdateWindowTitle(hWnd);
}

// ── Fullscreen toggle ─────────────────────────────────────────────────────────

static void ToggleFullscreen(HWND hWnd)
{
    DWORD style = GetWindowLong(hWnd, GWL_STYLE);
    if (!g_fullscreen) {
        GetWindowPlacement(hWnd, &g_wpPrev);
        SetWindowLong(hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowPos(hWnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right  - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        // Hide browser in fullscreen
        if (g_hBrowser) ShowWindow(g_hBrowser, SW_HIDE);
    } else {
        SetWindowLong(hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hWnd, &g_wpPrev);
        SetWindowPos(hWnd, nullptr, 0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
        if (g_hBrowser && g_showBrowser) ShowWindow(g_hBrowser, SW_SHOW);
    }
    g_fullscreen = !g_fullscreen;
}

// ── Amiga effects ─────────────────────────────────────────────────────────────
// Music sync helpers — called every paint tick (33 ms).
// We use ch.vol (0-64, updated by the mixer per-tick) which is far more
// reactive than cv.peak (which is a slow-decay max). We also compute an
// instant RMS from the scope ring buffer for amplitude-based sync.

static float ScopeRms(const IMixer::ChannelVis& cv)
{
    double s = 0.0;
    for (float v : cv.scope) s += v * v;
    return (float)sqrt(s / IMixer::kScopeLen);
}

// Combined stereo energy [0..1] from instant scope RMS
static float MusicEnergy()
{
    float e = 0.f;
    for (int c = 0; c < IMixer::kVisChannels; ++c)
        e += ScopeRms(g_activeMixer->vis[c]);
    return std::min(1.f, e);
}

// Per-channel normalised volume from cv.vol (0-64 → 0-1)
static float ChVol(int c)
{
    return g_activeMixer->vis[c].vol / 64.f;
}

static int  g_prevRow   = -1;   // detect row changes for beat flash
static float g_beatFlash = 0.f; // decays per frame [0..1]

static void UpdateBeatFlash()
{
    int row = g_activeMixer->CurrentRow();
    if (row != g_prevRow) {
        g_beatFlash = 1.f;
        g_prevRow   = row;
    }
    g_beatFlash = std::max(0.f, g_beatFlash - 0.12f);
}

static void DrawEffects(HDC hdc, int x0, int y0, int W, int H, int tick)
{
    UpdateBeatFlash();

    const float energy = MusicEnergy();                  // instant loudness 0-1
    const float beat   = g_beatFlash;                    // 1 on new row, decays
    const float ch0    = ChVol(0), ch1 = ChVol(1);
    const float ch2    = ChVol(2), ch3 = ChVol(3);
    const float lVol   = (ch0 + ch3) * 0.5f;            // left channels avg
    const float rVol   = (ch1 + ch2) * 0.5f;            // right channels avg

    switch (g_effectMode) {

    case 1: { // ── Raster bars — beat-synced heights, volume-driven width ──
        const int nBars = 7;
        static const COLORREF kPal[] = {
            RGB(255,0,80), RGB(255,100,0), RGB(255,220,0),
            RGB(0,255,120), RGB(0,160,255), RGB(160,0,255), RGB(255,0,200)
        };
        // Beat flash: brighten all bars on new row
        float brightness = 0.6f + beat * 0.4f;

        for (int b = 0; b < nBars; ++b) {
            // Bar position: sine wave driven by tick, amplitude scaled by channel vol
            float chScale = (b < 3) ? lVol : rVol;
            float phase = tick * 0.025f + b * 0.95f;
            int cy = y0 + H/2 + (int)((sinf(phase) * 0.35f + sinf(phase*0.61f+b)*0.15f)
                                       * H * (0.4f + chScale * 0.6f));
            // Bar height grows with energy
            int bh = (int)((14 + energy * 20.f + beat * 18.f));

            COLORREF c2 = kPal[b % nBars];
            for (int g2 = 3; g2 >= 0; --g2) {
                float a = brightness * (4 - g2) / 4.f;
                int r  = (int)(GetRValue(c2) * a);
                int gv = (int)(GetGValue(c2) * a);
                int bv = (int)(GetBValue(c2) * a);
                HBRUSH br = CreateSolidBrush(RGB(r, gv, bv));
                RECT rr = { x0, cy - bh/2 - g2*5, x0+W, cy + bh/2 + g2*5 };
                rr.top    = std::clamp((long)rr.top,    (long)y0, (long)(y0+H));
                rr.bottom = std::clamp((long)rr.bottom, (long)y0, (long)(y0+H));
                FillRect(hdc, &rr, br);
                DeleteObject(br);
            }
        }
        break;
    }

    case 2: { // ── Starfield — speed and density driven by volume ──
        if (g_stars.empty()) {
            g_stars.resize(250);
            for (auto& s : g_stars) {
                s.x = ((rand()%2000)-1000) / 10.f;
                s.y = ((rand()%2000)-1000) / 10.f;
                s.z = (float)(rand()%100+1);
            }
        }
        float cx  = x0 + W/2.f;
        float cy2 = y0 + H/2.f;
        // Speed reacts to combined volume, beat adds a warp pulse
        float speed = 0.3f + energy * 2.5f + beat * 4.f;
        for (auto& s : g_stars) {
            s.z -= speed;
            if (s.z <= 0.f) {
                s.x = ((rand()%2000)-1000)/10.f;
                s.y = ((rand()%2000)-1000)/10.f;
                s.z = 100.f;
            }
            float sx = s.x / s.z * (W/2.f) + cx;
            float sy = s.y / s.z * (H/2.f) + cy2;
            if (sx < x0||sx >= x0+W||sy < y0||sy >= y0+H) continue;
            float depth = 1.f - s.z/100.f;
            int bright  = (int)(depth * 255.f);
            int sz      = std::max(1, (int)(depth * 3.f));
            // Tint stars by left/right channel volume
            int r  = std::min(255, (int)(bright * (1.f + lVol)));
            int bv = std::min(255, (int)(bright * (1.f + rVol)));
            HBRUSH br = CreateSolidBrush(RGB(r, bright, bv));
            RECT rr = {(LONG)(sx-sz),(LONG)(sy-sz),(LONG)(sx+sz),(LONG)(sy+sz)};
            FillRect(hdc, &rr, br);
            DeleteObject(br);
        }
        break;
    }

    case 3: { // ── Plasma — intensity and speed driven by music ──
        // Speed scales with energy; beat boosts it momentarily
        float t    = tick * (0.03f + energy * 0.05f) + beat * 0.8f;
        float amp  = 0.3f + energy * 0.7f;           // colour saturation
        int step   = std::max(1, H/80);
        for (int y = y0; y < y0+H; y += step) {
            float fy = (float)(y - y0 - H/2) / H;
            // Two channels independently modulate the two sine frequencies
            float v = sinf(fy * (4.f + lVol * 4.f) + t)
                    + sinf(fy * (2.5f + rVol * 3.f) - t*1.2f)
                    + sinf((fy + t*0.4f) * 5.f)
                    + sinf(sqrtf(fy*fy + 0.05f) * 8.f + t * 1.3f);
            v = (v/4.f + 1.f) * 0.5f;  // 0-1
            int r  = (int)(sinf(v*3.14159f)         * 127.f * amp + 128.f * amp);
            int g2 = (int)(sinf(v*3.14159f + 2.09f) * 127.f * amp + 128.f * amp);
            int bv = (int)(sinf(v*3.14159f + 4.19f) * 127.f * amp + 128.f * amp);
            HBRUSH br = CreateSolidBrush(RGB(
                std::clamp(r,0,255), std::clamp(g2,0,255), std::clamp(bv,0,255)));
            RECT rr = { x0, y, x0+W, y+step };
            FillRect(hdc, &rr, br);
            DeleteObject(br);
        }
        break;
    }

    case 4: { // ── Copper scroller — scroll speed and wave from music ──
        static const wchar_t kMsg[] =
            L"    * MODPLAYER * AMIGA DEMOSCENE RULES * GREETS TO ALL SCENERS *"
            L"    PRESS 1-5 FOR EFFECTS   F=FULLSCREEN   B=BACKEND   T=BROWSER    ";
        static const int kMsgLen = (int)(sizeof(kMsg)/sizeof(wchar_t)) - 1;

        // Copper gradient background — hue shifts with left channel volume
        for (int y = y0; y < y0+H; y += 2) {
            float fy = (float)(y-y0)/H;
            float phase = fy*3.14f + tick*0.025f;
            int r  = (int)(sinf(phase + lVol*2.f)*60.f + 20.f);
            int bv = (int)(sinf(phase*1.7f + rVol*2.f)*80.f + 40.f);
            HBRUSH br = CreateSolidBrush(RGB(
                std::clamp(r,0,255), (int)(beat*40.f), std::clamp(bv,0,255)));
            RECT rr = {x0,y,x0+W,y+2};
            FillRect(hdc,&rr,br); DeleteObject(br);
        }

        // Scroll speed: base + energy boost
        int scrollSpeed = (int)(2.f + energy * 6.f);
        static int scrollPx = 0;
        scrollPx += scrollSpeed;

        HFONT font = CreateFontW(32,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
            FF_DONTCARE, L"Courier New");
        HFONT old = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);

        const int charW = 20;
        int startX = -(scrollPx % (kMsgLen * charW));
        int cy3    = y0 + H/2;

        for (int i = 0; i < W/charW + 2; ++i) {
            int xi = x0 + startX + i * charW;
            if (xi < x0 - charW || xi > x0+W) continue;

            int ci = (scrollPx/charW + i) % kMsgLen;
            if (ci < 0) ci += kMsgLen;

            // Sine wave: amplitude scales with left+right volume
            float wave = lVol * 0.2f + rVol * 0.2f + 0.18f;
            float angle = (float)(xi - x0) / W * 6.28318f + tick * 0.04f;
            int yi = cy3 + (int)(sinf(angle) * H * wave) - 16;

            // Rainbow colour + beat brightens it
            float hue = (float)ci / kMsgLen + tick * 0.01f;
            int r2  = (int)(sinf(hue*6.28f)*127+128+beat*80.f);
            int g2  = (int)(sinf(hue*6.28f+2.09f)*127+128+beat*60.f);
            SetTextColor(hdc, RGB(std::clamp(r2,0,255), std::clamp(g2,0,255), 220));
            wchar_t ch[2] = {kMsg[ci], 0};
            TextOutW(hdc, xi, yi, ch, 1);
        }
        SelectObject(hdc, old);
        DeleteObject(font);
        break;
    }

    case 5: { // ── Spectrum — scope RMS drives individual bars ──
        // Use per-channel scope RMS (actual audio energy, ~0.05-0.35 typical).
        // ChVol is the mixer's volume *setting* (often 1.0 whenever a note plays)
        // and would keep bars pinned at the top — scope RMS is far more dynamic.
        const int nBars = 32;
        static float barLvl[32]{};
        static float barPk [32]{};

        float chRms[4];
        for (int c = 0; c < 4; ++c) chRms[c] = ScopeRms(g_activeMixer->vis[c]);

        // Left channels (0, 3) spread across the left portion of the spectrum;
        // right channels (1, 2) spread across the right portion.
        const float lRms = (chRms[0] + chRms[3]) * 0.5f;
        const float rRms = (chRms[1] + chRms[2]) * 0.5f;

        for (int b = 0; b < nBars; ++b) {
            float f = (float)b / nBars;
            float lW = powf(std::max(0.f, sinf((1.f-f) * 3.14159f)), 1.5f);
            float rW = powf(std::max(0.f, sinf(f       * 3.14159f)), 1.5f);
            // Scale factor ~3: typical RMS 0.1-0.3 → bars at 30-90%
            float target = std::min(1.f, (lRms*lW + rRms*rW) * 3.f + beat*0.15f);
            float attack = (target > barLvl[b]) ? 0.55f : 0.10f;
            barLvl[b] += (target - barLvl[b]) * attack;
            barPk[b]   = std::max(barPk[b] - 0.014f, barLvl[b]);
        }

        int barW = W / nBars;
        for (int b = 0; b < nBars; ++b) {
            int bx  = x0 + b * barW;
            int bh2 = (int)(barLvl[b] * (H - 4));
            int ph  = (int)(barPk[b]  * (H - 4));

            // Colour: gradient green→yellow→red, brightens on beat
            float v  = barLvl[b];
            float br = std::min(1.f, v * 2.f + beat * 0.3f);
            int r3  = std::min(255,(int)(br*255));
            int g3  = std::min(255,(int)((1.f-std::min(1.f,v*1.5f))*255));
            HBRUSH fillBr = CreateSolidBrush(RGB(r3,g3,0));
            RECT rr = {bx+1, y0+H-bh2, bx+barW-1, y0+H};
            FillRect(hdc, &rr, fillBr); DeleteObject(fillBr);

            // Peak marker (white, flashes yellow on beat)
            if (ph > 2) {
                int pm = (int)(beat * 200.f);
                HBRUSH pkBr = CreateSolidBrush(RGB(255, 255-pm, 0));
                RECT pr = {bx+1, y0+H-ph-3, bx+barW-1, y0+H-ph};
                FillRect(hdc, &pr, pkBr); DeleteObject(pkBr);
            }
        }
        break;
    }

    case 6: { // ── Channel circles ──
        // One circle per channel. Size = scope RMS. Colour hue cycles slowly and
        // shifts on each note trigger (period change). Active effect (E0x, vibrato,
        // portamento etc.) is shown as a pulsing halo ring.

        // Per-channel state
        struct CircleState {
            float hue    = 0.f;   // current colour hue [0..1]
            float hueVel = 0.f;   // hue drift per tick
            uint16_t lastPeriod = 0;
            float    flash  = 0.f; // note-trigger flash [0..1]
            float    x = 0.f, y = 0.f;  // current position (drift)
            float    vx= 0.f, vy= 0.f;  // velocity
        };
        static CircleState cs[4]{};
        static bool csInit = false;
        if (!csInit) {
            csInit = true;
            for (int c = 0; c < 4; ++c) {
                cs[c].hue = c * 0.25f;
                cs[c].hueVel = 0.002f + c * 0.0007f;
            }
        }

        // Helper: HSV→RGB (h 0-1, s 0-1, v 0-1)
        auto hsv = [](float h, float s, float v) -> COLORREF {
            h = h - floorf(h);
            int hi = (int)(h * 6.f);
            float f = h*6.f - hi;
            float p = v*(1-s), q = v*(1-s*f), t = v*(1-s*(1-f));
            float r,g,b;
            switch(hi%6){
            case 0: r=v;g=t;b=p; break; case 1: r=q;g=v;b=p; break;
            case 2: r=p;g=v;b=t; break; case 3: r=p;g=q;b=v; break;
            case 4: r=t;g=p;b=v; break; default:r=v;g=p;b=q; break;
            }
            return RGB((int)(r*255),(int)(g*255),(int)(b*255));
        };

        // Helper: filled ellipse via polygon approximation
        auto fillCircle = [&](HDC dc, int cx, int cy, int rx, int ry, COLORREF col) {
            if (rx < 1 || ry < 1) return;
            HBRUSH br = CreateSolidBrush(col);
            HBRUSH old = (HBRUSH)SelectObject(dc, br);
            HPEN pn = CreatePen(PS_NULL,0,0);
            HPEN op = (HPEN)SelectObject(dc, pn);
            Ellipse(dc, cx-rx, cy-ry, cx+rx, cy+ry);
            SelectObject(dc, old); DeleteObject(br);
            SelectObject(dc, op);  DeleteObject(pn);
        };
        auto drawRing = [&](HDC dc, int cx, int cy, int r, int thick, COLORREF col) {
            if (r < 1) return;
            HPEN pn = CreatePen(PS_SOLID, thick, col);
            HPEN op = (HPEN)SelectObject(dc, pn);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx-r, cy-r, cx+r, cy+r);
            SelectObject(dc, op); DeleteObject(pn);
            SelectObject(dc, ob);
        };

        // Quadrant centres for the 4 channels: L1 top-left, R1 top-right,
        //                                       R2 bottom-right, L2 bottom-left
        const int qw = W/2, qh = H/2;
        const int qcx[4] = { qw/2,       qw + qw/2, qw + qw/2, qw/2       };
        const int qcy[4] = { qh/2,       qh/2,      qh + qh/2, qh + qh/2  };

        for (int c = 0; c < 4; ++c) {
            const auto& cv  = g_activeMixer->vis[c];
            CircleState& s  = cs[c];

            float rms  = ScopeRms(cv);
            float vol  = cv.vol / 64.f;

            // Detect note trigger (period change) → hue jump + flash
            if (cv.period != s.lastPeriod && cv.period > 0) {
                s.lastPeriod = cv.period;
                s.flash = 1.f;
                // Jump hue based on note pitch (period → hue offset)
                s.hue += (856.f - (float)cv.period) / 856.f * 0.18f + 0.05f;
                // Velocity kick toward quadrant centre (organic bounce)
                s.vx += ((float)(qcx[c] + x0) - (x0 + qcx[c] + s.x)) * 0.03f;
                s.vy += ((float)(qcy[c] + y0) - (y0 + qcy[c] + s.y)) * 0.03f;
            }
            s.flash = std::max(0.f, s.flash - 0.07f);
            s.hue  += s.hueVel + energy * 0.0015f;

            // Slow drift within quadrant
            s.vx += (float)(rand()%100-50)*0.0004f;
            s.vy += (float)(rand()%100-50)*0.0004f;
            // Soft spring back toward quadrant centre
            s.vx -= s.x * 0.012f;
            s.vy -= s.y * 0.012f;
            s.vx *= 0.92f; s.vy *= 0.92f;
            s.x  += s.vx;  s.y  += s.vy;

            int cx = x0 + qcx[c] + (int)s.x;
            int cy = y0 + qcy[c] + (int)s.y;

            // Base radius from RMS (sound energy), min size from volume setting
            int maxR = std::min(qw, qh) * 2 / 5;
            int r    = std::max(8, (int)((rms*2.5f + vol*0.3f + beat*0.15f) * maxR));
            r = std::min(r, maxR);

            // Outer glow (halo) — brighter on note trigger flash
            float glow = 0.25f + s.flash * 0.5f + beat * 0.15f;
            for (int g2 = 5; g2 >= 1; --g2) {
                float a = glow * g2 / 5.f;
                COLORREF gc = hsv(s.hue, 0.7f, a * 0.6f);
                fillCircle(hdc, cx, cy, r + g2*7, r + g2*7, gc);
            }

            // Main circle — full saturation, value from volume
            float bright = 0.5f + vol * 0.5f + s.flash * 0.3f;
            COLORREF main_col = hsv(s.hue, 0.9f, std::min(1.f, bright));
            fillCircle(hdc, cx, cy, r, r, main_col);

            // Inner highlight (lighter centre)
            COLORREF hi_col = hsv(s.hue + 0.05f, 0.4f, std::min(1.f, bright + 0.3f));
            fillCircle(hdc, cx, cy, r/3, r/3, hi_col);

            // Trigger ring — expands and fades on note hit
            if (s.flash > 0.05f) {
                int ringR = r + (int)((1.f - s.flash) * maxR * 0.6f);
                COLORREF rc = hsv(s.hue + 0.1f, 0.6f, s.flash * 0.9f);
                drawRing(hdc, cx, cy, ringR, std::max(1,(int)(s.flash*4)), rc);
            }

            // Channel label
            static const wchar_t* kLabel[4] = {L"CH1",L"CH2",L"CH3",L"CH4"};
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, hsv(s.hue, 0.3f, 0.9f));
            RECT lr={cx-20, cy+r+4, cx+20, cy+r+20};
            DrawTextW(hdc, kLabel[c], -1, &lr, DT_CENTER|DT_SINGLELINE);
        }
        break;
    }

    default: break;
    }
}

// ── Win32 boilerplate ─────────────────────────────────────────────────────────

ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR, _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    srand((unsigned)time(nullptr));

    g_browserRoot = FindModsDir();
    if (g_browserRoot.empty()) {
        MessageBoxA(nullptr, "No MODS directory found", "ModPlayer", MB_ICONERROR);
        return 1;
    }
    g_browserDir = g_browserRoot;
    ScanModFiles();
    if (g_modPaths.empty()) {
        MessageBoxA(nullptr, "No MOD files found", "ModPlayer", MB_ICONERROR);
        return 1;
    }

    // Start with first file via libopenmpt
    {
        const int total = (int)g_modPaths.size();
        bool found = false;
        for (int i = 0; i < total; ++i) {
            const std::string path = g_modPaths[i].string();
            if (g_omptMixer.Load(path, kAudioSampleRate)) {
                g_modIndex = i;
                LoadMod(path, g_mod);
                found = true;
                break;
            }
            ++g_modIndex;
        }
        if (!found) {
            MessageBoxA(nullptr, "No playable MOD files found", "ModPlayer", MB_ICONERROR);
            return 1;
        }
    }
    g_activeMixer = &g_omptMixer;
    if (!g_audio.Open(&g_omptMixer)) {
        MessageBoxA(nullptr, "waveOutOpen failed", "AudioOut error", MB_ICONERROR);
        return 1;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_MODPLAYER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow)) return FALSE;

    HACCEL hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_MODPLAYER));
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == g_hBrowser) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else if (!TranslateAccelerator(msg.hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    g_audio.Close();
    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex{};
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MODPLAYER));
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName  = MAKEINTRESOURCEW(IDC_MODPLAYER);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

// ── Layout helper ─────────────────────────────────────────────────────────────

static void LayoutChildren(HWND hWnd)
{
    RECT rc; GetClientRect(hWnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int bw = (g_showBrowser && !g_fullscreen) ? kBrowserW : 0;
    const int vizW = W - bw;

    if (g_hLog)
        SetWindowPos(g_hLog, nullptr, 0, H - kLogH, vizW, kLogH, SWP_NOZORDER|SWP_NOACTIVATE);
    if (g_hBrowser)
        SetWindowPos(g_hBrowser, nullptr, vizW, 0, bw, H, SWP_NOZORDER|SWP_NOACTIVATE);
}

// ListBox subclass: keep arrow/page/home/end for list navigation;
// forward everything else to the parent so hotkeys (F, T, 1-5, Space…) still work.
static LRESULT CALLBACK BrowserSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN) {
        switch (wParam) {
        case VK_RETURN:
            // Treat as double-click (play / navigate)
            SendMessageW(GetParent(hWnd), WM_COMMAND,
                MAKEWPARAM(1001, LBN_DBLCLK), (LPARAM)hWnd);
            return 0;

        // Navigation keys the ListBox should handle itself
        case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT:
        case VK_HOME:  case VK_END:
            break;

        default:
            // Forward everything else to the main window
            SendMessageW(GetParent(hWnd), WM_KEYDOWN, wParam, lParam);
            return 0;
        }
    }
    return CallWindowProc(g_browserOrigProc, hWnd, msg, wParam, lParam);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;
    HWND hWnd = CreateWindowW(szWindowClass, L"ModPlayer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 860, 560, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return FALSE;

    RECT rc; GetClientRect(hWnd, &rc);

    // Log panel (Unicode EDIT)
    g_hLog = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        0, rc.bottom - kLogH, rc.right - kBrowserW, kLogH,
        hWnd, nullptr, hInstance, nullptr);
    SendMessageW(g_hLog, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);

    // File browser (ListBox)
    g_hBrowser = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        rc.right - kBrowserW, 0, kBrowserW, rc.bottom,
        hWnd, (HMENU)1001, hInstance, nullptr);
    // Dark background via subclassing isn't worth the complexity; set via WM_CTLCOLORLISTBOX
    HFONT lbFont = CreateFontW(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,FF_DONTCARE,L"Consolas");
    SendMessageW(g_hBrowser, WM_SETFONT, (WPARAM)lbFont, TRUE);

    // Subclass the ListBox so Enter triggers play/navigate
    g_browserOrigProc = (WNDPROC)SetWindowLongPtrW(g_hBrowser, GWLP_WNDPROC,
        (LONG_PTR)BrowserSubclassProc);

    BrowserPopulate();

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    UpdateWindowTitle(hWnd);
    SetTimer(hWnd, 1, 33, nullptr);
    return TRUE;
}

// ── Seek bar hit-test and draw ────────────────────────────────────────────────

static RECT SeekBarRect(int W)
{
    // Second row of banner (y=38..56)
    return RECT{ 4, 38, W - (g_showBrowser && !g_fullscreen ? kBrowserW : 0) - 4, 54 };
}

static void DrawSeekBar(HDC hdc, int W)
{
    RECT sr = SeekBarRect(W);
    HBRUSH bg = CreateSolidBrush(RGB(30, 30, 45));
    FillRect(hdc, &sr, bg); DeleteObject(bg);

    double dur = g_activeMixer->GetDurationSeconds();
    double pos = g_activeMixer->GetPositionSeconds();
    if (dur > 0.0) {
        int barW = sr.right - sr.left;
        int filled = (int)(pos / dur * barW);
        HBRUSH fill = CreateSolidBrush(
            (g_activeMixer == &g_omptMixer) ? RGB(255,200,60) : RGB(100,200,100));
        RECT fr = { sr.left, sr.top, sr.left + filled, sr.bottom };
        FillRect(hdc, &fr, fill); DeleteObject(fill);

        // Time labels
        auto fmt_time = [](double t) -> std::string {
            int m = (int)t / 60, s = (int)t % 60;
            return std::format("{:d}:{:02d}", m, s);
        };
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(200,200,200));
        std::string pos_s = fmt_time(pos) + " / " + fmt_time(dur);
        RECT tr = sr; tr.left += 4;
        DrawTextA(hdc, pos_s.c_str(), -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(100,100,130));
        DrawTextA(hdc, "-- no seek info --", -1, &sr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
}

// ── WndProc ───────────────────────────────────────────────────────────────────

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {

    case WM_COMMAND: {
        const int ctrl = LOWORD(wParam);
        if (ctrl == IDM_ABOUT) {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
        } else if (ctrl == IDM_EXIT) {
            DestroyWindow(hWnd);
        } else if (ctrl == 1001 &&
                   (HIWORD(wParam) == LBN_DBLCLK || HIWORD(wParam) == LBN_SELCHANGE)) {
            int sel = (int)SendMessageW(g_hBrowser, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_browserItems.size()) {
                auto& item = g_browserItems[sel];
                if (item.isDir) {
                    // Navigate into folder on any selection (single-click or double-click)
                    g_browserDir = item.path;
                    BrowserPopulate();
                } else {
                    // Play file immediately on single-click
                    PlayFileByPath(hWnd, item.path);
                }
            }
        } else {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    }

    case WM_KEYDOWN: {
        const int total = (int)g_modPaths.size();
        switch (wParam) {
        case VK_SPACE:
        case VK_RIGHT:
            g_modIndex = (g_modIndex + 1) % total;
            g_audio.SetPaused(false);
            LoadModAtIndex(hWnd);
            break;
        case VK_LEFT:
        case VK_BACK:
            g_modIndex = (g_modIndex - 1 + total) % total;
            g_audio.SetPaused(false);
            LoadModAtIndex(hWnd);
            break;
        case 'P':
            g_audio.SetPaused(!g_audio.IsPaused());
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case 'B':
            SwitchBackend(hWnd);
            break;
        case 'R':
            if (g_audio.IsRecording()) {
                g_audio.StopRecording();
            } else {
                g_audio.SetPaused(false);
                LoadModAtIndex(hWnd);
                g_audio.StartRecording("tests\\bin\\player_recording.wav");
            }
            UpdateWindowTitle(hWnd);
            break;
        case 'T':  // Toggle browser panel
            g_showBrowser = !g_showBrowser;
            if (g_hBrowser) ShowWindow(g_hBrowser, g_showBrowser ? SW_SHOW : SW_HIDE);
            LayoutChildren(hWnd);
            InvalidateRect(hWnd, nullptr, TRUE);
            break;
        case VK_F11:
        case 'F':
            ToggleFullscreen(hWnd);
            break;
        case '0': g_effectMode = 0; InvalidateRect(hWnd,nullptr,FALSE); break;
        case '1': g_effectMode = 1; InvalidateRect(hWnd,nullptr,FALSE); break;
        case '2': g_effectMode = 2; g_stars.clear(); InvalidateRect(hWnd,nullptr,FALSE); break;
        case '3': g_effectMode = 3; InvalidateRect(hWnd,nullptr,FALSE); break;
        case '4': g_effectMode = 4; InvalidateRect(hWnd,nullptr,FALSE); break;
        case '5': g_effectMode = 5; InvalidateRect(hWnd,nullptr,FALSE); break;
        case '6': g_effectMode = 6; InvalidateRect(hWnd,nullptr,FALSE); break;
        }
        break;
    }

    // Seek bar click
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        RECT sr = SeekBarRect(rc.right);
        if (mx >= sr.left && mx <= sr.right && my >= sr.top && my <= sr.bottom) {
            double dur = g_activeMixer->GetDurationSeconds();
            if (dur > 0.0) {
                double frac = (double)(mx - sr.left) / (sr.right - sr.left);
                std::lock_guard<std::mutex> lk(g_audio.GetMutex());
                g_activeMixer->SeekSeconds(frac * dur);
            }
        }
        break;
    }

    case WM_SIZE:
        LayoutChildren(hWnd);
        break;

    case WM_TIMER:
        ++g_effectTick;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    // Colour the browser dark
    case WM_CTLCOLORLISTBOX:
        if ((HWND)lParam == g_hBrowser) {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(200,220,255));
            SetBkColor(dc, RGB(16,18,28));
            static HBRUSH kBrowserBg = CreateSolidBrush(RGB(16,18,28));
            return (LRESULT)kBrowserBg;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcReal = BeginPaint(hWnd, &ps);

        RECT rc; GetClientRect(hWnd, &rc);
        const int W = rc.right, H = rc.bottom;
        const int bw = (g_showBrowser && !g_fullscreen) ? kBrowserW : 0;
        const int vizW = W - bw;

        HDC hdc = CreateCompatibleDC(hdcReal);
        HBITMAP bmp    = CreateCompatibleBitmap(hdcReal, vizW, H);
        HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);

        // Background
        HBRUSH bgBr = CreateSolidBrush(RGB(14, 15, 22));
        RECT vrc = {0,0,vizW,H};
        FillRect(hdc, &vrc, bgBr);
        DeleteObject(bgBr);

        // ── Banner row 1 (y 0-38) ──
        {
            HBRUSH banBr = CreateSolidBrush(RGB(20,22,35));
            RECT br={0,0,vizW,kBannerH};
            FillRect(hdc,&br,banBr); DeleteObject(banBr);

            const bool isOmpt  = (g_activeMixer == &g_omptMixer);
            const bool paused  = g_audio.IsPaused();
            COLORREF col = isOmpt ? RGB(255,200,60) : RGB(100,200,100);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, col);

            char row1[128];
            sprintf_s(row1, "BACKEND:%s  [%d/%d]%s%s",
                g_activeMixer->BackendName(),
                g_modIndex+1, (int)g_modPaths.size(),
                paused  ? "  [PAUSED]"  : "",
                g_audio.IsRecording() ? "  [REC]" : "");
            RECT r1={4,2,vizW-4,20};
            DrawTextA(hdc,row1,-1,&r1,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

            // Row 2: position + hints
            int ord = g_activeMixer->CurrentOrder();
            int row = g_activeMixer->CurrentRow();
            int ords= g_activeMixer->SongOrders();
            SetTextColor(hdc, RGB(140,140,170));
            char row2[160];
            sprintf_s(row2,"ORD %d/%d ROW %2d | SPC/\x1a\x1b=prev/next  P=pause  B=backend  F=full  T=browser  1-6=effects",
                ord+1,ords,row);
            RECT r2={4,20,vizW-4,38};
            DrawTextA(hdc,row2,-1,&r2,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

            // Seek bar
            DrawSeekBar(hdc, vizW);
        }

        // ── Scope / effect area ──
        const int scopeTop = kBannerH;
        const int scopeBot = H - kLogH;
        const int scopeH   = scopeBot - scopeTop;

        if (g_effectMode != 0 && scopeH > 0) {
            // Draw chosen effect
            HBRUSH efBg = CreateSolidBrush(RGB(8,9,14));
            RECT er = {0, scopeTop, vizW, scopeBot};
            FillRect(hdc, &er, efBg); DeleteObject(efBg);
            DrawEffects(hdc, 0, scopeTop, vizW, scopeH, g_effectTick);
        } else {
            // Classic 4-channel oscilloscopes
            static const COLORREF kChCol[4] = {
                RGB(64,148,255), RGB(64,220,128),
                RGB(64,220,128), RGB(64,148,255)
            };
            static const char* kChSide[4] = {"L","R","R","L"};
            const int panelW = vizW / IMixer::kVisChannels;
            const int labelH = 20;
            const int peakH  = 12;
            const int waveH  = scopeH - labelH - peakH - 4;

            for (int c = 0; c < IMixer::kVisChannels; ++c) {
                const auto& cv = g_activeMixer->vis[c];
                const COLORREF col = kChCol[c];
                const int px = c * panelW;
                const int py = scopeTop;

                HPEN divPen = CreatePen(PS_SOLID,1,RGB(35,38,52));
                HPEN oldPen = (HPEN)SelectObject(hdc,divPen);
                MoveToEx(hdc,px+panelW-1,py,nullptr);
                LineTo(hdc,px+panelW-1,scopeBot);
                SelectObject(hdc,oldPen); DeleteObject(divPen);

                // Label
                char lbl[40];
                if (cv.active && cv.period > 0)
                    sprintf_s(lbl,"CH%d %s %s v%d",c+1,kChSide[c],NoteName(PeriodToNoteIndex(cv.period,0)),(int)cv.vol);
                else if (cv.active)
                    sprintf_s(lbl,"CH%d %s v%d",c+1,kChSide[c],(int)cv.vol);
                else
                    sprintf_s(lbl,"CH%d %s",c+1,kChSide[c]);
                SetBkMode(hdc,TRANSPARENT);
                SetTextColor(hdc,col);
                RECT lr={px+3,py+2,px+panelW-2,py+labelH};
                DrawTextA(hdc,lbl,-1,&lr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOCLIP);

                // Scope bg
                HBRUSH sbg = CreateSolidBrush(RGB(8,9,14));
                RECT sr2={px,py+labelH,px+panelW-1,py+labelH+waveH};
                FillRect(hdc,&sr2,sbg); DeleteObject(sbg);

                // Centre line
                HPEN gp=CreatePen(PS_SOLID,1,RGB(28,32,44));
                oldPen=(HPEN)SelectObject(hdc,gp);
                int cy=py+labelH+waveH/2;
                MoveToEx(hdc,px,cy,nullptr); LineTo(hdc,px+panelW-1,cy);
                SelectObject(hdc,oldPen); DeleteObject(gp);

                // Waveform
                const int pts = panelW - 2;
                if (pts > 0 && (cv.active || cv.peak > 0.001f)) {
                    HPEN wp=CreatePen(PS_SOLID,1,col);
                    oldPen=(HPEN)SelectObject(hdc,wp);
                    std::vector<POINT> poly(pts);
                    for (int x=0;x<pts;++x) {
                        int idx=(cv.scopePos+x*IMixer::kScopeLen/pts)&(IMixer::kScopeLen-1);
                        int sy=cy-(int)(cv.scope[idx]*(waveH/2-2));
                        sy=std::clamp(sy,py+labelH+1,py+labelH+waveH-2);
                        poly[x]={px+1+x,sy};
                    }
                    Polyline(hdc,poly.data(),pts);
                    SelectObject(hdc,oldPen); DeleteObject(wp);
                }

                // Peak bar
                int peakY=py+labelH+waveH+2;
                HBRUSH pbg=CreateSolidBrush(RGB(18,20,30));
                RECT pb={px+2,peakY,px+panelW-2,peakY+peakH-2};
                FillRect(hdc,&pb,pbg); DeleteObject(pbg);
                int px2=(int)(cv.peak*(panelW-4));
                if (px2>0){HBRUSH pf=CreateSolidBrush(col);RECT pf2={px+2,peakY,px+2+px2,peakY+peakH-2};FillRect(hdc,&pf2,pf);DeleteObject(pf);}
            }
        }

        BitBlt(hdcReal,0,0,vizW,H,hdc,0,0,SRCCOPY);
        SelectObject(hdc,oldBmp); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hWnd,&ps);
        break;
    }

    case WM_DESTROY:
        KillTimer(hWnd,1);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd,message,wParam,lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG: return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam)==IDOK||LOWORD(wParam)==IDCANCEL)
            EndDialog(hDlg,LOWORD(wParam));
        break;
    }
    return FALSE;
}
