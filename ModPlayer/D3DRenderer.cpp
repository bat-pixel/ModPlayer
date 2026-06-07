#include "framework.h"
#include "D3DRenderer.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ── HLSL shared header (cbuffer + VS) ────────────────────────────────────────

static const char* kSharedHLSL = R"hlsl(
cbuffer CB : register(b0) {
    float time;   float energy; float beat; float resX;
    float resY;   float ch0;   float ch1;  float ch2;
    float ch3;    float lVol;  float rVol; float pad;
};

// Fullscreen triangle from vertex ID — no vertex buffer needed.
// Winding is CW so D3D11's default CULL_BACK keeps it (CW = front face).
//   id=0: uv=(0,0) NDC=(-1, 1)  top-left
//   id=1: uv=(2,0) NDC=( 3, 1)  oversized top-right  → clips to (1, 1)
//   id=2: uv=(0,2) NDC=(-1,-3)  oversized bottom-left → clips to (-1,-1)
void VS_Main(uint id : SV_VertexID,
             out float4 pos : SV_Position,
             out float2 uv  : TEXCOORD0)
{
    uv  = float2((id == 1) ? 2.0 : 0.0,
                 (id == 2) ? 2.0 : 0.0);
    pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

float3 hsv(float h, float s, float v) {
    h = frac(h);
    float3 rgb = saturate(abs(fmod(h * 6.0 + float3(0,4,2), 6.0) - 3.0) - 1.0);
    return v * lerp(float3(1,1,1), rgb, s);
}
)hlsl";

// ── Pixel shaders (one per effect mode) ──────────────────────────────────────

// Mode 1 — Plasma with music-driven colour spin
static const char* kPS1 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = uv * 2.0 - 1.0;
    float  t = time * (0.4 + energy * 0.7) + beat * 1.5;
    float  v = sin(p.x * (4.0 + ch0 * 4.0) + t)
             + sin(p.y * (3.0 + ch1 * 3.0) - t * 1.1)
             + sin((p.x + p.y + t * 0.4) * 4.5)
             + sin(length(p) * 7.0 - t * 1.4);
    v = (v / 4.0 + 1.0) * 0.5;
    float amp = 0.35 + energy * 0.65;
    float3 col = float3(
        sin(v * 3.14159 + lVol * 2.0) * amp + 0.5 * amp,
        sin(v * 3.14159 + 2.094 + rVol * 1.5) * amp + 0.5 * amp,
        sin(v * 3.14159 + 4.189 + (ch0-ch2) * 1.5) * amp + 0.5 * amp
    );
    col = pow(saturate(col), 0.75);
    return float4(col * (0.7 + beat * 0.3), 1.0);
}
)hlsl";

// Mode 2 — Julia fractal (animated c parameter)
static const char* kPS2 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 z = (uv * 2.0 - 1.0) * float2(resX / resY, 1.0) * 1.6;
    float  ta = time * 0.13 + beat * 0.4;
    float2 c  = float2(cos(ta) * (0.65 + lVol * 0.2),
                       sin(ta) * (0.35 + rVol * 0.15));
    int    i;
    float  d2 = dot(z, z);
    for (i = 0; i < 64 && d2 < 4.0; ++i) {
        z  = float2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + c;
        d2 = dot(z, z);
    }
    if (i >= 64) return float4(0,0,0,1);
    float t2  = (float)i / 64.0 - log2(max(1.0, log2(sqrt(d2)))) / 64.0;
    float3 col = hsv(t2 * 2.5 + time * 0.04 + energy * 0.3, 0.85, pow(t2, 0.6));
    return float4(col * (0.8 + beat * 0.2), 1.0);
}
)hlsl";

// Mode 3 — Vortex tunnel with brick pattern
static const char* kPS3 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = uv * 2.0 - 1.0;
    p.x *= resX / resY;
    float  r  = length(p);
    float  a  = atan2(p.y, p.x);
    float  t  = time * (0.3 + energy * 0.5) + beat;
    float  tu = a / 6.28318 + sin(r * 3.0 - t) * 0.07 + t * 0.2;
    float  tv = 0.25 / (r + 0.01) + t * 0.6;
    float  bk = fmod(floor(tu * 7.0) + floor(tv * 4.0), 2.0);
    float  edge = 1.0 - smoothstep(0.0, 0.08, r);  // centre fade
    float  fog  = 1.0 / (1.0 + r * r * 0.5);
    float3 ca = hsv(a / 6.28318 + lVol * 0.3 + time * 0.03, 0.9, 1.0);
    float3 cb = hsv(a / 6.28318 + rVol * 0.3 + time * 0.05 + 0.5, 0.8, 0.7);
    float3 col = lerp(ca, cb, bk) * (1.0 - edge) * fog;
    return float4(saturate(col * (0.6 + beat * 0.4)), 1.0);
}
)hlsl";

// Mode 4 — Synthwave retro grid with sun
static const char* kPS4 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = uv;
    float  t = time * (0.2 + energy * 0.2);
    float3 col;

    // Sky
    float  skyFade = 1.0 - smoothstep(0.3, 0.55, p.y);
    col = lerp(float3(0.6,0.1,0.5), float3(0.05,0,0.15), p.y) * skyFade
        + lerp(float3(0.05,0,0.15), float3(0,0,0.05), p.y) * (1.0 - skyFade);

    // Sun
    float2 sunC = float2(0.5 + sin(t * 0.2) * 0.15, 0.47);
    float  sunR = 0.10 + energy * 0.015 + beat * 0.02;
    float  sunD = length(p - sunC);
    float  sun  = smoothstep(sunR, sunR * 0.88, sunD);
    // Horizontal stripes across sun
    sun *= step(0.5, frac((p.y - 0.47) * 18.0)) + step(sunD, sunR * 0.5);
    col += float3(1.0, 0.65, 0.05) * sun * (0.8 + beat * 0.5);
    // Halo
    col += float3(0.9, 0.2, 0.6) * smoothstep(sunR * 3.0, 0.0, sunD) * 0.3;

    // Horizon glow
    float hor = exp(-abs(p.y - 0.5) * 25.0);
    col += float3(1.0, 0.3, 0.8) * hor * (0.4 + energy * 0.6 + beat * 0.3);

    // Ground grid (perspective)
    if (p.y > 0.5) {
        float  gy = p.y - 0.5;
        float2 gp = float2((p.x - 0.5) / (gy + 0.01), 1.0 / (gy + 0.01));
        float  lx = smoothstep(0.96, 1.0, frac(gp.x * 5.0));
        float  ly = smoothstep(0.94, 1.0, frac((gp.y + t * 3.0) * 0.5));
        float  grid = max(lx, ly) * (0.5 + lVol * 0.5);
        float3 gc   = float3(grid * rVol, 0, grid);
        col  = lerp(float3(0,0,0.06), col + gc, smoothstep(0.5, 0.52, p.y));
    }

    return float4(saturate(col), 1.0);
}
)hlsl";

// Mode 5 — Spectrum bars with GPU glow
static const char* kPS5 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const int N   = 32;
    float     f   = uv.x;
    float     lW  = pow(max(0.0, sin((1.0 - f) * 3.14159)), 1.5);
    float     rW  = pow(max(0.0, sin(f * 3.14159)), 1.5);
    float     lR  = (ch0 + ch3) * 0.5;
    float     rR  = (ch1 + ch2) * 0.5;
    float     lvl = min(1.0, (lR * lW + rR * rW) * 3.0 + beat * 0.18);

    // Pixel-consistent bar gap: 2px gap regardless of window width
    float  gapFrac = max(0.02, 2.0 * N / resX);
    float  barFrac = frac(uv.x * N);
    float  inBar   = step(1.0 - lvl, uv.y) * step(barFrac, 1.0 - gapFrac);

    // Full-spectrum rainbow: hue varies across bars + shifts with height + slow drift
    float  relY = saturate((uv.y - (1.0 - lvl)) / max(0.001, lvl));
    float  hue  = frac(uv.x * 0.75 + time * 0.06 - relY * 0.45);
    float3 barC = hsv(hue, 0.97, 1.0);

    // Vivid glow scaled to ~12px width regardless of window height
    float  edgeDist = abs(uv.y - (1.0 - lvl));
    float  glow     = exp(-edgeDist * resY * 0.07) * lvl * 1.4;
    float3 glowC    = hsv(frac(hue + 0.15 + beat * 0.1), 0.85, 1.0) * glow;

    // Dark background with subtle audio-reactive colour tint
    float3 bg  = float3(0.02, 0.02, 0.06) + hsv(frac(time * 0.03 + uv.x * 0.2), 0.9, 1.0) * energy * 0.04;
    float3 col = lerp(bg + glowC, barC, inBar);
    col = pow(saturate(col), 0.75);
    return float4(col * (0.85 + energy * 0.15 + beat * 0.2), 1.0);
}
)hlsl";

// Mode 6 — Channel orbs (4 glowing spheres with beat rings)
static const char* kPS6 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p   = uv * 2.0 - 1.0;
    float  asp = resX / resY;
    p.x       *= asp;
    float  rms[4]; rms[0]=ch0; rms[1]=ch1; rms[2]=ch2; rms[3]=ch3;
    // Quadrant centres (aspect-corrected)
    float2 ctr[4];
    ctr[0] = float2(-0.5 * asp,  0.5);
    ctr[1] = float2( 0.5 * asp,  0.5);
    ctr[2] = float2( 0.5 * asp, -0.5);
    ctr[3] = float2(-0.5 * asp, -0.5);

    float3 col = float3(0.03, 0.03, 0.07);

    for (int i = 0; i < 4; ++i) {
        float  r    = max(0.07, rms[i] * 0.55 + 0.05 + beat * 0.05);
        float  dist = length(p - ctr[i]);
        float  hue  = frac((float)i * 0.25 + time * 0.04);

        // Multi-layer glow
        float  g1   = exp(-dist * dist / (r * r * 4.0));        // wide soft glow
        float  g2   = exp(-dist * dist / (r * r * 0.8)) * 1.5;  // core glow
        float  core = smoothstep(r, r * 0.7, dist);             // sharp disc

        float3 hc   = hsv(hue, 0.85, 1.0);
        col += hc * (g1 * 0.5 + g2 * 0.8 + core);

        // Beat ring: expands outward from the orb edge
        float  ringR = r + beat * r * 2.5;
        float  ring  = exp(-abs(dist - ringR) * 40.0) * beat * 0.6;
        col += hsv(hue + 0.05, 0.6, 1.0) * ring;

        // Inner bright highlight
        col += lerp(float3(1,1,1), hc, 0.5) * exp(-dist * dist / (r * r * 0.05)) * 0.4;
    }

    return float4(saturate(pow(col, 0.85)), 1.0);
}
)hlsl";

// Mode 7 — Neon 3D equalizer banks (4 channels × 8 bars with reflection floor)
static const char* kPS7 = R"hlsl(
float4 PS_Main(float4 svp : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float rms[4]; rms[0]=ch0; rms[1]=ch1; rms[2]=ch2; rms[3]=ch3;

    const int   NCH  = 4;     // channels across width
    const int   NB   = 8;     // bars per channel
    const float MIRR = 0.58;  // mirror Y (EQ above, reflection below)
    const float GAP  = 0.08;  // gap fraction of bar slot width
    const float BEV  = 0.10;  // right-bevel fraction of bar body width
    const float CAPH = 0.05;  // top-cap height as fraction of bar height

    float panX = uv.x * NCH;
    int   ci   = clamp((int)panX, 0, NCH - 1);
    float lx   = frac(panX);

    float slotF = lx * NB;
    int   bi    = clamp((int)slotF, 0, NB - 1);
    float bf    = frac(slotF);

    bool  isR = uv.y > MIRR;
    float ey  = isR ? (uv.y - MIRR) / (1.0 - MIRR)
                    : (MIRR - uv.y) / MIRR;

    // Synthetic bar height: bell-curve shaped from single RMS + time wobble
    float r   = rms[ci];
    float bif = (float)bi / (float)(NB - 1);
    float pk  = 0.38 + sin(time * 0.22 + (float)ci * 1.57) * 0.10;
    float bel = exp(-pow((bif - pk) / 0.40, 2.0) * 4.0);
    float wob = sin(time * (1.8 + (float)bi * 0.45) + (float)ci * 2.09) * 0.07;
    float bh  = saturate(r * 1.7 * bel + wob * max(r, 0.08) + beat * 0.09 * bel);
    bh = max(bh, 0.04 - bif * 0.015);

    bool inGap  = bf < GAP * 0.5 || bf > 1.0 - GAP * 0.5;
    bool inBev  = !inGap && bf > 1.0 - GAP * 0.5 - BEV;
    bool inBody = !isR && ey < bh && !inGap;
    bool inCap  = inBody && ey > bh * (1.0 - CAPH);

    // Per-bar hue: channel + wider position spread + faster drift; shifts more with height
    float hue  = frac((float)ci * 0.25 + bif * 0.35 + time * 0.05);
    float relY = saturate(ey / max(0.001, bh));
    float3 barC = hsv(frac(hue + relY * 0.30), 0.98, 1.0);
    float3 capC = lerp(float3(1,1,1), barC, 0.20);
    float3 bevC = barC * 0.55;

    // Background: deep purple-black with pixel-scaled scanlines + vivid separator + floor glow
    float3 col = float3(0.03, 0.02, 0.06);
    col += float3(0.08, 0.03, 0.12) * step(0.5, frac(uv.y * resY * 0.25)) * 0.08;
    float sx = abs(frac(panX + 0.5) - 0.5) * 2.0;
    col += hsv(hue, 0.9, 1.0) * exp(-(1.0 - sx) * resX * 0.006) * 0.14;
    col += float3(0.28, 0.08, 0.50) * exp(-abs(uv.y - MIRR) * resY * 0.028) * (0.6 + r * 0.9);

    // Bar body
    if (inBody) {
        if (inCap)
            col = capC * (1.3 + beat * 0.8);
        else if (inBev)
            col = bevC;
        else
            col = barC * (0.90 + beat * 0.15);
    }

    // Top-edge glow halo (EQ side only) — pixel-scaled ~10px width
    if (!isR && !inGap) {
        float gd = abs(ey - bh);
        col += hsv(frac(hue + 0.08), 0.7, 1.0) * exp(-gd * resY * MIRR * 0.06) * bh * 0.80 * (1.0 + beat * 0.7);
    }

    // Reflection: mirror the bars downward, fade with distance
    if (isR && !inGap) {
        if (ey < bh) {
            float fade = pow(1.0 - ey / max(0.001, bh), 2.0);
            col = lerp(col, barC * 0.55, fade * 0.75);
        }
        col *= max(0.0, 1.0 - ey * 2.2);
    }

    // Beat flash and energy ambient — brighter and more colourful
    col += hsv(time * 0.07, 0.9, 1.0) * beat * 0.12;
    col += hsv(hue + 0.5, 0.85, 1.0) * energy * 0.045;

    float2 cv = uv * 2.0 - 1.0;
    col *= 1.0 - saturate(dot(cv, cv) * 0.35);

    return float4(saturate(pow(col, 0.82)), 1.0);
}
)hlsl";

// ── Helpers ───────────────────────────────────────────────────────────────────

template<class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::string BuildFullPS(const char* psBody)
{
    return std::string(kSharedHLSL) + "\n" + psBody;
}

// ── D3DRenderer::Init ─────────────────────────────────────────────────────────

bool D3DRenderer::Init(HWND hwndParent, int x, int y, int w, int h)
{
    // Register child window class (once per process)
    static bool kClassDone = false;
    if (!kClassDone) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"D3DEffectChild";
        RegisterClassExW(&wc);
        kClassDone = true;
    }

    hwnd_ = CreateWindowExW(0, L"D3DEffectChild", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        x, y, w, h, hwndParent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return false;

    w_ = w; h_ = h;

    if (!CreateDeviceAndSwapChain(w, h) || !CreateRTV() || !CompileShaders()) {
        Shutdown();
        return false;
    }

    ready_ = true;
    return true;
}

bool D3DRenderer::CreateDeviceAndSwapChain(int w, int h)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = (UINT)w;
    sd.BufferDesc.Height                  = (UINT)h;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd_;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl  = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL got = fl;
    UINT flags = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        &fl, 1, D3D11_SDK_VERSION, &sd, &swap_, &dev_, &got, &ctx_);
    if (FAILED(hr)) return false;

    // Constant buffer
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(ShaderCB);
    bd.Usage          = D3D11_USAGE_DEFAULT;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return false;

    return true;
}

bool D3DRenderer::CreateRTV()
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    HRESULT hr = dev_->CreateRenderTargetView(bb, nullptr, &rtv_);
    bb->Release();
    return SUCCEEDED(hr);
}

void D3DRenderer::ReleaseRTV()
{
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->Flush();
    SafeRelease(rtv_);
}

bool D3DRenderer::CompilePS(int idx, const char* hlsl)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* err  = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr,
                            "PS_Main", "ps_5_0", flags, 0, &code, &err);
    if (err) {
        OutputDebugStringA("D3D shader error: ");
        OutputDebugStringA((char*)err->GetBufferPointer());
        err->Release();
    }
    if (FAILED(hr)) return false;
    hr = dev_->CreatePixelShader(code->GetBufferPointer(),
                                 code->GetBufferSize(), nullptr, &ps_[idx]);
    code->Release();
    return SUCCEEDED(hr);
}

bool D3DRenderer::CompileShaders()
{
    // Vertex shader (shared fullscreen triangle)
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* err  = nullptr;
        std::string src = std::string(kSharedHLSL);
        HRESULT hr = D3DCompile(src.c_str(), src.size(), nullptr, nullptr, nullptr,
                                "VS_Main", "vs_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &err);
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        if (FAILED(hr)) return false;
        hr = dev_->CreateVertexShader(code->GetBufferPointer(),
                                      code->GetBufferSize(), nullptr, &vs_);
        code->Release();
        if (FAILED(hr)) return false;
    }

    // Pixel shaders
    static const char* kPSSrc[] = { kPS1, kPS2, kPS3, kPS4, kPS5, kPS6, kPS7 };
    for (int i = 0; i < kModes; ++i) {
        std::string full = BuildFullPS(kPSSrc[i]);
        if (!CompilePS(i, full.c_str())) return false;
    }
    return true;
}

// ── D3DRenderer::Resize ───────────────────────────────────────────────────────

void D3DRenderer::Resize(int x, int y, int w, int h)
{
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (!ready_ || (w == w_ && h == h_)) return;

    ReleaseRTV();
    swap_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    CreateRTV();
    w_ = w; h_ = h;
}

// ── D3DRenderer::RenderFrame ──────────────────────────────────────────────────

void D3DRenderer::RenderFrame(int mode, const ShaderCB& cbData)
{
    if (!ready_ || mode < 1 || mode > kModes) return;

    ctx_->UpdateSubresource(cb_, 0, nullptr, &cbData, 0, 0);

    const float kClear[4] = { 0.04f, 0.04f, 0.07f, 1.f };
    ctx_->ClearRenderTargetView(rtv_, kClear);
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);

    D3D11_VIEWPORT vp{ 0.f, 0.f, (float)w_, (float)h_, 0.f, 1.f };
    ctx_->RSSetViewports(1, &vp);

    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetShader(ps_[mode - 1], nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);

    ctx_->Draw(3, 0);   // fullscreen triangle, no vertex buffer

    swap_->Present(0, 0);
}

// ── D3DRenderer::Shutdown ─────────────────────────────────────────────────────

void D3DRenderer::Shutdown()
{
    ready_ = false;
    if (ctx_) ctx_->ClearState();
    for (int i = 0; i < kModes; ++i) SafeRelease(ps_[i]);
    SafeRelease(vs_);
    SafeRelease(cb_);
    ReleaseRTV();
    SafeRelease(swap_);
    SafeRelease(ctx_);
    SafeRelease(dev_);
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    w_ = h_ = 0;
}
