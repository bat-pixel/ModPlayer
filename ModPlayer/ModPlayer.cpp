// ModPlayer.cpp : Defines the entry point for the application.

#include "framework.h"
#include "ModPlayer.h"
#include "ModFile.h"
#include "ModMixer.h"
#include "AudioOut.h"
#include <format>
#include <filesystem>
#include <algorithm>

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
        CW_USEDEFAULT, 0, 480, 120, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
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
        HDC hdc = BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_DESTROY:
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
