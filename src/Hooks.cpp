#include "Hooks.hpp"

#include "BadgeTextures.hpp"
#include "Deck.hpp"
#include "DepthClip.hpp"
#include "GameState.hpp"
#include "Graffito.hpp"
#include "ParticleTextures.hpp"
#include "ProjectManifest.hpp"
#include "RasterQuality.hpp"
#include "Renderer.hpp"
#include "RenderSampling.hpp"
#include "SceneMeter.hpp"
#include "Settings.hpp"
#include "TextPostProcess.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <wrl/client.h>
#include <chrono>
#include <exception>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Hooks
{

// State lives in four function-local statics:
//   Init()  - initialization lifecycle. Clearing one of the "loaded" flags
//             (mipmapsGenerated, particleTexturesLoaded, badgeTexturesLoaded,
//             postProcessInitialized) forces that resource to rebuild on the
//             next frame. backendReinitRequested is the opposite polarity:
//             setting it requests the rebuild, and the next frame clears it.
//   Frame() - per-frame flags, reset at the start of each PostDisplay thunk.
//   Diag()  - exception counters and one-shot log gates.
//   D3D()   - cached device, context, swapchain, the glyph-owned font atlas
//             SRV, and the original Present pointer. StateMutex() guards the
//             device, context and swapchain pointers. originalPresent is an
//             atomic, which the Present fast path reads without the lock;
//             fontAtlasSRV is written only by the render thread.

struct InitFlags
{
    std::atomic<bool> initialized{false};
    std::atomic<bool> initializing{false};
    std::atomic<std::uint64_t> nextInitRetryAtMs{0};
    std::atomic<bool> mipmapsGenerated{false};
    std::atomic<bool> particleTexturesLoaded{false};
    std::atomic<bool> badgeTexturesLoaded{false};
    std::atomic<uint32_t> badgeTexturesGen{0};
    std::atomic<bool> postProcessInitialized{false};
    std::atomic<std::uint64_t> nextGraffitoRetryAtMs{0};
    std::atomic<bool> backendReinitRequested{false};
};

struct FrameFlags
{
    // Records the gate decision of the current frame. Nothing in src/ reads it
    // back; the draw sites re-evaluate the gate through their own local copy.
    std::atomic<bool> shouldRenderOverlay{false};
    // Set by RenderOverlayNow. PresentHook reads it to tell "PostDisplay
    // already drew this frame" from "PostDisplay was skipped". PostDisplay
    // reads it again after the original HUD draw and skips its own draw when
    // the overlay already went out this frame.
    std::atomic<bool> overlayRenderedThisFrame{false};
};

struct DiagFlags
{
    std::atomic<uint32_t> renderExceptionCount{0};
    std::atomic<bool> missingPresentLogged{false};
    std::atomic<bool> deviceChangeLogged{false};
    std::atomic<bool> imguiInitializedLogged{false};
    std::atomic<bool> firstPostDisplayLogged{false};
    std::atomic<bool> presentBootstrapLogged{false};
};

static InitFlags& Init()
{
    static InitFlags f;
    return f;
}
static FrameFlags& Frame()
{
    static FrameFlags f;
    return f;
}
static DiagFlags& Diag()
{
    static DiagFlags f;
    return f;
}
static std::mutex& StateMutex()
{
    static std::mutex instance;
    return instance;
}

// Signature of IDXGISwapChain::Present.
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

// D3D11 device, context, swap chain, and original Present pointer. Reached
// through a function-local static so no non-trivially destructible object sits
// at namespace scope, where destruction order on DLL unload is unsafe.
struct D3DState
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
    std::atomic<PresentFn> originalPresent{nullptr};
    // glyph-owned mipmapped font atlas SRV bound into io.Fonts->TexID. Kept
    // separate from the ImGui backend's own font texture, which the backend
    // creates and frees in InvalidateDeviceObjects.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fontAtlasSRV;
};

static D3DState& D3D()
{
    static D3DState s;
    return s;
}

HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

// Draws one overlay frame. Called by PresentHook and by PostDisplay::thunk.
void RenderOverlayNow();

// Installs the Present hook on one swapchain and stores the original Present.
bool TryInstallPresentHook(IDXGISwapChain* swapChain);

bool TryInstallPresentHook(IDXGISwapChain* swapChain)
{
    if (!swapChain)
    {
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!vtable || !vtable[8])
    {
        return false;
    }

    const auto currentPresent = reinterpret_cast<PresentFn>(vtable[8]);
    const auto ourPresent = reinterpret_cast<PresentFn>(&PresentHook);

    // Hold the lock across both the original-pointer store and the vtable write,
    // so a concurrent Present call never sees one without the other.
    const std::lock_guard<std::mutex> lock(StateMutex());
    if (currentPresent == ourPresent)
    {
        return D3D().originalPresent.load(std::memory_order_relaxed) != nullptr;
    }
    D3D().originalPresent.store(currentPresent, std::memory_order_release);

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        logger::error("Hooks: VirtualProtect failed while patching Present vtable slot");
        return false;
    }
    vtable[8] = reinterpret_cast<void*>(&PresentHook);
    VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// Detects a device/context/swapchain change, refreshes the cached pointers,
// re-installs the Present hook on the new swapchain, and schedules every
// GPU-owning subsystem for rebuild. Rebuilding itself happens lazily on the
// next RenderOverlayNow.
//
// Only the CreateD3DAndSwapChain thunk calls this, so a device or swapchain
// swap that does not re-enter that call is not detected.
static void HandleDeviceChange()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer || !renderer->data.renderWindows)
    {
        return;
    }

    auto& data = renderer->data;
    auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
    auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
    auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
    bool changed = false;
    if (swapChain && device && context)
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        changed = (swapChain != D3D().swapChain.Get() || device != D3D().device.Get() ||
                   context != D3D().context.Get());
        if (changed)
        {
            D3D().swapChain = swapChain;
            D3D().device = device;
            D3D().context = context;
        }
    }
    if (changed)
    {
        ParticleTextures::Shutdown();
        BadgeTextures::Shutdown();
        Deck::Shutdown();
        TextPostProcess::Shutdown();
        SceneMeter::Shutdown();
        DepthClip::Shutdown();
        Graffito::Shutdown();
        RenderSampling::Shutdown();
        Init().mipmapsGenerated.store(false, std::memory_order_release);
        Init().particleTexturesLoaded.store(false, std::memory_order_release);
        Init().badgeTexturesLoaded.store(false, std::memory_order_release);
        Init().postProcessInitialized.store(false, std::memory_order_release);
        Init().nextGraffitoRetryAtMs.store(0, std::memory_order_release);
        Init().backendReinitRequested.store(true, std::memory_order_release);
        if (!TryInstallPresentHook(swapChain))
        {
            logger::error("Hooks: Failed to (re)install Present hook on updated swapchain");
        }
        if (!Diag().deviceChangeLogged.exchange(true, std::memory_order_acq_rel))
        {
            logger::warn(
                "Hooks: Detected renderer device/swapchain change, scheduling backend "
                "refresh");
        }
    }
}

// Populate all four fixed font slots at one raster density. The configured font
// size remains unchanged because RasterizerDensity changes source sampling only.
// The caller holds Settings::Mutex() for shared access.
static bool BuildFontAtlas(float density)
{
    auto& atlas = *ImGui::GetIO().Fonts;
    atlas.Clear();
    atlas.TexGlyphPadding = RasterQuality::FONT_GLYPH_PADDING;

    // Glyph range: Basic Latin plus Latin-1 Supplement (0x0020-0x00FF), that is
    // Western European only. Cyrillic (0x0400+) and CJK (0x4E00+) fall back to
    // ImGui's default glyph.
    // TODO: configurable glyph ranges for broader locale support.
    static const ImWchar ranges[] = {
        0x0020,
        0x00FF,
        0,
    };

    // FreeType with light hinting keeps scaled text smooth.
    ImFontConfig config;
    config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
    config.OversampleH = 2;  // FreeType plus mipmaps make 4x unnecessary
    config.OversampleV = 2;
    config.PixelSnapH = false;  // Off, so glyphs keep subpixel positions
    config.RasterizerDensity = density;
    config.RasterizerMultiply = 1.15f;  // Give title, name, and level strokes more weight

    // Font paths come from the obfuscated asset manifest, falling back to the
    // INI path when the manifest has no entry. Both are already GUID-named.
    const auto fontPath = [](const std::string& mapped,
                             const std::string& ini) -> const std::string&
    { return mapped.empty() ? ini : mapped; };

    const auto addFontSlot = [&](const std::string& path, float size) -> bool
    {
        if (!path.empty() &&
            atlas.AddFontFromFileTTF(path.c_str(), size, &config, ranges) != nullptr)
        {
            return true;
        }
        return atlas.AddFontDefault(&config) != nullptr;
    };

    // The load order fixes the font indices that the renderer uses. Each slot
    // adds one fallback font when its configured asset is unavailable.
    const auto& font = Settings::Font();
    bool slotsReady =
        addFontSlot(fontPath(ProjectManifest::FontName(), font.NameFontPath), font.NameFontSize);
    slotsReady = addFontSlot(fontPath(ProjectManifest::FontLevel(), font.LevelFontPath),
                             font.LevelFontSize) &&
                 slotsReady;
    slotsReady = addFontSlot(fontPath(ProjectManifest::FontTitle(), font.TitleFontPath),
                             font.TitleFontSize) &&
                 slotsReady;

    config.RasterizerMultiply = 1.0f;  // Preserve the ornament font's authored weight.
    const auto& ornament = Settings::Ornament();
    slotsReady = addFontSlot(fontPath(ProjectManifest::FontOrnament(), ornament.FontPath),
                             ornament.FontSize) &&
                 slotsReady;

    return slotsReady && atlas.Build();
}

static bool FontAtlasFitsD3D11()
{
    const auto& atlas = *ImGui::GetIO().Fonts;
    return atlas.TexWidth > 0 && atlas.TexHeight > 0 &&
           atlas.TexWidth <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           atlas.TexHeight <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

// First-time ImGui init: create the context, load fonts, init the Win32 and
// DX11 backends, install the Present hook. Returns false while the swapchain is
// not ready yet; the caller may retry later. A failure part-way through undoes
// everything it created, so a retry starts clean.
static bool InitializeImGui()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return false;
    }

    auto& data = renderer->data;
    if (!data.renderWindows)
    {
        return false;
    }

    auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
    if (!swapChain)
    {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(std::addressof(desc))))
    {
        return false;
    }

    const auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
    const auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);

    if (!device || !context)
    {
        return false;
    }

    // Cache for the later mipmap generation pass.
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        D3D().device = device;
        D3D().context = context;
    }

    bool contextCreated = false;
    bool win32Initialized = false;
    bool dx11Initialized = false;
    auto cleanupFailedInit = [&]()
    {
        if (dx11Initialized)
        {
            ImGui_ImplDX11_Shutdown();
            dx11Initialized = false;
        }
        if (win32Initialized)
        {
            ImGui_ImplWin32_Shutdown();
            win32Initialized = false;
        }
        if (contextCreated)
        {
            ImGui::DestroyContext();
            contextCreated = false;
        }
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            D3D().device.Reset();
            D3D().context.Reset();
            D3D().swapChain.Reset();
            D3D().originalPresent.store(nullptr, std::memory_order_release);
        }
        ParticleTextures::Shutdown();
        TextPostProcess::Shutdown();
        SceneMeter::Shutdown();
        DepthClip::Shutdown();
        Graffito::Shutdown();
        RenderSampling::Shutdown();
    };

    ImGui::CreateContext();
    contextCreated = true;

    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = false;
    io.IniFilename = nullptr;

    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    float atlasDensity = RasterQuality::FONT_DENSITY;
    bool atlasReady = BuildFontAtlas(RasterQuality::FONT_DENSITY);
    if (!atlasReady || !FontAtlasFitsD3D11())
    {
        logger::warn(
            "Hooks: {:.1f}x font atlas build produced {}x{} pixels or failed; rebuilding "
            "at {:.1f}x",
            RasterQuality::FONT_DENSITY,
            io.Fonts->TexWidth,
            io.Fonts->TexHeight,
            RasterQuality::FONT_FALLBACK_DENSITY);
        atlasReady = BuildFontAtlas(RasterQuality::FONT_FALLBACK_DENSITY);
        if (!atlasReady || !FontAtlasFitsD3D11())
        {
            logger::error(
                "Hooks: fallback font atlas build failed or exceeded D3D11 limits "
                "({}x{})",
                io.Fonts->TexWidth,
                io.Fonts->TexHeight);
            cleanupFailedInit();
            return false;
        }
        atlasDensity = RasterQuality::FONT_FALLBACK_DENSITY;
    }
    logger::info("Hooks: built font atlas at {:.1f}x ({}x{}, {} px glyph padding)",
                 atlasDensity,
                 io.Fonts->TexWidth,
                 io.Fonts->TexHeight,
                 io.Fonts->TexGlyphPadding);

    if (!ImGui_ImplWin32_Init(desc.OutputWindow))
    {
        logger::error("Hooks: ImGui Win32 backend initialization failed");
        cleanupFailedInit();
        return false;
    }
    win32Initialized = true;
    if (!ImGui_ImplDX11_Init(device, context))
    {
        logger::error("Hooks: ImGui DX11 backend initialization failed");
        cleanupFailedInit();
        return false;
    }
    dx11Initialized = true;

    // Cache the swap chain, then hook Present for the post-upscaler path.
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        D3D().swapChain = swapChain;
    }

    if (!TryInstallPresentHook(swapChain))
    {
        logger::error("Hooks: Failed to install Present hook");
        cleanupFailedInit();
        return false;
    }

    Init().initialized.store(true, std::memory_order_release);
    Init().nextInitRetryAtMs.store(0, std::memory_order_release);
    if (!Diag().imguiInitializedLogged.exchange(true, std::memory_order_acq_rel))
    {
        logger::info("Hooks: ImGui/DX11 initialized");
    }
    return true;
}

// Late-bootstrap path: the D3D creation hook can fire before glyph is loaded,
// so ImGui init must also be reachable from the per-frame hooks. The two
// callers are CreateD3DAndSwapChain::thunk and PostDisplay::thunk. PresentHook
// never calls this, so it cannot bootstrap the overlay on its own.
// Three guards make concurrent calls from those two thunks safe:
//   1. initialized       - acquire-load early-out. It stays true for the rest
//                          of the session unless RenderOverlayNow clears it
//                          after a failed DX11 backend reinit, which lets this
//                          function run again.
//   2. nextInitRetryAtMs - after a failed attempt, wait
//                          INIT_RETRY_INTERVAL_MS (5 s); retrying every frame
//                          floods the log.
//   3. initializing      - CAS re-entry guard. InitializeImGui touches the
//                          ImGui and D3D singletons, so concurrent entry races.
// CAS plus RAII, not a mutex: a mutex would hold render-thread frames behind an
// in-flight init, while the CAS lets a non-winner return immediately.
static void EnsureOverlayInitialized()
{
    static constexpr std::uint64_t INIT_RETRY_INTERVAL_MS = 5000;
    const auto nowMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());

    if (Init().initialized.load(std::memory_order_acquire))
    {
        return;
    }

    const auto nextRetryAt = Init().nextInitRetryAtMs.load(std::memory_order_acquire);
    if (nextRetryAt != 0 && nowMs < nextRetryAt)
    {
        return;
    }

    bool expected = false;
    if (!Init().initializing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    struct InitScope
    {
        ~InitScope() { Init().initializing.store(false, std::memory_order_release); }
    } _;

    if (!InitializeImGui())
    {
        Init().nextInitRetryAtMs.store(nowMs + INIT_RETRY_INTERVAL_MS, std::memory_order_release);
    }
}

// Thunk-call hook on D3D11 device and swap chain creation. Runs the original
// call first, then either initializes ImGui or handles a device change.
struct CreateD3DAndSwapChain
{
    static void thunk()
    {
        func();

        if (Init().initialized.load(std::memory_order_acquire))
        {
            HandleDeviceChange();
            return;
        }

        EnsureOverlayInitialized();
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// Builds a mipmapped copy of the ImGui font atlas and binds it as io.Fonts->TexID
// so text stays clean as plates scale down with distance. Runs once per device.
static void GenerateMipmappedFontAtlas(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (Init().mipmapsGenerated.load(std::memory_order_acquire) || !device || !context)
    {
        return;
    }

    auto& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    bool mipmapsReady = false;
    if (pixels && width > 0 && height > 0)
    {
        int mipLevels = 1;
        int maxDim = (width > height) ? width : height;
        while (maxDim > 1)
        {
            maxDim >>= 1;
            mipLevels++;
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = mipLevels;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> fontTexture;
        if (SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, fontTexture.GetAddressOf())))
        {
            context->UpdateSubresource(fontTexture.Get(), 0, nullptr, pixels, width * 4, 0);

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = mipLevels;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fontSRV;
            if (SUCCEEDED(device->CreateShaderResourceView(
                    fontTexture.Get(), &srvDesc, fontSRV.GetAddressOf())))
            {
                context->GenerateMips(fontSRV.Get());
                // The SRV already in io.Fonts->TexID belongs to the ImGui DX11
                // backend (bd->pFontTextureView). Releasing it here would dangle
                // that pointer, and the backend would double-free it in
                // InvalidateDeviceObjects on the next device rebuild. Keep the
                // mipmapped SRV in a glyph-owned ComPtr instead: reassigning it
                // releases the previous glyph atlas, and the backend keeps
                // owning and freeing its own non-mipmapped texture.
                D3D().fontAtlasSRV = fontSRV;
                io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(fontSRV.Get()));
                mipmapsReady = true;
            }
        }
    }
    if (mipmapsReady)
    {
        Init().mipmapsGenerated.store(true, std::memory_order_release);
    }
}

// Load particle textures if enabled and not yet loaded. Turning the setting off
// does not unload them; a device change (HandleDeviceChange) or a failed ImGui
// init (cleanupFailedInit) releases them. A failed Initialize leaves the flag
// clear, so the next frame retries.
static void EnsureParticleTexturesLoaded(ID3D11Device* device, bool useParticleTextures)
{
    if (useParticleTextures && !Init().particleTexturesLoaded.load(std::memory_order_acquire) &&
        device)
    {
        if (ParticleTextures::Initialize(device))
        {
            Init().particleTexturesLoaded.store(true, std::memory_order_release);
        }
    }
}

// Rasterizes the status badge SVGs. A settings hot reload bumps the generation
// counter, which drops the cache, so a renamed icon or a new folder takes effect
// without a restart.
static void EnsureBadgeTexturesLoaded(ID3D11Device* device)
{
    if (!device)
    {
        return;
    }
    const uint32_t gen = Settings::Generation().load(std::memory_order_acquire);
    if (Init().badgeTexturesLoaded.load(std::memory_order_acquire) &&
        Init().badgeTexturesGen.load(std::memory_order_acquire) == gen)
    {
        return;
    }

    bool enabled = false;
    bool tierImages = false;
    std::string folder;
    std::vector<std::string> names;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        const auto& ic = Settings::Icons();
        enabled = ic.Enabled && !ic.Folder.empty();
        folder = ic.Folder;
        tierImages = ic.TierBadgeImages;
        names = {ic.FollowerIcon,
                 ic.AllyIcon,
                 ic.HostileIcon,
                 ic.WeakIcon,
                 ic.StrongIcon,
                 ic.DeadlyIcon,
                 ic.BeastIcon,
                 ic.UndeadIcon,
                 ic.DaedraIcon,
                 ic.DragonIcon,
                 // Always-on slots: further NPC and player indicators.
                 ic.NeutralIcon,
                 ic.HumanoidIcon,
                 ic.EvenIcon,
                 ic.GuardIcon,
                 ic.MerchantIcon,
                 ic.CommonerIcon,
                 ic.EssentialIcon,
                 ic.ProtectedIcon,
                 ic.MortalIcon,
                 ic.CombatIcon,
                 ic.AlertIcon,
                 ic.IdleIcon,
                 ic.SneakHiddenIcon,
                 ic.SneakDetectedIcon,
                 ic.SneakOffIcon,
                 ic.EncumberedIcon,
                 ic.NormalWeightIcon,
                 ic.WantedIcon,
                 ic.BountyClearIcon,
                 ic.TierLowIcon,
                 ic.TierMidIcon,
                 ic.TierHighIcon};
    }

    if (enabled)
    {
        BadgeTextures::Initialize(device, folder, names);
    }
    else
    {
        BadgeTextures::Shutdown();
    }
    // Full-color tier emblem PNGs come from the obfuscated manifest, in rank
    // order. An empty list clears them (icons disabled, or the feature is off).
    static const std::vector<std::string> kNoBadges{};
    BadgeTextures::InitializeTierImages(
        device, (enabled && tierImages) ? ProjectManifest::TierBadges() : kNoBadges);
    Init().badgeTexturesGen.store(gen, std::memory_order_release);
    Init().badgeTexturesLoaded.store(true, std::memory_order_release);
}

// Builds and submits one overlay frame: first-use resource setup, ImGui frame,
// Deck compose, draw-data submit. Returns without drawing when ImGui is not
// initialized yet or the context is gone. All exceptions are caught and logged
// at a decaying rate: the first 5, then every 120th.
void RenderOverlayNow()
{
    if (!Init().initialized.load(std::memory_order_acquire))
    {
        return;
    }
    if (!ImGui::GetCurrentContext())
    {
        return;
    }

    try
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            device = D3D().device;
            context = D3D().context;
            swapChain = D3D().swapChain;
        }

        if (Init().backendReinitRequested.exchange(false, std::memory_order_acq_rel))
        {
            if (!device || !context)
            {
                logger::error("Hooks: Backend refresh requested without valid D3D device/context");
                return;
            }
            ImGui_ImplDX11_Shutdown();
            if (!ImGui_ImplDX11_Init(device.Get(), context.Get()))
            {
                logger::error(
                    "Hooks: Failed to reinitialize ImGui DX11 backend after device change");
                Init().initialized.store(false, std::memory_order_release);
                return;
            }
            Init().mipmapsGenerated.store(false, std::memory_order_release);
            Init().particleTexturesLoaded.store(false, std::memory_order_release);
            Init().postProcessInitialized.store(false, std::memory_order_release);
            Init().nextGraffitoRetryAtMs.store(0, std::memory_order_release);
            logger::info("Hooks: Reinitialized ImGui DX11 backend after device change");
        }

        // A device change releases the old sampler states. Initialize attempts
        // creation once for each device and leaves ImGui sampling unchanged on failure.
        RenderSampling::Initialize(device.Get(), context.Get());

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // First frame after a device change rebuilds the mipmapped font atlas.
        GenerateMipmappedFontAtlas(device.Get(), context.Get());

        bool useParticleTextures = false;
        {
            const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
            useParticleTextures = Settings::Particle().UseParticleTextures;
        }

        // Load particle textures on first frame
        EnsureParticleTexturesLoaded(device.Get(), useParticleTextures);

        // Rasterize status badge SVGs (re-runs after settings hot reload)
        EnsureBadgeTexturesLoaded(device.Get());

        // Initialize GPU post-processing on first frame. SceneMeter and
        // DepthClip share the TextPostProcess gate, so while
        // TextPostProcess::Initialize keeps failing all three are retried every
        // frame. Each one degrades to a no-op when its own init fails.
        if (!Init().postProcessInitialized.load(std::memory_order_acquire) && device && context)
        {
            if (TextPostProcess::Initialize(device.Get(), context.Get()))
            {
                Init().postProcessInitialized.store(true, std::memory_order_release);
            }
            SceneMeter::Initialize(device.Get(), context.Get());
            DepthClip::Initialize(device.Get(), context.Get());
        }

        // Graffito owns a separate shader pipeline, so a transient failure must
        // not become permanent just because the other post-process helpers
        // succeeded. Retry on a slow cadence to keep the log quiet.
        if (!Graffito::IsInitialized() && device && context)
        {
            static constexpr std::uint64_t GRAFFITO_RETRY_INTERVAL_MS = 5000;
            const auto nowMs =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
            const auto nextRetryAt = Init().nextGraffitoRetryAtMs.load(std::memory_order_acquire);
            if (nextRetryAt == 0 || nowMs >= nextRetryAt)
            {
                if (Graffito::Initialize(device.Get(), context.Get()))
                {
                    Init().nextGraffitoRetryAtMs.store(0, std::memory_order_release);
                }
                else
                {
                    Init().nextGraffitoRetryAtMs.store(nowMs + GRAFFITO_RETRY_INTERVAL_MS,
                                                       std::memory_order_release);
                }
            }
        }

        // Set display size to actual screen resolution
        {
            const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
            auto& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(screenSize.width);
            io.DisplaySize.y = static_cast<float>(screenSize.height);
            TextPostProcess::OnResize(screenSize.width, screenSize.height);
            SceneMeter::OnResize(screenSize.width, screenSize.height);
        }

        ImGui::NewFrame();

        // Clear the nav windowing target so the overlay never takes keyboard nav.
        if (auto g = ImGui::GetCurrentContext())
        {
            g->NavWindowingTarget = nullptr;
        }

        // Deck is independent of the ambient nameplate toggle. Its status
        // toast still renders while a card is developing, but disabled
        // nameplates must not reappear merely because Deck requested a frame.
        if (Renderer::IsOverlayAllowedRT())
        {
            Renderer::Draw();
        }
        Deck::DrawNotification();

        ImGui::EndFrame();
        ImGui::Render();
        // Compose/read back the card before the normal draw data lands on the
        // game target. The scene texture was copied earlier this frame, pre-HUD
        // in PostDisplay or in PresentHook on the fallback path. If neither
        // copy ran, Deck::Process captures here instead, after the HUD.
        Deck::Process(device.Get(), context.Get(), swapChain.Get());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        Frame().overlayRenderedThisFrame.store(true, std::memory_order_release);
    }
    catch (const std::exception& e)
    {
        const uint32_t count =
            Diag().renderExceptionCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count <= 5 || (count % 120) == 0)
        {
            logger::error("Hooks: Exception in RenderOverlayNow (#{}): {}", count, e.what());
        }
    }
    catch (...)
    {
        const uint32_t count =
            Diag().renderExceptionCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count <= 5 || (count % 120) == 0)
        {
            logger::error("Hooks: Unknown exception in RenderOverlayNow (#{}).", count);
        }
    }
}

// Safety net for overlay rendering. Some upscalers (DLSS, FSR) restructure the
// pipeline so PostDisplay is skipped or deferred; the overlay is drawn here
// instead, just before the original Present flips the backbuffer.
//
// Recovery limitation: when D3D().originalPresent is null, the fallback reads
// vtable[8] from the swapchain. That slot already holds PresentHook, so the
// candidate is rejected. Recovery therefore succeeds only when a third-party
// hook has overwritten vtable[8] with its own function, which is then adopted
// as "original" - correct for a linear hook chain, but the wrong call order if
// that third-party hook saved PresentHook as *its* original.
// TryInstallPresentHook always sets originalPresent before any Present call can
// occur, so this path is unreachable in practice.
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    // Present is the frame boundary for the fallback path. Consume the marker
    // left by PostDisplay; when none was published, service a complete fallback
    // frame. Clearing the marker again after this render keeps a Present-only
    // pipeline from turning into a one-shot path.
    const bool renderedByPostDisplay =
        Frame().overlayRenderedThisFrame.exchange(false, std::memory_order_acq_rel);
    if (!renderedByPostDisplay)
    {
        // Tick unconditionally so the game thread can publish the gate. Do not
        // call CanDrawOverlay() here: it dereferences cell state off the game
        // thread.
        Renderer::TickRT();
        Renderer::PrepareDeckCaptureRT();

        if (Deck::NeedsSceneCapture())
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            {
                const std::lock_guard<std::mutex> lock(StateMutex());
                device = D3D().device;
                context = D3D().context;
            }
            Deck::CaptureScene(device.Get(), context.Get(), swapChain);
        }

        const bool shouldRender = Init().initialized.load(std::memory_order_acquire) &&
                                  (Renderer::IsOverlayAllowedRT() || Deck::NeedsFrame());
        Frame().shouldRenderOverlay.store(shouldRender, std::memory_order_release);

        if (shouldRender &&
            !Diag().presentBootstrapLogged.exchange(true, std::memory_order_acq_rel))
        {
            logger::info("Hooks: Present fallback bootstrapped overlay rendering");
        }
        if (shouldRender)
        {
            RenderOverlayNow();
            Frame().overlayRenderedThisFrame.store(false, std::memory_order_release);
        }
    }
    Frame().shouldRenderOverlay.store(false, std::memory_order_release);

    // Fast path: the atomic load avoids the mutex in the common case.
    PresentFn originalPresent = D3D().originalPresent.load(std::memory_order_acquire);

    if (!originalPresent)
    {
        // Slow path: lock and re-read in case of a concurrent store.
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            originalPresent = D3D().originalPresent.load(std::memory_order_relaxed);
        }

        if (!originalPresent)
        {
            // Attempt on-the-fly recovery from the swapchain vtable.
            if (swapChain)
            {
                void** vtable = *reinterpret_cast<void***>(swapChain);
                if (vtable && vtable[8])
                {
                    auto candidate = reinterpret_cast<PresentFn>(vtable[8]);
                    if (candidate != reinterpret_cast<PresentFn>(&PresentHook))
                    {
                        const std::lock_guard<std::mutex> lock(StateMutex());
                        if (!D3D().originalPresent.load(std::memory_order_relaxed))
                        {
                            D3D().originalPresent.store(candidate, std::memory_order_release);
                        }
                        originalPresent = D3D().originalPresent.load(std::memory_order_relaxed);
                    }
                }
            }

            if (!originalPresent)
            {
                if (!Diag().missingPresentLogged.exchange(true, std::memory_order_acq_rel))
                {
                    logger::error(
                        "Hooks: Missing original IDXGISwapChain::Present pointer, returning "
                        "success to avoid frame hard-fail");
                }
                return S_OK;
            }
        }
    }

    // Forward to the real IDXGISwapChain::Present we saved during init.
    return originalPresent(swapChain, syncInterval, flags);
}

// VTable hook for HUDMenu::PostDisplay (vtable index 6), installed with
// Stl::WriteVfunc<RE::HUDMenu, PostDisplay>(). Runs on the render thread every
// frame and calls the original PostDisplay first, so the game HUD is drawn
// before the overlay; the overlay then lands on top and in screenshots.
// See Renderer::TickRT / IsOverlayAllowedRT / Draw, and PresentHook for the
// fallback path when upscalers skip PostDisplay.
struct PostDisplay
{
    // Thunk installed in place of HUDMenu::PostDisplay. a_menu is the HUD menu
    // instance and may be null.
    static void thunk(RE::IMenu* a_menu)
    {
        Frame().shouldRenderOverlay.store(false, std::memory_order_release);
        Frame().overlayRenderedThisFrame.store(false, std::memory_order_release);

        if (!Diag().firstPostDisplayLogged.exchange(true, std::memory_order_acq_rel))
        {
            logger::debug("Hooks: HUDMenu::PostDisplay hit");
        }

        EnsureOverlayInitialized();

        // Queue the game-thread snapshot unconditionally so it can publish the
        // allowOverlay gate. The render thread must not call CanDrawOverlay()
        // itself: it dereferences player->GetParentCell(), which can race with
        // cell teardown during exterior streaming. QueueSnapshotUpdate is
        // idempotent per frame and self-clears the snapshot when not allowed.
        Renderer::TickRT();

        // Backend not up yet: run the original HUD draw and skip the overlay.
        if (!Init().initialized.load(std::memory_order_acquire))
        {
            func(a_menu);
            return;
        }

        // The game hides the HUD movie while it takes a screenshot. Glyph is an
        // independent overlay and must still render into that capture, so only
        // require a valid HUD menu and movie here, never their visibility.
        if (!a_menu || !a_menu->uiMovie)
        {
            func(a_menu);
            return;
        }

        // Resolve the key press from the plain actor snapshot, then copy the
        // scene before HUDMenu draws the crosshair and meters. If this copy
        // fails, Deck::Process falls back to the post-HUD target.
        Renderer::PrepareDeckCaptureRT();
        if (Deck::NeedsSceneCapture())
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
            {
                const std::lock_guard<std::mutex> lock(StateMutex());
                device = D3D().device;
                context = D3D().context;
                swapChain = D3D().swapChain;
            }
            Deck::CaptureScene(device.Get(), context.Get(), swapChain.Get());
        }

        // Gate rendering on the game-thread-published atomic, not a direct
        // game-state read.
        bool shouldRender = Renderer::IsOverlayAllowedRT() || Deck::NeedsFrame();
        Frame().shouldRenderOverlay.store(shouldRender, std::memory_order_release);

        // Original HUD draw, before the overlay.
        func(a_menu);

        if (shouldRender && !Frame().overlayRenderedThisFrame.load(std::memory_order_acquire))
        {
            RenderOverlayNow();
        }
    }

    static inline REL::Relocation<decltype(thunk)> func;
    // Virtual function table index for PostDisplay
    static inline std::size_t idx = 0x6;
};

void Install()
{
    // Defense in depth: plugin loading rejects anything except SE/AE. This
    // protects the patch site if Install() is reached through another path.
    // GLYPH_OFFSET selects the matching SE or AE/GOG call offset at runtime.
    if (!REL::Module::IsSE() && !REL::Module::IsAE())
    {
        SKSE::log::error("Hooks: Unsupported Skyrim runtime; skipping hook installation");
        return;
    }

    bool d3dHookInstalled = false;
    bool hudHookInstalled = false;

    try
    {
        // D3D11 device creation drives ImGui initialization.
        REL::Relocation<std::uintptr_t> target{RELOCATION_ID(75595, 77226),
                                               GLYPH_OFFSET(0x9, 0x275)};
        // WriteThunkCall (write_call<5>) overwrites a 5-byte relative CALL, so
        // verify the patch site starts with the CALL rel32 opcode (0xE8). An
        // offset drift then fails loudly instead of clobbering code.
        const auto patchOpcode = *reinterpret_cast<const std::uint8_t*>(target.address());
        if (patchOpcode != 0xE8)
        {
            SKSE::log::error(
                "Hooks: CreateD3DAndSwapChain patch site reads 0x{:02X}, expected 0xE8 "
                "(CALL rel32); aborting D3D hook to avoid corrupting code",
                patchOpcode);
        }
        else
        {
            Stl::WriteThunkCall<CreateD3DAndSwapChain>(target.address());
            d3dHookInstalled = true;
        }
    }
    catch (const std::exception& e)
    {
        SKSE::log::error("Hooks: Failed to install CreateD3DAndSwapChain hook: {}", e.what());
    }
    catch (...)
    {
        SKSE::log::error("Hooks: Failed to install CreateD3DAndSwapChain hook (unknown error)");
    }

    try
    {
        // HUD post-display draws the overlay after the game HUD is complete.
        Stl::WriteVfunc<RE::HUDMenu, PostDisplay>();
        hudHookInstalled = true;
    }
    catch (const std::exception& e)
    {
        SKSE::log::error("Hooks: Failed to install HUDMenu::PostDisplay hook: {}", e.what());
    }
    catch (...)
    {
        SKSE::log::error("Hooks: Failed to install HUDMenu::PostDisplay hook (unknown error)");
    }

    if (d3dHookInstalled && hudHookInstalled)
    {
        SKSE::log::info("Hooks: Installed");
    }
    else
    {
        SKSE::log::warn("Hooks: Partial install (CreateD3DAndSwapChain={}, HUDPostDisplay={})",
                        d3dHookInstalled,
                        hudHookInstalled);
    }
}
}  // namespace Hooks
