#include "PCH.hpp"

#include "SceneMeter.hpp"

#include <SKSE/SKSE.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace SceneMeter
{
namespace
{
ComPtr<ID3D11Device> s_Device;
ComPtr<ID3D11DeviceContext> s_Context;

// Full-res mip pyramid the backbuffer is copied into each capture.
ComPtr<ID3D11Texture2D> s_MipTex;
ComPtr<ID3D11ShaderResourceView> s_MipSRV;

// Staging ring: written on capture, mapped 2 frames later so the CPU never
// waits on the GPU.
constexpr int RING_SIZE = 3;
ComPtr<ID3D11Texture2D> s_Staging[RING_SIZE];
bool s_StagingPending[RING_SIZE] = {};
uint64_t s_CaptureCounter = 0;

// Source description the lazy resources were built for.  A mismatch
// (resize, ENB format flip) tears them down and rebuilds.
D3D11_TEXTURE2D_DESC s_SourceDesc{};
bool s_ResourcesReady = false;

// Selected mip + CPU grid.
UINT s_GridMip = 0;
UINT s_GridW = 0;
UINT s_GridH = 0;
std::vector<float> s_GridLum;  // gridW * gridH
std::vector<float> s_GridRGB;  // gridW * gridH * 3
bool s_GridValid = false;

bool s_Initialized = false;
bool s_Failed = false;  // latched hard-failure: feature off, logged once

void FailOnce(const char* reason)
{
    if (!s_Failed)
    {
        s_Failed = true;
        logger::warn("SceneMeter: disabled - {}", reason);
    }
}

// IEEE 754 half -> float (staging readback of R16G16B16A16_FLOAT).
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    if (exp == 0)
    {
        if (mant == 0)
        {
            const uint32_t bits = sign;
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        // Subnormal: normalize.
        while ((mant & 0x400u) == 0)
        {
            mant <<= 1;
            --exp;
        }
        ++exp;
        mant &= 0x3FFu;
    }
    else if (exp == 31)
    {
        exp = 255 - 112;  // Inf/NaN -> big float; callers clamp
    }
    const uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Decode one texel of a supported backbuffer format to linear-ish [0,1] RGB.
// Returns false for unsupported formats (checked once at resource build).
bool DecodeTexel(DXGI_FORMAT fmt, const uint8_t* p, float rgb[3])
{
    switch (fmt)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            rgb[0] = p[0] / 255.0f;
            rgb[1] = p[1] / 255.0f;
            rgb[2] = p[2] / 255.0f;
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            rgb[0] = p[2] / 255.0f;
            rgb[1] = p[1] / 255.0f;
            rgb[2] = p[0] / 255.0f;
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            rgb[0] = static_cast<float>(v & 0x3FFu) / 1023.0f;
            rgb[1] = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
            rgb[2] = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        {
            uint16_t h[3];
            std::memcpy(h, p, 6);
            rgb[0] = std::clamp(HalfToFloat(h[0]), .0f, 1.0f);
            rgb[1] = std::clamp(HalfToFloat(h[1]), .0f, 1.0f);
            rgb[2] = std::clamp(HalfToFloat(h[2]), .0f, 1.0f);
            return true;
        }
        default:
            return false;
    }
}

UINT BytesPerTexel(DXGI_FORMAT fmt)
{
    return fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
}

// (Re)build the mip pyramid + staging ring for the given backbuffer desc.
bool BuildResources(const D3D11_TEXTURE2D_DESC& src)
{
    s_MipTex.Reset();
    s_MipSRV.Reset();
    for (auto& st : s_Staging)
    {
        st.Reset();
    }
    for (auto& pending : s_StagingPending)
    {
        pending = false;
    }
    s_GridValid = false;
    s_ResourcesReady = false;

    if (src.SampleDesc.Count > 1)
    {
        FailOnce("multisampled backbuffer");
        return false;
    }
    float probe[3];
    uint8_t zeros[8] = {};
    if (!DecodeTexel(src.Format, zeros, probe))
    {
        FailOnce("unsupported backbuffer format");
        return false;
    }
    UINT support = 0;
    if (FAILED(s_Device->CheckFormatSupport(src.Format, &support)) ||
        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) == 0)
    {
        FailOnce("no mip autogen for backbuffer format");
        return false;
    }

    // Descend to the first mip at or below 64 texels wide: 60 at 1920 and
    // 3840, 40 at 2560.  The h > 2 guard stops extreme aspect ratios from
    // collapsing the grid height.
    UINT mip = 0;
    UINT w = src.Width;
    UINT h = src.Height;
    while (w > 64 && h > 2)
    {
        w = (std::max)(1u, w >> 1);
        h = (std::max)(1u, h >> 1);
        ++mip;
    }

    D3D11_TEXTURE2D_DESC mipDesc{};
    mipDesc.Width = src.Width;
    mipDesc.Height = src.Height;
    mipDesc.MipLevels = 0;  // full chain
    mipDesc.ArraySize = 1;
    mipDesc.Format = src.Format;
    mipDesc.SampleDesc.Count = 1;
    mipDesc.Usage = D3D11_USAGE_DEFAULT;
    mipDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    mipDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(s_Device->CreateTexture2D(&mipDesc, nullptr, s_MipTex.GetAddressOf())) ||
        FAILED(
            s_Device->CreateShaderResourceView(s_MipTex.Get(), nullptr, s_MipSRV.GetAddressOf())))
    {
        FailOnce("mip pyramid creation failed");
        return false;
    }

    D3D11_TEXTURE2D_DESC stDesc{};
    stDesc.Width = w;
    stDesc.Height = h;
    stDesc.MipLevels = 1;
    stDesc.ArraySize = 1;
    stDesc.Format = src.Format;
    stDesc.SampleDesc.Count = 1;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& st : s_Staging)
    {
        if (FAILED(s_Device->CreateTexture2D(&stDesc, nullptr, st.GetAddressOf())))
        {
            FailOnce("staging ring creation failed");
            return false;
        }
    }

    s_SourceDesc = src;
    s_GridMip = mip;
    s_GridW = w;
    s_GridH = h;
    s_GridLum.assign(static_cast<size_t>(w) * h, .5f);
    s_GridRGB.assign(static_cast<size_t>(w) * h * 3, .5f);
    s_ResourcesReady = true;
    logger::info("SceneMeter: metering {}x{} backbuffer via mip {} ({}x{} grid)",
                 src.Width,
                 src.Height,
                 mip,
                 w,
                 h);
    return true;
}
}  // namespace

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context)
    {
        return false;
    }
    s_Device = device;
    s_Context = context;
    s_Initialized = true;
    s_Failed = false;
    return true;
}

bool IsInitialized()
{
    return s_Initialized && !s_Failed;
}

void Shutdown()
{
    s_MipTex.Reset();
    s_MipSRV.Reset();
    for (auto& st : s_Staging)
    {
        st.Reset();
    }
    s_Context.Reset();
    s_Device.Reset();
    s_Initialized = false;
    s_Failed = false;
    s_ResourcesReady = false;
    s_GridValid = false;
}

void OnResize(uint32_t width, uint32_t height)
{
    // Resources rebuild lazily against the actual bound backbuffer (the only
    // authority on format under ENB/upscaler stacks); only a genuine size
    // change needs to invalidate them here.
    if (s_ResourcesReady && (width != s_SourceDesc.Width || height != s_SourceDesc.Height))
    {
        s_ResourcesReady = false;
        s_GridValid = false;
    }
}

void CaptureCallback(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Initialized || s_Failed || !s_Context)
    {
        return;
    }

    // Resolve the currently bound render target (the composed scene).
    ComPtr<ID3D11RenderTargetView> rtv;
    s_Context->OMGetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    if (!rtv)
    {
        return;
    }
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex)))
    {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    if (!s_ResourcesReady || desc.Width != s_SourceDesc.Width ||
        desc.Height != s_SourceDesc.Height || desc.Format != s_SourceDesc.Format)
    {
        if (!BuildResources(desc))
        {
            return;
        }
    }

    // Downsample: copy -> mip chain -> tiny mip -> staging ring slot.
    // Copies and GenerateMips leave the application pipeline state intact,
    // so this callback needs no ResetRenderState.
    s_Context->CopySubresourceRegion(s_MipTex.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
    s_Context->GenerateMips(s_MipSRV.Get());

    const int slot = static_cast<int>(s_CaptureCounter % RING_SIZE);
    s_Context->CopySubresourceRegion(
        s_Staging[slot].Get(), 0, 0, 0, 0, s_MipTex.Get(), s_GridMip, nullptr);
    s_StagingPending[slot] = true;
    ++s_CaptureCounter;
}

void CollectResults()
{
    if (!s_Initialized || s_Failed || !s_ResourcesReady || s_CaptureCounter == 0)
    {
        return;
    }

    // Map the oldest pending slot - written RING_SIZE-1 captures ago, so the
    // GPU has almost certainly finished with it.  Never wait.
    const int slot = static_cast<int>((s_CaptureCounter + 1) % RING_SIZE);
    if (!s_StagingPending[slot])
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = s_Context->Map(
        s_Staging[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
    {
        return;  // keep last frame's grid
    }
    if (FAILED(hr))
    {
        return;
    }

    const UINT bpp = BytesPerTexel(s_SourceDesc.Format);
    for (UINT y = 0; y < s_GridH; ++y)
    {
        const auto* row =
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < s_GridW; ++x)
        {
            float rgb[3];
            if (!DecodeTexel(s_SourceDesc.Format, row + static_cast<size_t>(x) * bpp, rgb))
            {
                continue;
            }
            const size_t idx = static_cast<size_t>(y) * s_GridW + x;
            s_GridLum[idx] =
                std::clamp(.2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2], .0f, 1.0f);
            s_GridRGB[idx * 3 + 0] = rgb[0];
            s_GridRGB[idx * 3 + 1] = rgb[1];
            s_GridRGB[idx * 3 + 2] = rgb[2];
        }
    }
    s_Context->Unmap(s_Staging[slot].Get(), 0);
    s_StagingPending[slot] = false;
    s_GridValid = true;
}

bool Sample(float x01, float y01, float& outLum, float outRGB[3])
{
    if (!s_GridValid || s_GridW < 2 || s_GridH < 2)
    {
        return false;
    }

    const float fx = std::clamp(x01, .0f, 1.0f) * static_cast<float>(s_GridW - 1);
    const float fy = std::clamp(y01, .0f, 1.0f) * static_cast<float>(s_GridH - 1);
    const UINT x0 = static_cast<UINT>(fx);
    const UINT y0 = static_cast<UINT>(fy);
    const UINT x1 = (std::min)(x0 + 1, s_GridW - 1);
    const UINT y1 = (std::min)(y0 + 1, s_GridH - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const auto lerp2 = [&](const float* grid, size_t stride, size_t channel)
    {
        const auto at = [&](UINT x, UINT y)
        { return grid[(static_cast<size_t>(y) * s_GridW + x) * stride + channel]; };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
        return top + (bottom - top) * ty;
    };

    outLum = lerp2(s_GridLum.data(), 1, 0);
    outRGB[0] = lerp2(s_GridRGB.data(), 3, 0);
    outRGB[1] = lerp2(s_GridRGB.data(), 3, 1);
    outRGB[2] = lerp2(s_GridRGB.data(), 3, 2);
    return true;
}
}  // namespace SceneMeter
