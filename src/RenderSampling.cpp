#include <SKSE/SKSE.h>

#include "RenderSampling.hpp"

#include "RasterQuality.hpp"

#include <wrl/client.h>

#include <atomic>
#include <utility>
#include <vector>

namespace RenderSampling
{
using Microsoft::WRL::ComPtr;

namespace
{
struct SavedSamplerState
{
    ComPtr<ID3D11SamplerState> sampler;
    bool active = false;
};

struct SamplingState
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11SamplerState> fontSampler;
    ComPtr<ID3D11SamplerState> badgeSampler;
    std::vector<SavedSamplerState> stack;
    bool attempted = false;
};

SamplingState& State()
{
    static SamplingState state;
    return state;
}

std::atomic<bool>& FailureLogged()
{
    static std::atomic<bool> logged{false};
    return logged;
}

bool IsReady()
{
    const auto& state = State();
    return state.context && state.fontSampler && state.badgeSampler;
}

D3D11_SAMPLER_DESC MakeSamplerDescription(float maxLod)
{
    D3D11_SAMPLER_DESC description{};
    description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    description.MinLOD = .0f;
    description.MaxLOD = maxLod;
    return description;
}

void PushSamplerCallback(const ImDrawList*, const ImDrawCmd* command)
{
    auto& state = State();
    auto* sampler =
        reinterpret_cast<ID3D11SamplerState*>(command ? command->UserCallbackData : nullptr);
    SavedSamplerState saved{};
    if (state.context && sampler)
    {
        state.context->PSGetSamplers(0, 1, saved.sampler.GetAddressOf());
        saved.active = true;
        state.context->PSSetSamplers(0, 1, &sampler);
    }
    state.stack.push_back(std::move(saved));
}

void PopSamplerCallback(const ImDrawList*, const ImDrawCmd*)
{
    auto& state = State();
    if (state.stack.empty())
    {
        return;
    }

    SavedSamplerState saved = std::move(state.stack.back());
    state.stack.pop_back();
    if (state.context && saved.active)
    {
        ID3D11SamplerState* sampler = saved.sampler.Get();
        state.context->PSSetSamplers(0, 1, &sampler);
    }
}

void AddSamplerPush(ImDrawList* drawList, ID3D11SamplerState* sampler)
{
    if (drawList && IsReady() && sampler)
    {
        drawList->AddCallback(PushSamplerCallback, sampler);
    }
}
}  // namespace

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context)
    {
        return false;
    }

    auto& state = State();
    if (state.attempted && state.device.Get() == device)
    {
        return IsReady();
    }

    state.stack.clear();
    state.fontSampler.Reset();
    state.badgeSampler.Reset();
    state.context = context;
    state.device = device;
    state.attempted = true;

    const D3D11_SAMPLER_DESC fontDescription =
        MakeSamplerDescription(static_cast<float>(RasterQuality::FONT_MIP_LIMIT));
    const D3D11_SAMPLER_DESC badgeDescription = MakeSamplerDescription(D3D11_FLOAT32_MAX);

    ComPtr<ID3D11SamplerState> fontSampler;
    ComPtr<ID3D11SamplerState> badgeSampler;
    const HRESULT fontResult =
        device->CreateSamplerState(&fontDescription, fontSampler.GetAddressOf());
    const HRESULT badgeResult =
        device->CreateSamplerState(&badgeDescription, badgeSampler.GetAddressOf());
    if (FAILED(fontResult) || FAILED(badgeResult))
    {
        if (!FailureLogged().exchange(true, std::memory_order_acq_rel))
        {
            SKSE::log::warn(
                "RenderSampling: quality sampler creation failed (font=0x{:08X}, "
                "badge=0x{:08X}); retaining ImGui sampling",
                static_cast<unsigned>(fontResult),
                static_cast<unsigned>(badgeResult));
        }
        return false;
    }

    state.fontSampler = std::move(fontSampler);
    state.badgeSampler = std::move(badgeSampler);
    return true;
}

void Shutdown()
{
    auto& state = State();
    state.stack.clear();
    state.fontSampler.Reset();
    state.badgeSampler.Reset();
    state.context.Reset();
    state.device.Reset();
    state.attempted = false;
}

void PushFontSampler(ImDrawList* drawList)
{
    AddSamplerPush(drawList, State().fontSampler.Get());
}

void PushBadgeSampler(ImDrawList* drawList)
{
    AddSamplerPush(drawList, State().badgeSampler.Get());
}

void PopSampler(ImDrawList* drawList)
{
    if (drawList && IsReady())
    {
        drawList->AddCallback(PopSamplerCallback, nullptr);
    }
}
}  // namespace RenderSampling
