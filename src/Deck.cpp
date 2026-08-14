// Deck - one-key character card: capture, composition, and PNG export.
//
// All state lives in one DeckState. It carries no lock because the frame hooks call every
// entry point on the render thread; Shutdown is the exception, called from the device-creation
// thunk. Only the PNG encoder runs elsewhere, behind its own mutex. Four fields drive the
// pipeline, and each stage clears the one before it:
//
//   captureRequested   A key edge was accepted. Renderer must resolve a CardRequest.
//   request            A resolved card waits for a scene copy and a compose pass.
//   sceneCaptured      sceneTexture and sceneSRV hold the portrait source for `request`.
//   readback           The card was drawn; a staging copy waits on an event query.
//
// Every failure path drops the card and shows a toast. No failure path changes the frame's own
// render state: the compose pass restores the render target and the viewport it replaced.
//
// The card is painted with the renderer's own fonts and text effects, so this TU includes
// RendererInternal.hpp for Renderer::GetFontAt and Renderer::ApplyTextEffect. It reads no
// renderer snapshot and dereferences no game object.

#include "PCH.hpp"

#include "Deck.hpp"
#include "DeckPng.hpp"

#include "BadgeTextures.hpp"
#include "ParticleTextures.hpp"
#include "RenderConstants.hpp"
#include "RendererInternal.hpp"
#include "RenderSampling.hpp"

#include <d3dcompiler.h>
#include <imgui_impl_dx11.h>
#include <wrl/client.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

#pragma comment(lib, "d3dcompiler.lib")

namespace Deck
{
namespace
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct EncodeJob
{
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    std::string outputFolder;
    std::string actorName;
    std::uint32_t formID = 0;
};

struct EncodeResult
{
    bool ok = false;
    std::string path;
    std::string error;
};

static std::string HResultText(const char* action, HRESULT hr)
{
    std::ostringstream out;
    out << action << " (HRESULT 0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr)
        << ')';
    return out.str();
}

// Local time, not UTC: the stamp goes into the filename, so it must match the clock the player
// sees. The field order keeps the filenames sorting chronologically.
static std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &tt);

    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

static std::filesystem::path PickOutputPath(const EncodeJob& job, std::string& error)
{
    std::filesystem::path folder = job.outputFolder.empty()
                                       ? std::filesystem::path("Data/SKSE/Plugins/glyph/cards")
                                       : std::filesystem::path(job.outputFolder);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec)
    {
        error = "Could not create Deck output folder: " + ec.message();
        return {};
    }

    std::ostringstream id;
    id << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << job.formID;
    const std::string base =
        SanitizeFilenameStem(job.actorName) + '-' + id.str() + '-' + Timestamp();

    std::filesystem::path candidate = folder / (base + ".png");
    for (int suffix = 2; std::filesystem::exists(candidate, ec) && suffix < 1000; ++suffix)
    {
        candidate = folder / (base + '-' + std::to_string(suffix) + ".png");
    }
    if (ec)
    {
        error = "Could not inspect Deck output path: " + ec.message();
        return {};
    }
    return candidate;
}

static EncodeResult EncodePng(const EncodeJob& job)
{
    EncodeResult result;
    std::string pathError;
    const std::filesystem::path path = PickOutputPath(job, pathError);
    if (path.empty())
    {
        result.error = std::move(pathError);
        return result;
    }

    if (!EncodeBgraPng(path,
                       job.width,
                       job.height,
                       std::span<const std::uint8_t>(job.bgra.data(), job.bgra.size()),
                       result.error))
    {
        return result;
    }
    result.ok = true;
    result.path = path.string();
    return result;
}

// One private worker thread for the whole process. Jobs run first-in first-out, and each
// result waits in m_Results until PollEncoderResults picks it up on the render thread.
// Deck::Shutdown does not stop the worker: the thread ends when the DeckState static is
// destroyed, which lets the encode in progress finish and drops whatever is still queued.
class EncoderWorker
{
public:
    EncoderWorker()
        : m_Thread([this](std::stop_token stop) { Run(stop); })
    {
    }

    ~EncoderWorker()
    {
        m_Thread.request_stop();
        m_Cv.notify_all();
    }

    void Enqueue(EncodeJob job)
    {
        {
            const std::lock_guard<std::mutex> lock(m_Mutex);
            m_Jobs.push_back(std::move(job));
            m_Outstanding.fetch_add(1, std::memory_order_release);
        }
        m_Cv.notify_one();
    }

    std::vector<EncodeResult> DrainResults()
    {
        std::vector<EncodeResult> out;
        const std::lock_guard<std::mutex> lock(m_Mutex);
        out.reserve(m_Results.size());
        while (!m_Results.empty())
        {
            out.push_back(std::move(m_Results.front()));
            m_Results.pop_front();
        }
        if (!out.empty())
        {
            m_Outstanding.fetch_sub(static_cast<int>(out.size()), std::memory_order_acq_rel);
        }
        return out;
    }

    int Outstanding() const { return m_Outstanding.load(std::memory_order_acquire); }

private:
    void Run(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            EncodeJob job;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Cv.wait(lock, [&]() { return stop.stop_requested() || !m_Jobs.empty(); });
                if (stop.stop_requested())
                {
                    return;
                }
                job = std::move(m_Jobs.front());
                m_Jobs.pop_front();
            }

            EncodeResult result;
            try
            {
                result = EncodePng(job);
            }
            catch (const std::exception& e)
            {
                result.error = std::string("Deck encoder exception: ") + e.what();
            }
            catch (...)
            {
                result.error = "Deck encoder failed with an unknown exception";
            }
            const std::lock_guard<std::mutex> lock(m_Mutex);
            m_Results.push_back(std::move(result));
        }
    }

    mutable std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::deque<EncodeJob> m_Jobs;
    std::deque<EncodeResult> m_Results;
    std::atomic<int> m_Outstanding{0};
    std::jthread m_Thread;
};

struct PendingReadback
{
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> completion;
    int width = 0;
    int height = 0;
    std::string outputFolder;
    std::string actorName;
    std::uint32_t formID = 0;
};

struct DeckState
{
    bool keyWasDown = false;
    bool captureRequested = false;
    bool sceneCaptured = false;
    std::optional<CardRequest> request;
    std::optional<PendingReadback> readback;

    ComPtr<ID3D11Texture2D> sceneTexture;
    ComPtr<ID3D11ShaderResourceView> sceneSRV;
    int sceneWidth = 0;
    int sceneHeight = 0;

    ComPtr<ID3D11Texture2D> cardTexture;
    ComPtr<ID3D11RenderTargetView> cardRTV;
    ComPtr<ID3D11PixelShader> portraitPixelShader;
    int cardWidth = 0;
    int cardHeight = 0;

    std::string status;
    bool statusError = false;
    Clock::time_point statusUntil{};
    EncoderWorker encoder;
};

static DeckState& State()
{
    static DeckState state;
    return state;
}

struct PortraitShaderCallbackData
{
    ID3D11DeviceContext* context = nullptr;
    ID3D11PixelShader* shader = nullptr;
};

// Portrait pixel shader. The stock ImGui shader multiplies the vertex alpha by the texture
// alpha, so a scene copy that carries alpha 0 would draw as nothing. This shader takes RGB
// from the scene copy and alpha from the vertex color only, which keeps the portrait opaque
// whatever alpha the captured target holds. The callback swaps the shader in for one draw;
// DrawCard resets the render state right after it.
static void ApplyPortraitShader(const ImDrawList*, const ImDrawCmd* command)
{
    const auto* data = static_cast<const PortraitShaderCallbackData*>(command->UserCallbackData);
    if (data && data->context && data->shader)
    {
        data->context->PSSetShader(data->shader, nullptr, 0);
    }
}

static bool EnsurePortraitShader(ID3D11Device* device)
{
    auto& state = State();
    if (state.portraitPixelShader)
    {
        return true;
    }
    if (!device)
    {
        return false;
    }

    static constexpr char source[] = R"(
        struct PS_INPUT
        {
            float4 pos : SV_POSITION;
            float4 col : COLOR0;
            float2 uv  : TEXCOORD0;
        };

        sampler sampler0 : register(s0);
        Texture2D texture0 : register(t0);

        float4 main(PS_INPUT input) : SV_Target
        {
            const float3 rgb = texture0.Sample(sampler0, input.uv).rgb;
            return float4(input.col.rgb * rgb, input.col.a);
        }
    )";

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT compileHr = D3DCompile(source,
                                         sizeof(source) - 1,
                                         "DeckPortrait",
                                         nullptr,
                                         nullptr,
                                         "main",
                                         "ps_4_0",
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                         0,
                                         bytecode.GetAddressOf(),
                                         errors.GetAddressOf());
    if (FAILED(compileHr))
    {
        const char* detail = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        logger::error("Deck: Portrait shader compilation failed (0x{:08X}): {}",
                      static_cast<unsigned>(compileHr),
                      detail);
        return false;
    }

    const HRESULT createHr = device->CreatePixelShader(bytecode->GetBufferPointer(),
                                                       bytecode->GetBufferSize(),
                                                       nullptr,
                                                       state.portraitPixelShader.GetAddressOf());
    if (FAILED(createHr))
    {
        logger::error("Deck: Portrait shader creation failed (0x{:08X})",
                      static_cast<unsigned>(createHr));
        return false;
    }
    return true;
}

static void SetStatus(std::string text, bool error, float seconds)
{
    auto& state = State();
    state.status = std::move(text);
    state.statusError = error;
    state.statusUntil = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
}

// Format pair for the scene copy. The copy is created in the source format's typeless family
// and viewed through the matching non-sRGB format, so an sRGB backbuffer is sampled as raw
// bytes. Without that bypass the sampler would linearize pixels that are already encoded, and
// the PNG would come out brighter than the frame the player saw. An unlisted format is passed
// through unchanged.
static DXGI_FORMAT ToTypeless(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default:
            return format;
    }
}

static DXGI_FORMAT ToShaderFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return format;
    }
}

static ImU32 Pack(const Settings::Color3& c, float alpha = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(c.r, .0f, 1.0f),
                                                 std::clamp(c.g, .0f, 1.0f),
                                                 std::clamp(c.b, .0f, 1.0f),
                                                 std::clamp(alpha, .0f, 1.0f)));
}

static Settings::Color3 Darken(const Settings::Color3& c, float amount)
{
    return {c.r * amount, c.g * amount, c.b * amount};
}

static Settings::Color3 Lighten(const Settings::Color3& c, float amount)
{
    return {c.r + (1.0f - c.r) * amount, c.g + (1.0f - c.g) * amount, c.b + (1.0f - c.b) * amount};
}

// ASCII-only uppercase. Bytes of 128 and above stay as they are, so a UTF-8 title keeps its
// own case instead of being corrupted byte by byte.
static std::string UpperAscii(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

static Settings::Color3 Desaturate(const Settings::Color3& c, float amount)
{
    const float luma = c.r * .299f + c.g * .587f + c.b * .114f;
    return {c.r + (luma - c.r) * amount, c.g + (luma - c.g) * amount, c.b + (luma - c.b) * amount};
}

// Shrink-to-fit only. The size is scaled down by the width ratio when the text is too wide,
// and returned unchanged when it already fits, so short text never grows to fill the box.
static float FitText(ImFont* font, float desired, float maxWidth, const std::string& text)
{
    if (!font || text.empty())
    {
        return desired;
    }
    const ImVec2 measured = font->CalcTextSizeA(desired, FLT_MAX, .0f, text.c_str());
    return measured.x > maxWidth && measured.x > .0f ? desired * maxWidth / measured.x : desired;
}

static void DrawEffectCentered(ImDrawList& drawList,
                               ImFont* font,
                               float fontSize,
                               float centerX,
                               float y,
                               float maxWidth,
                               const std::string& text,
                               const Settings::EffectParams& effect,
                               const Settings::Color3& left,
                               const Settings::Color3& right,
                               const Settings::Color3& highlight,
                               std::uint32_t formID,
                               float strength,
                               float outlineWidth)
{
    if (!font || text.empty())
    {
        return;
    }
    fontSize = FitText(font, fontSize, maxWidth, text);
    const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, .0f, text.c_str());
    const ImVec2 pos(centerX - size.x * .5f, y);
    Renderer::ApplyTextEffect(&drawList,
                              font,
                              fontSize,
                              pos,
                              text.c_str(),
                              effect,
                              Pack(left),
                              Pack(right),
                              Pack(highlight),
                              IM_COL32(4, 6, 9, 245),
                              outlineWidth,
                              DeterministicPhase(formID),
                              strength,
                              1.0f,
                              1.0f,
                              false);
}

// Pick the tier emblem for a position on the tier ladder. The 1.8 exponent bends the mapping
// toward the low emblems, so the top emblems stay reserved for the last tiers.
static int TierImageIndex(int tierIndex, int tierCount, int imageCount)
{
    if (imageCount <= 1 || tierCount <= 1)
    {
        return 0;
    }
    const float t =
        std::clamp(static_cast<float>(tierIndex) / static_cast<float>(tierCount - 1), .0f, 1.0f);
    return std::clamp(
        static_cast<int>(std::floor(std::pow(t, 1.8f) * imageCount)), 0, imageCount - 1);
}

// Parse a [TierN] ParticleTypes list into at most two styles. Whitespace is removed and the
// token is lowercased before the lookup. A ":weight" suffix is dropped: the card draws every
// style it keeps, so the nameplate weights carry no meaning here. An unrecognized token is
// skipped and the scan continues, so the result holds the first two recognized tokens.
static std::vector<Settings::ParticleStyle> ResolveParticleStyles(const std::string& list)
{
    std::vector<Settings::ParticleStyle> out;
    std::size_t start = 0;
    while (start < list.size() && out.size() < 2)
    {
        const std::size_t comma = list.find(',', start);
        const std::size_t end = comma == std::string::npos ? list.size() : comma;
        const std::size_t colon = list.find(':', start);
        const std::size_t tokenEnd = colon != std::string::npos && colon < end ? colon : end;
        std::string token = list.substr(start, tokenEnd - start);
        token.erase(
            std::remove_if(
                token.begin(), token.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
            token.end());
        std::transform(token.begin(),
                       token.end(),
                       token.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& entry : Settings::kParticleStyleTokens)
        {
            if (token == entry.token)
            {
                out.push_back(entry.style);
                break;
            }
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return out;
}

static void DrawHolographicFoil(ImDrawList& drawList,
                                const CardRequest& request,
                                const ImVec2& min,
                                const ImVec2& max)
{
    if (request.rarity != Rarity::Legendary)
    {
        return;
    }

    static constexpr ImU32 kFoil[] = {IM_COL32(94, 255, 220, 22),
                                      IM_COL32(120, 170, 255, 20),
                                      IM_COL32(220, 120, 255, 18),
                                      IM_COL32(255, 170, 100, 20),
                                      IM_COL32(255, 245, 150, 18)};
    const float span = max.x - min.x;
    const float phase = DeterministicPhase(request.formID) * span;
    drawList.PushClipRect(min, max, true);
    for (int i = -2; i < 7; ++i)
    {
        const float x =
            min.x + std::fmod(phase + i * span * .24f + span * 2.0f, span * 1.5f) - span * .3f;
        ImVec2 points[4] = {{x, min.y},
                            {x + span * .11f, min.y},
                            {x - span * .18f, max.y},
                            {x - span * .29f, max.y}};
        drawList.AddConvexPolyFilled(points, 4, kFoil[(i + 10) % 5]);
    }

    // The glint positions come from the FormID, so repeat captures match.
    std::uint32_t hash = request.formID ^ 0x9E3779B9u;
    for (int i = 0; i < 34; ++i)
    {
        hash ^= hash << 13;
        hash ^= hash >> 17;
        hash ^= hash << 5;
        const float x = min.x + (hash & 0xFFFFu) / 65535.0f * (max.x - min.x);
        const float y = min.y + ((hash >> 16) & 0xFFFFu) / 65535.0f * (max.y - min.y);
        const float r = 1.2f + static_cast<float>(hash & 3u);
        drawList.AddCircleFilled(ImVec2(x, y), r, Pack(request.highlight, .22f));
    }
    drawList.PopClipRect();
}

static void DrawTierParticles(ImDrawList& drawList,
                              const CardRequest& request,
                              const ImVec2& min,
                              const ImVec2& max,
                              float scale)
{
    if (request.rarity != Rarity::Legendary || request.particleTypes.empty())
    {
        return;
    }
    const auto styles = ResolveParticleStyles(request.particleTypes);
    if (styles.empty())
    {
        return;
    }

    drawList.PushClipRect(min, max, true);
    for (int i = 0; i < static_cast<int>(styles.size()); ++i)
    {
        TextEffects::ParticleAuraParams params{};
        params.list = &drawList;
        params.center = ImVec2((min.x + max.x) * .5f, (min.y + max.y) * .52f);
        params.radiusX = (max.x - min.x) * .43f;
        params.radiusY = (max.y - min.y) * .40f;
        params.color = Pack(request.particleColor, .62f);
        params.colorSecondary = Pack(request.highlight, .55f);
        params.alpha = .58f;
        params.style = styles[i];
        params.particleCount = std::clamp(request.particleCount / 2, 4, 10);
        params.particleSize = 11.0f * scale;
        params.speed = .65f;
        params.time = DeterministicPhase(request.formID) * 23.0f;
        params.styleIndex = i;
        params.enabledStyleCount = static_cast<int>(styles.size());
        params.useParticleTextures = ParticleTextures::IsInitialized();
        params.blendMode = 2;  // Alpha, so the baked PNG stays predictable.
        params.depthStrength = .65f;
        params.colorWarmth = .5f;
        params.glowStrength = .20f;
        params.glowSize = 1.8f;
        params.shineThreshold = .86f;
        TextEffects::DrawParticleAura(params);
    }
    drawList.PopClipRect();
}

static void DrawBadgeStrip(
    ImDrawList& drawList, const CardRequest& request, float y, float left, float right, float scale)
{
    struct Drawable
    {
        ImTextureID texture = 0;
        Settings::Color3 color{};
        bool muted = false;
        bool fullColor = false;
    };
    std::vector<Drawable> drawables;
    drawables.reserve(request.badges.size());
    for (const auto& badge : request.badges)
    {
        const ImTextureID texture = badge.tierImage >= 0
                                        ? BadgeTextures::GetTierImage(badge.tierImage)
                                        : BadgeTextures::Get(badge.icon);
        if (texture)
        {
            drawables.push_back({texture, badge.color, badge.muted, badge.tierImage >= 0});
        }
    }
    if (drawables.empty())
    {
        return;
    }

    const float icon = 58.0f * scale;
    const float gap = 15.0f * scale;
    const float total = icon * drawables.size() + gap * (drawables.size() - 1);
    float x = std::max(left, (left + right - total) * .5f);
    RenderSampling::PushBadgeSampler(&drawList);
    for (const auto& badge : drawables)
    {
        drawList.AddCircleFilled(ImVec2(x + icon * .5f, y + icon * .5f),
                                 icon * .55f,
                                 IM_COL32(5, 8, 12, badge.muted ? 150 : 205));
        const Settings::Color3 tint = badge.muted ? Desaturate(badge.color, .35f) : badge.color;
        const ImU32 color =
            badge.fullColor ? IM_COL32_WHITE : Pack(tint, badge.muted ? .72f : .98f);
        drawList.AddImage(badge.texture, ImVec2(x, y), ImVec2(x + icon, y + icon), {}, {}, color);
        x += icon + gap;
    }
    RenderSampling::PopSampler(&drawList);
}

// Card layout. Every length below is a design-space value times scale = CardLayoutScale(w, h),
// the fit of the 750x1050 reference into the target, so the whole card grows with the target.
// m = 38*scale is the content margin and pb = 0.655*h is the portrait bottom.
//
//   +==========================================================+  0             card edge
//   |  +----------------------------------------------------+  |  0.45m/0.72m   frame insets
//   |  | [RARITY]                                  (emblem) |  |  42*scale      portrait top
//   |  |                                                    |  |
//   |  |                   portrait crop                    |  |
//   |  |                                                    |  |
//   |  | <left ornaments>                 <right ornaments> |  |  pb - 58*scale
//   |  +----------------------------------------------------+  |  pb            portrait end
//   |                       TITLE                              |  pb + 16*scale
//   |                        NAME                              |  pb + 54*scale
//   |   ------------------------------------------------       |  pb + 178*scale  divider
//   |  (LEVEL)    [badge] [badge] [badge]                      |  divider + 26*scale
//   |                TIER NAME  /  #0001A2B3                   |  h - m - 31*scale
//   +==========================================================+  h
//
// Draw order matters at the portrait: the vignette lands before the foil and the particle
// block, so the bottom fade does not dim them, and the portrait image sits between the shader
// callback and the reset callback, so only that one draw uses the portrait pixel shader.
static void DrawCard(ImDrawList& drawList,
                     const CardRequest& request,
                     ImTextureID portraitTexture,
                     void* portraitShaderData)
{
    const float width = static_cast<float>(request.width);
    const float height = static_cast<float>(request.height);
    const float scale = CardLayoutScale(width, height);
    const float margin = 38.0f * scale;
    const float portraitTop = 42.0f * scale;
    const float portraitBottom = height * .655f;
    const ImVec2 portraitMin(margin, portraitTop);
    const ImVec2 portraitMax(width - margin, portraitBottom);
    const int rarityRank = static_cast<int>(request.rarity);

    const Settings::Color3 bgTop = Darken(request.frameLeft, .16f);
    const Settings::Color3 bgBottom = Darken(request.frameRight, .055f);
    drawList.AddRectFilledMultiColor(ImVec2(0, 0),
                                     ImVec2(width, height),
                                     Pack(bgTop),
                                     Pack(Darken(bgTop, .75f)),
                                     Pack(bgBottom),
                                     Pack(Darken(bgBottom, .72f)));

    drawList.AddCallback(ApplyPortraitShader, portraitShaderData);
    drawList.AddImageRounded(portraitTexture,
                             portraitMin,
                             portraitMax,
                             ImVec2(request.portrait.left, request.portrait.top),
                             ImVec2(request.portrait.right, request.portrait.bottom),
                             IM_COL32_WHITE,
                             20.0f * scale,
                             ImDrawFlags_RoundCornersAll);
    drawList.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    RenderSampling::PushFontSampler(&drawList);

    // Vignette fading the portrait bottom into the text panel.
    const float vignetteTop = portraitBottom - 180.0f * scale;
    drawList.AddRectFilledMultiColor(ImVec2(margin, vignetteTop),
                                     portraitMax,
                                     IM_COL32(0, 0, 0, 0),
                                     IM_COL32(0, 0, 0, 0),
                                     IM_COL32(2, 4, 8, 230),
                                     IM_COL32(2, 4, 8, 230));

    DrawHolographicFoil(drawList, request, portraitMin, portraitMax);
    DrawTierParticles(drawList, request, portraitMin, portraitMax, scale);

    // Background of the bottom text panel.
    drawList.AddRectFilledMultiColor(ImVec2(margin, portraitBottom - 5.0f * scale),
                                     ImVec2(width - margin, height - margin),
                                     Pack(Darken(request.frameLeft, .12f), .98f),
                                     Pack(Darken(request.frameRight, .12f), .98f),
                                     IM_COL32(3, 5, 9, 255),
                                     IM_COL32(3, 5, 9, 255));

    // Card frame. Higher rarity adds border weight and extra layers.
    drawList.AddRect(ImVec2(margin * .45f, margin * .45f),
                     ImVec2(width - margin * .45f, height - margin * .45f),
                     Pack(request.frameLeft),
                     24.0f * scale,
                     ImDrawFlags_RoundCornersAll,
                     (5.0f + rarityRank * .65f) * scale);
    drawList.AddRect(ImVec2(margin * .72f, margin * .72f),
                     ImVec2(width - margin * .72f, height - margin * .72f),
                     Pack(request.frameRight, .92f),
                     19.0f * scale,
                     ImDrawFlags_RoundCornersAll,
                     2.0f * scale);
    drawList.AddRect(portraitMin,
                     portraitMax,
                     Pack(request.highlight, .82f),
                     20.0f * scale,
                     ImDrawFlags_RoundCornersAll,
                     (1.5f + rarityRank * .35f) * scale);

    if (request.rarity == Rarity::Legendary)
    {
        static constexpr ImU32 kSpectral[] = {IM_COL32(105, 255, 215, 170),
                                              IM_COL32(110, 170, 255, 160),
                                              IM_COL32(232, 130, 255, 150),
                                              IM_COL32(255, 205, 105, 160)};
        for (int i = 0; i < 4; ++i)
        {
            const float inset = (11.0f + i * 2.2f) * scale;
            drawList.AddRect(ImVec2(inset, inset),
                             ImVec2(width - inset, height - inset),
                             kSpectral[i],
                             26.0f * scale,
                             ImDrawFlags_RoundCornersAll,
                             .85f * scale);
        }
    }

    ImFont* nameFont = Renderer::GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = Renderer::GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = Renderer::GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    ImFont* ornamentFont = Renderer::GetFontAt(RenderConstants::FONT_INDEX_ORNAMENT);

    // Rarity plaque.
    const std::string rarityText = UpperAscii(std::string(RarityName(request.rarity)));
    const float plaqueX = margin + 17.0f * scale;
    const float plaqueY = portraitTop + 17.0f * scale;
    const float plaqueW = 184.0f * scale;
    const float plaqueH = 52.0f * scale;
    drawList.AddRectFilled(ImVec2(plaqueX, plaqueY),
                           ImVec2(plaqueX + plaqueW, plaqueY + plaqueH),
                           IM_COL32(3, 6, 10, 205),
                           12.0f * scale);
    drawList.AddRect(ImVec2(plaqueX, plaqueY),
                     ImVec2(plaqueX + plaqueW, plaqueY + plaqueH),
                     Pack(request.highlight, .85f),
                     12.0f * scale,
                     ImDrawFlags_RoundCornersAll,
                     1.4f * scale);
    if (titleFont)
    {
        const float raritySize = 28.0f * scale;
        const ImVec2 textSize =
            titleFont->CalcTextSizeA(raritySize, FLT_MAX, .0f, rarityText.c_str());
        drawList.AddText(
            titleFont,
            raritySize,
            ImVec2(plaqueX + (plaqueW - textSize.x) * .5f, plaqueY + (plaqueH - textSize.y) * .5f),
            Pack(request.highlight),
            rarityText.c_str());
    }

    // Tier emblem. Drawn in full color, untinted, on every card.
    const int tierImageCount = BadgeTextures::TierImageCount();
    if (tierImageCount > 0)
    {
        const int imageIndex = TierImageIndex(request.tierIndex, request.tierCount, tierImageCount);
        if (const ImTextureID emblem = BadgeTextures::GetTierImage(imageIndex))
        {
            const float emblemSize = 102.0f * scale;
            const ImVec2 emblemMin(width - margin - emblemSize - 8.0f * scale,
                                   portraitTop + 8.0f * scale);
            drawList.AddCircleFilled(emblemMin + ImVec2(emblemSize * .5f, emblemSize * .5f),
                                     emblemSize * .46f,
                                     Pack(request.highlight, .16f));
            RenderSampling::PushBadgeSampler(&drawList);
            drawList.AddImage(emblem,
                              emblemMin,
                              emblemMin + ImVec2(emblemSize, emblemSize),
                              {},
                              {},
                              IM_COL32_WHITE);
            RenderSampling::PopSampler(&drawList);
        }
    }

    const float panelTop = portraitBottom + 16.0f * scale;
    const float maxTextWidth = width - 2.0f * (margin + 22.0f * scale);
    const float effectStrength = .78f + rarityRank * .055f;
    const std::string displayTitle = UpperAscii(request.title);
    DrawEffectCentered(drawList,
                       titleFont,
                       36.0f * scale,
                       width * .5f,
                       panelTop,
                       maxTextWidth,
                       displayTitle,
                       request.titleEffect,
                       request.titleLeft,
                       request.titleRight,
                       request.highlight,
                       request.formID,
                       effectStrength,
                       2.0f * scale);
    DrawEffectCentered(drawList,
                       nameFont,
                       104.0f * scale,
                       width * .5f,
                       panelTop + 38.0f * scale,
                       maxTextWidth,
                       request.name,
                       request.nameEffect,
                       request.nameLeft,
                       request.nameRight,
                       request.highlight,
                       request.formID,
                       effectStrength,
                       3.0f * scale);

    const float dividerY = panelTop + 162.0f * scale;
    drawList.AddLine(ImVec2(margin + 28.0f * scale, dividerY),
                     ImVec2(width - margin - 28.0f * scale, dividerY),
                     Pack(request.frameLeft, .65f),
                     1.5f * scale);

    // Level medallion.
    const ImVec2 levelCenter(margin + 73.0f * scale, dividerY + 55.0f * scale);
    drawList.AddCircleFilled(levelCenter, 49.0f * scale, IM_COL32(3, 6, 10, 225));
    drawList.AddCircle(levelCenter, 49.0f * scale, Pack(request.frameLeft), 0, 2.2f * scale);
    if (titleFont)
    {
        const float labelSize = 22.0f * scale;
        const char* levelLabel = "LEVEL";
        const ImVec2 label = titleFont->CalcTextSizeA(labelSize, FLT_MAX, .0f, levelLabel);
        drawList.AddText(titleFont,
                         labelSize,
                         ImVec2(levelCenter.x - label.x * .5f, levelCenter.y - 29.0f * scale),
                         Pack(request.titleLeft, .84f),
                         levelLabel);
    }
    if (levelFont)
    {
        const std::string value = std::to_string(request.level);
        const float valueSize = FitText(levelFont, 60.0f * scale, 78.0f * scale, value);
        const ImVec2 valueExtent = levelFont->CalcTextSizeA(valueSize, FLT_MAX, .0f, value.c_str());
        drawList.AddText(levelFont,
                         valueSize,
                         ImVec2(levelCenter.x - valueExtent.x * .5f, levelCenter.y - 7.0f * scale),
                         Pack(request.nameLeft),
                         value.c_str());
    }

    DrawBadgeStrip(drawList,
                   request,
                   dividerY + 26.0f * scale,
                   margin + 143.0f * scale,
                   width - margin - 18.0f * scale,
                   scale);

    // Footer: tier name plus the FormID as a stable card number.
    if (titleFont)
    {
        std::ostringstream footer;
        footer << request.tierName << "  /  #" << std::uppercase << std::hex << std::setw(8)
               << std::setfill('0') << request.formID;
        const std::string footerText = UpperAscii(footer.str());
        const float footerSize = FitText(titleFont, 25.0f * scale, maxTextWidth, footerText);
        const ImVec2 footerExtent =
            titleFont->CalcTextSizeA(footerSize, FLT_MAX, .0f, footerText.c_str());
        drawList.AddText(titleFont,
                         footerSize,
                         ImVec2((width - footerExtent.x) * .5f, height - margin - 31.0f * scale),
                         Pack(Lighten(request.titleRight, .22f), .90f),
                         footerText.c_str());
    }

    // Tier ornament glyphs sit at the portrait's bottom corners, not beside the name.
    if (ornamentFont)
    {
        const float ornamentSize = 58.0f * scale;
        if (!request.leftOrnaments.empty())
        {
            drawList.AddText(ornamentFont,
                             ornamentSize,
                             ImVec2(margin + 8.0f * scale, portraitBottom - 58.0f * scale),
                             Pack(request.ornamentLeft, .88f),
                             request.leftOrnaments.c_str());
        }
        if (!request.rightOrnaments.empty())
        {
            const ImVec2 extent = ornamentFont->CalcTextSizeA(
                ornamentSize, FLT_MAX, .0f, request.rightOrnaments.c_str());
            drawList.AddText(
                ornamentFont,
                ornamentSize,
                ImVec2(width - margin - extent.x - 8.0f * scale, portraitBottom - 58.0f * scale),
                Pack(request.ornamentRight, .88f),
                request.rightOrnaments.c_str());
        }
    }
    RenderSampling::PopSampler(&drawList);
}

static bool EnsureCardTarget(ID3D11Device* device, int width, int height)
{
    auto& state = State();
    if (state.cardTexture && state.cardRTV && state.cardWidth == width &&
        state.cardHeight == height)
    {
        return true;
    }
    state.cardTexture.Reset();
    state.cardRTV.Reset();
    state.cardWidth = 0;
    state.cardHeight = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, state.cardTexture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = device->CreateRenderTargetView(
            state.cardTexture.Get(), nullptr, state.cardRTV.GetAddressOf());
    }
    if (FAILED(hr))
    {
        logger::error("Deck: Failed to create {}x{} card target (0x{:08X})",
                      width,
                      height,
                      static_cast<unsigned>(hr));
        state.cardTexture.Reset();
        state.cardRTV.Reset();
        return false;
    }
    state.cardWidth = width;
    state.cardHeight = height;
    return true;
}

// Saves the bound render targets, depth-stencil view and viewports on construction, and
// rebinds exactly those on destruction, so drawing into the card target leaves the frame's
// output-merger and rasterizer state as it was.
class RenderTargetStateGuard
{
public:
    explicit RenderTargetStateGuard(ID3D11DeviceContext* context)
        : m_Context(context)
    {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        m_Context->OMGetRenderTargets(
            static_cast<UINT>(targets.size()), targets.data(), m_DepthStencil.GetAddressOf());
        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            m_RenderTargets[i].Attach(targets[i]);
        }

        UINT viewportCount = 0;
        m_Context->RSGetViewports(&viewportCount, nullptr);
        m_Viewports.resize(viewportCount);
        if (viewportCount > 0)
        {
            m_Context->RSGetViewports(&viewportCount, m_Viewports.data());
            m_Viewports.resize(viewportCount);
        }
    }

    ~RenderTargetStateGuard()
    {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            targets[i] = m_RenderTargets[i].Get();
        }
        m_Context->OMSetRenderTargets(
            static_cast<UINT>(targets.size()), targets.data(), m_DepthStencil.Get());
        m_Context->RSSetViewports(static_cast<UINT>(m_Viewports.size()),
                                  m_Viewports.empty() ? nullptr : m_Viewports.data());
    }

    RenderTargetStateGuard(const RenderTargetStateGuard&) = delete;
    RenderTargetStateGuard& operator=(const RenderTargetStateGuard&) = delete;

private:
    ID3D11DeviceContext* m_Context = nullptr;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        m_RenderTargets;
    ComPtr<ID3D11DepthStencilView> m_DepthStencil;
    std::vector<D3D11_VIEWPORT> m_Viewports;
};

// Compose the card into its own render target. The card is built in a private ImDrawList and
// ImDrawData, so its coordinates are card pixels instead of screen pixels and nothing of it
// reaches the frame's own draw data. The clear color is opaque: the staging copy keeps alpha,
// so any pixel the layout does not cover must still read as background, not as a hole.
static bool RenderToCardTarget(ID3D11Device* device,
                               ID3D11DeviceContext* context,
                               const CardRequest& request)
{
    auto& state = State();
    if (!EnsureCardTarget(device, request.width, request.height) || !EnsurePortraitShader(device) ||
        !state.sceneSRV)
    {
        return false;
    }

    const RenderTargetStateGuard restoreState(context);

    ID3D11RenderTargetView* target = state.cardRTV.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(request.width);
    viewport.Height = static_cast<float>(request.height);
    viewport.MinDepth = .0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    constexpr float clear[4] = {.01f, .015f, .025f, 1.0f};
    context->ClearRenderTargetView(target, clear);

    ImDrawList drawList(ImGui::GetDrawListSharedData());
    drawList._ResetForNewFrame();
    drawList.PushTextureID(ImGui::GetIO().Fonts->TexID);
    drawList.PushClipRect(
        ImVec2(0, 0),
        ImVec2(static_cast<float>(request.width), static_cast<float>(request.height)),
        false);
    PortraitShaderCallbackData portraitShaderData{context, state.portraitPixelShader.Get()};
    DrawCard(drawList,
             request,
             reinterpret_cast<ImTextureID>(state.sceneSRV.Get()),
             &portraitShaderData);
    drawList.PopClipRect();
    drawList.PopTextureID();

    ImDrawData drawData;
    drawData.Valid = true;
    drawData.DisplayPos = ImVec2(0, 0);
    drawData.DisplaySize =
        ImVec2(static_cast<float>(request.width), static_cast<float>(request.height));
    drawData.FramebufferScale = ImVec2(1, 1);
    drawData.AddDrawList(&drawList);
    ImGui_ImplDX11_RenderDrawData(&drawData);

    return true;
}

static bool QueueReadback(ID3D11Device* device,
                          ID3D11DeviceContext* context,
                          const CardRequest& request)
{
    auto& state = State();
    if (!state.cardTexture || state.readback)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    state.cardTexture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    PendingReadback readback;
    readback.width = request.width;
    readback.height = request.height;
    readback.outputFolder = request.outputFolder;
    readback.actorName = request.name;
    readback.formID = request.formID;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, readback.staging.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_EVENT;
        hr = device->CreateQuery(&queryDesc, readback.completion.GetAddressOf());
    }
    if (FAILED(hr))
    {
        logger::error("Deck: Failed to create staging/query resources (0x{:08X})",
                      static_cast<unsigned>(hr));
        return false;
    }

    context->CopyResource(readback.staging.Get(), state.cardTexture.Get());
    context->End(readback.completion.Get());
    state.readback = std::move(readback);
    return true;
}

static void PollEncoderResults()
{
    for (auto& result : State().encoder.DrainResults())
    {
        if (result.ok)
        {
            logger::info("Deck: Saved card to {}", result.path);
            const std::filesystem::path path(result.path);
            SetStatus("Card saved: " + path.filename().string(), false, 4.0f);
        }
        else
        {
            logger::error("Deck: {}", result.error);
            SetStatus(result.error, true, 5.0f);
        }
    }
}

static void PollReadback(ID3D11DeviceContext* context)
{
    auto& state = State();
    if (!state.readback)
    {
        return;
    }

    // Poll, never block. S_FALSE means the GPU has not reached the event yet, and DONOTFLUSH
    // keeps this poll from forcing a flush, so the copy lands on a later frame instead of
    // stalling this one.
    auto& pending = *state.readback;
    const HRESULT ready =
        context->GetData(pending.completion.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (ready == S_FALSE)
    {
        return;
    }
    if (FAILED(ready))
    {
        NotifyError(HResultText("Deck GPU readback failed", ready));
        state.readback.reset();
        return;
    }

    EncodeJob job;
    job.width = pending.width;
    job.height = pending.height;
    job.outputFolder = pending.outputFolder;
    job.actorName = pending.actorName;
    job.formID = pending.formID;
    job.bgra.resize(static_cast<std::size_t>(job.width) * job.height * 4);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = context->Map(pending.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapHr))
    {
        NotifyError(HResultText("Deck could not map card pixels", mapHr));
        state.readback.reset();
        return;
    }

    // The card target is RGBA8 and the WIC PNG path is most portable as BGRA8. Copy row by
    // row, honoring the driver's RowPitch, and swizzle.
    const bool copied = CopyRgbaToBgra(static_cast<const std::uint8_t*>(mapped.pData),
                                       mapped.RowPitch,
                                       job.width,
                                       job.height,
                                       job.bgra.data(),
                                       job.bgra.size());
    context->Unmap(pending.staging.Get(), 0);
    if (!copied)
    {
        NotifyError("Deck received an invalid mapped card texture");
        state.readback.reset();
        return;
    }

    state.encoder.Enqueue(std::move(job));
    state.readback.reset();
    SetStatus("Writing card PNG...", false, 30.0f);
}
}  // namespace

void PollInput(bool enabled, int virtualKey, bool worldReady)
{
    auto& state = State();
    if (!enabled || virtualKey <= 0)
    {
        state.keyWasDown = false;
        state.captureRequested = false;
        return;
    }

    const bool keyDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    if (keyDown && !state.keyWasDown)
    {
        if (!worldReady)
        {
            NotifyError("Deck is unavailable while the world is loading or a menu is open");
        }
        else if (state.captureRequested || state.request || state.readback)
        {
            NotifyError("Deck is still developing the previous card");
        }
        else
        {
            state.captureRequested = true;
            SetStatus("Framing character card...", false, 4.0f);
        }
    }
    state.keyWasDown = keyDown;
}

bool ConsumeCaptureRequest()
{
    auto& state = State();
    return state.captureRequested && std::exchange(state.captureRequested, false);
}

bool Queue(CardRequest request)
{
    auto& state = State();
    if (state.request || state.readback)
    {
        NotifyError("Deck is still developing the previous card");
        return false;
    }
    state.request = std::move(request);
    state.sceneCaptured = false;
    SetStatus("Composing " + std::string(RarityName(state.request->rarity)) + " " +
                  state.request->name + "...",
              false,
              8.0f);
    return true;
}

void NotifyError(std::string message)
{
    logger::warn("Deck: {}", message);
    SetStatus(std::move(message), true, 5.0f);
}

bool NeedsSceneCapture()
{
    const auto& state = State();
    return state.request.has_value() && !state.sceneCaptured;
}

bool CaptureScene(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain)
{
    auto& state = State();
    if (!NeedsSceneCapture() || !device || !context)
    {
        return false;
    }

    ComPtr<ID3D11Texture2D> source;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    D3D11_RENDER_TARGET_VIEW_DESC sourceViewDesc{};
    bool usingSwapChain = false;

    // HUDMenu::PostDisplay can enter with a cleared UI or offscreen render target bound. The
    // swap-chain buffer holds the composed game frame and is still valid before the HUD
    // movie draws, so prefer it whenever the hook can supply one.
    if (swapChain)
    {
        const HRESULT swapHr = swapChain->GetBuffer(
            0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(source.GetAddressOf()));
        if (SUCCEEDED(swapHr) && source)
        {
            source->GetDesc(&sourceDesc);
            sourceViewDesc.Format = sourceDesc.Format;
            sourceViewDesc.ViewDimension = sourceDesc.SampleDesc.Count > 1
                                               ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                               : D3D11_RTV_DIMENSION_TEXTURE2D;
            usingSwapChain = true;
        }
        else
        {
            logger::warn(
                "Deck: Could not acquire swap-chain backbuffer (0x{:08X}); using bound "
                "render target",
                static_cast<unsigned>(swapHr));
            source.Reset();
        }
    }

    ComPtr<ID3D11RenderTargetView> sourceRTV;
    if (!source)
    {
        context->OMGetRenderTargets(1, sourceRTV.GetAddressOf(), nullptr);
        if (!sourceRTV)
        {
            return false;
        }
        ComPtr<ID3D11Resource> sourceResource;
        sourceRTV->GetResource(sourceResource.GetAddressOf());
        if (!sourceResource || FAILED(sourceResource.As(&source)) || !source)
        {
            return false;
        }
        source->GetDesc(&sourceDesc);
        sourceRTV->GetDesc(&sourceViewDesc);
    }

    UINT sourceMip = 0;
    UINT sourceArraySlice = 0;
    bool viewIsMultisampled = false;
    switch (sourceViewDesc.ViewDimension)
    {
        case D3D11_RTV_DIMENSION_TEXTURE2D:
            sourceMip = sourceViewDesc.Texture2D.MipSlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
            if (sourceViewDesc.Texture2DArray.ArraySize != 1)
            {
                logger::error("Deck: Cannot capture a multi-slice render target view");
                return false;
            }
            sourceMip = sourceViewDesc.Texture2DArray.MipSlice;
            sourceArraySlice = sourceViewDesc.Texture2DArray.FirstArraySlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMS:
            viewIsMultisampled = true;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
            if (sourceViewDesc.Texture2DMSArray.ArraySize != 1)
            {
                logger::error("Deck: Cannot capture a multi-slice MSAA render target view");
                return false;
            }
            sourceArraySlice = sourceViewDesc.Texture2DMSArray.FirstArraySlice;
            viewIsMultisampled = true;
            break;
        default:
            logger::error("Deck: Unsupported render target view dimension {}",
                          static_cast<int>(sourceViewDesc.ViewDimension));
            return false;
    }

    const bool resourceIsMultisampled = sourceDesc.SampleDesc.Count > 1;
    if (sourceDesc.MipLevels == 0 || sourceDesc.ArraySize == 0 ||
        sourceMip >= sourceDesc.MipLevels || sourceArraySlice >= sourceDesc.ArraySize ||
        viewIsMultisampled != resourceIsMultisampled)
    {
        logger::error("Deck: Render target view selects an invalid texture subresource");
        return false;
    }

    const UINT sourceSubresource =
        D3D11CalcSubresource(sourceMip, sourceArraySlice, sourceDesc.MipLevels);
    const UINT captureWidth = std::max(1U, sourceDesc.Width >> sourceMip);
    const UINT captureHeight = std::max(1U, sourceDesc.Height >> sourceMip);

    D3D11_TEXTURE2D_DESC copyDesc{};
    copyDesc.Width = captureWidth;
    copyDesc.Height = captureHeight;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Format = ToTypeless(sourceDesc.Format);
    copyDesc.SampleDesc.Count = 1;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> sceneTexture;
    HRESULT hr = device->CreateTexture2D(&copyDesc, nullptr, sceneTexture.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("Deck: Failed to create scene copy (format {}, 0x{:08X})",
                      static_cast<int>(copyDesc.Format),
                      static_cast<unsigned>(hr));
        return false;
    }

    if (viewIsMultisampled)
    {
        const DXGI_FORMAT resolveFormat = sourceViewDesc.Format;
        UINT formatSupport = 0;
        if (resolveFormat == DXGI_FORMAT_UNKNOWN ||
            FAILED(device->CheckFormatSupport(resolveFormat, &formatSupport)) ||
            (formatSupport & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) == 0)
        {
            logger::error("Deck: Render target format {} cannot be resolved from MSAA",
                          static_cast<int>(resolveFormat));
            return false;
        }
        context->ResolveSubresource(
            sceneTexture.Get(), 0, source.Get(), sourceSubresource, resolveFormat);
    }
    else
    {
        context->CopySubresourceRegion(
            sceneTexture.Get(), 0, 0, 0, 0, source.Get(), sourceSubresource, nullptr);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = ToShaderFormat(sourceDesc.Format);
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> sceneSRV;
    hr = device->CreateShaderResourceView(sceneTexture.Get(), &srvDesc, sceneSRV.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("Deck: Failed to create scene SRV (format {}, 0x{:08X})",
                      static_cast<int>(srvDesc.Format),
                      static_cast<unsigned>(hr));
        return false;
    }

    state.sceneTexture = std::move(sceneTexture);
    state.sceneSRV = std::move(sceneSRV);
    state.sceneWidth = static_cast<int>(captureWidth);
    state.sceneHeight = static_cast<int>(captureHeight);
    state.sceneCaptured = true;
    logger::debug("Deck: Captured {}x{} portrait source from {}",
                  captureWidth,
                  captureHeight,
                  usingSwapChain ? "swap chain" : "bound render target");
    return true;
}

bool NeedsFrame()
{
    const auto& state = State();
    return state.captureRequested || state.request.has_value() || state.readback.has_value() ||
           state.encoder.Outstanding() > 0 ||
           (!state.status.empty() && Clock::now() < state.statusUntil);
}

void Process(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain)
{
    // Drain first, so a finished encode still reports its result on a frame that has no
    // device or context.
    PollEncoderResults();
    if (!device || !context)
    {
        return;
    }
    PollReadback(context);

    auto& state = State();
    if (!state.request)
    {
        return;
    }
    if (!state.sceneCaptured && !CaptureScene(device, context, swapChain))
    {
        NotifyError("Deck could not capture the current frame");
        state.request.reset();
        return;
    }

    // Copy, not a reference: the pending slot is cleared below while the name and the rarity
    // are still needed for the toast.
    const CardRequest request = *state.request;
    if (!RenderToCardTarget(device, context, request) || !QueueReadback(device, context, request))
    {
        NotifyError("Deck could not render the card target");
        state.request.reset();
        state.sceneCaptured = false;
        return;
    }

    state.request.reset();
    state.sceneCaptured = false;
    SetStatus("Developing " + std::string(RarityName(request.rarity)) + " " + request.name + "...",
              false,
              30.0f);
}

void DrawNotification()
{
    const auto& state = State();
    if (state.status.empty() || Clock::now() >= state.statusUntil)
    {
        return;
    }

    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x - 24.0f, 24.0f), ImGuiCond_Always, ImVec2(1.0f, .0f));
    ImGui::SetNextWindowBgAlpha(.90f);
    ImGui::Begin("##glyphDeckStatus",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    const ImVec4 color =
        state.statusError ? ImVec4(1.0f, .48f, .42f, 1.0f) : ImVec4(.74f, .92f, 1.0f, 1.0f);
    ImGui::TextColored(color, "%s", state.status.c_str());
    ImGui::End();
}

void Shutdown()
{
    auto& state = State();
    const bool abandoned = state.request.has_value() || state.readback.has_value();
    state.request.reset();
    state.readback.reset();
    state.sceneTexture.Reset();
    state.sceneSRV.Reset();
    state.cardTexture.Reset();
    state.cardRTV.Reset();
    state.portraitPixelShader.Reset();
    state.sceneCaptured = false;
    state.sceneWidth = 0;
    state.sceneHeight = 0;
    state.cardWidth = 0;
    state.cardHeight = 0;
    if (abandoned)
    {
        NotifyError("Deck capture was interrupted by a renderer reset");
    }
}
}  // namespace Deck
