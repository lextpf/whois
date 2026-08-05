#include "DeckPng.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincodec.h>
#include <Windows.h>
#include <wrl/client.h>

#include <iomanip>
#include <limits>
#include <sstream>

namespace Deck
{
namespace
{
using Microsoft::WRL::ComPtr;

std::string HResultText(const char* action, HRESULT hr)
{
    std::ostringstream out;
    out << action << " (HRESULT 0x" << std::uppercase << std::hex << static_cast<unsigned long>(hr)
        << ')';
    return out.str();
}

// Scoped COM apartment. CoUninitialize runs only when CoInitializeEx succeeded, which keeps
// the reference count balanced in both success shapes: S_OK for a fresh apartment and S_FALSE
// for a thread already in a multithreaded one. A caller that is already in a single-threaded
// apartment gets RPC_E_CHANGED_MODE, a failure, so its apartment is left untouched.
// EncodeBgraPng therefore accepts that one HRESULT as a usable state.
class ComApartment
{
public:
    ComApartment()
        : m_Result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~ComApartment()
    {
        if (SUCCEEDED(m_Result))
        {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT Result() const { return m_Result; }

private:
    HRESULT m_Result;
};
}  // namespace

bool EncodeBgraPng(const std::filesystem::path& path,
                   int width,
                   int height,
                   std::span<const std::uint8_t> bgra,
                   std::string& error)
{
    error.clear();
    if (path.empty() || width <= 0 || height <= 0 ||
        static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / 4)
    {
        error = "Deck encoder received invalid PNG dimensions or path";
        return false;
    }

    const std::size_t strideBytes = static_cast<std::size_t>(width) * 4;
    if (static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / strideBytes ||
        bgra.size() != strideBytes * static_cast<std::size_t>(height) ||
        strideBytes > std::numeric_limits<UINT>::max() ||
        bgra.size() > std::numeric_limits<UINT>::max())
    {
        error = "Deck encoder received invalid PNG pixel data";
        return false;
    }

    const ComApartment apartment;
    const HRESULT comHr = apartment.Result();
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
    {
        error = HResultText("Could not initialize COM for Deck", comHr);
        return false;
    }

    HRESULT hr = S_OK;
    {
        // Keep every object that can hold the destination file open inside this
        // scope so a failed encode can remove the partial file below.
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory,
                              nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf()));

        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (SUCCEEDED(hr))
        {
            hr = factory->CreateStream(stream.GetAddressOf());
        }
        if (SUCCEEDED(hr))
        {
            hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        }
        if (SUCCEEDED(hr))
        {
            hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
        }
        if (SUCCEEDED(hr))
        {
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }
        if (SUCCEEDED(hr))
        {
            hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->Initialize(properties.Get());
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->SetResolution(96.0, 96.0);
        }

        // SetPixelFormat is a negotiation: WIC writes back the closest format the PNG encoder
        // supports, and WritePixels then expects the pixels in that format. Fail instead, so a
        // card is never written with shifted channels or a dropped alpha.
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr))
        {
            hr = frame->SetPixelFormat(&format);
            if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA)
            {
                hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
            }
        }
        if (SUCCEEDED(hr))
        {
            // WritePixels takes a non-const buffer but only reads from it.
            hr = frame->WritePixels(static_cast<UINT>(height),
                                    static_cast<UINT>(strideBytes),
                                    static_cast<UINT>(bgra.size()),
                                    const_cast<BYTE*>(bgra.data()));
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->Commit();
        }
        if (SUCCEEDED(hr))
        {
            hr = encoder->Commit();
        }
    }

    if (FAILED(hr))
    {
        error = HResultText("Could not encode Deck PNG", hr);
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        return false;
    }
    return true;
}
}  // namespace Deck
