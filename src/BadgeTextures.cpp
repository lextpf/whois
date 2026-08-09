#include <SKSE/SKSE.h>

#include "BadgeTextures.hpp"

#include "RasterQuality.hpp"

#include <nanosvg.h>
#include <nanosvgrast.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace BadgeTextures
{
using Microsoft::WRL::ComPtr;

namespace
{
std::mutex& Mutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>>& Map()
{
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> m;
    return m;
}

std::atomic<bool>& Initialized()
{
    static std::atomic<bool> b{false};
    return b;
}

// Full-color prestige emblem textures for the tier badge (index 0..N-1),
// separate from the tinted SVG mask Map() above.
std::vector<ComPtr<ID3D11ShaderResourceView>>& TierImages()
{
    static std::vector<ComPtr<ID3D11ShaderResourceView>> v;
    return v;
}

std::atomic<int>& TierImageCountAtomic()
{
    static std::atomic<int> n{0};
    return n;
}

// Fraction of the canvas the trimmed emblem fills, so every emblem reads at the
// same on-screen size regardless of its own transparent margin.
constexpr float TIER_IMG_FILL = .92f;

// Rasterize <path> into a centered square float-alpha mask.
// The SVG's own layer opacities (duotone secondary .4, primary 1.0) land in the
// alpha; an icon whose strongest layer is translucent is normalized so its peak
// alpha is 1.0. Returns false when the file does not parse or the mask is empty.
bool RasterizeIcon(NSVGrasterizer* rast, const std::string& path, std::vector<float>& outAlpha)
{
    NSVGimage* img = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!img)
    {
        return false;
    }
    if (img->width <= .0f || img->height <= .0f)
    {
        nsvgDelete(img);
        return false;
    }

    // The duotone secondary layer ships at opacity .4, so half of every icon
    // rasterizes at 40% and reads faint. Lift any translucent shape to the floor
    // before rasterizing; the primary layer (1.0) is untouched, and the peak
    // normalization below keeps the strongest layer at 1.0.
    constexpr float kSecondaryOpacityFloor = .80f;
    for (NSVGshape* shape = img->shapes; shape != nullptr; shape = shape->next)
    {
        if (shape->opacity < kSecondaryOpacityFloor)
        {
            shape->opacity = kSecondaryOpacityFloor;
        }
    }

    const int size = RasterQuality::STATUS_ICON_TEXTURE_SIZE;
    const float maxDim = (img->width > img->height) ? img->width : img->height;
    const float scale = static_cast<float>(size) / maxDim;
    const float tx = (static_cast<float>(size) - img->width * scale) * .5f;
    const float ty = (static_cast<float>(size) - img->height * scale) * .5f;

    std::vector<unsigned char> rgba(static_cast<size_t>(size) * size * 4, 0);
    nsvgRasterize(rast, img, tx, ty, scale, rgba.data(), size, size, size * 4);
    nsvgDelete(img);

    outAlpha.resize(static_cast<size_t>(size) * size);
    float maxAlpha = .0f;
    for (size_t i = 0; i < outAlpha.size(); ++i)
    {
        outAlpha[i] = static_cast<float>(rgba[i * 4 + 3]) * (1.0f / 255.0f);
        maxAlpha = (std::max)(maxAlpha, outAlpha[i]);
    }
    if (maxAlpha <= .0f)
    {
        return false;
    }
    if (maxAlpha < 1.0f)
    {
        const float norm = 1.0f / maxAlpha;
        for (float& a : outAlpha)
        {
            a = (std::min)(1.0f, a * norm);
        }
    }
    return true;
}

// Create a mipmapped white-RGBA texture from a float alpha mask (same
// pipeline as the procedural particle sprites: 2x2 box reduction in float,
// black RGB on fully transparent texels).
ComPtr<ID3D11ShaderResourceView> CreateBadgeTexture(ID3D11Device* device,
                                                    std::vector<float> levelAlpha)
{
    const int size = RasterQuality::STATUS_ICON_TEXTURE_SIZE;
    int mipLevels = 1;
    for (int d = size; d > 1; d >>= 1)
    {
        ++mipLevels;
    }

    std::vector<std::vector<BYTE>> mipPixels(static_cast<size_t>(mipLevels));
    std::vector<D3D11_SUBRESOURCE_DATA> initData(static_cast<size_t>(mipLevels));
    int dim = size;
    for (int level = 0; level < mipLevels; ++level)
    {
        if (level > 0)
        {
            const int prevDim = dim;
            dim = (std::max)(1, dim >> 1);
            std::vector<float> reduced(static_cast<size_t>(dim) * dim);
            for (int y = 0; y < dim; ++y)
            {
                for (int x = 0; x < dim; ++x)
                {
                    const size_t i00 =
                        static_cast<size_t>(y) * 2 * prevDim + static_cast<size_t>(x) * 2;
                    const size_t i10 = i00 + prevDim;
                    reduced[static_cast<size_t>(y) * dim + x] =
                        (levelAlpha[i00] + levelAlpha[i00 + 1] + levelAlpha[i10] +
                         levelAlpha[i10 + 1]) *
                        .25f;
                }
            }
            levelAlpha = std::move(reduced);
        }

        auto& pixels = mipPixels[level];
        pixels.resize(static_cast<size_t>(dim) * dim * 4);
        for (int i = 0; i < dim * dim; ++i)
        {
            const float a = levelAlpha[static_cast<size_t>(i)];
            const BYTE alphaByte =
                static_cast<BYTE>(std::clamp(static_cast<int>(a * 255.0f + .5f), 0, 255));
            const BYTE rgb = (alphaByte == 0) ? 0 : 255;
            pixels[static_cast<size_t>(i) * 4 + 0] = rgb;
            pixels[static_cast<size_t>(i) * 4 + 1] = rgb;
            pixels[static_cast<size_t>(i) * 4 + 2] = rgb;
            pixels[static_cast<size_t>(i) * 4 + 3] = alphaByte;
        }
        initData[level].pSysMem = pixels.data();
        initData[level].SysMemPitch = static_cast<UINT>(dim) * 4;
        initData[level].SysMemSlicePitch = 0;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(size);
    texDesc.Height = static_cast<UINT>(size);
    texDesc.MipLevels = static_cast<UINT>(mipLevels);
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texDesc, initData.data(), &texture)))
    {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(mipLevels);

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf())))
    {
        return nullptr;
    }
    return srv;
}

// Decode a PNG into tightly-packed 32bpp RGBA via WIC.  RGB on fully
// transparent texels is zeroed so downscales cannot bleed hidden color.
bool LoadPngRGBA(IWICImagingFactory* wic,
                 const std::string& path,
                 std::vector<uint8_t>& outPixels,
                 int& outW,
                 int& outH)
{
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        return false;
    }
    std::wstring widePath(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.data(), wideLen);

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(
            widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
    {
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)))
    {
        return false;
    }
    if (FAILED(converter->Initialize(frame.Get(),
                                     GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone,
                                     nullptr,
                                     .0f,
                                     WICBitmapPaletteTypeCustom)))
    {
        return false;
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0)
    {
        return false;
    }
    // Sanity cap (~64 MP) so a bad file cannot request an enormous buffer.
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > (1ull << 26))
    {
        return false;
    }
    const UINT stride = w * 4;
    outPixels.assign(static_cast<size_t>(stride) * static_cast<size_t>(h), 0);
    if (FAILED(converter->CopyPixels(
            nullptr, stride, static_cast<UINT>(outPixels.size()), outPixels.data())))
    {
        return false;
    }
    for (size_t i = 0; i + 3 < outPixels.size(); i += 4)
    {
        if (outPixels[i + 3] == 0)
        {
            outPixels[i + 0] = 0;
            outPixels[i + 1] = 0;
            outPixels[i + 2] = 0;
        }
    }
    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    return true;
}

// Trim the emblem to its opaque bounding box and area-resample it (alpha-
// weighted, so transparent margins never darken edges) into a centered NxN
// straight-alpha float RGBA canvas filling TIER_IMG_FILL of the square.
bool BuildTierBase(const std::vector<uint8_t>& src, int sw, int sh, std::vector<float>& out, int N)
{
    if (sw <= 0 || sh <= 0)
    {
        return false;
    }
    int x0 = sw;
    int y0 = sh;
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < sh; ++y)
    {
        for (int x = 0; x < sw; ++x)
        {
            if (src[(static_cast<size_t>(y) * sw + x) * 4 + 3] > 8)
            {
                x0 = (std::min)(x0, x);
                x1 = (std::max)(x1, x);
                y0 = (std::min)(y0, y);
                y1 = (std::max)(y1, y);
            }
        }
    }
    if (x1 < x0 || y1 < y0)
    {
        return false;
    }
    const int cw = x1 - x0 + 1;
    const int ch = y1 - y0 + 1;
    const float avail = static_cast<float>(N) * TIER_IMG_FILL;
    const float s = (std::min)(avail / static_cast<float>(cw), avail / static_cast<float>(ch));
    const int dw = (std::max)(1, static_cast<int>(std::lround(cw * s)));
    const int dh = (std::max)(1, static_cast<int>(std::lround(ch * s)));
    const int ox = (N - dw) / 2;
    const int oy = (N - dh) / 2;

    out.assign(static_cast<size_t>(N) * static_cast<size_t>(N) * 4, .0f);
    for (int dy = 0; dy < dh; ++dy)
    {
        const int sy0 = y0 + static_cast<int>(std::floor(static_cast<float>(dy) * ch / dh));
        const int sy1 =
            (std::min)(y1 + 1,
                       y0 + static_cast<int>(std::ceil(static_cast<float>(dy + 1) * ch / dh)));
        for (int dx = 0; dx < dw; ++dx)
        {
            const int sx0 = x0 + static_cast<int>(std::floor(static_cast<float>(dx) * cw / dw));
            const int sx1 =
                (std::min)(x1 + 1,
                           x0 + static_cast<int>(std::ceil(static_cast<float>(dx + 1) * cw / dw)));
            float ar = .0f;
            float ag = .0f;
            float ab = .0f;
            float asum = .0f;
            int wsum = 0;
            for (int yy = (std::max)(y0, sy0); yy < sy1; ++yy)
            {
                for (int xx = (std::max)(x0, sx0); xx < sx1; ++xx)
                {
                    const size_t si = (static_cast<size_t>(yy) * sw + xx) * 4;
                    const float a = src[si + 3] * (1.0f / 255.0f);
                    ar += src[si + 0] * (1.0f / 255.0f) * a;
                    ag += src[si + 1] * (1.0f / 255.0f) * a;
                    ab += src[si + 2] * (1.0f / 255.0f) * a;
                    asum += a;
                    ++wsum;
                }
            }
            const size_t di =
                (static_cast<size_t>(oy + dy) * static_cast<size_t>(N) + (ox + dx)) * 4;
            if (asum > 1e-6f)
            {
                out[di + 0] = ar / asum;
                out[di + 1] = ag / asum;
                out[di + 2] = ab / asum;
            }
            out[di + 3] = (wsum > 0) ? asum / static_cast<float>(wsum) : .0f;
        }
    }
    return true;
}

// Create a mipmapped full-color RGBA texture from a straight-alpha float base
// (alpha-weighted 2x2 reduction; RGB zeroed on empty texels).
ComPtr<ID3D11ShaderResourceView> CreateRGBATexture(ID3D11Device* device,
                                                   std::vector<float> base,
                                                   int size)
{
    int mipLevels = 1;
    for (int d = size; d > 1; d >>= 1)
    {
        ++mipLevels;
    }

    std::vector<std::vector<BYTE>> mipPixels(static_cast<size_t>(mipLevels));
    std::vector<D3D11_SUBRESOURCE_DATA> initData(static_cast<size_t>(mipLevels));
    int dim = size;
    std::vector<float> cur = std::move(base);
    for (int level = 0; level < mipLevels; ++level)
    {
        if (level > 0)
        {
            const int prev = dim;
            dim = (std::max)(1, dim >> 1);
            std::vector<float> reduced(static_cast<size_t>(dim) * dim * 4);
            for (int y = 0; y < dim; ++y)
            {
                for (int x = 0; x < dim; ++x)
                {
                    const size_t i00 =
                        (static_cast<size_t>(y) * 2 * prev + static_cast<size_t>(x) * 2) * 4;
                    const size_t i01 = i00 + 4;
                    const size_t i10 = i00 + static_cast<size_t>(prev) * 4;
                    const size_t i11 = i10 + 4;
                    const float a00 = cur[i00 + 3];
                    const float a01 = cur[i01 + 3];
                    const float a10 = cur[i10 + 3];
                    const float a11 = cur[i11 + 3];
                    const float asum = a00 + a01 + a10 + a11;
                    const size_t di = (static_cast<size_t>(y) * dim + x) * 4;
                    for (int c = 0; c < 3; ++c)
                    {
                        reduced[di + c] = (asum > 1e-6f)
                                              ? (cur[i00 + c] * a00 + cur[i01 + c] * a01 +
                                                 cur[i10 + c] * a10 + cur[i11 + c] * a11) /
                                                    asum
                                              : .0f;
                    }
                    reduced[di + 3] = asum * .25f;
                }
            }
            cur = std::move(reduced);
        }

        auto& pixels = mipPixels[static_cast<size_t>(level)];
        pixels.resize(static_cast<size_t>(dim) * dim * 4);
        const auto toByte = [](float v)
        { return static_cast<BYTE>(std::clamp(static_cast<int>(v * 255.0f + .5f), 0, 255)); };
        for (int i = 0; i < dim * dim; ++i)
        {
            const size_t s = static_cast<size_t>(i) * 4;
            const BYTE alphaByte = toByte(cur[s + 3]);
            pixels[s + 0] = (alphaByte == 0) ? 0 : toByte(cur[s + 0]);
            pixels[s + 1] = (alphaByte == 0) ? 0 : toByte(cur[s + 1]);
            pixels[s + 2] = (alphaByte == 0) ? 0 : toByte(cur[s + 2]);
            pixels[s + 3] = alphaByte;
        }
        initData[static_cast<size_t>(level)].pSysMem = pixels.data();
        initData[static_cast<size_t>(level)].SysMemPitch = static_cast<UINT>(dim) * 4;
        initData[static_cast<size_t>(level)].SysMemSlicePitch = 0;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(size);
    texDesc.Height = static_cast<UINT>(size);
    texDesc.MipLevels = static_cast<UINT>(mipLevels);
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texDesc, initData.data(), &texture)))
    {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(mipLevels);

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf())))
    {
        return nullptr;
    }
    return srv;
}
}  // namespace

bool Initialize(ID3D11Device* device,
                const std::string& folder,
                const std::vector<std::string>& names)
{
    const std::lock_guard<std::mutex> lock(Mutex());
    Map().clear();

    if (!device || folder.empty())
    {
        Initialized().store(true, std::memory_order_release);
        return false;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast)
    {
        Initialized().store(true, std::memory_order_release);
        return false;
    }

    int loaded = 0;
    int requested = 0;
    for (const auto& name : names)
    {
        if (name.empty() || Map().contains(name))
        {
            continue;
        }
        ++requested;

        const std::string path = folder + "/" + name + ".svg";
        std::vector<float> alpha;
        if (!RasterizeIcon(rast, path, alpha))
        {
            SKSE::log::warn("BadgeTextures: failed to load '{}'", path);
            continue;
        }
        auto srv = CreateBadgeTexture(device, std::move(alpha));
        if (!srv)
        {
            SKSE::log::warn("BadgeTextures: texture creation failed for '{}'", path);
            continue;
        }
        Map().emplace(name, std::move(srv));
        ++loaded;
    }
    nsvgDeleteRasterizer(rast);

    SKSE::log::info("BadgeTextures: loaded {}/{} badge icons from '{}'", loaded, requested, folder);
    Initialized().store(true, std::memory_order_release);
    return loaded > 0;
}

bool IsInitialized()
{
    return Initialized().load(std::memory_order_acquire);
}

void Shutdown()
{
    const std::lock_guard<std::mutex> lock(Mutex());
    Map().clear();
    TierImages().clear();
    TierImageCountAtomic().store(0, std::memory_order_release);
    Initialized().store(false, std::memory_order_release);
}

ImTextureID Get(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(Mutex());
    auto it = Map().find(name);
    if (it == Map().end() || !it->second)
    {
        return 0;
    }
    return reinterpret_cast<ImTextureID>(it->second.Get());
}

int InitializeTierImages(ID3D11Device* device, const std::vector<std::string>& paths)
{
    const std::lock_guard<std::mutex> lock(Mutex());
    TierImages().clear();
    TierImageCountAtomic().store(0, std::memory_order_release);

    if (!device || paths.empty())
    {
        return 0;
    }

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        !wic)
    {
        SKSE::log::warn("BadgeTextures: WIC unavailable - tier emblem images disabled");
        return 0;
    }

    // Push one entry per rank (nullptr on failure) so emblem-to-rank alignment
    // survives a bad file; a fully-empty set is cleared below to trigger the FA
    // fallback.
    int loaded = 0;
    for (const std::string& path : paths)
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        std::vector<uint8_t> src;
        int sw = 0;
        int sh = 0;
        std::vector<float> base;
        if (!path.empty() && LoadPngRGBA(wic.Get(), path, src, sw, sh) &&
            BuildTierBase(src, sw, sh, base, RasterQuality::RANK_EMBLEM_TEXTURE_SIZE))
        {
            srv =
                CreateRGBATexture(device, std::move(base), RasterQuality::RANK_EMBLEM_TEXTURE_SIZE);
        }
        if (srv)
        {
            ++loaded;
        }
        else
        {
            SKSE::log::warn("BadgeTextures: tier emblem '{}' failed to load - rank blank", path);
        }
        TierImages().push_back(std::move(srv));
    }

    if (loaded == 0)
    {
        TierImages().clear();  // nothing usable -> fall back to the FA tier icons
    }
    TierImageCountAtomic().store(static_cast<int>(TierImages().size()), std::memory_order_release);
    SKSE::log::info(
        "BadgeTextures: loaded {}/{} tier emblem image(s) from manifest", loaded, paths.size());
    return loaded;
}

ImTextureID GetTierImage(int index)
{
    const std::lock_guard<std::mutex> lock(Mutex());
    if (index < 0 || index >= static_cast<int>(TierImages().size()) ||
        !TierImages()[static_cast<size_t>(index)])
    {
        return 0;
    }
    return reinterpret_cast<ImTextureID>(TierImages()[static_cast<size_t>(index)].Get());
}

int TierImageCount()
{
    return TierImageCountAtomic().load(std::memory_order_acquire);
}
}  // namespace BadgeTextures
