// ModPlayer.cpp : Defines the entry point for the application.

#include "framework.h"
#include "ModPlayer.h"
#include "ModFile.h"
#include "ModMixer.h"
#include "AudioOut.h"
#include <format>
#include <filesystem>
#include <algorithm>
#include <vector>

#define MAX_LOADSTRING 100

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

static ModFile  g_mod;
static ModMixer g_mixer;
static AudioOut g_audio;

static std::vector<std::filesystem::path> g_modPaths;
static int g_modIndex = 0;

static bool IsModFilename(const std::filesystem::path& p)
{
    // Accept "song.mod" / "song.MOD" and Amiga-style "mod.SongName"
    auto name = p.filename().string();
    std::string lo = name;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);

    if (lo.size() > 4 && lo.substr(lo.size() - 4) == ".mod") return true;
    if (lo.size() > 4 && lo.substr(0, 4) == "mod.") return true;
    return false;
}

static void ScanModFiles()
{
    namespace fs = std::filesystem;
    const fs::path modsDir = R"(C:\Users\gusta\source\repos\ModPlayer\MODS)";
    try {
        for (auto& entry : fs::recursive_directory_iterator(modsDir)) {
            if (entry.is_regular_file() && IsModFilename(entry.path()))
                g_modPaths.push_back(entry.path());
        }
        std::sort(g_modPaths.begin(), g_modPaths.end());
    } catch (...) {}
}

static void LoadModAtIndex(HWND hWnd)
{
    if (g_modPaths.empty()) return;
    const int total = static_cast<int>(g_modPaths.size());

    // Try up to 'total' files starting at g_modIndex, skipping any that fail
    // the magic-word check inside LoadMod (8-ch, MED, XM, etc.).
    for (int tries = 0; tries < total; ++tries) {
        ModFile newMod;
        if (LoadMod(g_modPaths[g_modIndex].string(), newMod)) {
            std::lock_guard<std::mutex> lk(g_audio.GetMutex());
            g_mod = std::move(newMod);
            g_mixer.Init(g_mod, kAudioSampleRate);

            std::wstring title = std::format(L"ModPlayer [{}/{}] — {}",
                g_modIndex + 1, total,
                std::wstring(g_mod.songName, g_mod.songName + strlen(g_mod.songName)));
            SetWindowTextW(hWnd, title.c_str());
            return;
        }
        // Not a supported format — advance to next
        g_modIndex = (g_modIndex + 1) % total;
    }
}

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

    // Find the first supported 4-channel file
    {
        const int total = static_cast<int>(g_modPaths.size());
        bool found = false;
        for (int i = 0; i < total; ++i) {
            if (LoadMod(g_modPaths[i].string(), g_mod)) {
                g_modIndex = i;
                found = true;
                break;
            }
            ++g_modIndex;
        }
        if (!found) {
            MessageBoxA(nullptr, "No supported 4-channel MOD files found", "ModPlayer", MB_ICONERROR);
            return 1;
        }
    }

    g_mixer.Init(g_mod, kAudioSampleRate);

    if (!g_audio.Open(&g_mixer)) {
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

    // Show song name in the title bar
    std::wstring title = std::format(L"ModPlayer [{}/{}] — {}",
        g_modIndex + 1, static_cast<int>(g_modPaths.size()),
        std::wstring(g_mod.songName, g_mod.songName + strlen(g_mod.songName)));

    HWND hWnd = CreateWindowW(szWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 488, 340, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
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
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            g_modIndex = (g_modIndex + 1) % static_cast<int>(g_modPaths.size());
            LoadModAtIndex(hWnd);
        }
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

        // Layout
        const int panelW = W / MOD_CHANNELS;
        const int labelH = 22;
        const int peakH  = 14;
        const int scopeH = H - labelH - peakH - 4;

        // L=blue, R=green
        static const COLORREF kChColor[MOD_CHANNELS] = {
            RGB(64, 148, 255), RGB(64, 220, 128),
            RGB(64, 220, 128), RGB(64, 148, 255),
        };
        static const char* kChSide[MOD_CHANNELS] = { "L", "R", "R", "L" };

        for (int c = 0; c < MOD_CHANNELS; ++c) {
            const auto& cv  = g_mixer.vis[c];
            const COLORREF  col = kChColor[c];
            const int x0 = c * panelW;

            // Panel divider
            HPEN divPen = CreatePen(PS_SOLID, 1, RGB(45, 45, 58));
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, divPen));
            MoveToEx(hdc, x0 + panelW - 1, 0, nullptr);
            LineTo(hdc, x0 + panelW - 1, H);
            SelectObject(hdc, oldPen);
            DeleteObject(divPen);

            // Label
            int noteIdx = cv.active ? PeriodToNoteIndex(cv.period, 0) : -1;
            char label[40];
            if (cv.active)
                sprintf_s(label, "CH%d %s  %s  v%d", c + 1, kChSide[c],
                          NoteName(noteIdx), static_cast<int>(cv.vol));
            else
                sprintf_s(label, "CH%d %s", c + 1, kChSide[c]);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, col);
            RECT lr = { x0 + 4, 2, x0 + panelW - 2, labelH };
            DrawTextA(hdc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

            // Scope background
            RECT sr = { x0, labelH, x0 + panelW - 1, labelH + scopeH };
            HBRUSH scopeBg = CreateSolidBrush(RGB(10, 11, 16));
            FillRect(hdc, &sr, scopeBg);
            DeleteObject(scopeBg);

            // Centre line
            HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(32, 36, 48));
            oldPen = static_cast<HPEN>(SelectObject(hdc, gridPen));
            int cy = labelH + scopeH / 2;
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
                constexpr int kLen = ModMixer::kScopeLen;
                for (int x = 0; x < pts; ++x) {
                    int idx = (cv.scopePos + x * kLen / pts) & (kLen - 1);
                    float samp = cv.scope[idx];
                    int sy = cy - static_cast<int>(samp * (scopeH / 2 - 2));
                    sy = std::clamp(sy, labelH + 1, labelH + scopeH - 2);
                    poly[x] = { x0 + 1 + x, sy };
                }
                Polyline(hdc, poly.data(), pts);

                SelectObject(hdc, oldPen);
                DeleteObject(wavePen);
            }

            // Peak bar
            const int peakY = labelH + scopeH + 2;
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
    case WM_TIMER:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
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
