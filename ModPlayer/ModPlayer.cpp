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

    // Highlight the currently playing file
    for (int i = 0; i < (int)g_browserItems.size(); ++i) {
        if (!g_browserItems[i].isDir &&
            g_modIndex < (int)g_modPaths.size() &&
            g_browserItems[i].path == g_modPaths[g_modIndex]) {
            SendMessageW(g_hBrowser, LB_SETCURSEL, i, 0);
            break;
        }
    }
}

// Sync g_modIndex to a file path from the browser
static void SyncModIndexToPath(const fs::path& p)
{
    for (int i = 0; i < (int)g_modPaths.size(); ++i) {
        if (g_modPaths[i] == p) { g_modIndex = i; return; }
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

static void DrawEffects(HDC hdc, int x0, int y0, int W, int H, int tick)
{
    // Channel peak values [0-1]
    float peaks[4]{};
    for (int c = 0; c < 4; ++c) peaks[c] = g_activeMixer->vis[c].peak;

    switch (g_effectMode) {

    case 1: { // ── Raster bars ──
        const int nBars = 7;
        static const COLORREF kPal[] = {
            RGB(255,0,80), RGB(255,100,0), RGB(255,220,0),
            RGB(0,255,120), RGB(0,160,255), RGB(160,0,255), RGB(255,0,200)
        };
        for (int b = 0; b < nBars; ++b) {
            float phase = (tick * 0.03f) + b * 0.9f;
            int cy = y0 + H / 2 + (int)(sinf(phase) * H * 0.35f
                          + sinf(phase * 0.7f + b) * H * 0.15f);
            int bh = 16 + (int)(sinf(phase * 1.3f) * 8.f);
            // Glow: draw 3 rects with decreasing alpha simulation
            for (int g = 3; g >= 0; --g) {
                COLORREF c2 = kPal[b % nBars];
                int r = (GetRValue(c2) * (4-g)) / 4;
                int gv= (GetGValue(c2) * (4-g)) / 4;
                int bv= (GetBValue(c2) * (4-g)) / 4;
                HBRUSH br = CreateSolidBrush(RGB(r,gv,bv));
                RECT rr = { x0, cy - bh/2 - g*4, x0+W, cy + bh/2 + g*4 };
                rr.top    = std::clamp((long)rr.top, (long)y0, (long)(y0+H));
                rr.bottom = std::clamp((long)rr.bottom, (long)y0, (long)(y0+H));
                FillRect(hdc, &rr, br);
                DeleteObject(br);
            }
        }
        break;
    }

    case 2: { // ── Starfield ──
        // Init stars
        if (g_stars.empty()) {
            g_stars.resize(200);
            for (auto& s : g_stars) {
                s.x = ((rand()%1000)-500) / 10.f;
                s.y = ((rand()%1000)-500) / 10.f;
                s.z = (float)(rand()%100+1);
            }
        }
        float cx = x0 + W/2.f, cy2 = y0 + H/2.f;
        float speed = 0.5f + peaks[0] + peaks[1];
        for (auto& s : g_stars) {
            s.z -= speed;
            if (s.z <= 0.f) { s.x = ((rand()%1000)-500)/10.f; s.y = ((rand()%1000)-500)/10.f; s.z = 100.f; }
            float sx = s.x / s.z * (W/2.f) + cx;
            float sy = s.y / s.z * (H/2.f) + cy2;
            if (sx < x0||sx>=x0+W||sy<y0||sy>=y0+H) continue;
            int bright = (int)((1.f - s.z/100.f) * 255.f);
            int sz = std::max(1, (int)((1.f - s.z/100.f) * 3.f));
            HBRUSH br = CreateSolidBrush(RGB(bright,bright,bright));
            RECT rr = {(LONG)(sx-sz),(LONG)(sy-sz),(LONG)(sx+sz),(LONG)(sy+sz)};
            FillRect(hdc, &rr, br);
            DeleteObject(br);
        }
        break;
    }

    case 3: { // ── Plasma (scanline-based) ──
        // Draw horizontal lines with colour from sine mix
        float t = tick * 0.05f;
        float vol = (peaks[0]+peaks[1]+peaks[2]+peaks[3]) * 0.25f;
        int step = std::max(1, H/80);
        for (int y = y0; y < y0+H; y += step) {
            float fy = (float)(y - y0 - H/2) / H;
            float v  = sinf(fy * 6.f + t) + sinf(fy * 3.7f - t*1.3f)
                     + sinf((fy + t*0.5f) * 5.f)
                     + sinf(sqrtf(fy*fy + 0.1f) * 8.f + t);
            v = (v + 4.f) / 8.f; // 0-1
            int r = (int)(sinf(v * 3.14159f        ) * 127.f + 128.f);
            int g = (int)(sinf(v * 3.14159f + 2.09f) * 127.f + 128.f);
            int bv= (int)(sinf(v * 3.14159f + 4.19f) * 127.f + 128.f);
            // Amplify with music volume
            r = std::min(255, (int)(r * (0.5f + vol)));
            g = std::min(255, (int)(g * (0.5f + vol)));
            bv= std::min(255, (int)(bv* (0.5f + vol)));
            HBRUSH br = CreateSolidBrush(RGB(r,g,bv));
            RECT rr = { x0, y, x0+W, y+step };
            FillRect(hdc, &rr, br);
            DeleteObject(br);
        }
        break;
    }

    case 4: { // ── Copper scroller ──
        static const wchar_t kMsg[] =
            L"    * MODPLAYER * AMIGA DEMOSCENE RULES * GREETS TO ALL SCENERS *"
            L"    PRESS 1-5 FOR EFFECTS, F FOR FULLSCREEN, B TO TOGGLE BACKEND *    ";
        static const int kMsgLen = (int)(sizeof(kMsg)/sizeof(wchar_t)) - 1;

        // Copper bars background
        for (int y = y0; y < y0+H; y += 2) {
            float fy = (float)(y-y0)/H;
            int r = (int)(sinf(fy*3.14f+tick*0.03f)*60.f+20.f);
            int bv= (int)(sinf(fy*3.14f*2.f-tick*0.05f)*80.f+40.f);
            HBRUSH br = CreateSolidBrush(RGB(std::clamp(r,0,255),0,std::clamp(bv,0,255)));
            RECT rr={x0,y,x0+W,y+2};
            FillRect(hdc,&rr,br);
            DeleteObject(br);
        }

        // Sine-wave scrolling text
        HFONT font = CreateFontW(32,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
            FF_DONTCARE,L"Courier New");
        HFONT old = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);

        int charW = 18;
        int scrollX = -(tick * 3) % (kMsgLen * charW);
        int cy3 = y0 + H/2;

        for (int i = 0; i < W/charW + 2; ++i) {
            int xi = x0 + scrollX + i * charW;
            if (xi < x0 - charW || xi > x0+W) continue;
            int ci = ((tick * 3 / charW) + i) % kMsgLen;
            if (ci < 0) ci += kMsgLen;
            float angle = (float)(xi - x0) / W * 3.14159f * 2.f + tick*0.04f;
            int yi = cy3 + (int)(sinf(angle)*H*0.28f) - 16;

            // Colour from position
            int r2=(int)(sinf(i*0.3f+tick*0.05f)*127+128);
            int g2=(int)(sinf(i*0.3f+1.f+tick*0.05f)*127+128);
            SetTextColor(hdc, RGB(r2,g2,220));
            wchar_t ch[2]={kMsg[ci],0};
            TextOutW(hdc, xi, yi, ch, 1);
        }
        SelectObject(hdc, old);
        DeleteObject(font);
        break;
    }

    case 5: { // ── Spectrum bars ──
        // Use channel peaks and position to drive animated EQ bars
        const int nBars = 32;
        static float barH[32]{};
        static float barV[32]{};
        // Drive bar levels from channel peaks + position oscillation
        for (int b = 0; b < nBars; ++b) {
            float f = (float)b / nBars;
            float target = 0.f;
            for (int c = 0; c < 4; ++c)
                target += peaks[c] * powf(sinf(f*3.14f*(c+1) + tick*0.02f*(c+1)), 2.f);
            target = std::min(1.f, target * 1.5f);
            barV[b] += (target - barV[b]) * 0.35f;
            barH[b] = std::max(barH[b] - 0.008f, barV[b]);
        }
        int barW = W / nBars;
        for (int b = 0; b < nBars; ++b) {
            int bx = x0 + b * barW;
            int bh2 = (int)(barV[b] * (H - 4));
            int ph  = (int)(barH[b] * (H - 4));
            // Bar colour — gradient green→yellow→red
            float v = barV[b];
            int r3 = std::min(255,(int)(v*2*255));
            int g3 = std::min(255,(int)((1.f-v)*2*255));
            HBRUSH br = CreateSolidBrush(RGB(r3,g3,0));
            RECT rr = {bx+1, y0+H-bh2, bx+barW-1, y0+H};
            FillRect(hdc,&rr,br);
            DeleteObject(br);
            // Peak marker
            HBRUSH pb = CreateSolidBrush(RGB(255,255,255));
            RECT pr = {bx+1,y0+H-ph-3,bx+barW-1,y0+H-ph};
            FillRect(hdc,&pr,pb);
            DeleteObject(pb);
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
        } else if (ctrl == 1001 && HIWORD(wParam) == LBN_DBLCLK) {
            // Double-click in browser
            int sel = (int)SendMessageW(g_hBrowser, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_browserItems.size()) {
                auto& item = g_browserItems[sel];
                if (item.isDir) {
                    g_browserDir = item.path;
                    BrowserPopulate();
                } else {
                    SyncModIndexToPath(item.path);
                    g_audio.SetPaused(false);
                    LoadModAtIndex(hWnd);
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
        // Browser keyboard: Enter selects current item
        case VK_RETURN: {
            int sel = (int)SendMessageW(g_hBrowser, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_browserItems.size()) {
                auto& item = g_browserItems[sel];
                if (item.isDir) { g_browserDir = item.path; BrowserPopulate(); }
                else { SyncModIndexToPath(item.path); g_audio.SetPaused(false); LoadModAtIndex(hWnd); }
            }
            break;
        }
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
            sprintf_s(row2,"ORD %d/%d ROW %2d | SPC/\x1a\x1b=prev/next  P=pause  B=backend  F=full  T=browser  1-5=effects",
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
