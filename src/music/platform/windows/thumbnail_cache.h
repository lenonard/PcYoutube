#pragma once

#include <d3d11.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <wrl/client.h>

namespace pcyoutube::windows {

class ThumbnailCache {
public:
    explicit ThumbnailCache(ID3D11Device* device);
    ~ThumbnailCache();

    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Returns nullptr while a thumbnail is being downloaded/decoded.
    // Calling this method also schedules an unseen video id for background loading.
    ID3D11ShaderResourceView* get_or_request(std::string_view video_id);

private:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State state = State::Loading;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
    };

    void worker_loop(std::stop_token stop_token);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::jthread worker_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<std::string> pending_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace pcyoutube::windows
