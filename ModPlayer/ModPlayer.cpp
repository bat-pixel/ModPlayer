// ModPlayer.cpp : Defines the entry point for the application.

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

#define MAX_LOADSTRING 100

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

static ModFile          g_mod;            // always kept in sync; needed by native backend
static ModMixer         g_nativeMixer;    // our ProTracker engine
static LibOpenMptMixer  g_omptMixer;      // libopenmpt engine
static IMixer*          g_activeMixer = &g_nativeMixer;
static AudioOut         g_audio;

static std::vector<std::filesystem::path> g_modPaths;
static int  g_modIndex = 0;

// Log window for unimplemented-effect warnings
static HWND g_hLog = nullptr;

// ── File scanning ─────────────────────────────────────────────────────────────

static bool IsModFilename(const std::filesystem::path& p)
{
    auto name = p.filename().string();
    std::string lo = name;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    if (lo.size() > 4 && lo.substr(lo.size() - 4) == ".mod") return true;
    if (lo.size() > 4 && lo.substr(0, 4) == "mod.") return true;
    return false;
}

static std::filesystem::path FindModsDir()
{
    namespace fs = std::filesystem;
    WCHAR exeBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path dir = fs::path(exeBuf).parent_path();
    // Walk up to 4 levels from the exe looking for a "MODS" subdirectory.
    for (int depth = 0; depth <= 4; ++depth) {
        fs::path candidate = dir / L"MODS";
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec))
            return candidate;
        dir = dir.parent_path();
    }
    return {};
}

static void ScanModFiles()
{
    namespace fs = std::filesystem;
    const fs::path modsDir = FindModsDir();
    if (modsDir.empty()) return;
    try {
        for (auto& entry : fs::recursive_directory_iterator(modsDir)) {
            if (entry.is_regular_file() && IsModFilename(entry.path()))
                g_modPaths.push_back(entry.path());
        }
        std::sort(g_modPaths.begin(), g_modPaths.end());
    } catch (...) {}
}

// ── Log window ───────────────────────────────────────────────────────────────

static void LogClear()
{
    if (g_hLog) SetWindowTextA(g_hLog, "");
}

static void LogAppend(const std::string& line)
{
    if (!g_hLog) return;
    int len = GetWindowTextLengthA(g_hLog);
    SendMessageA(g_hLog, EM_SETSEL, len, len);
    SendMessageA(g_hLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

// Scan the loaded MOD for effects that the native backend does not implement
// and write warnings to the log window.
static void LogUnimplementedEffects(const ModFile& mod)
{
    // Effects with no native implementation (after all recent additions)
    static constexpr struct { uint8_t effect; uint8_t subMask; uint8_t subVal; const char* name; } kUnimpl[] = {
        { 0xE, 0xF0, 0x00, "E0x Filter On/Off"    },
        { 0xE, 0xF0, 0xF0, "EFx Funk Repeat"       },
    };

    std::string warnings;
    for (const auto& u : kUnimpl) {
        int count = 0;
        for (const auto& pat : mod.patterns)
            for (const auto& row : pat)
                for (const auto& n : row)
                    if (n.effect == u.effect && (n.param & u.subMask) == u.subVal)
                        ++count;
        if (count > 0)
            warnings += std::format("  [!] {} — {} uses (not implemented)\r\n", u.name, count);
    }

    LogClear();
    if (warnings.empty()) {
        LogAppend("All effects in this MOD are implemented.\r\n");
    } else {
        LogAppend(std::format("Unimplemented effects in \"{}\":\r\n", mod.songName));
        LogAppend(warnings);
    }
}

// ── Window title ──────────────────────────────────────────────────────────────

static void UpdateWindowTitle(HWND hWnd)
{
    const int total = static_cast<int>(g_modPaths.size());
    const char* bn  = g_activeMixer->BackendName();
    std::string title = g_activeMixer->SongTitle();

    // BackendName and SongTitle are ASCII-safe; widen naively.
    std::wstring wbn(bn, bn + strlen(bn));
    std::wstring wtitle(title.begin(), title.end());

    const wchar_t* recTag = g_audio.IsRecording() ? L" [REC]" : L"";
    SetWindowTextW(hWnd, std::format(L"ModPlayer [{}/{}] [{}]{} — {}",
        g_modIndex + 1, total, wbn, recTag, wtitle).c_str());
}

// ── Load mod at index ─────────────────────────────────────────────────────────

static void LoadModAtIndex(HWND hWnd)
{
    if (g_modPaths.empty()) return;
    const int total = static_cast<int>(g_modPaths.size());

    for (int tries = 0; tries < total; ++tries) {
        const std::string path = g_modPaths[g_modIndex].string();

        if (g_activeMixer == &g_nativeMixer) {
            // Native: only 4-channel ProTracker MODs.
            // File I/O happens outside the lock; Init() inside.
            ModFile newMod;
            if (LoadMod(path, newMod)) {
                std::lock_guard<std::mutex> lk(g_audio.GetMutex());
                g_mod = std::move(newMod);
                g_nativeMixer.Init(g_mod, kAudioSampleRate);
                LogUnimplementedEffects(g_mod);
                UpdateWindowTitle(hWnd);
                return;
            }
        } else {
            // libopenmpt: accepts any format it understands.
            // Also parse natively (best-effort) so backend switching works.
            ModFile newMod;
            const bool nativeOk = LoadMod(path, newMod);
            bool loaded = false;
            {
                std::lock_guard<std::mutex> lk(g_audio.GetMutex());
                loaded = g_omptMixer.Load(path, kAudioSampleRate);
                if (loaded && nativeOk)
                    g_mod = std::move(newMod);
            }
            if (loaded) {
                if (nativeOk) LogUnimplementedEffects(g_mod);
                else          LogAppend("(libopenmpt-only format — effect analysis unavailable)\r\n");
                UpdateWindowTitle(hWnd);
                return;
            }
        }
        g_modIndex = (g_modIndex + 1) % total;
    }
}

// ── Backend switching ─────────────────────────────────────────────────────────

static void SwitchBackend(HWND hWnd)
{
    if (g_modPaths.empty()) return;
    const std::string path = g_modPaths[g_modIndex].string();

    if (g_activeMixer == &g_nativeMixer) {
        // Switch to libopenmpt
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(g_audio.GetMutex());
            ok = g_omptMixer.Load(path, kAudioSampleRate);
            if (ok) g_activeMixer = &g_omptMixer;
        }
        if (ok) {
            g_audio.SetMixer(&g_omptMixer);
        } else {
            MessageBoxA(hWnd,
                ("libopenmpt failed to load:\n" + path).c_str(),
                "Switch Backend", MB_ICONWARNING | MB_OK);
        }
    } else {
        // Switch back to native
        {
            std::lock_guard<std::mutex> lk(g_audio.GetMutex());
            g_nativeMixer.Init(g_mod, kAudioSampleRate);
            g_activeMixer = &g_nativeMixer;
        }
        g_audio.SetMixer(&g_nativeMixer);
    }
    UpdateWindowTitle(hWnd);
}

// ── Win32 boilerplate ─────────────────────────────────────────────────────────

ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    ScanModFiles();
    if (g_modPaths.empty()) {
        MessageBoxA(nullptr, "No MOD files found in MODS directory", "ModPlayer", MB_ICONERROR);
        return 1;
    }

    // Find and load the first file. Try libopenmpt first (sounds identical to ffplay);
    // fall back to native for 4-channel MODs if libopenmpt is unavailable.
    {
        const int total = static_cast<int>(g_modPaths.size());
        bool found = false;
        for (int i = 0; i < total; ++i) {
            const std::string path = g_modPaths[i].string();
            if (g_omptMixer.Load(path, kAudioSampleRate)) {
                g_modIndex = i;
                // Also load natively (best-effort) so backend switching works.
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

    // Default to libopenmpt backend — sounds identical to ffplay.
    g_activeMixer = &g_omptMixer;

    if (!g_audio.Open(&g_omptMixer)) {
        MessageBoxA(nullptr, "waveOutOpen failed", "AudioOut error", MB_ICONERROR);
        return 1;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_MODPLAYER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
        return FALSE;

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_MODPLAYER));
    MSG msg;

    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    g_audio.Close();
    return static_cast<int>(msg.wParam);
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
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszMenuName  = MAKEINTRESOURCEW(IDC_MODPLAYER);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    // Main visualizer window (488 wide, 380 tall — extra height for log panel)
    HWND hWnd = CreateWindowW(szWindowClass, L"ModPlayer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 488, 440, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return FALSE;

    // Log panel: read-only multiline edit at the bottom
    RECT rc; GetClientRect(hWnd, &rc);
    g_hLog = CreateWindowExA(0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, rc.bottom - 80, rc.right, 80,
        hWnd, nullptr, hInstance, nullptr);
    SendMessageA(g_hLog, WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    UpdateWindowTitle(hWnd);
    SetTimer(hWnd, 1, 33, nullptr);  // ~30 fps repaint for visualizer
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    }
    case WM_KEYDOWN: {
        const int total = static_cast<int>(g_modPaths.size());
        if (wParam == VK_SPACE || wParam == VK_RIGHT) {
            g_modIndex = (g_modIndex + 1) % total;
            g_audio.SetPaused(false);
            LoadModAtIndex(hWnd);
        } else if (wParam == VK_LEFT || wParam == VK_BACK) {
            g_modIndex = (g_modIndex - 1 + total) % total;
            g_audio.SetPaused(false);
            LoadModAtIndex(hWnd);
        } else if (wParam == 'P') {
            g_audio.SetPaused(!g_audio.IsPaused());
            InvalidateRect(hWnd, nullptr, FALSE);
        } else if (wParam == 'B') {
            SwitchBackend(hWnd);
        } else if (wParam == 'R') {
            if (g_audio.IsRecording()) {
                g_audio.StopRecording();
                MessageBoxA(hWnd,
                    "Recording saved to:\ntests\\bin\\player_recording.wav\n\n"
                    "Compare with tests\\bin\\native_ackerlight.wav using:\n"
                    "python tests\\compare_wav.py tests\\bin\\native_ackerlight.wav "
                    "tests\\bin\\player_recording.wav --block-ms 50",
                    "Recording stopped", MB_OK | MB_ICONINFORMATION);
            } else {
                // Restart the current MOD from position 0 so the recording
                // captures from the beginning of the song.
                g_audio.SetPaused(false);
                LoadModAtIndex(hWnd);
                g_audio.StartRecording("tests\\bin\\player_recording.wav");
            }
            UpdateWindowTitle(hWnd);
        }
        break;
    }
    case WM_SIZE:
        if (g_hLog) {
            RECT rc; GetClientRect(hWnd, &rc);
            SetWindowPos(g_hLog, nullptr, 0, rc.bottom - 80, rc.right, 80,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
    case WM_TIMER:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcReal = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);
        const int W = rc.right;
        const int H = rc.bottom;

        // Double-buffer
        HDC     hdc    = CreateCompatibleDC(hdcReal);
        HBITMAP bmp    = CreateCompatibleBitmap(hdcReal, W, H);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(hdc, bmp));

        // Clear background
        HBRUSH bgBrush = CreateSolidBrush(RGB(18, 18, 26));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        // Two-row banner at the very top
        const int bannerH = 38;  // two rows of 19px each
        {
            const char* bn    = g_activeMixer->BackendName();
            const bool isOmpt = (g_activeMixer != &g_nativeMixer);
            const bool paused = g_audio.IsPaused();
            COLORREF bannerCol = isOmpt ? RGB(255, 200, 60) : RGB(100, 200, 100);
            COLORREF pauseCol  = RGB(255, 100, 100);

            HBRUSH bannerBr = CreateSolidBrush(RGB(24, 24, 36));
            RECT br = { 0, 0, W, bannerH };
            FillRect(hdc, &br, bannerBr);
            DeleteObject(bannerBr);
            SetBkMode(hdc, TRANSPARENT);

            // Row 1: backend name + pause indicator
            SetTextColor(hdc, bannerCol);
            char row1[80];
            sprintf_s(row1, "BACKEND: %s  (B=toggle)%s", bn, paused ? "   [PAUSED]" : "");
            if (paused) {
                // Draw the [PAUSED] portion in red — simplest: draw whole line then overdraw
                RECT r1 = { 4, 2, W - 4, 20 };
                DrawTextA(hdc, row1, -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            } else {
                RECT r1 = { 4, 2, W - 4, 20 };
                DrawTextA(hdc, row1, -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            // Row 2: position info + key hints
            const int ord    = g_activeMixer->CurrentOrder();
            const int row    = g_activeMixer->CurrentRow();
            const int orders = g_activeMixer->SongOrders();
            SetTextColor(hdc, RGB(160, 160, 180));
            char row2[120];
            sprintf_s(row2, "ORD %d/%d  ROW %d  |  SPC/\x1a=next  \x1b=prev  P=pause  B=backend  R=record",
                      ord + 1, orders, row);
            RECT r2 = { 4, 20, W - 4, bannerH - 2 };
            DrawTextA(hdc, row2, -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // Layout — subtract the log panel height (80px) from available scope area
        const int logH   = 80;
        const int panelW = W / IMixer::kVisChannels;
        const int labelH = 22;
        const int peakH  = 14;
        const int scopeH = (H - logH) - bannerH - labelH - peakH - 4;

        // L=blue, R=green (Amiga hard-pan: L R R L)
        static const COLORREF kChColor[IMixer::kVisChannels] = {
            RGB(64, 148, 255), RGB(64, 220, 128),
            RGB(64, 220, 128), RGB(64, 148, 255),
        };
        static const char* kChSide[IMixer::kVisChannels] = { "L", "R", "R", "L" };

        for (int c = 0; c < IMixer::kVisChannels; ++c) {
            const auto&    cv  = g_activeMixer->vis[c];
            const COLORREF col = kChColor[c];
            const int      x0  = c * panelW;
            const int      y0  = bannerH;  // panels start below banner

            // Panel divider
            HPEN divPen = CreatePen(PS_SOLID, 1, RGB(45, 45, 58));
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, divPen));
            MoveToEx(hdc, x0 + panelW - 1, y0, nullptr);
            LineTo(hdc, x0 + panelW - 1, H - logH);
            SelectObject(hdc, oldPen);
            DeleteObject(divPen);

            // Label: "CH1 L  C-3  v64" (period only meaningful for native)
            int noteIdx = cv.active ? PeriodToNoteIndex(cv.period, 0) : -1;
            char label[48];
            if (cv.active && cv.period > 0)
                sprintf_s(label, "CH%d %s  %s  v%d", c + 1, kChSide[c],
                          NoteName(noteIdx), static_cast<int>(cv.vol));
            else if (cv.active)
                sprintf_s(label, "CH%d %s  v%d", c + 1, kChSide[c],
                          static_cast<int>(cv.vol));
            else
                sprintf_s(label, "CH%d %s", c + 1, kChSide[c]);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, col);
            RECT lr = { x0 + 4, y0 + 2, x0 + panelW - 2, y0 + labelH };
            DrawTextA(hdc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

            // Scope background
            RECT sr = { x0, y0 + labelH, x0 + panelW - 1, y0 + labelH + scopeH };
            HBRUSH scopeBg = CreateSolidBrush(RGB(10, 11, 16));
            FillRect(hdc, &sr, scopeBg);
            DeleteObject(scopeBg);

            // Centre line
            HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(32, 36, 48));
            oldPen = static_cast<HPEN>(SelectObject(hdc, gridPen));
            int cy = y0 + labelH + scopeH / 2;
            MoveToEx(hdc, x0, cy, nullptr);
            LineTo(hdc, x0 + panelW - 1, cy);
            SelectObject(hdc, oldPen);
            DeleteObject(gridPen);

            // Oscilloscope waveform
            const int pts = panelW - 2;
            if (pts > 0 && (cv.active || cv.peak > 0.001f)) {
                HPEN wavePen = CreatePen(PS_SOLID, 1, col);
                oldPen = static_cast<HPEN>(SelectObject(hdc, wavePen));

                std::vector<POINT> poly(static_cast<size_t>(pts));
                constexpr int kLen = IMixer::kScopeLen;
                for (int x = 0; x < pts; ++x) {
                    int idx = (cv.scopePos + x * kLen / pts) & (kLen - 1);
                    float samp = cv.scope[idx];
                    int sy = cy - static_cast<int>(samp * (scopeH / 2 - 2));
                    sy = std::clamp(sy, y0 + labelH + 1, y0 + labelH + scopeH - 2);
                    poly[x] = { x0 + 1 + x, sy };
                }
                Polyline(hdc, poly.data(), pts);

                SelectObject(hdc, oldPen);
                DeleteObject(wavePen);
            }

            // Peak bar
            const int peakY = y0 + labelH + scopeH + 2;
            RECT pbg = { x0 + 2, peakY, x0 + panelW - 2, peakY + peakH - 2 };
            HBRUSH pbgBrush = CreateSolidBrush(RGB(22, 24, 34));
            FillRect(hdc, &pbg, pbgBrush);
            DeleteObject(pbgBrush);

            const int peakPx = static_cast<int>(cv.peak * (panelW - 4));
            if (peakPx > 0) {
                RECT pfill = { x0 + 2, peakY, x0 + 2 + peakPx, peakY + peakH - 2 };
                HBRUSH pfBrush = CreateSolidBrush(col);
                FillRect(hdc, &pfill, pfBrush);
                DeleteObject(pfBrush);
            }
        }

        BitBlt(hdcReal, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(hdc);

        EndPaint(hWnd, &ps);
        break;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}
