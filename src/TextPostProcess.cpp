#include <SKSE/SKSE.h>

#include "TextPostProcess.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace logger = SKSE::log;

namespace TextPostProcess
{

// ============================================================================
// Embedded HLSL sources
// ============================================================================

static const char* kFullscreenVS_HLSL = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

static const char* kGaussianBlurPS_HLSL = R"(
Texture2D    InputTex : register(t0);
SamplerState Sampler  : register(s0);
cbuffer BlurCB : register(b0) {
    float2 TexelDir;
    float  Sigma;
    float  _Pad;
};
static const int HALF_KERNEL = 16;
float Gauss(float x, float s) { return exp(-0.5 * (x * x) / (s * s)); }
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Clamp sigma so the 16-tap half-kernel always covers at least 3 sigma.
    // Prevents boxy edges when GlowRadius is large.
    float sigma = clamp(Sigma, 0.001, HALF_KERNEL / 3.0);
    float4 sum  = InputTex.Sample(Sampler, uv) * Gauss(0, sigma);
    float  wSum = Gauss(0, sigma);
    [unroll] for (int i = 1; i <= HALF_KERNEL; ++i) {
        float w = Gauss((float)i, sigma);
        sum  += InputTex.Sample(Sampler, uv + TexelDir * (float)i) * w;
        sum  += InputTex.Sample(Sampler, uv - TexelDir * (float)i) * w;
        wSum += w * 2.0;
    }
    return sum / wSum;
}
)";

static const char* kDividePS_HLSL = R"(
Texture2D    TextRT   : register(t0);
Texture2D    Snapshot : register(t1);
SamplerState Sampler  : register(s0);
cbuffer DivideCB : register(b0) {
    float Strength;
    float3 _Pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 text = TextRT.Sample(Sampler, uv);
    float4 bg   = Snapshot.Sample(Sampler, uv);
    if (text.a < 0.001) discard;

    // Luminance of the text pixel - dark outlines and shadows composite
    // normally so they do not invert to bright.
    float lum = dot(text.rgb, float3(0.299, 0.587, 0.114));
    float divideMask = smoothstep(0.10, 0.25, lum);

    float3 divided = float3(
        text.r > 0.001 ? saturate(bg.r / text.r) : 1.0,
        text.g > 0.001 ? saturate(bg.g / text.g) : 1.0,
        text.b > 0.001 ? saturate(bg.b / text.b) : 1.0
    );

    // Normal alpha composite as fallback for dark pixels
    float3 normal = lerp(bg.rgb, text.rgb, text.a);

    // Blend: dark pixels use normal composite, bright pixels use divide
    float effectiveStrength = Strength * divideMask;
    float3 result = lerp(normal, divided, effectiveStrength);
    return float4(result, 1.0);
}
)";

static const char* kCompositePS_HLSL = R"(
Texture2D    InputTex : register(t0);
SamplerState Sampler  : register(s0);
cbuffer CompositeCB : register(b0) {
    float Intensity;
    float3 _Pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = InputTex.Sample(Sampler, uv);
    // Intensity scales the captured color only, with no added white term,
    // so the captured hue is preserved.
    float3 bloom = saturate(c.rgb * (0.46 + Intensity * 0.34));
    float veilAlpha = saturate(pow(c.a, 0.72) * (0.14 + Intensity * 0.16));
    float coreExponent = lerp(1.18, 1.78, Intensity);
    float coreAlpha = saturate(pow(c.a, coreExponent) * (0.34 + Intensity * 0.18));
    float glowAlpha = saturate(veilAlpha + coreAlpha * 0.55);
    return float4(bloom, glowAlpha);
}
)";

// ============================================================================
// Constant buffer layouts
// ============================================================================

struct BlurConstants
{
    float texelDirX, texelDirY;
    float sigma;
    float pad;
};

struct CompositeConstants
{
    float intensity;
    float pad[3];
};

struct DivideConstants
{
    float strength;
    float pad[3];
};

// ============================================================================
// File-scope resources
// ============================================================================

static std::atomic<bool> s_Initialized{false};
static std::mutex& InitMutex()
{
    static std::mutex m;
    return m;
}

static ComPtr<ID3D11Device> s_Device;
static ComPtr<ID3D11DeviceContext> s_Context;

// Ping-pong render targets (half-res)
static ComPtr<ID3D11Texture2D> s_RT_A;
static ComPtr<ID3D11RenderTargetView> s_RTV_A;
static ComPtr<ID3D11ShaderResourceView> s_SRV_A;
static ComPtr<ID3D11Texture2D> s_RT_B;
static ComPtr<ID3D11RenderTargetView> s_RTV_B;
static ComPtr<ID3D11ShaderResourceView> s_SRV_B;

// Shaders
static ComPtr<ID3D11VertexShader> s_FullscreenVS;
static ComPtr<ID3D11PixelShader> s_BlurPS;
static ComPtr<ID3D11PixelShader> s_CompositePS;

// Pipeline objects
static ComPtr<ID3D11Buffer> s_BlurCB;
static ComPtr<ID3D11Buffer> s_CompositeCB;
static ComPtr<ID3D11SamplerState> s_LinearClampSampler;
static ComPtr<ID3D11BlendState> s_AdditiveBlend;  // Created, but no pass binds it
static ComPtr<ID3D11BlendState> s_ScreenBlend;
static ComPtr<ID3D11RasterizerState> s_NoCullRS;

// Saved main RT (between Begin/End callbacks)
static ComPtr<ID3D11RenderTargetView> s_SavedRTV;
static ComPtr<ID3D11DepthStencilView> s_SavedDSV;
static D3D11_VIEWPORT s_SavedViewport{};
static UINT s_SavedViewportCount = 0;

// RT dimensions
static uint32_t s_BlurWidth = 0;
static uint32_t s_BlurHeight = 0;
static uint32_t s_FullWidth = 0;
static uint32_t s_FullHeight = 0;

// Render target format, chosen at init from device format support
static DXGI_FORMAT s_RTFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Divide capture resources (full-res)
static ComPtr<ID3D11Texture2D> s_RT_Divide;
static ComPtr<ID3D11RenderTargetView> s_RTV_Divide;
static ComPtr<ID3D11ShaderResourceView> s_SRV_Divide;
static ComPtr<ID3D11Texture2D> s_RT_Snapshot;  // backbuffer copy (SRV only)
static ComPtr<ID3D11ShaderResourceView> s_SRV_Snapshot;
static ComPtr<ID3D11PixelShader> s_DividePS;
static ComPtr<ID3D11Buffer> s_DivideCB;

// Saved state for divide capture (separate from glow's saved state)
static ComPtr<ID3D11RenderTargetView> s_DivideSavedRTV;
static ComPtr<ID3D11DepthStencilView> s_DivideSavedDSV;
static D3D11_VIEWPORT s_DivideSavedViewport{};
static UINT s_DivideSavedViewportCount = 0;

// Glow params (set per-frame)
static float s_GlowRadius = 4.0f;
static float s_GlowIntensity = .5f;

// Divide params (set per-frame)
static float s_DivideStrength = .0f;

// ============================================================================
// Helpers
// ============================================================================

static ComPtr<ID3DBlob> CompileShader(const char* source, const char* target, const char* entry)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source,
                            strlen(source),
                            nullptr,
                            nullptr,
                            nullptr,
                            entry,
                            target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0,
                            blob.GetAddressOf(),
                            errors.GetAddressOf());
    if (FAILED(hr))
    {
        if (errors)
        {
            logger::error("TextPostProcess: Shader compile failed: {}",
                          static_cast<const char*>(errors->GetBufferPointer()));
        }
        return nullptr;
    }
    return blob;
}

static bool CreateRenderTarget(uint32_t w,
                               uint32_t h,
                               ComPtr<ID3D11Texture2D>& tex,
                               ComPtr<ID3D11RenderTargetView>& rtv,
                               ComPtr<ID3D11ShaderResourceView>& srv)
{
    tex.Reset();
    rtv.Reset();
    srv.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = s_RTFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = s_Device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    if (FAILED(hr))
        return false;

    hr = s_Device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr))
        return false;

    hr = s_Device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
    if (FAILED(hr))
        return false;

    return true;
}

static void ReleaseResources()
{
    s_RT_A.Reset();
    s_RTV_A.Reset();
    s_SRV_A.Reset();
    s_RT_B.Reset();
    s_RTV_B.Reset();
    s_SRV_B.Reset();
    s_FullscreenVS.Reset();
    s_BlurPS.Reset();
    s_CompositePS.Reset();
    s_DividePS.Reset();
    s_BlurCB.Reset();
    s_CompositeCB.Reset();
    s_DivideCB.Reset();
    s_LinearClampSampler.Reset();
    s_AdditiveBlend.Reset();
    s_ScreenBlend.Reset();
    s_NoCullRS.Reset();
    s_SavedRTV.Reset();
    s_SavedDSV.Reset();
    s_RT_Divide.Reset();
    s_RTV_Divide.Reset();
    s_SRV_Divide.Reset();
    s_RT_Snapshot.Reset();
    s_SRV_Snapshot.Reset();
    s_DivideSavedRTV.Reset();
    s_DivideSavedDSV.Reset();
    s_Context.Reset();
    s_Device.Reset();
    s_BlurWidth = 0;
    s_BlurHeight = 0;
    s_FullWidth = 0;
    s_FullHeight = 0;
    s_Initialized = false;
}

// Draw a fullscreen triangle (no vertex buffer, 3 vertices from SV_VertexID)
static void DrawFullscreenTriangle()
{
    s_Context->IASetInputLayout(nullptr);
    s_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_Context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    s_Context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    s_Context->Draw(3, 0);
}

// ============================================================================
// Public API
// ============================================================================

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    const std::lock_guard<std::mutex> lock(InitMutex());
    if (s_Initialized.load(std::memory_order_acquire))
        return true;

    if (!device || !context)
        return false;

    s_Device = device;
    s_Context = context;

    // Check for R16G16B16A16_FLOAT render target support; fall back to R8G8B8A8_UNORM
    UINT fmtSupport = 0;
    if (SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &fmtSupport)) &&
        (fmtSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET) &&
        (fmtSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
    {
        s_RTFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    else
    {
        s_RTFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        logger::warn(
            "TextPostProcess: R16G16B16A16_FLOAT not supported, falling back to R8G8B8A8_UNORM");
    }

    // Compile shaders
    auto vsBlob = CompileShader(kFullscreenVS_HLSL, "vs_5_0", "main");
    auto blurBlob = CompileShader(kGaussianBlurPS_HLSL, "ps_5_0", "main");
    auto compositeBlob = CompileShader(kCompositePS_HLSL, "ps_5_0", "main");
    auto divideBlob = CompileShader(kDividePS_HLSL, "ps_5_0", "main");

    if (!vsBlob || !blurBlob || !compositeBlob || !divideBlob)
    {
        logger::error("TextPostProcess: Failed to compile one or more shaders");
        ReleaseResources();
        return false;
    }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(),
                                    nullptr,
                                    s_FullscreenVS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create vertex shader");
        ReleaseResources();
        return false;
    }

    hr = device->CreatePixelShader(
        blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(), nullptr, s_BlurPS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create blur pixel shader");
        ReleaseResources();
        return false;
    }

    hr = device->CreatePixelShader(compositeBlob->GetBufferPointer(),
                                   compositeBlob->GetBufferSize(),
                                   nullptr,
                                   s_CompositePS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create composite pixel shader");
        ReleaseResources();
        return false;
    }

    hr = device->CreatePixelShader(divideBlob->GetBufferPointer(),
                                   divideBlob->GetBufferSize(),
                                   nullptr,
                                   s_DividePS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create divide pixel shader");
        ReleaseResources();
        return false;
    }

    // Constant buffers
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(BlurConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, nullptr, s_BlurCB.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    cbDesc.ByteWidth = sizeof(CompositeConstants);
    hr = device->CreateBuffer(&cbDesc, nullptr, s_CompositeCB.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    cbDesc.ByteWidth = sizeof(DivideConstants);
    hr = device->CreateBuffer(&cbDesc, nullptr, s_DivideCB.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // Linear clamp sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxAnisotropy = 1;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, s_LinearClampSampler.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // Additive blend state
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blendDesc, s_AdditiveBlend.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // Screen-style blend state: output = src.rgb * src.a + dst.rgb * (1 - src.rgb).
    // The destination factor falls with the source color, so a bright
    // background cannot clip to white the way pure additive does.
    D3D11_BLEND_DESC screenDesc{};
    screenDesc.RenderTarget[0].BlendEnable = TRUE;
    screenDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    screenDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    screenDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    screenDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    screenDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    screenDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    screenDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&screenDesc, s_ScreenBlend.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // No-cull rasterizer state for fullscreen triangle
    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.ScissorEnable = FALSE;
    rsDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rsDesc, s_NoCullRS.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    s_Initialized.store(true, std::memory_order_release);
    logger::info("TextPostProcess: Initialized (shaders compiled, pipeline ready)");
    return true;
}

bool IsInitialized()
{
    return s_Initialized.load(std::memory_order_acquire);
}

void Shutdown()
{
    const std::lock_guard<std::mutex> lock(InitMutex());
    if (!s_Initialized.load(std::memory_order_acquire))
        return;
    logger::info("TextPostProcess: Shutting down");
    ReleaseResources();
}

void OnResize(uint32_t width, uint32_t height)
{
    if (!s_Initialized.load(std::memory_order_acquire))
        return;
    if (width == 0 || height == 0)
        return;
    if (width == s_FullWidth && height == s_FullHeight)
        return;

    // The accepted size is recorded before creation, so the size test above also
    // suppresses a retry: a target that fails here stays missing, and its bracket
    // stays a no-op, until the resolution changes again.
    s_FullWidth = width;
    s_FullHeight = height;
    s_BlurWidth = (std::max)(width / 2u, 1u);
    s_BlurHeight = (std::max)(height / 2u, 1u);

    bool ok = CreateRenderTarget(s_BlurWidth, s_BlurHeight, s_RT_A, s_RTV_A, s_SRV_A) &&
              CreateRenderTarget(s_BlurWidth, s_BlurHeight, s_RT_B, s_RTV_B, s_SRV_B);
    if (!ok)
    {
        logger::error(
            "TextPostProcess: Failed to create render targets ({}x{})", s_BlurWidth, s_BlurHeight);
    }
    else
    {
        logger::info("TextPostProcess: Render targets created ({}x{} 1/2-res from {}x{})",
                     s_BlurWidth,
                     s_BlurHeight,
                     width,
                     height);
    }

    // Full-res divide capture RT (uses our HDR format for nametag rendering)
    bool divOk = CreateRenderTarget(width, height, s_RT_Divide, s_RTV_Divide, s_SRV_Divide);
    if (divOk)
    {
        // Snapshot must match the backbuffer format for CopyResource.
        // Query the actual backbuffer format from the main render target.
        // This depends on the caller: the game's main RT must be bound now, or
        // the snapshot keeps the fallback format below, and the copy in
        // BeginDivideCapture is then rejected unless that format happens to match.
        DXGI_FORMAT bbFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // safe default
        {
            ComPtr<ID3D11RenderTargetView> curRTV;
            s_Context->OMGetRenderTargets(1, curRTV.GetAddressOf(), nullptr);
            if (curRTV)
            {
                ComPtr<ID3D11Resource> res;
                curRTV->GetResource(res.GetAddressOf());
                ComPtr<ID3D11Texture2D> tex;
                if (res && SUCCEEDED(res.As(&tex)))
                {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    bbFormat = td.Format;
                }
            }
        }
        s_RT_Snapshot.Reset();
        s_SRV_Snapshot.Reset();
        D3D11_TEXTURE2D_DESC snapDesc{};
        snapDesc.Width = width;
        snapDesc.Height = height;
        snapDesc.MipLevels = 1;
        snapDesc.ArraySize = 1;
        snapDesc.Format = bbFormat;
        snapDesc.SampleDesc.Count = 1;
        snapDesc.Usage = D3D11_USAGE_DEFAULT;
        snapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr2 = s_Device->CreateTexture2D(&snapDesc, nullptr, s_RT_Snapshot.GetAddressOf());
        if (SUCCEEDED(hr2))
        {
            hr2 = s_Device->CreateShaderResourceView(
                s_RT_Snapshot.Get(), nullptr, s_SRV_Snapshot.GetAddressOf());
        }
        divOk = SUCCEEDED(hr2);
    }
    if (!divOk)
    {
        logger::warn(
            "TextPostProcess: Failed to create divide render targets ({}x{})", width, height);
    }
}

void SetGlowParams(float radius, float intensity)
{
    s_GlowRadius = radius;
    s_GlowIntensity = intensity;
}

// ============================================================================
// ImDrawCallbacks
// ============================================================================

void BeginGlowCapture(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Context || !s_RTV_A)
        return;

    // Save current render target and viewport
    s_SavedRTV.Reset();
    s_SavedDSV.Reset();
    s_Context->OMGetRenderTargets(1, s_SavedRTV.GetAddressOf(), s_SavedDSV.GetAddressOf());
    s_SavedViewportCount = 1;
    s_Context->RSGetViewports(&s_SavedViewportCount, &s_SavedViewport);

    // Switch to glow RT A (half-res)
    ID3D11RenderTargetView* rtv = s_RTV_A.Get();
    s_Context->OMSetRenderTargets(1, &rtv, nullptr);

    const float clearColor[4] = {0, 0, 0, 0};
    s_Context->ClearRenderTargetView(s_RTV_A.Get(), clearColor);

    // Set half-res viewport so ImGui text maps correctly
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(s_BlurWidth);
    vp.Height = static_cast<float>(s_BlurHeight);
    vp.MaxDepth = 1.0f;
    s_Context->RSSetViewports(1, &vp);
}

void EndGlowAndComposite(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Context || !s_RTV_A || !s_RTV_B || !s_SavedRTV)
        return;

    // RT A holds the glow text at half resolution.  Apply a separable
    // Gaussian blur: A -> B (horizontal), B -> A (vertical).

    // Sigma is in half-res texels, and one texel spans two backbuffer pixels,
    // so 0.48 gives about one full-res pixel of sigma per pixel of radius.  A
    // Gaussian reaches about 3 sigma, so the glow spreads roughly three times
    // the requested radius and stays diffuse instead of resolving as a hard
    // plate behind the text.  The shader clamps sigma to 16/3 texels.
    const float sigma = (std::max)(1.0f, s_GlowRadius * .48f);

    // Unbind RT A as target, we'll read from it
    ID3D11RenderTargetView* nullRTV = nullptr;
    s_Context->OMSetRenderTargets(1, &nullRTV, nullptr);

    // Common state for blur passes
    s_Context->VSSetShader(s_FullscreenVS.Get(), nullptr, 0);
    s_Context->PSSetShader(s_BlurPS.Get(), nullptr, 0);
    s_Context->RSSetState(s_NoCullRS.Get());

    ID3D11SamplerState* sampler = s_LinearClampSampler.Get();
    s_Context->PSSetSamplers(0, 1, &sampler);

    // Disable blending for blur passes (overwrite)
    const float blendFactor[4] = {0, 0, 0, 0};
    s_Context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

    D3D11_VIEWPORT halfVP{};
    halfVP.Width = static_cast<float>(s_BlurWidth);
    halfVP.Height = static_cast<float>(s_BlurHeight);
    halfVP.MaxDepth = 1.0f;
    s_Context->RSSetViewports(1, &halfVP);

    // --- Horizontal blur: read A -> write B ---
    {
        const float clearColor[4] = {0, 0, 0, 0};
        s_Context->ClearRenderTargetView(s_RTV_B.Get(), clearColor);

        ID3D11RenderTargetView* rtv = s_RTV_B.Get();
        s_Context->OMSetRenderTargets(1, &rtv, nullptr);

        ID3D11ShaderResourceView* srv = s_SRV_A.Get();
        s_Context->PSSetShaderResources(0, 1, &srv);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(s_Context->Map(s_BlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* cb = static_cast<BlurConstants*>(mapped.pData);
            cb->texelDirX = 1.0f / static_cast<float>(s_BlurWidth);
            cb->texelDirY = 0;
            cb->sigma = sigma;
            cb->pad = 0;
            s_Context->Unmap(s_BlurCB.Get(), 0);
        }

        ID3D11Buffer* cb = s_BlurCB.Get();
        s_Context->PSSetConstantBuffers(0, 1, &cb);
        DrawFullscreenTriangle();

        // Unbind SRV to allow B to be read next
        ID3D11ShaderResourceView* nullSRV = nullptr;
        s_Context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Vertical blur: read B -> write A ---
    {
        const float clearColor[4] = {0, 0, 0, 0};
        s_Context->ClearRenderTargetView(s_RTV_A.Get(), clearColor);

        ID3D11RenderTargetView* rtv = s_RTV_A.Get();
        s_Context->OMSetRenderTargets(1, &rtv, nullptr);

        ID3D11ShaderResourceView* srv = s_SRV_B.Get();
        s_Context->PSSetShaderResources(0, 1, &srv);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(s_Context->Map(s_BlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* cb = static_cast<BlurConstants*>(mapped.pData);
            cb->texelDirX = 0;
            cb->texelDirY = 1.0f / static_cast<float>(s_BlurHeight);
            cb->sigma = sigma;
            cb->pad = 0;
            s_Context->Unmap(s_BlurCB.Get(), 0);
        }

        ID3D11Buffer* cb = s_BlurCB.Get();
        s_Context->PSSetConstantBuffers(0, 1, &cb);
        DrawFullscreenTriangle();

        ID3D11ShaderResourceView* nullSRV = nullptr;
        s_Context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Composite: read blurred A -> main RT, screen blend (src + dst * (1 - src)) ---
    {
        // Restore main render target
        ID3D11RenderTargetView* mainRTV = s_SavedRTV.Get();
        s_Context->OMSetRenderTargets(1, &mainRTV, s_SavedDSV.Get());
        s_Context->RSSetViewports(s_SavedViewportCount, &s_SavedViewport);

        s_Context->PSSetShader(s_CompositePS.Get(), nullptr, 0);
        s_Context->OMSetBlendState(s_ScreenBlend.Get(), blendFactor, 0xFFFFFFFF);

        ID3D11ShaderResourceView* srv = s_SRV_A.Get();
        s_Context->PSSetShaderResources(0, 1, &srv);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(s_Context->Map(s_CompositeCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* cb = static_cast<CompositeConstants*>(mapped.pData);
            cb->intensity = s_GlowIntensity;
            cb->pad[0] = cb->pad[1] = cb->pad[2] = 0;
            s_Context->Unmap(s_CompositeCB.Get(), 0);
        }

        ID3D11Buffer* cb = s_CompositeCB.Get();
        s_Context->PSSetConstantBuffers(0, 1, &cb);
        DrawFullscreenTriangle();

        // Unbind SRV
        ID3D11ShaderResourceView* nullSRV = nullptr;
        s_Context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Clean up saved state refs
    s_SavedRTV.Reset();
    s_SavedDSV.Reset();

    // ImDrawCallback_ResetRenderState (added after this callback in the draw list)
    // will restore ImGui's own shader/blend/viewport/sampler state.
    // The render target is already restored above.
}

// ============================================================================
// Color divide callbacks
// ============================================================================

void SetDivideParams(float strength)
{
    s_DivideStrength = strength;
}

void BeginDivideCapture(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Context || !s_RTV_Divide || !s_RT_Snapshot)
        return;

    // Save the real main render target
    s_DivideSavedRTV.Reset();
    s_DivideSavedDSV.Reset();
    s_Context->OMGetRenderTargets(
        1, s_DivideSavedRTV.GetAddressOf(), s_DivideSavedDSV.GetAddressOf());
    s_DivideSavedViewportCount = 1;
    s_Context->RSGetViewports(&s_DivideSavedViewportCount, &s_DivideSavedViewport);

    // Snapshot the current backbuffer content (game world before nametags).
    // CopyResource needs matching dimensions and a compatible format, which
    // holds only while the backbuffer is the bound target.  When the glow
    // bracket already switched to the half-res glow RT, the copy is invalid and
    // the snapshot keeps stale content.
    if (s_DivideSavedRTV)
    {
        ComPtr<ID3D11Resource> mainRes;
        s_DivideSavedRTV->GetResource(mainRes.GetAddressOf());
        if (mainRes && s_RT_Snapshot)
        {
            s_Context->CopyResource(s_RT_Snapshot.Get(), mainRes.Get());
        }
    }

    // Switch to divide capture RT
    ID3D11RenderTargetView* rtv = s_RTV_Divide.Get();
    s_Context->OMSetRenderTargets(1, &rtv, nullptr);

    const float clearColor[4] = {0, 0, 0, 0};
    s_Context->ClearRenderTargetView(s_RTV_Divide.Get(), clearColor);

    // Full-res viewport so ImGui draws map correctly
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(s_FullWidth);
    vp.Height = static_cast<float>(s_FullHeight);
    vp.MaxDepth = 1.0f;
    s_Context->RSSetViewports(1, &vp);
}

void EndDivideAndComposite(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Context || !s_SRV_Divide || !s_SRV_Snapshot || !s_DivideSavedRTV)
        return;

    // Unbind divide RT so we can read from it
    ID3D11RenderTargetView* nullRTV = nullptr;
    s_Context->OMSetRenderTargets(1, &nullRTV, nullptr);

    // Restore the real main render target
    ID3D11RenderTargetView* mainRTV = s_DivideSavedRTV.Get();
    s_Context->OMSetRenderTargets(1, &mainRTV, s_DivideSavedDSV.Get());
    s_Context->RSSetViewports(s_DivideSavedViewportCount, &s_DivideSavedViewport);

    // Set up divide composite pass
    s_Context->VSSetShader(s_FullscreenVS.Get(), nullptr, 0);
    s_Context->PSSetShader(s_DividePS.Get(), nullptr, 0);
    s_Context->RSSetState(s_NoCullRS.Get());

    ID3D11SamplerState* sampler = s_LinearClampSampler.Get();
    s_Context->PSSetSamplers(0, 1, &sampler);

    // No blending - the shader outputs final pixels directly, taking the
    // background from the snapshot instead of from the destination.  It
    // discards captured pixels with alpha below 0.001, so pixels with no text
    // keep the destination content.
    const float blendFactor[4] = {0, 0, 0, 0};
    s_Context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

    // Bind captured nametag (t0) and backbuffer snapshot (t1)
    ID3D11ShaderResourceView* srvs[2] = {s_SRV_Divide.Get(), s_SRV_Snapshot.Get()};
    s_Context->PSSetShaderResources(0, 2, srvs);

    // Update strength constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(s_Context->Map(s_DivideCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        auto* cb = static_cast<DivideConstants*>(mapped.pData);
        cb->strength = s_DivideStrength;
        cb->pad[0] = cb->pad[1] = cb->pad[2] = 0;
        s_Context->Unmap(s_DivideCB.Get(), 0);
    }

    ID3D11Buffer* cb = s_DivideCB.Get();
    s_Context->PSSetConstantBuffers(0, 1, &cb);
    DrawFullscreenTriangle();

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
    s_Context->PSSetShaderResources(0, 2, nullSRVs);

    // Clean up saved state refs
    s_DivideSavedRTV.Reset();
    s_DivideSavedDSV.Reset();
}

}  // namespace TextPostProcess
