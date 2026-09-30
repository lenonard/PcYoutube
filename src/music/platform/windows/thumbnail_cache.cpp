#include "thumbnail_cache.h"

#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace pcyoutube::windows {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring widen_ascii(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

std::vector<std::uint8_t> download_thumbnail(std::string_view video_id) {
    std::vector<std::uint8_t> bytes;
    if (video_id.empty()) return bytes;

    HINTERNET session = WinHttpOpen(L"PcYoutube/0.6",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return bytes;

    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);
    HINTERNET connect = WinHttpConnect(session, L"i.ytimg.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect == nullptr) {
        WinHttpCloseHandle(session);
        return bytes;
    }

    const std::wstring path = L"/vi/" + widen_ascii(video_id) + L"/hqdefault.jpg";
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return bytes;
    }

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
              WinHttpReceiveResponse(request, nullptr) != FALSE;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX) != FALSE &&
             status >= 200 && status < 300;
    }

    constexpr std::size_t kMaxThumbnailBytes = 6U * 1024U * 1024U;
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        if (bytes.size() + available > kMaxThumbnailBytes) {
            bytes.clear();
            break;
        }

        const std::size_t offset = bytes.size();
        bytes.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes.data() + offset, available, &read)) {
            bytes.clear();
            break;
        }
        bytes.resize(offset + read);
        if (read == 0) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return bytes;
}

ComPtr<ID3D11ShaderResourceView> decode_texture(ID3D11Device* device,
                                                IWICImagingFactory* factory,
                                                std::vector<std::uint8_t>& bytes) {
    ComPtr<ID3D11ShaderResourceView> result;
    if (device == nullptr || factory == nullptr || bytes.empty()) return result;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return result;
    if (FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())))) {
        return result;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad, &decoder))) {
        return result;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return result;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > 4096 || height > 4096) {
        return result;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return result;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return result;
    }

    const UINT stride = width * 4U;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                     pixels.data()))) {
        return result;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &texture))) return result;

    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = desc.Format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &view_desc, &result))) {
        result.Reset();
    }
    return result;
}

}  // namespace

ThumbnailCache::ThumbnailCache(ID3D11Device* device) : device_(device) {
    worker_ = std::jthread([this](std::stop_token token) { worker_loop(token); });
}

ThumbnailCache::~ThumbnailCache() {
    if (worker_.joinable()) {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

ID3D11ShaderResourceView* ThumbnailCache::get_or_request(std::string_view video_id) {
    if (video_id.empty()) return nullptr;
    const std::string key(video_id);

    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        return found->second.state == State::Ready ? found->second.texture.Get() : nullptr;
    }

    entries_.emplace(key, Entry{});
    pending_.push_back(key);
    condition_.notify_one();
    return nullptr;
}

void ThumbnailCache::worker_loop(std::stop_token stop_token) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_result);

    ComPtr<IWICImagingFactory> factory;
    if (com_initialized) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory));
    }

    while (!stop_token.stop_requested()) {
        std::string id;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stop_token, [this] { return !pending_.empty(); });
            if (stop_token.stop_requested()) break;
            if (pending_.empty()) continue;
            id = std::move(pending_.front());
            pending_.pop_front();
        }

        ComPtr<ID3D11ShaderResourceView> texture;
        if (factory && device_) {
            auto bytes = download_thumbnail(id);
            texture = decode_texture(device_.Get(), factory.Get(), bytes);
        }

        {
            std::lock_guard lock(mutex_);
            auto it = entries_.find(id);
            if (it != entries_.end()) {
                if (texture) {
                    it->second.texture = std::move(texture);
                    it->second.state = State::Ready;
                } else {
                    it->second.state = State::Failed;
                }
            }
        }
    }

    if (com_initialized) CoUninitialize();
}

}  // namespace pcyoutube::windows
