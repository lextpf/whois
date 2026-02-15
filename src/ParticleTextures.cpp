#include <SKSE/SKSE.h>

#include "ParticleTextures.h"
#include "Settings.h"

#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cmath>
#include <filesystem>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace ParticleTextures
{
    // Number of particle texture types
    static constexpr int NUM_TYPES = 5;  // Stars, Sparks, Wisps, Runes, Orbs

    // Texture info struct
    struct TextureInfo {
        ID3D11ShaderResourceView* srv = nullptr;
        int width = 0;
        int height = 0;
    };

    // Multiple textures per particle type
    static std::array<std::vector<TextureInfo>, NUM_TYPES> g_textures;
    static bool g_initialized = false;

    // Point sampler for small sprites
    static ID3D11SamplerState* g_pointSampler = nullptr;
    // Linear sampler for high-resolution textures
    static ID3D11SamplerState* g_linearSampler = nullptr;
    static ID3D11BlendState* g_additiveBlend = nullptr;
    static ID3D11BlendState* g_screenBlend = nullptr;
    static ID3D11Device* g_device = nullptr;
    static ID3D11DeviceContext* g_context = nullptr;

    /**
     * Load a PNG file using WIC and create a D3D11 texture.
     * Returns TextureInfo with dimensions.
     */
    static TextureInfo LoadTextureFromFile(ID3D11Device* device, const std::string& path)
    {
        TextureInfo info;
        if (!device || path.empty()) return info;

        // Convert path to wide string
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wideLen <= 0) return info;

        std::wstring widePath(wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &widePath[0], wideLen);

        // Initialize COM
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // Create WIC factory
        ComPtr<IWICImagingFactory> wicFactory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create WIC factory for {}", path);
            return info;
        }

        // Load the image
        ComPtr<IWICBitmapDecoder> decoder;
        hr = wicFactory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) {
            SKSE::log::debug("ParticleTextures: Failed to load image: {}", path);
            return info;
        }

        // Get first frame
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return info;

        // Convert to RGBA
        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return info;

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0f,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return info;

        // Get dimensions
        UINT width, height;
        hr = converter->GetSize(&width, &height);
        if (FAILED(hr)) return info;

        // Read pixels
        UINT stride = width * 4;
        UINT bufferSize = stride * height;
        std::vector<BYTE> pixels(bufferSize);
        hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());
        if (FAILED(hr)) return info;

        // Sanitize transparent pixels so blend modes (especially screen-like) don't pick up
        // hidden RGB from fully transparent texels and produce box artifacts.
        {
            const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
            for (size_t i = 0; i < pixelCount; ++i) {
                const size_t idx = i * 4;
                BYTE& r = pixels[idx + 0];
                BYTE& g = pixels[idx + 1];
                BYTE& b = pixels[idx + 2];
                const BYTE a = pixels[idx + 3];
                if (a == 0) {
                    r = 0;
                    g = 0;
                    b = 0;
                }
            }
        }

        // Create D3D11 texture
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = pixels.data();
        initData.SysMemPitch = stride;

        ComPtr<ID3D11Texture2D> texture;
        hr = device->CreateTexture2D(&texDesc, &initData, &texture);
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create texture for {}", path);
            return info;
        }

        // Create SRV
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        hr = device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv);
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create SRV for {}", path);
            return info;
        }

        info.srv = srv;
        info.width = static_cast<int>(width);
        info.height = static_cast<int>(height);

        SKSE::log::debug("ParticleTextures: Loaded {}x{} texture: {}", width, height, path);
        return info;
    }

    /**
     * Load all PNG files from a folder into the texture array for a particle type.
     */
    static int LoadTexturesFromFolder(ID3D11Device* device, int styleIndex, const std::string& folderPath)
    {
        if (styleIndex < 0 || styleIndex >= NUM_TYPES) return 0;
        if (folderPath.empty()) return 0;

        int loadedCount = 0;

        try {
            fs::path folder(folderPath);
            if (!fs::exists(folder) || !fs::is_directory(folder)) {
                SKSE::log::debug("ParticleTextures: Folder not found: {}", folderPath);
                return 0;
            }

            for (const auto& entry : fs::directory_iterator(folder)) {
                if (!entry.is_regular_file()) continue;

                auto ext = entry.path().extension().string();
                // Case-insensitive PNG check
                if (ext != ".png" && ext != ".PNG") continue;

                auto info = LoadTextureFromFile(device, entry.path().string());
                if (info.srv) {
                    g_textures[styleIndex].push_back(info);
                    loadedCount++;
                }
            }

            if (loadedCount > 0) {
                SKSE::log::info("ParticleTextures: Loaded {} textures from {}", loadedCount, folderPath);
            }
        } catch (const std::exception& e) {
            SKSE::log::warn("ParticleTextures: Error scanning folder {}: {}", folderPath, e.what());
        }

        return loadedCount;
    }

    bool Initialize(ID3D11Device* device)
    {
        if (g_initialized || !device) return g_initialized;

        SKSE::log::info("ParticleTextures: Initializing particle textures...");

        // Store device and get context
        g_device = device;
        device->GetImmediateContext(&g_context);

        // Create point sampler for small sprites
        D3D11_SAMPLER_DESC pointDesc = {};
        pointDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // No interpolation
        pointDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        pointDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        pointDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        pointDesc.MipLODBias = 0.0f;
        pointDesc.MaxAnisotropy = 1;
        pointDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        pointDesc.MinLOD = 0;
        pointDesc.MaxLOD = D3D11_FLOAT32_MAX;

        HRESULT hr = device->CreateSamplerState(&pointDesc, &g_pointSampler);
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create point sampler");
        } else {
            SKSE::log::info("ParticleTextures: Created point sampler for small sprites");
        }

        // Create linear sampler for high-resolution textures
        D3D11_SAMPLER_DESC linearDesc = pointDesc;
        linearDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        hr = device->CreateSamplerState(&linearDesc, &g_linearSampler);
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create linear sampler");
        } else {
            SKSE::log::info("ParticleTextures: Created linear sampler for HD particles");
        }

        // Create additive blend state for bright magical particles
        D3D11_BLEND_DESC addDesc = {};
        addDesc.AlphaToCoverageEnable = FALSE;
        addDesc.IndependentBlendEnable = FALSE;
        addDesc.RenderTarget[0].BlendEnable = TRUE;
        addDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        addDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        addDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        addDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        addDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        addDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        addDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&addDesc, &g_additiveBlend);
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create additive blend state");
        } else {
            SKSE::log::info("ParticleTextures: Created additive blend state");
        }

        // Create a screen-like blend state for softer luminous sprites.
        // Gate source contribution by source alpha to avoid rectangle artifacts.
        D3D11_BLEND_DESC screenDesc = addDesc;
        screenDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        screenDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
        hr = device->CreateBlendState(&screenDesc, &g_screenBlend);
        if (FAILED(hr)) {
            SKSE::log::warn("ParticleTextures: Failed to create screen blend state");
        } else {
            SKSE::log::info("ParticleTextures: Created screen blend state");
        }

        // Base path for particle textures
        const std::string basePath = "Data/SKSE/Plugins/whois/particles/";

        // Map particle styles to folder names
        struct FolderMapping {
            int style;
            const char* folder;
        };

        FolderMapping mappings[] = {
            { 0, "stars" },   // Stars
            { 1, "sparks" },  // Sparks
            { 2, "wisps" },   // Wisps
            { 3, "runes" },   // Runes
            { 4, "orbs" },    // Orbs
        };

        int totalLoaded = 0;
        for (const auto& m : mappings) {
            std::string folderPath = basePath + m.folder;
            int count = LoadTexturesFromFolder(device, m.style, folderPath);
            if (count > 0) {
                SKSE::log::info("ParticleTextures: [{}] loaded {} textures from {}", m.folder, count, folderPath);
            } else {
                SKSE::log::warn("ParticleTextures: [{}] NO textures found in {}", m.folder, folderPath);
            }
            totalLoaded += count;
        }

        g_initialized = (totalLoaded > 0);
        SKSE::log::info("ParticleTextures: === TOTAL: {} particle textures loaded ===", totalLoaded);
        if (!g_initialized) {
            SKSE::log::error("ParticleTextures: NO TEXTURES LOADED - falling back to shape rendering");
        }
        return g_initialized;
    }

    // Callback to set a specific sampler before drawing a sprite.
    static void SetSamplerCallback(const ImDrawList*, const ImDrawCmd* cmd)
    {
        auto* sampler = reinterpret_cast<ID3D11SamplerState*>(cmd ? cmd->UserCallbackData : nullptr);
        if (g_context && sampler) {
            g_context->PSSetSamplers(0, 1, &sampler);
        }
    }

    static void SetBlendCallback(const ImDrawList*, const ImDrawCmd* cmd)
    {
        auto* blend = reinterpret_cast<ID3D11BlendState*>(cmd ? cmd->UserCallbackData : nullptr);
        if (g_context && blend) {
            constexpr float blendFactor[4] = {0, 0, 0, 0};
            g_context->OMSetBlendState(blend, blendFactor, 0xFFFFFFFF);
        }
    }

    bool IsInitialized()
    {
        return g_initialized;
    }

    int GetTextureCount(int style)
    {
        if (style < 0 || style >= NUM_TYPES) return 0;
        return static_cast<int>(g_textures[style].size());
    }

    // Simple hash for better texture distribution while remaining stable
    static size_t HashIndex(int style, int particleIndex)
    {
        // Mix style and index for varied distribution
        // Using prime multipliers for better spread
        size_t hash = static_cast<size_t>(particleIndex);
        hash ^= hash >> 16;
        hash *= 0x85ebca6b;
        hash ^= hash >> 13;
        hash *= 0xc2b2ae35;
        hash ^= hash >> 16;
        hash ^= static_cast<size_t>(style) * 2654435761;
        return hash;
    }

    static const TextureInfo* GetTextureInfoForIndex(int style, int particleIndex)
    {
        if (style < 0 || style >= NUM_TYPES) return nullptr;
        if (g_textures[style].empty()) return nullptr;

        const size_t texCount = g_textures[style].size();
        const size_t texIndex = HashIndex(style, particleIndex) % texCount;
        return &g_textures[style][texIndex];
    }

    ImTextureID GetRandomTexture(int style, int particleIndex)
    {
        const TextureInfo* info = GetTextureInfoForIndex(style, particleIndex);
        return info ? reinterpret_cast<ImTextureID>(info->srv) : ImTextureID{};
    }

    void DrawSpriteWithIndex(ImDrawList* list, const ImVec2& center, float size,
                             int style, int particleIndex, ImU32 color,
                             BlendMode blendMode, float rotation)
    {
        if (!list || style < 0 || style >= NUM_TYPES) return;

        const TextureInfo* texInfo = GetTextureInfoForIndex(style, particleIndex);
        if (!texInfo || !texInfo->srv) return;
        ImTextureID tex = reinterpret_cast<ImTextureID>(texInfo->srv);

        // Normalize display size for high-resolution textures.
        // 2K (2048px) sources should display at a visible size, not shrink to dots.
        const int texMaxPx = (texInfo->width > texInfo->height) ? texInfo->width : texInfo->height;
        const float texMaxDim = static_cast<float>(texMaxPx);
        const float resolutionScale = (texMaxDim > 0.0f)
            ? std::clamp(1200.0f / texMaxDim, 0.45f, 1.0f)
            : 1.0f;
        const float scaledSize = size * resolutionScale;
        float halfSize = scaledSize * 0.5f;
        if (halfSize <= 0.01f) return;

        // Use linear filtering for HD textures, point filtering for tiny sprites.
        ID3D11SamplerState* samplerToUse = (texMaxDim > 64.0f && g_linearSampler)
            ? g_linearSampler
            : g_pointSampler;

        ID3D11BlendState* blendToUse = nullptr;
        switch (blendMode) {
            case BlendMode::Additive:
                blendToUse = g_additiveBlend;
                break;
            case BlendMode::Screen:
                blendToUse = g_screenBlend;
                break;
            case BlendMode::Alpha:
            default:
                blendToUse = nullptr;
                break;
        }

        if (samplerToUse) {
            list->AddCallback(SetSamplerCallback, samplerToUse);
        }
        if (blendToUse) {
            list->AddCallback(SetBlendCallback, blendToUse);
        }

        if (rotation == 0.0f) {
            // Simple axis-aligned quad
            ImVec2 pMin(center.x - halfSize, center.y - halfSize);
            ImVec2 pMax(center.x + halfSize, center.y + halfSize);
            list->AddImage(tex, pMin, pMax, ImVec2(0, 0), ImVec2(1, 1), color);
        } else {
            // Rotated quad using AddImageQuad
            float cosR = std::cos(rotation);
            float sinR = std::sin(rotation);

            // Calculate rotated corners
            ImVec2 corners[4];
            float offsets[4][2] = {
                { -halfSize, -halfSize },  // Top-left
                {  halfSize, -halfSize },  // Top-right
                {  halfSize,  halfSize },  // Bottom-right
                { -halfSize,  halfSize },  // Bottom-left
            };

            for (int i = 0; i < 4; i++) {
                float rx = offsets[i][0] * cosR - offsets[i][1] * sinR;
                float ry = offsets[i][0] * sinR + offsets[i][1] * cosR;
                corners[i] = ImVec2(center.x + rx, center.y + ry);
            }

            // UV coordinates for the corners
            ImVec2 uvs[4] = {
                ImVec2(0, 0),  // Top-left
                ImVec2(1, 0),  // Top-right
                ImVec2(1, 1),  // Bottom-right
                ImVec2(0, 1),  // Bottom-left
            };

            list->AddImageQuad(tex, corners[0], corners[1], corners[2], corners[3],
                               uvs[0], uvs[1], uvs[2], uvs[3], color);
        }

        // Reset to let ImGui restore its default sampler
        if (samplerToUse || blendToUse) {
            list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }
    }
}
