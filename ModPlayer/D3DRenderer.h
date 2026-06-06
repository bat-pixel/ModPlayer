#pragma once
#include "framework.h"

// Forward-declare COM interfaces to keep this header lean.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11Buffer;

// Music data passed to every pixel shader via cbuffer b0.
// 12 floats = 48 bytes — multiple of 16 (3 float4 registers).
struct alignas(16) ShaderCB {
    float time;    // seconds since app start
    float energy;  // combined stereo RMS [0-1]
    float beat;    // beat-flash decay [0-1]
    float resX;    // viewport width
    float resY;    // viewport height
    float ch0;     // channel 0 scope RMS
    float ch1;     // channel 1 scope RMS
    float ch2;     // channel 2 scope RMS
    float ch3;     // channel 3 scope RMS
    float lVol;    // (ch0+ch3)*0.5
    float rVol;    // (ch1+ch2)*0.5
    float pad;
};
static_assert(sizeof(ShaderCB) == 48);
static_assert(sizeof(ShaderCB) % 16 == 0);

// D3D11 renderer — renders pixel-shader effects into a child HWND.
// All methods must be called from the UI thread only.
class D3DRenderer {
public:
    // Create device + swap chain targeting a new child HWND inside hwndParent.
    // Compiles all pixel shaders.  Returns false if D3D11 is not available.
    bool Init(HWND hwndParent, int x, int y, int w, int h);

    // Resize the swap chain and reposition the child HWND.
    void Resize(int x, int y, int w, int h);

    // Render one frame for effectMode in [1..7].
    void RenderFrame(int effectMode, const ShaderCB& cb);

    // Release all D3D resources and destroy the child HWND.
    void Shutdown();

    bool IsReady()  const { return ready_; }
    HWND GetHwnd()  const { return hwnd_; }

    ~D3DRenderer() { Shutdown(); }

private:
    bool CreateDeviceAndSwapChain(int w, int h);
    bool CreateRTV();
    void ReleaseRTV();
    bool CompileShaders();
    bool CompilePS(int idx, const char* hlsl);

    bool                    ready_  = false;
    HWND                    hwnd_   = nullptr;
    int                     w_ = 0, h_ = 0;

    ID3D11Device*           dev_    = nullptr;
    ID3D11DeviceContext*    ctx_    = nullptr;
    IDXGISwapChain*         swap_   = nullptr;
    ID3D11RenderTargetView* rtv_    = nullptr;
    ID3D11VertexShader*     vs_     = nullptr;
    ID3D11Buffer*           cb_     = nullptr;

    static constexpr int kModes = 7;   // modes 1-7
    ID3D11PixelShader*  ps_[kModes]{};
};
