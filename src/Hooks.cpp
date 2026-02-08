#include "Hooks.h"
#include "ParticleTextures.h"
#include "Renderer.h"
#include "Settings.h"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Hooks
{
    /// Atomic flag indicating whether ImGui has been initialized
    std::atomic<bool> initialized = false;

    /// Flag indicating whether mipmapped font atlas has been created
    std::atomic<bool> mipmapsGenerated = false;

    /// Flag indicating whether particle textures have been loaded
    std::atomic<bool> particleTexturesLoaded = false;

    /// Flag indicating overlay should be rendered this frame
    std::atomic<bool> shouldRenderOverlay = false;

    /// Flag indicating overlay has been rendered this frame
    std::atomic<bool> overlayRenderedThisFrame = false;

    /// Stored D3D11 device for mipmap generation
    ID3D11Device* g_device = nullptr;

    /// Stored D3D11 context for mipmap generation
    ID3D11DeviceContext* g_context = nullptr;

    /// Stored swap chain for Present hook
    IDXGISwapChain* g_swapChain = nullptr;

    /// Original Present function pointer
    using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_originalPresent = nullptr;

    // Forward declaration of Present hook
    HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

    // Forward declaration of overlay render function (used by screenshot hook)
    void RenderOverlayNow();

    // Hook for D3D11 device/swap chain creation
    struct CreateD3DAndSwapChain
    {
        static void thunk()
        {
            func();

            // Ensure initialize once using atomic compare-and-swap
            bool expected = false;
            if (!initialized.compare_exchange_strong(expected, true)) {
                return;  // Already initialized, bail out
            }

            // Get Skyrim's renderer singleton
            auto renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer) {
                return;
            }

            // Access the renderer's data which contains D3D objects
            auto& data = renderer->data;
            if (!data.renderWindows) {
                return;
            }

            // Get the swap chain from the first render window
            auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
            if (!swapChain) {
                return;
            }

            // Retrieve swap chain description to get window handle
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(swapChain->GetDesc(std::addressof(desc)))) {
                return;
            }

            // Get D3D11 device and context from Skyrim's renderer
            const auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
            const auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);

            if (!device || !context) {
                return;
            }

            // Store for later mipmap generation
            g_device = device;
            g_context = context;

            // Create ImGui context for our overlay
            ImGui::CreateContext();

            // Configure ImGui I/O settings
            auto& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard navigation
            io.MouseDrawCursor = false;                            // Do not draw ImGui cursor
            io.IniFilename = nullptr;                              // Disable imgui.ini file

            // Load custom fonts for different text elements
            // Character range: Basic Latin + Latin-1 Supplement
            static const ImWchar ranges[] = { 0x0020, 0x00FF, 0, };
            
            // Font config: High oversampling for quality when scaling down
            // Fonts can render at 25%
            ImFontConfig config;
            config.OversampleH = 4;     // High horizontal oversampling for scaled text
            config.OversampleV = 4;     // High vertical oversampling for scaled text
            config.PixelSnapH = false;  // Disable pixel snapping for smooth subpixel rendering

            // Font Index 0: Name font
            ImFont* nameFont = io.Fonts->AddFontFromFileTTF(Settings::NameFontPath.c_str(), Settings::NameFontSize, &config, ranges);
            if (!nameFont) {
                // Fallback to default font if custom font fails to load
                io.Fonts->AddFontDefault();
            }

            // Font Index 1: Level font
            ImFont* levelFont = io.Fonts->AddFontFromFileTTF(Settings::LevelFontPath.c_str(), Settings::LevelFontSize, &config, ranges); 
            if (!levelFont) {
                io.Fonts->AddFontDefault();
            }

            // Font Index 2: Title font
            ImFont* titleFont = io.Fonts->AddFontFromFileTTF(Settings::TitleFontPath.c_str(), Settings::TitleFontSize, &config, ranges);
            if (!titleFont) {
                io.Fonts->AddFontDefault();
            }

            // Font Index 3: Ornament font
            if (!Settings::OrnamentFontPath.empty()) {
                ImFont* ornamentFont = io.Fonts->AddFontFromFileTTF(Settings::OrnamentFontPath.c_str(), Settings::OrnamentFontSize, &config, ranges);
                if (!ornamentFont) {
                    io.Fonts->AddFontDefault();
                }
            } else {
                // Add placeholder so font indices stay consistent
                io.Fonts->AddFontDefault();
            }

            // Initialize ImGui backends for Win32 and DirectX 11
            if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
                return;
            }
            if (!ImGui_ImplDX11_Init(device, context)) {
                return;
            }

            // Store swap chain and hook Present for post-upscaler rendering
            g_swapChain = swapChain;

            // Hook IDXGISwapChain::Present via COM vtable patching.
            void** vtable = *reinterpret_cast<void***>(swapChain);
            g_originalPresent = reinterpret_cast<PresentFn>(vtable[8]);

            // The vtable lives in read-only memory. Temporarily mark the single
            // slot as writable, swap in our hook, then restore the original
            // protection flags to leave memory in its expected state.
            DWORD oldProtect;
            VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
            vtable[8] = reinterpret_cast<void*>(&PresentHook);
            VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);

            // Mark initialization as complete
            initialized.store(true);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    /// Render overlay immediately
    void RenderOverlayNow()
    {
        if (!initialized.load(std::memory_order_acquire)) return;

        try {
            // Start new ImGui frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();

            // Generate mipmapped font atlas on first frame
            if (!mipmapsGenerated.exchange(true) && g_device && g_context) {
                auto& io = ImGui::GetIO();
                unsigned char* pixels = nullptr;
                int width = 0, height = 0;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

                if (pixels && width > 0 && height > 0) {
                    int mipLevels = 1;
                    int maxDim = (width > height) ? width : height;
                    while (maxDim > 1) { maxDim >>= 1; mipLevels++; }

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

                    ID3D11Texture2D* fontTexture = nullptr;
                    if (SUCCEEDED(g_device->CreateTexture2D(&texDesc, nullptr, &fontTexture))) {
                        g_context->UpdateSubresource(fontTexture, 0, nullptr, pixels, width * 4, 0);

                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = mipLevels;

                        ID3D11ShaderResourceView* fontSRV = nullptr;
                        if (SUCCEEDED(g_device->CreateShaderResourceView(fontTexture, &srvDesc, &fontSRV))) {
                            g_context->GenerateMips(fontSRV);
                            if (io.Fonts->TexID) {
                                ((ID3D11ShaderResourceView*)io.Fonts->TexID)->Release();
                            }
                            io.Fonts->SetTexID((ImTextureID)fontSRV);
                        }
                        fontTexture->Release();
                    }
                }
            }

            // Load particle textures on first frame
            if (!particleTexturesLoaded.exchange(true) && g_device && Settings::UseParticleTextures) {
                ParticleTextures::Initialize(g_device);
            }

            // Set display size to actual screen resolution
            {
                static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
                auto& io = ImGui::GetIO();
                io.DisplaySize.x = static_cast<float>(screenSize.width);
                io.DisplaySize.y = static_cast<float>(screenSize.height);
            }

            ImGui::NewFrame();

            // Disable nav system
            if (auto g = ImGui::GetCurrentContext()) {
                g->NavWindowingTarget = nullptr;
            }

            // Draw overlay
            Renderer::Draw();

            // Finalize and render
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            // Mark that overlay was rendered this frame
            overlayRenderedThisFrame.store(true, std::memory_order_release);
        } catch (...) {
            // Silently handle errors to maintain game stability
        }
    }

    /// Present hook, safety net for overlay rendering.
    /// Some upscalers (DLSS, FSR, etc.) restructure the rendering pipeline in
    /// ways that can cause PostDisplay to be skipped or deferred.
    /// This hook catches that case, we render it here as a last resort,
    /// right before the original Present flips the backbuffer to screen.
    HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        if (shouldRenderOverlay.load(std::memory_order_acquire) &&
            !overlayRenderedThisFrame.load(std::memory_order_acquire)) {
            RenderOverlayNow();
        }

        // Forward to the real IDXGISwapChain::Present we saved during init.
        return g_originalPresent(swapChain, syncInterval, flags);
    }

    // Hook for HUD post-display, collects data and renders overlay.
    // Rendering happens after the original PostDisplay, so HUD is complete
    // which ensures overlay is in backbuffer for screenshots
    struct PostDisplay
    {
        static void thunk(RE::IMenu* a_menu)
        {
            // Reset render flags at start of frame
            shouldRenderOverlay.store(false, std::memory_order_release);
            overlayRenderedThisFrame.store(false, std::memory_order_release);

            // Early exit checks
            if (!initialized.load(std::memory_order_acquire) || !CanDrawOverlay()) {
                func(a_menu);
                return;
            }

            // Verify menu is valid and visible
            if (!a_menu || !a_menu->uiMovie || !a_menu->uiMovie->GetVisible()) {
                func(a_menu);
                return;
            }

            // Update render thread state to queue actor data updates
            Renderer::TickRT();

            // Check if we need to apply appearance template
            extern void Renderer_CheckAppearanceTemplate();
            Renderer_CheckAppearanceTemplate();

            // Check if overlay should be rendered
            bool shouldRender = Renderer::IsOverlayAllowedRT();
            shouldRenderOverlay.store(shouldRender, std::memory_order_release);

            // Call the original PostDisplay function first
            func(a_menu);

            if (shouldRender && !overlayRenderedThisFrame.exchange(true)) {
                RenderOverlayNow();
            }
        }

        /// Original function pointer
        static inline REL::Relocation<decltype(thunk)> func;
        /// Virtual function table index for PostDisplay
        static inline std::size_t idx = 0x6;
    };

    void Install()
    {
        // Hook D3D11 device creation for ImGui initialization
        REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(75595, 77226), OFFSET(0x9, 0x275) };
        stl::write_thunk_call<CreateD3DAndSwapChain>(target.address());

        // Hook HUD post-display, renders overlay after game HUD is complete
        stl::write_vfunc<RE::HUDMenu, PostDisplay>();

        SKSE::log::info("Hooks: Installed");
    }
}
